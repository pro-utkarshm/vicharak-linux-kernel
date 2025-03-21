/*
** Purpose of periplex.c file :
** 1.handle Read/write data coming from specific dummy devices like uart,i2c,spi,gpio
** and so on.
** 2.this is parent file of all other peripheral's(uart,i2c,spi,etc.)device file.
** 3.create separate bus for `periplex`, and all the peripheral's (uart,i2c,spi,
** gpio,pwm,etc..) runing on top the bus.
** 4.Also create a one character device for periplex, which is used for read/write of
** peripheral's through ioctl calls.
*/

#include <linux/fs.h>
#include <linux/err.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>

#include <asm/uaccess.h>
#include <asm/errno.h>

/*
** header file through which device can communicate and generated
*/
#include <linux/peripheral.h>
// #include "include/peripheral.h"

/*
** ioctl call for transfer configuration structure from kernel space to user-space
** (used only in configuration write case)
*/
#define WAIR_FOR_CONFIGURATION _IOW('a', 'a', struct kernel_config *)

/*
** this is ioctl call for acknowledge the interrupt call form user space to kernel
** space (used in configuration transfer)
*/
#define DONE_CONFIGURATION _IOR('a', 'b', int *)

/*
** ioctl call for transfer structure from kernel space to user-space
** (used only in write case)
*/
#define WAIT_FOR_STRUCTURE _IOW('a', 'c', struct kernel_data *)

/*
** ioctl call for reading the address of message from user-space and then transfer
** actual message from kernel-space to user-space(used only in write case)
*/
#define SND_ADDRESS _IOW('a', 'd', unsigned long *)

/*
** this is ioctl call for acknowledge the interrupt call form user side to kernel
** side(used in data transfer)
*/
#define DONE_DATA _IOR('a', 'e', int *)

/*
** ioctl call for transfer data from user-space to kernel-space
** (used only in read case)
*/
#define SND_READ_STRUCTURE _IOR('a', 'f', unsigned long *)

static int register_count = 0;

/* these variable are used in write case */
char *write_message = NULL;
struct kernel_data w_data = {0};
struct kernel_config w_config = {0};
unsigned long write_message_address;

/* these variable are used in read case */
char *read_message = NULL;
struct kernel_buffer r_data = {0};

/* Wait queue and flag for data  */
wait_queue_head_t wait_data_queue_ioctl;
int wait_data_queue_flag;

/* Wait queue and flag for ack data  */
wait_queue_head_t wait_done_data_queue_ioctl;
int wait_done_data_queue_flag;

/* Wait queue and flag for config  */
wait_queue_head_t wait_configuration_queue_ioctl;
int wait_configuration_queue_flag;

/* Wait queue and flag for ack config  */
wait_queue_head_t wait_done_configuration_queue_ioctl;
int wait_done_configuration_queue_flag;

struct mutex periplex_mutex;

static unsigned long periplex_dev[128] = {0};

/* device number */
dev_t periplex_num = 0;

/* class */
static struct class *periplex_class;

/* cdev variable */
struct cdev periplex_cdev;

/* Periplex bus methods */
static int periplex_bus_match(struct device *dev, struct device_driver *drv);
static int periplex_driver_probe(struct device *dev);
static int periplex_driver_remove(struct device *dev);

/* Structure representing periplex bus type */
struct bus_type periplex_bus_type = {
	.name = "periplex",
	.match = periplex_bus_match,
	.probe = periplex_driver_probe,
	.remove = periplex_driver_remove,
};

/* Function to register periplex device */
int periplex_register_device(struct periplex_device *pdev)
{
	int ret;

	device_initialize(&pdev->dev);
	pdev->dev.release = device_release_driver;

	ret = dev_set_name(&pdev->dev, "%s", kbasename(pdev->dev.of_node->full_name));
	if (ret)
	{
		return ret;
	}

	ret = device_add(&pdev->dev);
	if (ret)
	{
		dev_err(&pdev->dev, "Failed to add device '%s'\n", dev_name(&pdev->dev));
		return ret;
	}

	return 0;
}

/* Function to unregister periplex device */
void periplex_unregister_device(struct periplex_device *pdev)
{
	device_unregister(&pdev->dev);
	kfree(pdev);
}

/* Periplex bus device match function */
static int periplex_bus_match(struct device *dev, struct device_driver *drv)
{
	return of_driver_match_device(dev, drv);
}

