#include <linux/kthread.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/freezer.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/buffer_head.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Adil Ahmad");
MODULE_DESCRIPTION("A Block Abstraction Read/Write for a USB device.");
MODULE_VERSION("1.0");

static char *device = "/dev/sda";
module_param(device, charp, S_IRUGO);

static struct file       *usb_filp    = NULL;
struct block_device     *bdevice      = NULL;

bool kmod_ioctl_init(void);
void kmod_ioctl_teardown(void);

static bool open_usb(void)
{
    usb_filp = filp_open(device, O_RDWR|O_LARGEFILE, 0);
    if (IS_ERR(usb_filp))
        return false;

    bdevice = I_BDEV(usb_filp->f_path.dentry->d_inode);
    if (!bdevice) {
        filp_close(usb_filp, NULL);
        usb_filp = NULL;
        return false;
    }

    pr_info("Opened block device %s\n", device);
    return true;
}

static void close_usb(void)
{
    if (usb_filp) {
        filp_close(usb_filp, NULL);
        usb_filp = NULL;
        bdevice = NULL;
    }
}

static int __init kmod_init(void)
{
    int ret;

    pr_info("Hello World!\n");
    if (!open_usb())
        return -ENODEV;

    ret = kmod_ioctl_init();
    if (!ret) {
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
