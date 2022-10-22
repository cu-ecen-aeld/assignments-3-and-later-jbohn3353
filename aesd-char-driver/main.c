/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/syscalls.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("James Bohn");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */

    filp->private_data = &aesd_device;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */

    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t bytes_read = 0;
    struct aesd_dev *dev = (struct aesd_dev *)filp->private_data;
    size_t offset = 0;
    struct aesd_buffer_entry *entry;

    if(buf == NULL || filp == NULL || f_pos == NULL){
        PDEBUG("invalid pointer input to read\n");
        return -EINVAL;
    }

    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);

    if(count == 0){
        return 0;
    }

    mutex_lock(&dev->mtx);

    // find the line and offset for the desired pos
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buf, *f_pos, &offset);

    // If an entry isn't found, (offset too big) we've reached the end of the buffer
    // and can't read anything
    if(entry == NULL){
        return 0;
    }

    // only read count bytes if that's smaller than what's remaining on the 
    // current line
    bytes_read = min(count, entry->size - offset);

    // actually copy the data out
    if(copy_to_user(buf, entry->buffptr, bytes_read)){
        PDEBUG("failed to read data into user memory\n");
        mutex_unlock(&dev->mtx);
        return -EFAULT;
    }

    // adjust position
    *f_pos += bytes_read;

    mutex_unlock(&dev->mtx);
    return bytes_read;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t bytes_written = 0;
    const char *buf_to_free;
    struct aesd_dev *dev = (struct aesd_dev *)filp->private_data;

    if(buf == NULL || filp == NULL || f_pos == NULL){
        PDEBUG("invalid pointer input to write\n");
        return -EINVAL;
    }

    PDEBUG("write %zu bytes with offset %lld\n",count,*f_pos);

    if(count == 0){
        return 0;
    }

    // lock it down
    mutex_lock(&dev->mtx);

    // Allocate kernel memory for incoming data (expand or create line buf)
    if(dev->line_len){
        // line already started
        dev->line_buf = krealloc(dev->line_buf, dev->line_len + count, GFP_KERNEL);
        if(dev->line_buf == NULL){
            PDEBUG("failed to widen line buf\n");
            mutex_unlock(&dev->mtx);
            return -ENOMEM;
        }
    }
    else{
        // brand new line
        dev->line_buf = kmalloc(count, GFP_KERNEL);
        if(dev->line_buf == NULL){
            PDEBUG("failed to create line buf\n");
            mutex_unlock(&dev->mtx);
            return -ENOMEM;
        }
    }

    // loop through write data and handle complete lines as they come
    while(bytes_written < count){
        if(get_user(dev->line_buf[dev->line_len++], &buf[bytes_written++])){
            PDEBUG("failed to read from user space\n");
            mutex_unlock(&dev->mtx);
            return -EFAULT;
        }

        // check if we just found the end of a line
        if(dev->line_buf[dev->line_len - 1] == '\n'){
            // allocate a new entry
            struct aesd_buffer_entry entry;
            entry.buffptr = kmalloc(dev->line_len, GFP_KERNEL);
            entry.size = dev->line_len;
            if(entry.buffptr == NULL){
                PDEBUG("failed to create entry\n");
                mutex_unlock(&dev->mtx);
                return -ENOMEM; 
            }

            // copy line into new entry (all in kernel space)
            memcpy((void *)entry.buffptr, dev->line_buf, dev->line_len);

            // add new entry to buffer and free old entry if overwritten
            // (if nothing was overwritten it will call kfree(NULL))
            buf_to_free = aesd_circular_buffer_add_entry(&dev->buf, &entry);
            kfree(buf_to_free);

            // free the old line buf since we no longer need any already
            // written data
            kfree(dev->line_buf);
            dev->line_len = 0;

            // only allocate more space if necessary
            if(bytes_written < count){
                dev->line_buf = kmalloc(count - bytes_written, GFP_KERNEL);
                if(dev->line_buf == NULL){
                    PDEBUG("failed to widen line buf\n");
                    mutex_unlock(&dev->mtx);
                    return -ENOMEM;
                }  
            }
        }
    }

    mutex_unlock(&dev->mtx);
    return bytes_written;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    mutex_init(&aesd_device.mtx);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    struct aesd_buffer_entry *entry;
    uint8_t i;

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buf, i){
        if(entry->size > 0){
            kfree(entry->buffptr);
        }
    }

    mutex_destroy(&aesd_device.mtx);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