/* Probe function for the periplex bus */
static int periplex_driver_probe(struct device *dev)
{
	struct periplex_driver *pdrv = to_periplex_driver(dev->driver);
	struct periplex_device *pdev = to_periplex_device(dev);

	if (pdrv->probe)
	{
		return pdrv->probe(pdev);
	}

	return -ENODEV;
}

/* Remove function for the periplex bus */
static int periplex_driver_remove(struct device *dev)
{
	struct periplex_driver *pdrv = to_periplex_driver(dev->driver);
	struct periplex_device *pdev = to_periplex_device(dev);

	if (pdrv->remove)
	{
		return pdrv->remove(pdev);
	}

	return 0;
}

/* Periplex bus init function */
static int periplex_bus_init(void)
{
	struct device_node *periplex_node;
	struct device_node *np;
	struct periplex_device *pdev;
	struct platform_device *periplex;
	int ret;

	/* Get 'periplex' node from device tree */
	periplex_node = of_find_compatible_node(NULL, NULL, "vicharak,periplex");
	if (!periplex_node)
	{
		return -ENODEV;
	}

	periplex = of_find_device_by_node(periplex_node);
	if (!periplex)
	{
		return -ENODEV;
	}

	/* Parse DT to find and register periplex devices */
	for_each_child_of_node(periplex_node, np)
	{

		pdev = kzalloc(sizeof(*pdev), GFP_KERNEL);
		if (!pdev)
			return -ENOMEM;

		pdev->dev.of_node = np;
		pdev->dev.parent = &periplex->dev;
		pdev->dev.bus = &periplex_bus_type;

		ret = periplex_register_device(pdev);
		if (ret)
		{
			kfree(pdev);
			continue;
		}
	}

	return 0;
}

/* Callback function to remove devices registered under periplex bus */
static int periplex_unregister_device_cb(struct device *dev, void *data)
{
	struct periplex_device *pdev = to_periplex_device(dev);
	periplex_unregister_device(pdev);
	return 0;
}

/* Function to remove devices registered under periplex bus */
static void remove_periplex_bus_devices(void)
{
	bus_for_each_dev(&periplex_bus_type, NULL, NULL, periplex_unregister_device_cb);
}

/*set configuration for a specific peripheral(use in write case) */
void set_periplex_configuration(int peri_id, uint8_t config_id, int configuration)
{
	mutex_lock(&periplex_mutex);
	w_config.peri_id = peri_id;
	w_config.configuration_id = config_id;
	w_config.configuration = configuration;

	wait_configuration_queue_flag = 1;
	wake_up_interruptible(&wait_configuration_queue_ioctl);

	wait_done_configuration_queue_flag = 0;
	wait_event_interruptible(wait_done_configuration_queue_ioctl,
							 wait_done_configuration_queue_flag != 0);

	mutex_unlock(&periplex_mutex);
}
EXPORT_SYMBOL(set_periplex_configuration);

/*set data for a specific peripheral (use in write case) */
void set_periplex_data(int peri_id, int length, char *message)
{
	mutex_lock(&periplex_mutex);
	w_data.peri_id = peri_id;
	w_data.length = length;

	write_message = kmalloc(length, GFP_KERNEL);
	if (write_message == NULL)
	{
		pr_err("Memory allocation failed\n");
		mutex_unlock(&periplex_mutex);
		return;
	}

	if (memcpy(write_message, message, length) == NULL)
	{
		pr_err("Memory copy failed\n");
		kfree(write_message);
		mutex_unlock(&periplex_mutex);
		return;
	}

	wait_data_queue_flag = 1;
	wake_up_interruptible(&wait_data_queue_ioctl);

	wait_done_data_queue_flag = 0;
	wait_event_interruptible(wait_done_data_queue_ioctl, wait_done_data_queue_flag != 0);

	mutex_unlock(&periplex_mutex);
}
EXPORT_SYMBOL(set_periplex_data);

