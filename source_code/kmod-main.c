#include <linux/kthread.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/skbuff.h>
#include <linux/freezer.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>

/* File IO-related headers */
#include <linux/fs.h>
#include <linux/bio.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/err.h>
#include <linux/blkpg.h>
#include <linux/namei.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Adil Ahmad");
MODULE_DESCRIPTION("A Block Abstraction Read/Write for a USB device.");
MODULE_VERSION("1.0");

/* USB device name argument */
static char *device = "/dev/sda";
module_param(device, charp, S_IRUGO);

/* Block device handle and device pointer */
struct block_device *bdevice = NULL;

bool kmod_ioctl_init(void);
void kmod_ioctl_teardown(void);

static bool open_usb(void)
{
    dev_t devt;
    int err;

    /* Lookup the device number */
    err = lookup_bdev(device, &devt);
    if (err) {
        pr_err("lookup_bdev(%s) failed: %d\n", device, err);
        return false;
    }

    /* Get the block_device for I/O */
    bdevice = blkdev_get_by_dev(devt,
                                FMODE_READ | FMODE_WRITE,
                                NULL);
    if (IS_ERR(bdevice)) {
        pr_err("blkdev_get_by_dev(%s) failed: %ld\n",
               device, PTR_ERR(bdevice));
        bdevice = NULL;
        return false;
    }

    pr_info("Opened block device %s successfully\n", device);
    return true;
}

static void close_usb(void)
{
    if (bdevice) {
        blkdev_put(bdevice, NULL);
        pr_info("Closed block device %s\n", device);
        bdevice = NULL;
    }
}

static int __init kmod_init(void)
{
    int ret;

    pr_info("Hello World!\n");
    if (!open_usb()) {
        pr_err("Failed to open USB block device\n");
        return -ENODEV;
    }
    ret = kmod_ioctl_init();
    if (!ret) {
        pr_err("IOCTL init failed\n");
        close_usb();
        return -EIO;
    }
    return 0;
}

static void __exit kmod_fini(void)
{
    close_usb();
    kmod_ioctl_teardown();
    pr_info("Goodbye, World!\n");
}

module_init(kmod_init);
module_exit(kmod_fini);
