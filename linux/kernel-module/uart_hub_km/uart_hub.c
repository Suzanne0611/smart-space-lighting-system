#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/tty.h>         

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Suzanne");
MODULE_DESCRIPTION("UART Hub KM: /dev/light_sensor + /dev/lighting via ttyAMA0");

#define CLASS_NAME   "smart_space"
#define UART_DEVICE  "/dev/ttyAMA0"
//#define UART_DEVICE  "/dev/ttyACM0"
#define BUF_SIZE     64

#define MINOR_LIGHT    0
#define MINOR_LIGHTING 1
#define DEV_COUNT      2

static dev_t         dev_num;
static struct class *dev_class;
static struct cdev   hub_cdev;

static struct file  *uart_fp = NULL;
static DEFINE_MUTEX(uart_tx_mutex);

static char         lux_str[BUF_SIZE] = "LUX:0.0\n";
static DEFINE_MUTEX(lux_mutex);

static DECLARE_WAIT_QUEUE_HEAD(lux_wq);
static atomic_t new_data = ATOMIC_INIT(0);

#define TIMEOUT_MS   10000
static ktime_t  last_lux_time;
static atomic_t is_timeout   = ATOMIC_INIT(0);
static atomic_t lux_received = ATOMIC_INIT(0);

static struct task_struct *reader_thread;

static int uart_reader(void *data)
{
    char   line_buf[BUF_SIZE];
    char   c;
    int    pos = 0;

    printk(KERN_INFO "[uart_hub] RX kthread starts\n");

    uart_fp->f_flags |= O_NONBLOCK;

    last_lux_time = ktime_get();

    while (!kthread_should_stop()) {
        loff_t offset = 0;
        ssize_t ret = kernel_read(uart_fp, &c, 1, &offset);

        if (ret == -EAGAIN || ret == 0) {
            msleep(10);

            if (atomic_read(&lux_received) && !atomic_read(&is_timeout)) {
                s64 elapsed = ktime_ms_delta(ktime_get(), last_lux_time);
                if (elapsed > TIMEOUT_MS) {
                    atomic_set(&is_timeout, 1);
                    mutex_lock(&lux_mutex);
                    strncpy(lux_str, "ERROR:TIMEOUT\n", sizeof(lux_str));
                    mutex_unlock(&lux_mutex);
                    atomic_set(&new_data, 1);
                    wake_up_interruptible(&lux_wq);
                    printk(KERN_WARNING "[uart_hub] TIMEOUT:%lld ms didin't recive LUX\n", elapsed);
                }
            }
            continue;
        }

        if (ret < 0) {
            msleep(100);
            continue;
        }

        if (c == '\r') continue;

        if (c == '\n') {
            line_buf[pos] = '\0';
            pos = 0;

            if (strncmp(line_buf, "LUX:", 4) == 0) {
                mutex_lock(&lux_mutex);
                snprintf(lux_str, sizeof(lux_str), "%s\n", line_buf);
                mutex_unlock(&lux_mutex);

                last_lux_time = ktime_get();
                atomic_set(&is_timeout, 0);
                atomic_set(&lux_received, 1);
                atomic_set(&new_data, 1);
                wake_up_interruptible(&lux_wq);

                printk(KERN_INFO "[uart_hub] RX updated：%s", lux_str);
            }
            continue;
        }

        if (pos < BUF_SIZE - 1)
            line_buf[pos++] = c;
    }

    printk(KERN_INFO "[uart_hub] RX kthread over\n");
    return 0;
}

static int hub_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "[uart_hub] open() minor=%d\n", iminor(inode));
    return 0;
}

static int hub_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t hub_read(struct file *file, char __user *buf,
                        size_t count, loff_t *ppos)
{
    char tmp[BUF_SIZE];
    int  len, ret;

    if (iminor(file_inode(file)) != MINOR_LIGHT)
        return -EPERM;

    if (file->f_flags & O_NONBLOCK) {
        if (!atomic_read(&new_data))
            return -EAGAIN;
    } else {
        ret = wait_event_interruptible(lux_wq, atomic_read(&new_data) == 1);
        if (ret)
            return -ERESTARTSYS;
    }

    atomic_set(&new_data, 0);

    mutex_lock(&lux_mutex);
    strncpy(tmp, lux_str, sizeof(tmp));
    mutex_unlock(&lux_mutex);

    len = strlen(tmp);
    if (count < len)
        return -EINVAL;
    if (copy_to_user(buf, tmp, len))
        return -EFAULT;

    *ppos = 0;
    return len;
}

