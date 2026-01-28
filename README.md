# Enzian FPGA Memory Linux Driver

This kernel module enables mapping of the FPGA memory into user space with proper page attributes.
It supports only 1GB pages. Linux does not support it fully, therefore it outputs a warning during unmapping.
It also provides protected L2$ instructions to the user space as ioctls.

To build the module:
$ make

To insert the module:
$ sudo insmod enzian_memory.ko

It will create a char device, /dev/fpgamem, which supports mmapping (FPGA memory space access) and ioctls (L2$ control).

The mem.c is an example of how to use the device.

# Enzian FPGA-Processor Interrupt Example (FPI aka SGI/IPI)

The repo provides 3 examples:
 - the kernel module to receive (INTID 8) and send interrupts (enzian_fpi.ko)
 - a user space program to wait for an interrupt (receive_fpi)
 - a user space program to send an interrupt (send_fpi), INTID 1 to the core 0.1.0.0 (affinity)
