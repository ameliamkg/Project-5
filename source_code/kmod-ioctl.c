#include <linux/blkdev.h>
#include <linux/completion.h>
#include <linux/dcache.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/kthread.h>
#include <linux/limits.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/freezer.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include <linux/ioctl.h>

#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/composite.h>

#include <linux/cdev.h>
#include <linux/nospec.h>

#include "../ioctl-defines.h"

#include <linux/vmalloc.h>
#include <linux/bio.h>
#include <linux/mm.h>    // For PAGE_SIZE

/* Device-related definitions */
static dev_t            dev = 0;
static struct class*    kmod_class;
static struct cdev      kmod_cdev;

/* Buffers for different operation requests */
struct block_rw_ops rw_request;
struct block_rwoffset_ops rwoffset_request;

bool kmod_ioctl_init(void);
void kmod_ioctl_teardown(void);

/**
 * Perform block I/O in 512-byte chunks and return total bytes read/written.
 */
static int do_bio_rw(void *buffer, unsigned int size,
                     sector_t sector_start, bool write)
{
    unsigned int offset = 0;
    int ret;
    int total = 0;

    while (offset < size) {
        unsigned int chunk = min(size - offset, (unsigned int)512);
        sector_t sector = sector_start + offset / 512;
        struct page *page = vmalloc_to_page(buffer + offset);
        unsigned int page_off = offset & (PAGE_SIZE - 1);
        struct bio *bio = bio_alloc(GFP_KERNEL, 1);
        if (!bio)
            return -ENOMEM;

        bio->bi_iter.bi_sector = sector;
        bio->bi_bdev = bdevice;
        bio->bi_opf = write ? REQ_OP_WRITE : REQ_OP_READ;

        if (bio_add_page(bio, page, chunk, page_off) != chunk) {
            bio_put(bio);
            return -EFAULT;
        }

        ret = submit_bio_wait(bio);
        bio_put(bio);
        if (ret < 0)
            return ret;

        total += ret;
        offset += chunk;
    }

    return total;
}

static long kmod_ioctl(struct file *f, unsigned int cmd, unsigned long arg) {
    char *kernbuf;
    int ret;

    switch (cmd)
    {
        case BREAD:
        case BWRITE:
        {
            sector_t sector = f->f_pos >> 9;
            /* Get request from user */
            if (copy_from_user(&rw_request, (void __user *)arg, sizeof(rw_request)))
                return -EFAULT;
            /* Allocate a kernel buffer to read/write user data */        
            kernbuf = vmalloc(rw_request.size);
            if (!kernbuf)
                return -ENOMEM;
            if (cmd == BWRITE) {
                /* If writing, copy data from user */
                if (copy_from_user(kernbuf, rw_request.data, rw_request.size)) {
                    vfree(kernbuf);
                    return -EFAULT;
                }
            }
            /* Perform the block operation */
            ret = do_bio_rw(kernbuf, rw_request.size, sector, (cmd == BWRITE));
            if (ret >= 0 && cmd == BREAD) {
                /* If reading, copy data back to user */
                if (copy_to_user(rw_request.data, kernbuf, ret))
                    ret = -EFAULT;
            }
            if (ret >= 0) {
                f->f_pos += ret;
            }
            vfree(kernbuf);
            return ret;
        }

        case BREADOFFSET:
        case BWRITEOFFSET:
        {
            /* Get request from user */
            if (copy_from_user(&rwoffset_request, (void __user *)arg, sizeof(rwoffset_request)))
                return -EFAULT;
            /* Allocate a kernel buffer to read/write user data */
            kernbuf = vmalloc(rwoffset_request.size);
            if (!kernbuf)
                return -ENOMEM;
            if (cmd == BWRITEOFFSET) {
                /* If writing, copy data from user */
                if (copy_from_user(kernbuf, rwoffset_request.data, rwoffset_request.size)) {
                    vfree(kernbuf);
                    return -EFAULT;
                }
            }
            /* Perform the block operation */
            ret = do_bio_rw(kernbuf, rwoffset_request.size,
                            (sector_t)(rwoffset_request.offset >> 9), (cmd == BWRITEOFFSET));
            if (ret >= 0 && cmd == BREADOFFSET) {
                /* If reading, copy data back to user */
                if (copy_to_user(rwoffset_request.data, kernbuf, ret))
                    ret = -EFAULT;
            }
            if (ret >= 0) {
                f->f_pos = rwoffset_request.offset + ret;
            }
            vfree(kernbuf);
            return ret;
        }
        default: 
            printk("Error: incorrect operation requested, returning.\n");
            return -EINVAL;
    }

    return 0;
}

static int kmod_open(struct inode* inode, struct file* file) {
    printk("Opened kmod. \n");
    return 0;
}

static int kmod_release(struct inode* inode, struct file* file) {
    printk("Closed kmod. \n");
    return 0;
}

static struct file_operations fops = 
{
    .owner          = THIS_MODULE,
    .open           = kmod_open,
    .release        = kmod_release,
    .unlocked_ioctl = kmod_ioctl,
};

/* Initialize the module for IOCTL commands */
bool kmod_ioctl_init(void) {

    /* Allocate a character device. */
    if (alloc_chrdev_region(&dev, 0, 1, "usbaccess") < 0) {
        printk("error: couldn't allocate 'usbaccess' character device.\n");
        return false;
    }

    /* Initialize the chardev with my fops. */
    cdev_init(&kmod_cdev, &fops);
    if (cdev_add(&kmod_cdev, dev, 1) < 0) {
        printk("error: couldn't add kmod_cdev.\n");
        goto cdevfailed;
    }

#if LINUX_VERSION_CODE <= KERNEL_VERSION(6,2,16)
    if ((kmod_class = class_create(THIS_MODULE, "kmod_class")) == NULL) {
#else
    if ((kmod_class = class_create("kmod_class")) == NULL) {
#endif
        printk("error: couldn't create kmod_class.\n");
        goto cdevfailed;
    }

    if ((device_create(kmod_class, NULL, dev, NULL, "kmod")) == NULL) {
        printk("error: couldn't create device.\n");
        goto classfailed;
    }

    printk("[*] IOCTL device initialization complete.\n");
    return true;

classfailed:
    class_destroy(kmod_class);
cdevfailed:
    unregister_chrdev_region(dev, 1);
    return false;
}

void kmod_ioctl_teardown(void) {
    /* Destroy the classes too (IOCTL-specific). */
    if (kmod_class) {
        device_destroy(kmod_class, dev);
        class_destroy(kmod_class);
    }
    cdev_del(&kmod_cdev);
    unregister_chrdev_region(dev,1);

    printk("[*] IOCTL device teardown complete.\n");
}
