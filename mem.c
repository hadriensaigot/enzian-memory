/*---------------------------------------------------------------------------*/
// Copyright (c) 2022 ETH Zurich.
// All rights reserved.
//
// This file is distributed under the terms in the attached LICENSE file.
// If you do not find this file, copies can be found by writing to:
// ETH Zurich D-INFK, Stampfenbachstrasse 114, CH-8092 Zurich. Attn: Systems Group
/*---------------------------------------------------------------------------*/

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdio.h>
#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>
#include <linux/mman.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

// Access the FPGA memory, read a value from the specified address or store a value
// Arguments: ./mem address [value]
// All values in hex (with or without "0x")

uint64_t read_tsc(void)
{
    uint64_t tsc;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r" (tsc));
    return tsc;
}

int main(int argc, char *argv[])
{
    int fd = 0, r;
    void *address;
    size_t size = 1024ULL * 1024 * 1024 * 1024; // 1TB, whole FPGA memory region
    uint64_t phys_addr, offset, val;
    volatile uint64_t *ptr;
    uint64_t ts1, ts2;

    fd = open("/dev/fpgamem", O_RDWR);
    assert(fd >= 0);

    offset = 0;
    if (argc > 1) {
        offset = strtoll(argv[1], NULL, 16);
    }
    phys_addr = 0x10000000000ULL; // Physical address of the FPGA memory
    printf("Mapping address: %016lx:%016lx\n", phys_addr, size);
    // Map the entire FPGA memory space, using 1GB pages
    address = mmap((void *)phys_addr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    printf("result: %s\n", strerror(errno));
    assert(address != MAP_FAILED);
    printf("Mapped address: %p\n", address);
    ptr = address;
    if (argc == 2) { // Read a value
        ts1 = read_tsc();
        val = ptr[offset / 8];
        ts2 = read_tsc();
        printf("Read %016lx in %lu ns\n", val, (ts2 - ts1) * 10);
    } else if (argc == 3) { // Store a value
        val = strtoull(argv[2], NULL, 16);
        ts1 = read_tsc();
        ptr[offset / 8] = val;
        __sync_synchronize();
        ioctl(fd, 5, phys_addr + offset); // write-back and invalidate the L2$
        ts2 = read_tsc();
        printf("Written %016lx in %lu ns\n", val, (ts2 - ts1) * 10);
    }
    r = munmap(address, size);
    assert(!r);
    close(fd);
    return 0;
}
