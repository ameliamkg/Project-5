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

/* Device-related definitions */
static dev_t            dev = 0;
static struct class*    kmod_class;
static struct cdev      kmod_cdev;

/* Buffers for different operation requests */
struct block_rw_ops rw_request;
struct block_rwoffset_ops rwoffset_request;

long rw_usb(char* data, unsigned int size, unsigned int  offset, bool flag);


bool kmod_ioctl_init(void);
void kmod_ioctl_teardown(void);

static long kmod_ioctl(struct file *f, unsigned int cmd, unsigned long arg) {
    char* kernbuf;
    long result;

    switch (cmd)
    {
        case BREAD:
        case BWRITE:
            /* Get request from user */
            if (copy_from_user((void*) &rw_request, (void*) arg, sizeof(struct block_rw_ops))) {
                printk("Error: Incorrect request parameters.\n");
                return -1;
            }

            /* Debugging */
            printk("REQUEST: RW (%p, %d)\n", rw_request.data, rw_request.size);

            /* Allocate a kernel buffer to read/write user data */
            kernbuf = (char*) vmalloc(rw_request.size);
            // kernbuf = (char*) kmalloc(rw_request.size);
            
            if (IS_ERR(kernbuf)) {
                printk("error: could not allocate memory for the write operation.\n");
                return -1;
            }

            /* Perform the block operation */

            if (cmd == BWRITE) { 
                /* WRITE */
                // Use copy_from_user() to copy rw_request.size bytes from rw_request.data into kernbuf
                if (copy_from_user(kernbuf, rw_request.data, rw_request.size)) {
                    printk("error: failed to copy data from user space.\n");
                    vfree(kernbuf);
                    return -1;
                }
                // Call rw_usb()
                result = rw_usb(kernbuf, rw_request.size, -1, true);
                if (result < 0) {
                    printk("error: write operation failed.\n");
                    vfree(kernbuf);
                    return -1;
                }
            } else {
                /* READ */
                // Call rw_usb()
                result = rw_usb(kernbuf, rw_request.size, -1, false);
                if (result < 0) {
                    printk("error: read operation failed.\n");
                    vfree(kernbuf);
                    return -1;
                }
                // Use copy_to_user() to transfer rw_request.size bytes    
                if (copy_to_user(rw_request.data, kernbuf, rw_request.size)) {
                    printk("error: failed to copy data to user space.\n");
                    vfree(kernbuf);
                    return -1;
                }
            }

            vfree(kernbuf);
            return result;

        case BREADOFFSET:
        case BWRITEOFFSET:
            /* Get request from user */
            if (copy_from_user((void*) &rwoffset_request, (void*) arg, sizeof(struct block_rwoffset_ops))) {
                printk("Error: Incorrect request parameters.\n");
                return -1;
            }

            /* Debugging */
            printk("REQUEST: RWOFFSET (%p, %d, %d)\n", 
                rwoffset_request.data, 
                rwoffset_request.size,
                rwoffset_request.offset);

            // Refer to the case BWRITE to complete this block of code
            /* Allocate a kernel buffer to read/write user data */
            kernbuf = (char*) vmalloc(rwoffset_request.size);
            
            if (IS_ERR(kernbuf)) {
                printk("error: could not allocate memory for the operation.\n");
                return -1;
            }

            /* Perform the block operation */
            if (cmd == BWRITEOFFSET) {  
                /* WRITEOFFSET */
                if (copy_from_user(kernbuf, rwoffset_request.data, rwoffset_request.size)) {
                    printk("error: failed to copy data from user space.\n");
                    vfree(kernbuf);
                    return -1;
                }
                result = rw_usb(kernbuf, rwoffset_request.size, rwoffset_request.offset, true);
                if (result < 0) {
                    printk("error: write offset operation failed.\n");
                    vfree(kernbuf);
                    return -1;
                }
            } else {
                /* READOFFSET */
                result = rw_usb(kernbuf, rwoffset_request.size, rwoffset_request.offset, false);
                if (result < 0) {
                    printk("error: read offset operation failed.\n");
                    vfree(kernbuf);
                    return -1;
                }
                if (copy_to_user(rwoffset_request.data, kernbuf, rwoffset_request.size)) {
                    printk("error: failed to copy data to user space.\n");
                    vfree(kernbuf);
                    return -1;
                }
            }
            
            vfree(kernbuf);
            return result;
        default: 
            printk("Error: incorrect operation requested, returning.\n");
            return -1;
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
        printk("error: couldn't allocate \'usbaccess\' character device.\n");
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
        printk("error: couldn't create kmod_class.\n");
        goto cdevfailed;
    }
#else
    if ((kmod_class = class_create("kmod_class")) == NULL) {
        printk("error: couldn't create kmod_class.\n");
        goto cdevfailed;
    }
#endif

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