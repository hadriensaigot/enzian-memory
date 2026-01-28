/*---------------------------------------------------------------------------*/
// Copyright (c) 2022 ETH Zurich.
// All rights reserved.
//
// This file is distributed under the terms in the attached LICENSE file.
// If you do not find this file, copies can be found by writing to:
// ETH Zurich D-INFK, Stampfenbachstrasse 114, CH-8092 Zurich. Attn: Systems Group
/*---------------------------------------------------------------------------*/
// Wait for an interrupt

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
    int fd, r;

    fd = open("/dev/fpi", O_RDWR);
    assert(fd >= 0);
    r = read(fd, NULL, 0);
    printf("FPI received %d\n", r);
    close(fd);
    return 0;
}