/* Function to register Periplex driver */
int periplex_register_driver(struct periplex_driver *drv)
{
	int ret;

	if (!drv)
	{
		return -EINVAL;
	}

	drv->driver.bus = &periplex_bus_type;

	ret = driver_register(&drv->driver);
	if (ret)
	{
		pr_err("Periplex driver: Failed to register driver '%s'\n", drv->driver.name);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(periplex_register_driver);

/* Function to unregister Periplex driver */
void periplex_unregister_driver(struct periplex_driver *drv)
{
	if (!drv)
	{
		pr_err("Periplex driver: NULL driver pointer during unregistration\n");
		return;
	}

	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL(periplex_unregister_driver);

/* Function to link any Peripheral's to periplex(mandatory) */
int periplex_link_device(struct periplex_device *pdev)
{

	if (register_count > 128)
	{
		pr_err("OUT_OF_RANGE:Your driver is not inserted successfully\n");
		return -EINVAL;
	}

	if (periplex_dev[pdev->periplex_id] == 0)
	{
		periplex_dev[pdev->periplex_id] = (unsigned long)pdev;
		pr_info("periplex_id register %d\n", pdev->periplex_id);
		register_count++;
		return 0;
	}
	else
	{
		return 1;
	}
}
EXPORT_SYMBOL(periplex_link_device);

/* Function to unlink any Peripheral's to periplex(mandatory) */
void periplex_unlink_device(struct periplex_device *pdev)
{

	int unregister_count = 0;
	for (unregister_count = 0; unregister_count < 128; unregister_count++)
	{
		if (periplex_dev[unregister_count])
		{
			if ((unsigned long)pdev == periplex_dev[unregister_count])
			{
				pr_info("periplex_id unregister %d\n", unregister_count);
				periplex_dev[unregister_count] = 0;
				register_count--;
				break;
			}
		}
	}
}
EXPORT_SYMBOL(periplex_unlink_device);

static int device_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int device_close(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t device_read(struct file *file, char __user *buf, size_t len,
						   loff_t *off)
{
	return 0;
}

static ssize_t device_write(struct file *file, const char __user *buf,
							size_t len, loff_t *off)
{
	return 0;
}

/* Device ioctl function handle read/write operation from/to peripherals*/
static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int done_data = 0;
	int done_configuration = 0;
	struct periplex_device *peri_dev;
	switch (cmd)
	{
	case SND_READ_STRUCTURE:
		if (copy_from_user(&r_data, (void *)arg, sizeof(r_data)))
		{
			pr_err("address is not copied perfectly");
			break;
		}
		read_message = kmalloc(r_data.length, GFP_KERNEL);
		if (read_message == NULL)
		{
			pr_err("Memory allocation for read_message failed");
			break;
		}
		if (copy_from_user(read_message, r_data.message, r_data.length))
		{
			pr_err("message is not copied perfectly");
			kfree(read_message);
			break;
		}

		if (periplex_dev[r_data.minor] == 0)
		{
			pr_err("No device registered for minor %d\n", r_data.minor);
			kfree(read_message);
			break;
		}

		peri_dev = ((struct periplex_device *)periplex_dev[r_data.minor]);
		if (!peri_dev)
		{
			pr_err("Failed to get periplex device for minor %d\n", r_data.minor);
			kfree(read_message);
			break;
		}

		if (!peri_dev->get_periplex_data)
		{
			pr_err("get_periplex_data function pointer is NULL for minor %d\n",
				   r_data.minor);
			kfree(read_message);
			break;
		}

		if (peri_dev->get_periplex_data(peri_dev, read_message, r_data.length))
		{
			pr_err("Failed to get periplex data for minor %d\n", r_data.minor);
			kfree(read_message);
			break;
		}

		kfree(read_message);
		break;

	case WAIR_FOR_CONFIGURATION:
		wait_configuration_queue_flag = 0;
		wait_event_interruptible(wait_configuration_queue_ioctl, wait_configuration_queue_flag != 0);
		if (copy_to_user((struct kernel_config *)arg, &w_config,
						 sizeof(w_config)))
		{
			pr_err("WAIT_FOR_CONFIGURATION: Not able to copy structure of config");
		}
		break;

	case WAIT_FOR_STRUCTURE:
		wait_data_queue_flag = 0;
		wait_event_interruptible(wait_data_queue_ioctl, wait_data_queue_flag != 0);
		if (copy_to_user((struct kernel_data *)arg, &w_data, sizeof(w_data)))
		{
			pr_err("WAIT_FOR_STRUCTURE: Not able to pass structure to the user-space\n");
		}
		break;

	case SND_ADDRESS:
		if (copy_from_user(&write_message_address, (void __user *)arg,
						   sizeof(write_message_address)))
		{
			pr_err("SND_ADDRESS: address is not copy perfectly");
			kfree(write_message);
			break;
		}
		if (copy_to_user((char *)write_message_address, write_message,
						 w_data.length))
		{
			pr_err("SND_ADDRESS: message is not passed perfectly");
			kfree(write_message);
			break;
		}
		kfree(write_message);
		break;

	case DONE_DATA:
		if (copy_from_user(&done_data, (int __user *)arg, sizeof(done_data)))
		{
			pr_err("DONE_DATA: Failed to copy data from user space\n");
			return -EFAULT;
		}
		if (done_data == 1)
		{
			wait_done_data_queue_flag = 1;
			wake_up_interruptible(&wait_done_data_queue_ioctl);
		}
		else
		{
			pr_warn("DONE_DATA: Unexpected value for done_data: %d\n", done_data);
		}
		break;

	case DONE_CONFIGURATION:
		if (copy_from_user(&done_configuration, (int __user *)arg, sizeof(done_configuration)))
		{
			pr_err("Done_CONFIGURATION: Failed to copy data from user space\n");
			return -EFAULT;
		}
		if (done_configuration == 1)
		{
			wait_done_configuration_queue_flag = 1;
			wake_up_interruptible(&wait_done_configuration_queue_ioctl);
		}
		else
		{
			pr_warn("DONE_CONFIGURATION: Unexpected value for "
					"done_configuration: %d\n",
					done_configuration);
		}
		break;

	default:
		pr_info("Default\n");
		break;
	}
	return 0;
}

/* device file operations */
struct file_operations periplex_ioctl_ops = {
	.owner = THIS_MODULE,
	.open = device_open,
	.write = device_write,
	.read = device_read,
	.unlocked_ioctl = device_ioctl,
	.release = device_close};

/* ioctl_init function */
static int __init ioctl_init(void)
{
	int ret;

	/* Initialize wait queue for configuration */
	init_waitqueue_head(&wait_configuration_queue_ioctl);

	/* Initialize wait queue for reading */
	init_waitqueue_head(&wait_data_queue_ioctl);

	/* Initialize wait queue for done data */
	init_waitqueue_head(&wait_done_data_queue_ioctl);

	/* Initialize wait queue for done configuration */
	init_waitqueue_head(&wait_done_configuration_queue_ioctl);

	/* Initialize mutex */
	mutex_init(&periplex_mutex);

	/* Allocating Major Number */
	if ((alloc_chrdev_region(&periplex_num, 0, 1, "periplex")) < 0)
	{
		pr_err("Cannot allocate major number for device\n");
		return ret;
	}

	/* Initialize the cdev structure with fops */
	cdev_init(&periplex_cdev, &periplex_ioctl_ops);

	/* Register a device (cdev structure) with VFS */
	cdev_add(&periplex_cdev, periplex_num, 1);

	/* Creating a device class unser /sys/class */
	if (IS_ERR(periplex_class = class_create(THIS_MODULE, "periplex")))
	{
		pr_err("Cannot create the struct class\n");
		goto r_class;
	}

	/* Creating device under /dev */
	if (IS_ERR(device_create(periplex_class, NULL, periplex_num, NULL, "periplex")))
	{
		pr_err("Cannot create the Device\n");
		goto r_device;
	}
	pr_info("periplex char device inserted Successfully\n");

	ret = bus_register(&periplex_bus_type);
	if (ret)
	{
		pr_err("periplex: Failed to register bus\n");
		return ret;
	}

	ret = periplex_bus_init();
	if (ret)
	{
		pr_err("periplex: Failed to initialize bus\n");
		bus_unregister(&periplex_bus_type);
		return ret;
	}
	pr_info("periplex bus inserted successfully\n");
	return 0;

r_device:

	/* Destroy class and device */
	device_destroy(periplex_class, periplex_num);
	class_destroy(periplex_class);

r_class:

	/* Unregister device major number */
	unregister_chrdev_region(periplex_num, 1);
	return ret;
}

/* Module exit function named as ioctl_exit */
static void __exit ioctl_exit(void)
{
	device_destroy(periplex_class, periplex_num);
	class_destroy(periplex_class);
	unregister_chrdev_region(periplex_num, 1);
	pr_info("Periplex Module is removed successfully...\n");
	remove_periplex_bus_devices();
	bus_unregister(&periplex_bus_type);
	pr_info("Periplex bus is removed successfully...\n");
}

/*  Module insert and exit */
module_init(ioctl_init);
module_exit(ioctl_exit);

MODULE_ALIAS("character:periplex");
MODULE_AUTHOR("Vatsal Kevadiya <vhkevadiya15@gmail.com>");
MODULE_DESCRIPTION("periplex: handle read/write operation from/to peripheral's");
MODULE_LICENSE("GPL");