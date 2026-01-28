/*---------------------------------------------------------------------------*/
// Copyright (c) 2022 ETH Zurich.
// All rights reserved.
//
// This file is distributed under the terms in the attached LICENSE file.
// If you do not find this file, copies can be found by writing to:
// ETH Zurich D-INFK, Stampfenbachstrasse 114, CH-8092 Zurich. Attn: Systems Group
/*---------------------------------------------------------------------------*/

/*
 * Enzian Memory Benchmark
 *
 * Latency and bandwidth memory benchmark using 1G hugepages
 * Core-to-core latency benchmark
 * To allocate huge pages in the main memory, do:
 * echo 3 > /sys/devices/system/node/node0/hugepages/hugepages-1048576kB/nr_hugepages
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <linux/mman.h>
#include <assert.h>
#include <pthread.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#include <string.h>

#define SIZE_EXP 26
#define SIZE (1UL << SIZE_EXP)
#define MASK ((SIZE / 8) - 1)
#define COUNT (SIZE)
#define STRIDE ((128) * 16)
#define CACHELINE_SIZE 128


static __inline__ uint64_t rdtsc(void)
{
#ifdef __aarch64__
    uint64_t t;
    __asm__ __volatile__(" isb\nmrs %0, cntvct_el0" : "=r" (t));
    return t;
#endif
#ifdef __amd64__
    uint32_t a;
    uint32_t d;
    __asm__ __volatile__(" lfence\nrdtsc\n" : "=a" (a), "=d" (d));
    return ((uint64_t) a) | (((uint64_t) d) << 32);
#endif
}

static __inline__ void cache_flush(volatile void *m)
{
#ifdef __aarch64__
    __asm__ __volatile__(" dc civac,%0\n" :: "r" (m));
#endif
#ifdef __amd64__
    __asm__ __volatile__(" clflush (%0)\n" :: "r" (m));
#endif
}

// SIMD types to fit in a 128-bit vector register, 8 vector in a cache line
// 4 * float
typedef float v4f __attribute__    ((vector_size (16)));
typedef v4f cacheline_float_t[8];
// 2 * uint
typedef uint64_t v2i __attribute__ ((vector_size (16)));
typedef v2i cacheline_uint64_t[8];

unsigned first_cpu, last_cpu, use_cpu_memory, do_overall, do_cache_to_cache, do_latency, do_seq_latency, do_throughput, do_stress;

void *area = NULL;
double rate = 1.0;
uint64_t itn;
uint64_t no_cpus = 1;
uint64_t l2_cache_size;
uint64_t area_test_size = 0;

// Barrier to launch threads simultaneously
pthread_barrier_t barrier;


uint64_t now(void)
{
    return rdtsc();
}

uint64_t now1(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec * 1000000000 + ts.tv_nsec;
}

const char * nice_size(unsigned int size)
{
    static char text[64];

    if (size < 1024) {
        snprintf(text, sizeof(text), "%9d", size);
    } else if (size < 1048576) {
        snprintf(text, sizeof(text), "%9dk", size / 1024);
    } else if (size < 1048576 * 1024) {
        snprintf(text, sizeof(text), "%9dM", size / 1024 / 1024);
    } else {
        snprintf(text, sizeof(text), "%9dG", size / 1024 / 1024 / 1024);
    }
    return text;
}

const char * nice_time(double t)
{
    static char text[64];

    t = t / rate;
    if (t < 1000.0) {
        snprintf(text, sizeof(text), "%.03fns", t);
    } else if (t < 1000000) {
        snprintf(text, sizeof(text), "%.03fus", t / 1000);
    } else if (t < 1000000000) {
        snprintf(text, sizeof(text), "%.03fms", t / 1000000);
    } else {
        snprintf(text, sizeof(text), "%.03fs", t / 1000000000);
    }
    return text;
}

// Start a thread on multiple cores and collect the results
uint64_t start_threads(uint64_t first_cpu, uint64_t last_cpu, void * (*thread_func)(void *))
{
    pthread_t tids[128];
    pthread_attr_t attr;
    cpu_set_t cpus;
    uint64_t i, min, retval;

    min = UINT64_MAX;
    pthread_barrier_init(&barrier, NULL, last_cpu - first_cpu + 2);
    for (i = first_cpu; i <= last_cpu; i++) {
        pthread_attr_init(&attr);
        CPU_ZERO(&cpus);
        CPU_SET(i, &cpus);
        pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
        pthread_create(tids + i, &attr, thread_func, (void *)i);
        pthread_attr_destroy(&attr);
    }
    pthread_barrier_wait(&barrier);
    for (i = 0; i < 3; i++) {
        pthread_barrier_wait(&barrier);
    }
    min = 0;
    for (i = first_cpu; i <= last_cpu; i++) {
        pthread_join(tids[i], (void *)&retval);
        min += retval;
    }
    pthread_barrier_destroy(&barrier);
    return min / (last_cpu - first_cpu + 1);
}

void * thread_write_cache_area(void *v)
{
    uint64_t min, cycles;
    uint64_t me = (uint64_t)v;
    uint64_t j, k;
    cacheline_float_t *a, *d, *e;

    v4f tab = {1.0, 2.0, 3.0, 4.0};
    a = area + (me * area_test_size);
    e = area + (me * area_test_size) + area_test_size;
    min = UINT64_MAX;

    for (k = 0; k < 4; k++) {
        pthread_barrier_wait(&barrier);
        cycles = now();
        for (j = 0; j < itn; j++) {
            for (d = a; d < e; d++) {
                d[0][0] = tab;
                d[0][1] = tab;
                d[0][2] = tab;
                d[0][3] = tab;
                d[0][4] = tab;
                d[0][5] = tab;
                d[0][6] = tab;
                d[0][7] = tab;
                asm("" : : "r" (d) : "memory");
            }
        }
        cycles = now() - cycles;
        if (cycles < min)
            min = cycles;
    }
    return (void *)min;
}

void * thread_clear_cache_area(void *v)
{
    uint64_t min, cycles;
    uint64_t me = (uint64_t)v;
    uint64_t j, k;
    cacheline_float_t *a, *d, *e;

    a = area + (me * area_test_size);
    e = area + (me * area_test_size) + area_test_size;
    min = UINT64_MAX;

    for (k = 0; k < 4; k++) {
        pthread_barrier_wait(&barrier);
        cycles = now();
        for (j = 0; j < itn; j++) {
            for (d = a; d < e; d++) {
                asm("dc zva, %0" : : "r" (d) : "memory");
            }
        }
        cycles = now() - cycles;
        if (cycles < min)
            min = cycles;
    }
    return (void *)min;
}

void * thread_read_cache_area(void *v)
{
    uint64_t min, cycles;
    uint64_t me = (uint64_t)v;
    uint64_t j, k;
    cacheline_float_t *a, *s, *e;
    cacheline_float_t tab;

    a = area + (me * area_test_size);
    e = area + (me * area_test_size) + area_test_size;
    min = UINT64_MAX;

    for (k = 0; k < 4; k++) {
        pthread_barrier_wait(&barrier);
        cycles = now();
        if (area_test_size <= 32768) {
            for (j = 0; j < itn; j++) {
                for (s = a; s < e; s++) {
                    tab[0] = s[0][0];
                    tab[1] = s[0][1];
                    tab[2] = s[0][2];
                    tab[3] = s[0][3];
                    tab[4] = s[0][4];
                    tab[5] = s[0][5];
                    tab[6] = s[0][6];
                    tab[7] = s[0][7];
                    asm("" : : "w" (tab[0]), "w" (tab[1]), "w" (tab[2]), "w" (tab[3]), "w" (tab[4]), "w" (tab[5]), "w" (tab[6]), "w" (tab[7]));
                }
            }
        } else if (area_test_size <= l2_cache_size) {
            for (j = 0; j < itn; j++) {
                for (s = a; s < e; s++) {
                    __builtin_prefetch(s + 4, 0, 3); // L1 prefetch
                    tab[0] = s[0][0];
                    tab[1] = s[0][1];
                    tab[2] = s[0][2];
                    tab[3] = s[0][3];
                    tab[4] = s[0][4];
                    tab[5] = s[0][5];
                    tab[6] = s[0][6];
                    tab[7] = s[0][7];
                    asm("" : : "w" (tab[0]), "w" (tab[1]), "w" (tab[2]), "w" (tab[3]), "w" (tab[4]), "w" (tab[5]), "w" (tab[6]), "w" (tab[7]));
                }
            }
        } else {
            for (j = 0; j < itn; j++) {
                for (s = a; s < e; s++) {
                    __builtin_prefetch(s + 4, 0, 3); // L1 prefetch
                    __builtin_prefetch(s + 64, 0, 2); // L2 prefetch
                    tab[0] = s[0][0];
                    tab[1] = s[0][1];
                    tab[2] = s[0][2];
                    tab[3] = s[0][3];
                    tab[4] = s[0][4];
                    tab[5] = s[0][5];
                    tab[6] = s[0][6];
                    tab[7] = s[0][7];
                    asm("" : : "w" (tab[0]), "w" (tab[1]), "w" (tab[2]), "w" (tab[3]), "w" (tab[4]), "w" (tab[5]), "w" (tab[6]), "w" (tab[7]));
                }
            }
        }
        cycles = now() - cycles;
        if (cycles < min)
            min = cycles;
    }
    return (void *)min;
}

// Do the pointer chasing latency
void do_latency_test(unsigned int size)
{
    uint64_t i, l, o, p;
    double t;
    uint64_t cycle, base, diff, avg, min;
    volatile cacheline_uint64_t *c;
    uint64_t ITS;

    l = (1 << size) / CACHELINE_SIZE; // number of cache lines
    ITS = size < 12 ? 1 << (12 - size) : 1;
    if (ITS == 0)
        ITS = 1;

    c = (cacheline_uint64_t *)area;

// find base latency
    min = UINT64_MAX;
    for (p = 0; p < 5; p++) {
        cycle = now();
        o = 0;
        for (i = 0; i < ITS * l / 8; i++) {
            asm("nop");
            asm("nop");
            asm("nop");
            asm("nop");
            asm("nop");
            asm("nop");
            asm("nop");
            asm("nop");
        }
        diff = now() - cycle;
        if (p > 0 && diff < min)
            min = diff;
    }
    base = min;

// fill the pointers using PRNG
    o = 0;
    for (i = 0; i < ITS * l; i++) {
        uint64_t no = ((o * 13 + 7) & (l - 1));
        c[o][0][0] = no;
        o = no;
    }

// do the chasing
    min = UINT64_MAX;
    for (p = 0; p < 5; p++) {
        cycle = now();
        o = 0;
        for (i = 0; i < ITS * l / 8; i++) {
            o = c[o][0][0];
            o = c[o][0][0];
            o = c[o][0][0];
            o = c[o][0][0];
            o = c[o][0][0];
            o = c[o][0][0];
            o = c[o][0][0];
            o = c[o][0][0];
            asm("":: "r" (o));
        }
        diff = now() - cycle;
        if (p > 0 && diff < min)
            min = diff;
    }
    avg = min;
    t = (double)(avg - base) / rate / (ITS * l);
    printf("Size:%s  Latency:%4.1fns  Cycles:%ld\n", nice_size(1 << size), t, (uint64_t)(t * 2 + 0.5));
}

// Do the sequential latency
void do_seq_latency_test(unsigned int size)
{
    uint64_t i, l, o, p;
    double t;
    uint64_t cycle, base, diff, avg, min;
    volatile cacheline_uint64_t *c;

    if (size) {
        l = (1 << size) / CACHELINE_SIZE; // number of cache lines
    } else { // continuous test
        l = (1 << 26) / CACHELINE_SIZE; // number of cache lines
    }
    c = (cacheline_uint64_t *)area;

// find base latency
    min = UINT64_MAX;
    for (p = 0; p < 5; p++) {
        cycle = now();
        o = 0;
        for (i = 0; i < l; i += 8) {
            asm("nop");
            asm("nop");
            asm("nop");
            asm("nop");
            asm("nop");
            asm("nop");
            asm("nop");
            asm("nop");
        }
        diff = now() - cycle;
        if (p > 0 && diff < min)
            min = diff;
    }
    base = min;

    do {
        min = UINT64_MAX;
        for (p = 0; p < 5; p++) {
            cycle = now();
            o = 0;
            for (i = 0; i < l; i += 8) {
                o |= c[i][0][0];
                o |= c[i + 1][0][0];
                o |= c[i + 2][0][0];
                o |= c[i + 3][0][0];
                o |= c[i + 4][0][0];
                o |= c[i + 5][0][0];
                o |= c[i + 6][0][0];
                o |= c[i + 7][0][0];
                asm("":: "r" (o));
            }
            diff = now() - cycle;
            if (p > 0 && diff < min)
                min = diff;
        }
        avg = min;
        t = (double)(avg - base) / rate / l;
        printf("Size:%s  Latency:%4.1fns  Cycles:%ld\n", nice_size(1 << size), t, (uint64_t)(t * 2 + 0.5));
    } while (size == 0);
}

// core-to-core test
static volatile uint64_t *c2c_data;

void * thread_c2c_1(void *f)
{
    int64_t i;
    uint64_t cycle;

    c2c_data[0] = 1;
    c2c_data[16] = 1;
    pthread_barrier_wait(&barrier);
    cycle = now();
    for (i = 1000; i >= 0; i--) {
        c2c_data[0] = i;
        __sync_synchronize();
        while (i != c2c_data[16])
            ;
    }
    cycle = now() - cycle;
    return (void *)cycle;
}

void * thread_c2c_2(void *f)
{
    pthread_barrier_wait(&barrier);
    for (;;) {
        uint64_t v;

        v = c2c_data[0];
        c2c_data[16] = v;
        __sync_synchronize();
        if (v == 0)
            break;
    }
    return NULL;
}

// Do the core-2-core test
// Launch 2 threads:
//  1. Write a value to the first cacheline and wait till it appears in the second one
//  2. Read a value from the first cacheline and write it to the second one
// Repeat 1000 times
// if second_cpu == -1 then do the test with the FPGA
uint64_t do_c2c_test(int first_cpu, int second_cpu)
{
    pthread_t tids[2];
    pthread_attr_t attr;
    cpu_set_t cpus;
    uint64_t retval;

    if (second_cpu == - 1) // use the FPGA
        c2c_data = area + 0x8000000080UL; // addresses of FPGA cache lines
    else // or another CPU thread
        c2c_data = area;
    pthread_barrier_init(&barrier, NULL, second_cpu != -1 ? 3: 2);
    {
        pthread_attr_init(&attr);
        CPU_ZERO(&cpus);
        CPU_SET(first_cpu, &cpus);
        pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
        pthread_create(tids, &attr, thread_c2c_1, NULL);
        pthread_attr_destroy(&attr);
    }
    if (second_cpu != -1) {
        pthread_attr_init(&attr);
        CPU_ZERO(&cpus);
        CPU_SET(second_cpu, &cpus);
        pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
        pthread_create(tids + 1, &attr, thread_c2c_2, NULL);
        pthread_attr_destroy(&attr);
    }
    pthread_barrier_wait(&barrier);
    pthread_join(tids[0], (void *)&retval);
    if (second_cpu != -1) {
        pthread_join(tids[1], NULL);
    }
    pthread_barrier_destroy(&barrier);

    return retval;
}


int main(int argc, char *argv[])
{
    uint64_t i, o;
    uint64_t ts[4];
    int opt;
    struct timespec tspec[2];

// gather info
    no_cpus = get_nprocs();

    first_cpu = 0;
    last_cpu = 0;
    do_overall = 0;
    do_cache_to_cache = 0;
    do_latency = 0;
    do_throughput = 0;
    use_cpu_memory = 0;
    do_stress = 0;
    while ((opt = getopt(argc, argv, "hbf:l:stmcpr:")) != -1) {
        switch(opt) {
        case 'h': // print help
            puts("Usage: mb_enzian [-h] [-f first_core_no] [-l last_core_no] [-s] [-t] [-m] [-c] [-p] [-r stress_type]");
            puts("-h");
            puts("      Print this help");
            puts("-b");
            puts("      Overall system benchmark");
            puts("-f first_core_no");
            puts("      Number of the first core used, 0 (1st core) by default");
            puts("-l last_core_no");
            puts("      Number of the first core used, 0 (1st core) by default");
            puts("-s");
            puts("      Perform a sequential latency test (only reads)");
            puts("-t");
            puts("      Perform a chaising-pointer latency test (writes and reads)");
            puts("-m");
            puts("      Perform a memory throughput test");
            puts("-c");
            puts("      Perforem a core-to-core latency test");
            puts("-p");
            puts("      Use the CPU memory instead of the FPGA memory, 1GB huge pages are used.");
            puts("      Allocate them first by executing it as root: 'echo 3 > /sys/devices/system/node/node0/hugepages/hugepages-1048576kB/nr_hugepages'");
            puts("-r stress_type");
            puts("      Stress testing, continuous access using 32MB blocks. The modes are:");
            puts("      w - writing");
            puts("      c - clearing");
            puts("      r - reading");
            puts("      l - sequential latency");
            break;
        case 'b':
            do_overall = 1;
            break;
        case 'f': // first cpu
            first_cpu = atoi(optarg);
            break;
        case 'l': // last cpu
            last_cpu = atoi(optarg);
            break;
        case 's': // do the sequential memory latency test
            do_seq_latency = 1;
            break;
        case 't': // do the memory latency test
            do_latency = 1;
            break;
        case 'm': // do the memory throughput test
            do_throughput = 1;
            break;
        case 'c': // do the core-2-core latency test
            do_cache_to_cache = 1;
            break;
        case 'p': // use the CPU memory instead of the FPGA
            use_cpu_memory = 1;
            break;
        case 'r': // stress test
            if (optarg[0] == 'w')
                do_stress = 1;
            else if (optarg[0] == 'c')
                do_stress = 2;
            else if (optarg[0] == 'r')
                do_stress = 3;
            else if (optarg[0] == 'l')
                do_stress = 4;
            else
                puts("Unsupported stress mode!");
            break;
        default:
            assert(0);
        }
    }
// L2 cache size per thread
    l2_cache_size = 16777216 / (last_cpu - first_cpu + 1);
// calibrate TSC
    ts[0] = now();
    clock_gettime(CLOCK_MONOTONIC_RAW, tspec);
    usleep(100000);
    ts[1] = now();
    clock_gettime(CLOCK_MONOTONIC_RAW, tspec + 1);
    i = ts[1] - ts[0];
    o = (tspec[1].tv_sec - tspec[0].tv_sec) * 1000000000 + tspec[1].tv_nsec - tspec[0].tv_nsec;
    rate = (double)i / o;
//    printf("tsc diff:%zd  clock diff:%zd  rate:%g  no cpus:%zd\n", i, o, rate, no_cpus);

    if (use_cpu_memory == 0) { // use FPGA mem
        int fd;
        void *virt_addr;

        fd = open("/dev/fpgamem", O_RDWR);
        assert(fd >= 0);
        virt_addr = (void *)0x100000000000UL; // 1TB aligned, same as the physical FPGA memory address for convenience
        area = mmap(virt_addr, 0x10000000000UL, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0); // map 1TB
    } else { // use CPU mem, allocate 3GB, 64MB for 48 threads, use HugeTLB 1GB pages
        area = mmap(NULL, SIZE * 48, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_1GB, -1, 0); // CPU mem
        if (area == MAP_FAILED) {
            fprintf(stderr, "mmap failed. Maybe there's no enough free 1GB huge pages.\n");
            exit(1);
        }
    }
    assert(area != MAP_FAILED);

    if (do_throughput || do_stress) {
        printf("Using %d thread(s), from CPU %d to CPU %d...\n", last_cpu - first_cpu + 1, first_cpu, last_cpu);
    }

// Touch and do the actual mapping of the test area
    ((uint8_t *)area)[0] = 0;           // 1st GB
    ((uint8_t *)area)[0x40000000] = 0;  // 2nd GB
    ((uint8_t *)area)[0x80000000] = 0;  // 3rd GB

    if (do_overall) { // use 2 threads
        uint64_t s;
        uint64_t cycle;
        cpu_set_t cpus;

        first_cpu = 0;
        last_cpu = 1;
        area_test_size = 1 << 25; // 32MB
        itn = 1;

        printf("Throughput:\t");
        fflush(stdout);

        s = start_threads(first_cpu, last_cpu, thread_write_cache_area);
        printf("write %s %.3fGB/s\t", nice_time(s), (double)(area_test_size * (last_cpu - first_cpu + 1) * itn) * rate * 1000000000.0 / s / 1048576.0 / 1024.0);
        fflush(stdout);

        s = start_threads(first_cpu, last_cpu, thread_clear_cache_area);
        printf("clear %s %.3fGB/s\t", nice_time(s), (double)(area_test_size * (last_cpu - first_cpu + 1) * itn) * rate * 1000000000.0 / s / 1048576.0 / 1024.0);
        fflush(stdout);

        s = start_threads(first_cpu, last_cpu, thread_read_cache_area);
        printf("read %s %.3fGB/s\n", nice_time(s), (double)(area_test_size * (last_cpu - first_cpu + 1) * itn) * rate * 1000000000.0 / s / 1048576.0 / 1024.0);

        cycle = do_c2c_test(first_cpu, use_cpu_memory ? last_cpu : -1);
        printf("Core-2-core (one trip, 3 hops) latency is %ldns\n", cycle / 200);

        CPU_ZERO(&cpus);
        CPU_SET(first_cpu, &cpus);
        assert(sched_setaffinity(0, sizeof(cpus), &cpus) == 0); // bind to the first_cpu core
        printf("Memory latency: ");
        do_seq_latency_test(26); // 64MB
    }

    if (do_throughput) {
        uint64_t s;

        for (i = 14; i <= 25; i++) { // from 16kiB to 32MiB
            area_test_size = 1 << i;
            itn = i > 22 ? 1: 1 << (22 - i);
            printf("Size: %s\t", nice_size(area_test_size));
            fflush(stdout);

            s = start_threads(first_cpu, last_cpu, thread_write_cache_area);
            printf("write %s %.3fGB/s\t", nice_time(s), (double)(area_test_size * (last_cpu - first_cpu + 1) * itn) * rate * 1000000000.0 / s / 1048576.0 / 1024.0);
            fflush(stdout);

            s = start_threads(first_cpu, last_cpu, thread_clear_cache_area);
            printf("clear %s %.3fGB/s\t", nice_time(s), (double)(area_test_size * (last_cpu - first_cpu + 1) * itn) * rate * 1000000000.0 / s / 1048576.0 / 1024.0);
            fflush(stdout);

            s = start_threads(first_cpu, last_cpu, thread_read_cache_area);
            printf("read %s %.3fGB/s\t", nice_time(s), (double)(area_test_size * (last_cpu - first_cpu + 1) * itn) * rate * 1000000000.0 / s / 1048576.0 / 1024.0);

            printf("\n");
        }
    }
    if (do_stress) {
        uint64_t s;
        cpu_set_t cpus;

        puts("Stressing...");
        area_test_size = 1 << 25; // 32MiB
        itn = 100;
        for (;;) {
            switch (do_stress) {
                case 1:
                    s = start_threads(first_cpu, last_cpu, thread_write_cache_area);
                    printf("write %s %.3fGB/s\n", nice_time(s), (double)(area_test_size * (last_cpu - first_cpu + 1) * itn) * rate * 1000000000.0 / s / 1048576.0 / 1024.0);
                    break;
                case 2:
                    s = start_threads(first_cpu, last_cpu, thread_clear_cache_area);
                    printf("clear %s %.3fGB/s\n", nice_time(s), (double)(area_test_size * (last_cpu - first_cpu + 1) * itn) * rate * 1000000000.0 / s / 1048576.0 / 1024.0);
                    break;
                case 3:
                    s = start_threads(first_cpu, last_cpu, thread_read_cache_area);
                    printf("read %s %.3fGB/s\n", nice_time(s), (double)(area_test_size * (last_cpu - first_cpu + 1) * itn) * rate * 1000000000.0 / s / 1048576.0 / 1024.0);
                    break;
                default: // 4
                    CPU_ZERO(&cpus);
                    CPU_SET(first_cpu, &cpus);
                    assert(sched_setaffinity(0, sizeof(cpus), &cpus) == 0); // bind to the first_cpu core
                    do_seq_latency_test(0);
                    break;
            }
        }
    }
    if (do_cache_to_cache) {
        uint64_t cycle;

        printf("Measuring the latency between core %d and ", first_cpu);
        if (use_cpu_memory)
            printf("core %d...\n", last_cpu);
        else
            printf("the FPGA...\n");
        cycle = do_c2c_test(first_cpu, use_cpu_memory ? last_cpu : -1);
        printf("Core-2-core (one trip, 3 hops) latency: %ldns\n", cycle / 200);
    }

    if (do_latency) {
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET(first_cpu, &cpus);
        assert(sched_setaffinity(0, sizeof(cpus), &cpus) == 0); // bind to the first_cpu core
        for (i = 14; i <= 26; i++)
            do_latency_test(i);
    }

    if (do_seq_latency) {
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET(first_cpu, &cpus);
        assert(sched_setaffinity(0, sizeof(cpus), &cpus) == 0); // bind to the first_cpu core
        for (i = 14; i <= 26; i++)
            do_seq_latency_test(i);
    }

    munmap(area, SIZE * no_cpus);
//    printf("Bye!\n");
    return 0;
}
