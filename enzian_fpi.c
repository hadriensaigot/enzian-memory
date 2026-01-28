/*---------------------------------------------------------------------------*/
// Copyright (c) 2025 ETH Zurich.
// All rights reserved.
//
// This file is distributed under the terms in the attached LICENSE file.
// If you do not find this file, copies can be found by writing to:
// ETH Zurich D-INFK, Stampfenbachstrasse 114, CH-8092 Zurich. Attn: Systems Group
/*---------------------------------------------------------------------------*/
//
// This is an example of how to acquire SGI INTID 8 and active it on all cores
// An interrupt wakes up a waiting read descriptor

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/version.h>
#include <asm/io.h>
#include <linux/smp.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/irqdomain.h>

#include <asm/arch_gicv3.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Adam S. Turowski");
MODULE_DESCRIPTION("Enzian FPGA-Processor Interrupt driver.");
MODULE_VERSION("1");

struct mychar_device_data {
    struct cdev cdev;
};

static int dev_major = 0;
static struct class *mychardev_class = NULL;
static struct mychar_device_data mychardev_data;
DEFINE_MUTEX(enzian_fpi_mmap_mutex);

static DEFINE_PER_CPU_READ_MOSTLY(int, fpi_cpu_number);
static unsigned fpi_irq_no;

wait_queue_head_t wq;
int wq_flag = 0;

int enzian_fpi_open(struct inode *inode, struct file *file);
long int enzian_fpi_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
int enzian_fpi_release(struct inode *inode, struct file *file);
ssize_t enzian_fpi_read(struct file *file, char __user *user_buffer, size_t size, loff_t *offset);

int enzian_fpi_open(struct inode *inode, struct file *file)
{
    return 0;
}

long int enzian_fpi_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case 0: // send interrupt to the FPGA
        printk("%s.%d[%2d]: %016lx\n", __func__, __LINE__, smp_processor_id(), arg);
        gic_write_sgi1r(arg);
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

int enzian_fpi_release(struct inode *inode, struct file *file)
{
    return 0;
}

ssize_t enzian_fpi_read(struct file *file, char __user *user_buffer, size_t size, loff_t *offset)
{
    wait_event_interruptible(wq, wq_flag != 0);
    wq_flag = 0;
    smp_wmb();
    return 0;
}

struct file_operations fops = {
    .open = enzian_fpi_open,
    .release = enzian_fpi_release,
    .unlocked_ioctl = enzian_fpi_ioctl,
    .read = enzian_fpi_read
};

static irqreturn_t fpi_handler(int irq, void *data)
{
    printk("%s.%d[%2d]: FPI %d\n", __func__, __LINE__, smp_processor_id(), irq);
    wq_flag = 1;
    smp_wmb();
    wake_up_interruptible(&wq);
    return IRQ_HANDLED;
}

static int do_fpi_irq_activate(void *unused)
{
    enable_percpu_irq(fpi_irq_no, 0);
    return 0;
}

static int do_fpi_irq_deactivate(void *unused)
{
    disable_percpu_irq(fpi_irq_no);
    return 0;
}

static int __init enzian_fpi_init(void)
{
    int err, cpu_no;
    dev_t dev;
    struct irq_data *gic_irq_data;
    struct irq_domain *gic_domain;
    struct fwnode_handle *fwnode;
    static struct irq_fwspec fwspec_fpi;

    err = alloc_chrdev_region(&dev, 0, 1, "fpi");
    WARN_ON(err < 0);
    dev_major = MAJOR(dev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,4,0)
    mychardev_class = class_create("fpi");
#else
    mychardev_class = class_create(THIS_MODULE, "fpi");
#endif
    cdev_init(&mychardev_data.cdev, &fops);
    mychardev_data.cdev.owner = THIS_MODULE;
    cdev_add(&mychardev_data.cdev, MKDEV(dev_major, 0), 1);
    device_create(mychardev_class, NULL, MKDEV(dev_major, 0), NULL, "fpi");

    init_waitqueue_head(&wq);
    wq_flag = 0;

    gic_irq_data = irq_get_irq_data(1U);
    gic_domain = gic_irq_data->domain;
    // Assuming that fwnode is the first element of structure gic_chip_data
    fwnode = *(struct fwnode_handle **)(gic_domain->host_data);

    fwspec_fpi.fwnode = fwnode;
    fwspec_fpi.param_count = 1;
    fwspec_fpi.param[0] = 8; // first free SGI interrupt
    err = irq_create_fwspec_mapping(&fwspec_fpi);
    WARN_ON(err < 0);
    fpi_irq_no = err;
    printk("Allocated interrupt number = %d\n", fpi_irq_no);
    smp_wmb();
    err = request_percpu_irq(fpi_irq_no, fpi_handler, "Enzian FPI", &fpi_cpu_number);
    WARN_ON(err < 0);
    for_each_online_cpu(cpu_no) { // active the interrupt on all cores
        err = smp_call_on_cpu(cpu_no, do_fpi_irq_activate, NULL, true);
        WARN_ON(err < 0);
    }

    return 0;
}

static void __exit enzian_fpi_exit(void)
{
    int err, cpu_no;

    for_each_online_cpu(cpu_no) { // deactive the interrupt on all cores
        err = smp_call_on_cpu(cpu_no, do_fpi_irq_deactivate, NULL, true);
        WARN_ON(err < 0);
    }
    free_percpu_irq(fpi_irq_no, &fpi_cpu_number);
    irq_dispose_mapping(fpi_irq_no);
    device_destroy(mychardev_class, MKDEV(dev_major, 0));
    class_destroy(mychardev_class);
    unregister_chrdev_region(MKDEV(dev_major, 0), MINORMASK);
}

module_init(enzian_fpi_init);
module_exit(enzian_fpi_exit);
