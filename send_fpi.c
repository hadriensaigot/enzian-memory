/*---------------------------------------------------------------------------*/
// Copyright (c) 2022 ETH Zurich.
// All rights reserved.
//
// This file is distributed under the terms in the attached LICENSE file.
// If you do not find this file, copies can be found by writing to:
// ETH Zurich D-INFK, Stampfenbachstrasse 114, CH-8092 Zurich. Attn: Systems Group
/*---------------------------------------------------------------------------*/
// Use ioctl to send SGI 1

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

int main(int argc, char *argv[])
{
    int fd;
    uint64_t affinity;

    fd = open("/dev/fpi", O_RDWR);
    assert(fd >= 0);

    affinity = 1; // by default send to the first core
    if (argc > 1) {
        affinity = strtoll(argv[1], NULL, 16);
    }
    ioctl(fd, 0, 0x0000000101000000ULL | affinity); // send INTID 1, affinity 2 set to 1 (FPGA), value written to ICC SGI1R_EL1
    close(fd);
    return 0;
}
