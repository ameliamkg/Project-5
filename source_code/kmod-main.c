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
#include <linux/version.h>
#include <linux/blkpg.h>
#include <linux/namei.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Adil Ahmad");
MODULE_DESCRIPTION("A Block Abstraction Read/Write for a USB device.");
MODULE_VERSION("1.0");

/* USB device name argument */
char *device = "/dev/sdb";
module_param(device, charp, S_IRUGO);

/* USB storage disk-related data structure */
static struct block_device *bdevice = NULL;

bool kmod_ioctl_init(void);
void kmod_ioctl_teardown(void);

static bool open_usb(void)
{
    int err;

    /* Open the block device at the given path */
    err = bdev_open_by_path(device, FMODE_READ | FMODE_WRITE, &bdevice);
    if (err) {
        pr_err("bdev_open_by_path(%s) failed: %d\n", device, err);
        return false;
    }
    pr_info("Opened block device %s successfully\n", device);
    return true;
}

static void close_usb(void)
{
    if (bdevice) {
        /* Close the block device */
        bdev_close(bdevice, FMODE_READ | FMODE_WRITE);
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
