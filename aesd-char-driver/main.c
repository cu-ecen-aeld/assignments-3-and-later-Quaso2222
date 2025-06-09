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
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/kfifo.h>
#include "aesdchar.h"
#include "aesd-circular-buffer.h"
int aesd_major = 0; // use dynamic major
int aesd_minor = 0;
#define ENTRYSIZE 1000
MODULE_AUTHOR("Your Name Here"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct temp_buffer_type
{
    char *buffer;
    size_t buffersize;
    size_t data_length;
} temp_buffer;

struct aesd_dev aesd_device;
#define MY_FIFO_SIZE 1024
unsigned int actual_write_in;

struct temp_buffer_type *temp_buffer_init(struct temp_buffer_type *temp_buffer, size_t size)
{
    temp_buffer->data_length = 0;
    temp_buffer->buffersize = size;
    temp_buffer->buffer = kmalloc(temp_buffer->buffersize, GFP_KERNEL);
    return temp_buffer;
}

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */
    struct aesd_dev *dev;
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev; // for other methods to access
    if ((filp->f_flags & O_ACCMODE) == O_WRONLY)
    {
        if (mutex_lock_interruptible(&dev->lock))
        {
            return -ERESTARTSYS; // handle interrupt
        }
        // aesd_circular_buffer_init(&dev->circular_buffer);
        PDEBUG("open", "aesd_dev initialized");
        mutex_unlock(&dev->lock);
    }
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
    ssize_t retval = 0;
    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);
    struct aesd_dev *dev = filp->private_data; // Get the device structure
    if (mutex_lock_interruptible(&dev->lock))
    {
        return -ERESTARTSYS; // Handle interrupt
    }
    PDEBUG("read: device locked");
    if (count == 0)
    {
        retval = 0; // 明确设置返回值为0，表示读取0字节
        goto out;   // 确保在返回前释放锁
    }
    size_t read_length = 0;
    size_t entry_offset_byte_rtn = 0;
    struct aesd_buffer_entry *current_entry = NULL;
    while (read_length < count)
    {
        current_entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->circular_buffer, *f_pos, &entry_offset_byte_rtn);
        if (current_entry == NULL)
        {
            // 没有更多数据可读（或者 *f_pos 已经超出所有数据）
            break; // 跳出循环
        }
        size_t bytes_to_copy_this_round = min(count - read_length, current_entry->size - entry_offset_byte_rtn);

        if (copy_to_user(buf + read_length, current_entry->buffptr + entry_offset_byte_rtn, bytes_to_copy_this_round))
        {
            PDEBUG("aesdchar: copy_to_user failed");
            retval = -EFAULT; // 发生错误，返回 EFAULT
            goto out;         // 跳转到 out 标签确保锁被释放
        }
        read_length += bytes_to_copy_this_round;
        *f_pos += bytes_to_copy_this_round;
    }
    retval = read_length; // 设置返回值为实际读取的字节数
// struct aesd_dev *dev = filp->private_data; // Get the device structure
// if (mutex_lock_interruptible(&dev->lock))
// {
//     return -ERESTARTSYS; // Handle interrupt
// }

/**
 * TODO: handle read
 */
out:
    mutex_unlock(&dev->lock); // Unlock the device after reading
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                   loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);
    struct aesd_dev *dev = filp->private_data; // Get the device structure
    if (mutex_lock_interruptible(&dev->lock))
    {
        return -ERESTARTSYS; // Handle interrupt
    }
    PDEBUG("write: device locked");
    if (count == 0)
    {
        retval = 0;
        goto out;
    }
    if (temp_buffer.data_length + count > temp_buffer.buffersize)
    {
        PDEBUG("aesdchar: temp_buffer溢出: 当前长度 %zu, 写入长度 %zu, 总共 %zu, 最大 %zu\n",
               temp_buffer.data_length, count, temp_buffer.data_length + count, temp_buffer.buffersize);
        retval = -ENOSPC; // 设备上没有空间
        goto out;         // 跳转到 out 标签确保锁被释放
    }
    if (copy_from_user(temp_buffer.buffer + temp_buffer.data_length, buf, count))
    {
        PDEBUG("aesdchar: temp_buffer.buffer未初始化!\n");
        retval = -EFAULT;
        goto out;
    }

    temp_buffer.data_length += count;
    retval = count;
    char *newline = memchr(temp_buffer.buffer, '\n', temp_buffer.data_length);
    if (newline)
    {
        size_t newline_length = newline - temp_buffer.buffer + 1;
        struct aesd_buffer_entry *entry = kmalloc(sizeof(struct aesd_buffer_entry), GFP_KERNEL);
        if (!entry)
        {
            retval = -ENOMEM; // 内存分配失败
            goto out;         // 跳转到 out 标签确保锁被释放
        }
        entry->buffptr = kmalloc(newline_length + 1, GFP_KERNEL);
        if (!entry->buffptr)
        {
            kfree(entry);
            retval = -ENOMEM; // 内存分配失败
            goto out;         // 跳转到 out 标签确保锁被释放
        }
        memcpy((void *)entry->buffptr, temp_buffer.buffer, newline_length);
        entry->size = newline_length;
        entry->buffptr[newline_length] = '\0';     // Null-terminate the string
        temp_buffer.data_length -= newline_length; // Update the data length
        // Shift remaining data to the beginning of the buffer
        memmove(temp_buffer.buffer, temp_buffer.buffer + newline_length, temp_buffer.data_length);
        PDEBUG("write: entry size %zu", entry->size);
        // Copy data from user space to kernel space
        aesd_circular_buffer_add_entry(&dev->circular_buffer, entry);
    }

    // Allocate memory for the buffer entry

    /**
     * TODO: handle write
     */
out:
    mutex_unlock(&dev->lock);
    return retval;
}
struct file_operations aesd_fops = {
    .owner = THIS_MODULE,
    .read = aesd_read,
    .write = aesd_write,
    .open = aesd_open,
    .release = aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add(&dev->cdev, devno, 1);
    if (err)
    {
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
    if (result < 0)
    {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device, 0, sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    aesd_circular_buffer_init(&aesd_device.circular_buffer);
    mutex_init(&aesd_device.lock); // Initialize the mutex lock
    PDEBUG("aesdchar: device initialized with major %d minor %d", aesd_major, aesd_minor);
    result = aesd_setup_cdev(&aesd_device);
    temp_buffer_init(&temp_buffer, 256);

    if (result)
    {
        unregister_chrdev_region(dev, 1);
    }
    return result;
}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */
    // aesd_circular_buffer_cleanup(&aesd_device.circular_buffer);
    mutex_destroy(&aesd_device.lock); // Destroy the mutex lock
    PDEBUG("aesdchar: cleanup module with major %d minor %d", aesd_major, aesd_minor);
    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
