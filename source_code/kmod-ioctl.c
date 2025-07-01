#include <linux/blkdev.h>
#include <linux/bio.h>
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
#include <linux/mm.h>
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

static dev_t            dev = 0;
static struct class*    kmod_class;
static struct cdev      kmod_cdev;

struct block_rw_ops        rw_request;
struct block_rwoffset_ops  rwoffset_request;

extern struct block_device *bdevice;

static int do_bio_rw(void *buffer, unsigned int size,
                     sector_t sector_start, bool write)
{
    unsigned int offset = 0;
    int ret, total = 0;

    while (offset < size) {
        unsigned int chunk = min(size - offset, 512u);
        sector_t sector = sector_start + offset / 512;
        struct page *page = vmalloc_to_page(buffer + offset);
        unsigned int page_offset = offset & (PAGE_SIZE - 1);

        struct bio *bio = bio_alloc(bdevice, GFP_KERNEL, 1);
        if (!bio)
            return -ENOMEM;

        bio->bi_iter.bi_sector = sector;
        bio->bi_bdev = bdevice;
        bio->bi_opf = write ? REQ_OP_WRITE : REQ_OP_READ;

        if (bio_add_page(bio, page, chunk, page_offset) != chunk) {
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

static long kmod_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    char *kernbuf;
    int ret;

    switch (cmd) {
    case BREAD:
    case BWRITE: {
        sector_t sector = f->f_pos >> 9;
        if (copy_from_user(&rw_request, (void __user *)arg, sizeof(rw_request)))
            return -EFAULT;
        kernbuf = vmalloc(rw_request.size);
        if (!kernbuf)
            return -ENOMEM;
        if (cmd == BWRITE) {
            if (copy_from_user(kernbuf, rw_request.data, rw_request.size)) {
                vfree(kernbuf);
                return -EFAULT;
            }
        }
        ret = do_bio_rw(kernbuf, rw_request.size, sector, cmd == BWRITE);
        if (ret >= 0 && cmd == BREAD) {
            if (copy_to_user(rw_request.data, kernbuf, ret))
                ret = -EFAULT;
        }
        if (ret >= 0)
            f->f_pos += ret;
        vfree(kernbuf);
        return ret;
    }

    case BREADOFFSET:
    case BWRITEOFFSET: {
        if (copy_from_user(&rwoffset_request, (void __user *)arg,
                           sizeof(rwoffset_request)))
            return -EFAULT;
        kernbuf = vmalloc(rwoffset_request.size);
        if (!kernbuf)
            return -ENOMEM;
        if (cmd == BWRITEOFFSET) {
            if (copy_from_user(kernbuf, rwoffset_request.data,
                               rwoffset_request.size)) {
                vfree(kernbuf);
                return -EFAULT;
            }
        }
        ret = do_bio_rw(kernbuf, rwoffset_request.size,
                        (sector_t)(rwoffset_request.offset >> 9),
                        cmd == BWRITEOFFSET);
        if (ret >= 0 && cmd == BREADOFFSET) {
            if (copy_to_user(rwoffset_request.data, kernbuf, ret))
                ret = -EFAULT;
        }
        if (ret >= 0)
            f->f_pos = rwoffset_request.offset + ret;
        vfree(kernbuf);
        return ret;
    }

    default:
        return -EINVAL;
    }
}

static int kmod_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int kmod_release(struct inode *inode, struct file *file)
{
    return 0;
}

static const struct file_operations fops = {
    .owner          = THIS_MODULE,
    .open           = kmod_open,
    .release        = kmod_release,
    .unlocked_ioctl = kmod_ioctl,
};

bool kmod_ioctl_init(void)
{
    if (alloc_chrdev_region(&dev, 0, 1, "usbaccess"))
        return false;
    cdev_init(&kmod_cdev, &fops);
    if (cdev_add(&kmod_cdev, dev, 1)) {
        unregister_chrdev_region(dev, 1);
        return false;
    }
#if LINUX_VERSION_CODE <= KERNEL_VERSION(6,2,16)
    kmod_class = class_create(THIS_MODULE, "kmod_class");
#else
    kmod_class = class_create("kmod_class");
#endif
    if (IS_ERR(kmod_class)) {
        cdev_del(&kmod_cdev);
        unregister_chrdev_region(dev, 1);
        return false;
    }
    if (!device_create(kmod_class, NULL, dev, NULL, "kmod")) {
        class_destroy(kmod_class);
        cdev_del(&kmod_cdev);
        unregister_chrdev_region(dev, 1);
        return false;
    }
    return true;
}

void kmod_ioctl_teardown(void)
{
    device_destroy(kmod_class, dev);
    class_destroy(kmod_class);
    cdev_del(&kmod_cdev);
    unregister_chrdev_region(dev, 1);
}
