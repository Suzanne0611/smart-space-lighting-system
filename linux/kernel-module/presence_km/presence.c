#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/string.h>
#include <linux/ktime.h>
#include <linux/poll.h>     

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Suzanne");
MODULE_DESCRIPTION("Presence KM: HC-SR505 PIR via GPIO17 → /dev/presence");

#define DEVICE_NAME     "presence"
#define CLASS_NAME      "smart_space"
#define GPIO_PIR        529      

static int           major;
static struct class  *dev_class;
static struct device *dev_device;
static struct cdev    pres_cdev;
static dev_t          dev_num;

// ── GPIO interrupt ────────────────────────────────────────────────────────────
static int irq_num;

// ── State and wait queue ────────────────────────────────────────────────────────
static atomic_t pir_state    = ATOMIC_INIT(0);
static atomic_t state_changed = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(pir_wq);

// ── Debounce ──────────────────────────────────────────────────────────────────
#define DEBOUNCE_MS     200     
static ktime_t last_irq_time;

// ── IRQ handler ───────────────────────────────────────────────────────────────
static irqreturn_t pir_irq_handler(int irq, void *dev_id)
{
    ktime_t now = ktime_get();

    if (ktime_ms_delta(now, last_irq_time) < DEBOUNCE_MS)
        return IRQ_HANDLED;
    last_irq_time = now;

    int val = gpio_get_value(GPIO_PIR);
    atomic_set(&pir_state, val);
    atomic_set(&state_changed, 1);
    wake_up_interruptible(&pir_wq);

    return IRQ_HANDLED;
}

// ── file operations ───────────────────────────────────────────────────────────

static int pres_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "[presence] open()\n");
    return 0;
}

static int pres_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t pres_read(struct file *file, char __user *buf,
                         size_t count, loff_t *ppos)
{
    char tmp[4];
    int  len;

    len = snprintf(tmp, sizeof(tmp), "%d\n", atomic_read(&pir_state));

    if (count < len)
        return -EINVAL;
    if (copy_to_user(buf, tmp, len))
        return -EFAULT;

    *ppos = 0;
    return len;
}

static __poll_t pres_poll(struct file *file, poll_table *wait)
{
    poll_wait(file, &pir_wq, wait);
    if (atomic_read(&state_changed))
        return EPOLLIN | EPOLLRDNORM;
    return 0;
}

static const struct file_operations pres_fops = {
    .owner   = THIS_MODULE,
    .open    = pres_open,
    .release = pres_release,
    .read    = pres_read,
    .poll    = pres_poll,
};

// ── init / exit ───────────────────────────────────────────────────────────────

static int __init presence_init(void)
{
    int ret;

    ret = gpio_request(GPIO_PIR, "pir_sensor");
    if (ret) {
        printk(KERN_ERR "[presence] gpio_request failed: %d\n", ret);
        return ret;
    }

    ret = gpio_direction_input(GPIO_PIR);
    if (ret) {
        printk(KERN_ERR "[presence] gpio_direction_input failed: %d\n", ret);
        goto err_gpio;
    }

    irq_num = gpio_to_irq(GPIO_PIR);
    if (irq_num < 0) {
        printk(KERN_ERR "[presence] gpio_to_irq failed: %d\n", irq_num);
        ret = irq_num;
        goto err_gpio;
    }

    ret = request_irq(irq_num, pir_irq_handler,
                      IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                      "pir_irq", NULL);
    if (ret) {
        printk(KERN_ERR "[presence] request_irq failed: %d\n", ret);
        goto err_gpio;
    }

  
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        printk(KERN_ERR "[presence] alloc_chrdev_region failed: %d\n", ret);
        goto err_irq;
    }
    major = MAJOR(dev_num);

    cdev_init(&pres_cdev, &pres_fops);
    pres_cdev.owner = THIS_MODULE;
    ret = cdev_add(&pres_cdev, dev_num, 1);
    if (ret < 0) {
        printk(KERN_ERR "[presence] cdev_add failed: %d\n", ret);
        goto err_cdev;
    }


    dev_class = class_create(CLASS_NAME);
    if (IS_ERR(dev_class)) {
        printk(KERN_WARNING "[presence] class_create failed\n");
        dev_class = NULL;   
    }


    {
        struct class *cls = dev_class;
        if (!cls) {
            cls = class_create("smart_space_pir");
            if (IS_ERR(cls)) {
                ret = PTR_ERR(cls);
                printk(KERN_ERR "[presence] class_create pir failed: %d\n", ret);
                goto err_class;
            }
            dev_class = cls;
        }

        dev_device = device_create(dev_class, NULL, dev_num, NULL, DEVICE_NAME);
        if (IS_ERR(dev_device)) {
            ret = PTR_ERR(dev_device);
            printk(KERN_ERR "[presence] device_create failed: %d\n", ret);
            goto err_device;
        }
    }

    // when init ,read current state onece
    atomic_set(&pir_state, gpio_get_value(GPIO_PIR));
    last_irq_time = ktime_get();

    printk(KERN_INFO "[presence] loading compelete,major=%d, IRQ=%d\n", major, irq_num);
    printk(KERN_INFO "[presence] /dev/presence buided, init state=%d\n",
           atomic_read(&pir_state));
    return 0;

err_device:
    class_destroy(dev_class);
err_class:
    cdev_del(&pres_cdev);
err_cdev:
    unregister_chrdev_region(dev_num, 1);
err_irq:
    free_irq(irq_num, NULL);
err_gpio:
    gpio_free(GPIO_PIR);
    return ret;
}

static void __exit presence_exit(void)
{
    device_destroy(dev_class, dev_num);
    if (dev_class)
        class_destroy(dev_class);
    cdev_del(&pres_cdev);
    unregister_chrdev_region(dev_num, 1);
    free_irq(irq_num, NULL);
    gpio_free(GPIO_PIR);

    printk(KERN_INFO "[presence] unload compelete\n");
}

module_init(presence_init);
module_exit(presence_exit);
