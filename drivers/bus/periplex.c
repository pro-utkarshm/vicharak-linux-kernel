/*
** Purpose of periplex.c file :
** 1.handle Read/write data coming from specific dummy devices like uart,i2c,spi,gpio
** and so on.
** 2.this is parent file of all other peripheral's(uart,i2c,spi,etc.)device file.
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
#include <linux/peripheral.h>

#include <asm/uaccess.h>
#include <asm/errno.h>

/* 
** header file through which device can communicate and generated 
*/
// #include "include/peripheral.h"

/*
** ioctl call for transfer configuration from kernel space to user-space (used only in
** write case)
*/
#define RD_CONFIGURATION _IOW('a', 'a', struct kernel_config *)

/*
** ioctl call for transfer structure from kernel space to user-space 
** (used only in write case)
*/
#define RD_STRUCTURE _IOW('a', 'b', struct kernel_data *)

/*
** ioctl call for reading the address of message from user-space and then transfer
** actual message from kernel-space to user-space(used only in write case)
*/
#define RD_ADDRESS _IOW('a', 'c', unsigned long *)

/*
** ioctl call for transfer data from user-space to kernel-space
** (used only in read case)
*/
#define WR_VALUE_ADDRESS _IOR('a', 'd', unsigned long *)

/*
** this is ioctl call for acknowledge the interrupt call form user side to kernel 
** side(used in data transfer)
*/
#define DONE_DATA _IOR('a', 'e', int *)

/*
** this is ioctl call for acknowledge the interrupt call form user side to kernel 
** side(used in configuration transfer)
*/
#define DONE_CONFIGURATION _IOR('a', 'f', int *)

static int register_count = 0;

/* these variable are used in write case */
char *write_message;
struct kernel_data w_data;
struct kernel_config w_config;
unsigned long write_message_address;

/* these variable are used in read case */
char *read_message;
struct read_buffer r_data;

/* Wait queue and flag for data  */
wait_queue_head_t wait_queue_data_ioctl;
int wait_queue_flag_data;

/* Wait queue and flag for ack data  */
wait_queue_head_t wait_queue_done_data_ioctl;
int wait_queue_flag_done_data;

/* Wait queue and flag for config  */
wait_queue_head_t wait_queue_config_ioctl;
int wait_queue_flag_config;

/* Wait queue and flag for ack config  */
wait_queue_head_t wait_queue_done_configuration_ioctl;
int wait_queue_flag_done_configuration;

struct mutex periplex_mutex;

static unsigned long periplex_dev[128] = {0};

/* device number */
dev_t periplex_num = 0;

/* class */
static struct class *periplex_class;

/* cdev variable */
struct cdev periplex_cdev;

void set_periplex_configuration(int peri_id, uint8_t config_id, int configuration)
{
	mutex_lock(&periplex_mutex);
	w_config.peri_id = peri_id;
	w_config.configuration_id = config_id;
	w_config.configuration = configuration;

	wait_queue_flag_config = 1;
	wake_up_interruptible(&wait_queue_config_ioctl);

	wait_queue_flag_done_configuration = 0;
	wait_event_interruptible(wait_queue_done_configuration_ioctl, 
			wait_queue_flag_done_configuration != 0);

	mutex_unlock(&periplex_mutex);
}
EXPORT_SYMBOL(set_periplex_configuration);

void set_periplex_data(int peri_id, int length, char *message)
{
	mutex_lock(&periplex_mutex);
	w_data.peri_id = peri_id;
	w_data.length = length;

	write_message = kmalloc(length, GFP_KERNEL);
	if (memcpy(write_message, message, length) == NULL)
	{
		pr_err("Not able to copy\n");
	}

	wait_queue_flag_data = 1;
	wake_up_interruptible(&wait_queue_data_ioctl);

	wait_queue_flag_done_data = 0;
	wait_event_interruptible(wait_queue_done_data_ioctl, 
			wait_queue_flag_done_data != 0);

	mutex_unlock(&periplex_mutex);
}
EXPORT_SYMBOL(set_periplex_data);

