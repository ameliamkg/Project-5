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
MODULE_AUTHOR("");
MODULE_DESCRIPTION("A Block Abstraction Read/Write for a USB device.");
MODULE_VERSION("1.0");

/* USB device name argument */
char* device = "/dev/sda";
module_param(device, charp, S_IRUGO);


/* USB device system handler */
unsigned int cur_dev_sector = 0;
static struct block_device *bdevice = NULL;
static struct bio *usb_bio = NULL;


bool kmod_ioctl_init(void);
void kmod_ioctl_teardown(void);
long rw_usb(char* data, unsigned int size, unsigned int  offset, bool flag);

static bool open_usb(void)
{
    /* Lookup the block device for the path */
    bdevice = lookup_bdev(device);
    if (IS_ERR(bdevice)) {
        printk("error: failed to lookup the device (%s).\n", device);
        bdevice = NULL;
        return false;
    }
    
    /* Open the block device */
    bdevice = blkdev_get(bdevice, FMODE_READ | FMODE_WRITE, NULL);
    if (IS_ERR(bdevice)) {
        printk("error: failed to open the device (%s).\n", device);
        bdevice = NULL;
        return false;
    }
    
    /* Perform various sanity checks to make sure the device works */
    if (!bdevice) {
        printk("error: failed to open the device (%s).\n", device);
        return false;
    }
    printk("success: opened %s as a block device.\n", bdevice->bd_disk->disk_name);

    usb_bio = bio_alloc(bdevice, 256, REQ_OP_WRITE, GFP_NOIO);
    if (!usb_bio || IS_ERR(usb_bio)) {
        printk("error: failed to allocate a bio structure.\n");
        blkdev_put(bdevice, FMODE_READ | FMODE_WRITE);
        return false;
    }
    printk("success: allocated usb_bio.\n");
    return true;
}

// API to write to the attached USB device
long rw_usb(
    char*         data,   /*  message buffer */ 
    unsigned int  size,   /*  message length */
    unsigned int  offset, /*  write location */
    bool          flag    /*  1=write, 0=read */
) 
{
    unsigned int remaining = size;
    unsigned int processed = 0;
    int len;

    unsigned int page_offset = 0;
    int bio_add_page_result = 0;

    /* Sanity check */
    if (IS_ERR(data)) {
        printk("error: message not correct.\n");
        return -1;
    }

    /* Break down writes into 512 bytes chunks */
    while (remaining > 0) {
        /* Set the BIO structure */

        // Use bio_set_dev() to assign usb_bio to bdevice
        bio_set_dev(usb_bio, bdevice);

        if (offset == -1) {
            /* Use the previous offset */
            // Set bi_iter.bi_sector to cur_dev_sector
            usb_bio->bi_iter.bi_sector = cur_dev_sector;
        
        } else {
            /* Use the provided offset */
            printk("offset is provided (bytes = %d, sector = %d)\n", offset, offset/512);
            // Set bi_iter.bi_sector
            usb_bio->bi_iter.bi_sector = offset / 512;

            // Update cur_dev_sector value for future tracking
            cur_dev_sector = offset / 512;
            
            // Reset offset so it's not reused in next iteration
            offset = -1;
        }

        /* Set read/write op based on the flag */
        if (flag == true) {
            usb_bio->bi_opf = REQ_OP_WRITE;
            printk("WRITE (size = %d, offset = %lld)\n", 
                size, usb_bio->bi_iter.bi_sector);
        } else {
            usb_bio->bi_opf = REQ_OP_READ;
            printk("READ (size = %d, offset = %lld)\n", 
                size, usb_bio->bi_iter.bi_sector);
        }

        /* Check if the current sector is overflowing the device */

        // Add data to the bio in chunks of 512 bytes
        // Use bio_add_page() to add bytes at the correct offset
        // Use vmalloc_to_page(data) to get the page from the virtual address
        unsigned int chunk_size = min(remaining, 512U);
        bio_add_page_result = bio_add_page(usb_bio, vmalloc_to_page(data), 
                                          chunk_size, page_offset);
        if (bio_add_page_result == 0) {
            printk("error: failed to add page to bio\n");
            return processed;
        }
      
        /* Submit BIO and wait for op completion */
        len = submit_bio_wait(usb_bio);
        if (len < 0) {
            printk("error: bio communication failed (ret = %d)\n", len);
            return processed;
        }
        bio_reset(usb_bio, bdevice, FMODE_WRITE);

        /* Decrement the chunk */
        // Increment cur_dev_sector
        cur_dev_sector++;
        
        // Calculate remaining bytes
        remaining -= chunk_size;
        // Calculate processed bytes
        processed += chunk_size;

        // Update page_offset
        page_offset += chunk_size;
        
        // If page_offset reaches one page, increment 'data'
        if (page_offset >= PAGE_SIZE) {
            data += PAGE_SIZE;
            // Reset page_offset
            page_offset = 0;
        }
    }

    return processed;
}


static void close_usb(void)
{
    /* Close the block device communication interface */

    if (usb_bio) {
        bio_put(usb_bio);
        usb_bio = NULL;
    }
    
    // Check if the bdevice is valid
    if (bdevice && !IS_ERR(bdevice)) {
        // Use bd_release() to release the device reference
        bd_release(bdevice);
        bdevice = NULL;
    }
}

static int __init kmod_init(void)
{
    pr_info("Hello World!\n");
    if (!open_usb()) {
        pr_err("Failed to open USB block device\n");
        return -ENODEV;
    }
    kmod_ioctl_init();
    return 0;
}

static void __exit kmod_fini(void)
{
    close_usb();
    kmod_ioctl_teardown();
    printk("Goodbye, World!\n");
}

module_init(kmod_init);
module_exit(kmod_fini);