static ssize_t hub_write(struct file *file, const char __user *buf,
                         size_t count, loff_t *ppos)
{
    char             kbuf[BUF_SIZE];
    struct tty_struct *tty;
    ssize_t          ret;

    if (iminor(file_inode(file)) != MINOR_LIGHTING)
        return -EPERM;

    if (count >= BUF_SIZE)
        return -EINVAL;
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;

    kbuf[count] = '\0';

	tty = ((struct tty_file_private *)uart_fp->private_data)->tty;
    if (!tty || !tty->ops || !tty->ops->write) {
        printk(KERN_ERR "[uart_hub] can't get tty\n");
        return -EIO;
    }

    mutex_lock(&uart_tx_mutex);
    ret = tty->ops->write(tty, kbuf, count);
    mutex_unlock(&uart_tx_mutex);

    if (ret < 0) {
        printk(KERN_ERR "[uart_hub] TX writing failed: %zd\n", ret);
        return ret;
    }

    printk(KERN_INFO "[uart_hub] TX sent：%s", kbuf);
    return count;
}

static const struct file_operations hub_fops = {
    .owner   = THIS_MODULE,
    .open    = hub_open,
    .release = hub_release,
    .read    = hub_read,
    .write   = hub_write,
};

static int __init uart_hub_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&dev_num, 0, DEV_COUNT, "uart_hub");
    if (ret < 0) {
        printk(KERN_ERR "[uart_hub] alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    cdev_init(&hub_cdev, &hub_fops);
    hub_cdev.owner = THIS_MODULE;
    ret = cdev_add(&hub_cdev, dev_num, DEV_COUNT);
    if (ret < 0) {
        printk(KERN_ERR "[uart_hub] cdev_add failed: %d\n", ret);
        goto err_cdev;
    }

    dev_class = class_create(CLASS_NAME);
    if (IS_ERR(dev_class)) {
        ret = PTR_ERR(dev_class);
        printk(KERN_ERR "[uart_hub] class_create failed: %d\n", ret);
        goto err_class;
    }

    if (IS_ERR(device_create(dev_class, NULL,
                             MKDEV(MAJOR(dev_num), MINOR_LIGHT),
                             NULL, "light_sensor"))) {
        ret = -ENOMEM;
        goto err_dev0;
    }

    if (IS_ERR(device_create(dev_class, NULL,
                             MKDEV(MAJOR(dev_num), MINOR_LIGHTING),
                             NULL, "lighting"))) {
        ret = -ENOMEM;
        goto err_dev1;
    }

    uart_fp = filp_open(UART_DEVICE, O_RDWR | O_NOCTTY, 0);
    if (IS_ERR(uart_fp)) {
        ret = PTR_ERR(uart_fp);
        uart_fp = NULL;
        printk(KERN_ERR "[uart_hub] won't open %s: %d\n", UART_DEVICE, ret);
        goto err_uart;
    }

    reader_thread = kthread_run(uart_reader, NULL, "uart_hub_rx");
    if (IS_ERR(reader_thread)) {
        ret = PTR_ERR(reader_thread);
        printk(KERN_ERR "[uart_hub] kthread_run failed: %d\n", ret);
        goto err_thread;
    }

    printk(KERN_INFO "[uart_hub] Loading successfully, major=%d\n", MAJOR(dev_num));
    printk(KERN_INFO "[uart_hub] /dev/light_sensor（minor=0）Created.\n");
    printk(KERN_INFO "[uart_hub] /dev/lighting（minor=1）Created.\n");
    return 0;

err_thread:
    filp_close(uart_fp, NULL);
    uart_fp = NULL;
err_uart:
    device_destroy(dev_class, MKDEV(MAJOR(dev_num), MINOR_LIGHTING));
err_dev1:
    device_destroy(dev_class, MKDEV(MAJOR(dev_num), MINOR_LIGHT));
err_dev0:
    class_destroy(dev_class);
err_class:
    cdev_del(&hub_cdev);
err_cdev:
    unregister_chrdev_region(dev_num, DEV_COUNT);
    return ret;
}

static void __exit uart_hub_exit(void)
{
    kthread_stop(reader_thread);

    if (uart_fp) {
        filp_close(uart_fp, NULL);
        uart_fp = NULL;
    }

    device_destroy(dev_class, MKDEV(MAJOR(dev_num), MINOR_LIGHTING));
    device_destroy(dev_class, MKDEV(MAJOR(dev_num), MINOR_LIGHT));
    class_destroy(dev_class);
    cdev_del(&hub_cdev);
    unregister_chrdev_region(dev_num, DEV_COUNT);

    printk(KERN_INFO "[uart_hub] Unloading compeleted\n");
}

module_init(uart_hub_init);
module_exit(uart_hub_exit);