int periplex_device_register(struct platform_device *pdev, int periplex_id)
{

	if (register_count > 128)
	{
		pr_err("Your driver is not inserted successfully\n");
		return -EINVAL;
	}

	if (periplex_dev[periplex_id] == 0)
	{
		periplex_dev[periplex_id] = (unsigned long)pdev;
		pr_info("periplex_id register %d\n", periplex_id);
		register_count++;
		return 0;
	}
	else
	{
		return 1;
	}
}
EXPORT_SYMBOL(periplex_device_register);

void periplex_device_unregister(struct platform_device *pdev)
{

	int unregister_count = 0;
	for (unregister_count = 0; unregister_count < 128; unregister_count++)
	{
		if (periplex_dev[unregister_count])
		{
			if ((unsigned long)pdev == periplex_dev[unregister_count])
			{	
				pr_info("periplex_id unregister %d\n",unregister_count);
				periplex_dev[unregister_count] = 0;
				register_count--;
				break;
			}
		}
	}
}
EXPORT_SYMBOL(periplex_device_unregister);

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
	int done_data;
	int done_configuration;
	struct periplex_device *peri_dev;
	switch (cmd)
	{
		case WR_VALUE_ADDRESS:
			if (copy_from_user(&r_data, (void *)arg, sizeof(r_data)))
			{
				pr_err("address is not copied perfectly");
				break;
			}
			read_message = kmalloc(r_data.length, GFP_KERNEL);
			if (!read_message)
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

			peri_dev = platform_get_drvdata((struct platform_device *)periplex_dev
			[r_data.minor]);
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

		case RD_CONFIGURATION:
			wait_queue_flag_config = 0;
			wait_event_interruptible(wait_queue_config_ioctl, wait_queue_flag_config != 0);
			if (copy_to_user((struct kernel_config *)arg, &w_config, 
			sizeof(struct kernel_config)))
			{
				pr_err("Not able to copy structure of config");
			}
			break;

		case RD_STRUCTURE:
			wait_queue_flag_data = 0;
			wait_event_interruptible(wait_queue_data_ioctl, wait_queue_flag_data != 0);
			if (copy_to_user((struct kernel_data *)arg, &w_data, 
					sizeof(struct kernel_data)))
			{
				pr_err("Not able to copy structure of data");
			}
			break;

		case RD_ADDRESS:
			if (copy_from_user(&write_message_address, (unsigned long *)arg, 
					sizeof(unsigned long *)))
			{
				pr_err("address is not copy perfectly");
				break;
			}
			if (copy_to_user((char *)write_message_address, write_message, 
					w_data.length))
			{
				pr_err("message is not passed perfectly");
			}
			kfree(write_message);
			break;

		case DONE_DATA:
			if (copy_from_user(&done_data, (int *)arg, sizeof(done_data)))
			{
				pr_err("Done_data : Err!\n");
				break;
			}
			if (done_data == 1)
			{
				wait_queue_flag_done_data = 1;
				wake_up_interruptible(&wait_queue_done_data_ioctl);
			}
			break;

		case DONE_CONFIGURATION:
			if (copy_from_user(&done_configuration, (int *)arg, sizeof(done_configuration)))
			{
				pr_err("Done_data : Err!\n");
				break;
			}
			if (done_configuration == 1)
			{
				wait_queue_flag_done_configuration = 1;
				wake_up_interruptible(&wait_queue_done_configuration_ioctl);
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
	pr_info("periplex Module is inserted Successfully\n");

	/* Initialize wait queue for configuration */
	init_waitqueue_head(&wait_queue_config_ioctl);

	/* Initialize wait queue for reading */
	init_waitqueue_head(&wait_queue_data_ioctl);

	/* Initialize wait queue for done data */
	init_waitqueue_head(&wait_queue_done_data_ioctl);

	/* Initialize wait queue for done configuration */
	init_waitqueue_head(&wait_queue_done_configuration_ioctl);

	/* Initialize mutex */
	mutex_init(&periplex_mutex);
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
}

/*  Module insert and exit */
module_init(ioctl_init);
module_exit(ioctl_exit);

MODULE_ALIAS("character:periplex");
MODULE_AUTHOR("Vatsal Kevadiya <vhkevadiya15@gmail.com>");
MODULE_DESCRIPTION("periplex: handle read/write operation from/to peripheral's");
MODULE_LICENSE("GPL");