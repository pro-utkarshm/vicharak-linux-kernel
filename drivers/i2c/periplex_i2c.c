/*
** Significant of i2c.c file :
** 1. Make multiple i2c devices with the use of dtso and create the
** i2c-* series into the /dev
** 2. Allow Write/read data in any specific i2c(i2c-*) device
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <net/sock.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/wait.h>
#include <linux/of.h>
#include <linux/moduleparam.h>

#include <asm/uaccess.h>
#include <asm/errno.h>

/*
** header file through which device can communicate and generated
*/
#include <linux/peripheral.h>
// #include "include/peripheral.h"

#define DRIVER_NAME "periplex-i2c"
#define FCLK 50000000

/*
** waitqueue used for internal i2c operations
*/
wait_queue_head_t wait_queue_i2c_ioctl;
int wait_queue_flag_com_i2c;

/* Debug flag (can be set via module parameter) */
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable or disable debug mode");

/* Macro to conditionally print debug info */
#define I2C_DEBUG(fmt, ...)              \
	do                                   \
	{                                    \
		if (debug)                       \
			pr_info(fmt, ##__VA_ARGS__); \
	} while (0)

/*
** This function used to get the functionalities that are supported
** by this bus driver.
*/
u32 i2c_func(struct i2c_adapter *adapter)
{
	return (I2C_FUNC_I2C |
			I2C_FUNC_SMBUS_QUICK |
			I2C_FUNC_SMBUS_BYTE |
			I2C_FUNC_SMBUS_BYTE_DATA |
			I2C_FUNC_SMBUS_WORD_DATA |
			I2C_SMBUS_I2C_BLOCK_DATA |
			I2C_FUNC_SMBUS_BLOCK_DATA |
			I2C_FUNC_SMBUS_I2C_BLOCK |
			I2C_FUNC_SMBUS_BLOCK_DATA);
}

char *read_data_i2c;
int data_from_usr;

/*
** this functions is used in read for i2c
*/
int read_data_for_i2c(struct periplex_device *pdev, char *message,
					  const int len)
{
	data_from_usr = len;
	I2C_DEBUG("i2c read calling\n");
	I2C_DEBUG("length is %d\n", len);

	read_data_i2c = kmalloc(len, GFP_KERNEL);
	if (!read_data_i2c)
	{
		pr_err("Failed to allocate memory for read_data_i2c\n");
		return -ENOMEM;
	}

	if (memcpy(read_data_i2c, message, len) == NULL)
	{
		pr_err("Not able to copy\n");
		kfree(read_data_i2c);
		return -EFAULT;
	}

	wait_queue_flag_com_i2c = 1;
	wake_up_interruptible(&wait_queue_i2c_ioctl);
	msleep(5);
	return 0;
}

/*
** This function will be called whenever you call I2C read, wirte APIs like
** i2c_master_send(), i2c_master_recv() etc.
*/
static s32 i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
					int num)
{
	int i = 0;
	char *message = NULL;
	int *p_id;
	int periplex_id;

	if (!adap || !adap->dev.driver_data)
	{
		pr_err("Invalid adapter or driver data\n");
		return -EINVAL;
	}

	p_id = adap->dev.driver_data;
	periplex_id = *p_id;

	I2C_DEBUG("i2c_xfer called with %d messages\n", num);

	for (i = 0; i < num; i++)
	{
		int j;
		int index = 0;
		struct i2c_msg *msg_temp = &msgs[i];
		bool is_read = (msg_temp->flags & I2C_M_RD) != 0;

		I2C_DEBUG("[Count: %d] [%s]: [Addr = 0x%x] [Len = %d] [Operation = %s]\n",
				  i, __func__, msg_temp->addr, msg_temp->len,
				  is_read ? "READ" : "WRITE");

		if (is_read)
		{
			int remaining, copy_size;
			message = kmalloc(2, GFP_KERNEL);
			if (!message)
			{
				pr_err("Failed to allocate memory for read message\n");
				return -ENOMEM;
			}

			message[0] = msg_temp->addr << 1 | is_read;
			message[1] = msg_temp->len;

			set_periplex_configuration(periplex_id, 1, 1);
			set_periplex_data(periplex_id, 2, message);
			kfree(message);
			message = NULL;

			while (index < msg_temp->len)
			{
				/* Reset wait flag before waiting */
				wait_queue_flag_com_i2c = 0;
				wait_event_interruptible(wait_queue_i2c_ioctl, wait_queue_flag_com_i2c != 0);

				if (!read_data_i2c)
				{
					pr_err("read_data_i2c is NULL\n");
					return -EFAULT;
				}

				/* Calculate remaining bytes and copy size */
				remaining = msg_temp->len - index;
				copy_size = min(remaining, data_from_usr);

				memcpy(msg_temp->buf + index, read_data_i2c, copy_size);
				index += copy_size;

				I2C_DEBUG("Read progress: copied %d bytes, %d remaining\n",
						  copy_size, msg_temp->len - index);

				kfree(read_data_i2c);
				read_data_i2c = NULL;
			}
		}
		else
		{
			I2C_DEBUG("write call\n");
			message = kmalloc(msg_temp->len + 2, GFP_KERNEL);
			if (!message)
			{
				pr_err("Failed to allocate memory for write message\n");
				return -ENOMEM;
			}

			message[0] = msg_temp->addr << 1 | is_read;
			message[1] = msg_temp->len;

			/* Copy data to message buffer */
			for (j = 0; j < msg_temp->len; j++)
			{
				I2C_DEBUG("[0x%02x] \n", msg_temp->buf[j]);
				message[j + 2] = msg_temp->buf[j];
			}
			set_periplex_configuration(periplex_id, 1, 1);
			set_periplex_data(periplex_id, msg_temp->len + 2, message);
			kfree(message);
			message = NULL;
		}
	}
	return num; // Return number of messages processed successfully
}

/*
** This function will be called whenever you call SMBUS read, wirte APIs
*/
s32 i2c_smbus_xfer(struct i2c_adapter *adap,
				   u16 addr,
				   unsigned short flags,
				   char read_write,
				   u8 command,
				   int size,
				   union i2c_smbus_data *data)
{
	int *p_id;
	int periplex_id;
	char *message = NULL;
	int ret = 0;

	if (!adap || !adap->dev.driver_data)
	{
		pr_err("Invalid adapter or driver data\n");
		return -EINVAL;
	}

	p_id = adap->dev.driver_data;
	periplex_id = *p_id;

	I2C_DEBUG("SMBUS XFER: addr=0x%x, rw=%d, command=0x%x, size=%d\n",
			  addr, read_write, command, size);

	switch (size)
	{
	case I2C_SMBUS_QUICK:
		message = kmalloc(2, GFP_KERNEL);
		if (!message)
			return -ENOMEM;

		// Force RW bit to 1 (read mode) regardless of read_write parameter
		message[0] = (addr << 1) | 1; // Always set LSB to 1 for read
		message[1] = 1 << 7;		  // Quick command flag

		I2C_DEBUG("Quick command: addr=0x%02x, forcing rw=1\n", addr);

		set_periplex_configuration(periplex_id, 1, 1);
		set_periplex_data(periplex_id, 2, message);
		kfree(message);

		// Since we're forcing read mode, we need to handle the read response
		wait_queue_flag_com_i2c = 0;
		wait_event_interruptible(wait_queue_i2c_ioctl, wait_queue_flag_com_i2c != 0);

		if (!read_data_i2c)
			return -EFAULT;

		// Check for device presence
		if (read_data_i2c[0] == 255)
		{
			I2C_DEBUG("No device present at addr 0x%02x\n", addr);
			ret = -ENXIO;
		}
		else
		{
			I2C_DEBUG("Device present at addr 0x%02x\n", addr);
		}
		kfree(read_data_i2c);
		break;

	case I2C_SMBUS_BYTE:
		message = kmalloc(2, GFP_KERNEL);
		if (!message)
			return -ENOMEM;

		message[0] = addr << 1 | (read_write & 1);
		message[1] = 1 << 7;

		set_periplex_configuration(periplex_id, 1, 1);
		set_periplex_data(periplex_id, 2, message);
		kfree(message);

		if (read_write == I2C_SMBUS_READ && data)
		{
			wait_queue_flag_com_i2c = 0;
			wait_event_interruptible(wait_queue_i2c_ioctl, wait_queue_flag_com_i2c != 0);

			I2C_DEBUG("wait for the SMBUS_slow byte\n");

			if (!read_data_i2c)
				return -EFAULT;

			I2C_DEBUG("read_data_i2c[0] %d\n", read_data_i2c[0]);

			if (read_data_i2c[0] == 255)
			{
				I2C_DEBUG("No device present\n");
				ret = -ENXIO; // No such device or address
			}
			else
			{
				I2C_DEBUG("Device present\n");
			}
			kfree(read_data_i2c);
		}
		break;

	case I2C_SMBUS_BLOCK_DATA:
	case I2C_SMBUS_I2C_BLOCK_DATA:
		if (!data)
			return -EINVAL;

		if (read_write == I2C_SMBUS_WRITE)
		{
			message = kmalloc(data->block[0] + 3, GFP_KERNEL);
			if (!message)
				return -ENOMEM;

			message[0] = addr << 1;			 // Slave address + Write bit
			message[1] = data->block[0] + 1; // Length byte
			message[2] = command;			 // Command/Register address
			memcpy(message + 3, &data->block[1], data->block[0]);

			I2C_DEBUG("%s Block Write: addr=0x%02x, cmd=0x%02x, len=%d\n",
					  size == I2C_SMBUS_BLOCK_DATA ? "SMBus" : "I2C",
					  addr, command, data->block[0]);

			set_periplex_configuration(periplex_id, 1, 1);
			set_periplex_data(periplex_id, data->block[0] + 3, message);
			kfree(message);
		}
		else
		{
			int index = 0;
			int remaining;

			message = kmalloc(5, GFP_KERNEL);
			if (!message)
				return -ENOMEM;

			message[0] = addr << 1 | 0;
			message[1] = 1;
			message[2] = command;
			message[3] = addr << 1 | 1;
			message[4] = (size == I2C_SMBUS_BLOCK_DATA) ? I2C_SMBUS_BLOCK_MAX : data->block[0];

			set_periplex_configuration(periplex_id, 1, 1);
			set_periplex_data(periplex_id, 5, message);
			kfree(message);

			do
			{
				/* Reset wait flag before waiting */
				wait_queue_flag_com_i2c = 0;
				wait_event_interruptible(wait_queue_i2c_ioctl, wait_queue_flag_com_i2c != 0);

				if (!read_data_i2c)
					return -EFAULT;

				remaining = data->block[0] - index;
				if (remaining <= 0)
					break;

				memcpy(data->block + index + 1, read_data_i2c,
					   min(remaining, data_from_usr));
				index += data_from_usr;

				I2C_DEBUG("Block read: copied %d bytes, %d remaining\n",
						  data_from_usr, remaining - data_from_usr);

				kfree(read_data_i2c);
				read_data_i2c = NULL;

			} while (remaining > 0);
		}
		break;

	default:
		return -EOPNOTSUPP;
	}

	return ret;
}

/*
** I2C algorithm Structure
*/
static struct i2c_algorithm i2c_algorithm_1f = {
	.smbus_xfer = i2c_smbus_xfer,
	.master_xfer = i2c_xfer,
	.functionality = i2c_func,
};

/*
** probe functions for device registration
*/
static int periplex_i2c_probe(struct periplex_device *pdev)
{
	int ret;
	int divider;
	int *periplex_id = kmalloc(sizeof(sizeof(int)), GFP_KERNEL);
	struct i2c_adapter *i2c_adapter;
	struct i2c_timings i2c_timings;

	/* 	initialize i2c internal wait queue */
	init_waitqueue_head(&wait_queue_i2c_ioctl);

	i2c_adapter = kzalloc(sizeof(struct i2c_adapter), GFP_KERNEL);
	if (!i2c_adapter)
		return -ENOMEM;

	if (device_property_read_u32(&pdev->dev, "periplex-id", periplex_id))
	{
		dev_err(&pdev->dev, "Failed to read periplex-id from device tree for i2c\n");
	}

	i2c_adapter->owner = THIS_MODULE;
	i2c_adapter->class = I2C_CLASS_HWMON;
	i2c_adapter->algo = &i2c_algorithm_1f;
	memcpy(i2c_adapter->name, "I2C", 3);
	i2c_adapter->nr = -1;
	i2c_adapter->dev.driver_data = periplex_id;
	i2c_adapter->dev.parent = &pdev->dev;
	i2c_adapter->dev.of_node = pdev->dev.of_node;

	pdev->periplex_id = *periplex_id;
	pdev->get_periplex_data = read_data_for_i2c;

	/* This is mandatory part to register device with periplex */
	periplex_link_device(pdev);
	periplex_set_drvdata(pdev, i2c_adapter);

	/* this section for frequency */
	i2c_parse_fw_timings(&i2c_adapter->dev, &i2c_timings, true);
	pr_info("I2C Bus Frequency: %u Hz\n", i2c_timings.bus_freq_hz);
	divider = (((FCLK) / (4 * i2c_timings.bus_freq_hz)) - 1);
	set_periplex_configuration(*periplex_id, 0, divider);

	ret = i2c_add_numbered_adapter(i2c_adapter);
	if (ret)
	{
		pr_err("Failed to add adapter %s\n", i2c_adapter->name);
		goto cleanup;
	}
	pr_info("i2c Bus Driver Added...%d\n", i2c_adapter->nr);

	return 0;

cleanup:
	i2c_del_adapter(i2c_adapter);
	return ret;
}

/*
** remove functions for device unregistration
*/
static int periplex_i2c_remove(struct periplex_device *pdev)
{
	struct i2c_adapter *i2c_adapter = periplex_get_drvdata(pdev);
	int *periplex_id = i2c_adapter->dev.driver_data;
	i2c_del_adapter(i2c_adapter);
	periplex_unlink_device(pdev);
	kfree(i2c_adapter);
	kfree(periplex_id);
	pr_info("i2c Bus Driver removed...\n");
	return 0;
}

/*
** compitable property match with DTSO of i2c
*/
struct of_device_id periplex_i2c_dt_match[] = {
	{.compatible = "vicharak,periplex-i2c"},
	{},
};
MODULE_DEVICE_TABLE(of, periplex_i2c_dt_match);

struct periplex_driver periplex_i2c_driver = {
	.probe = periplex_i2c_probe,
	.remove = periplex_i2c_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = periplex_i2c_dt_match,
	},
};
module_periplex_driver(periplex_i2c_driver);

MODULE_ALIAS("periplex:i2c");
MODULE_AUTHOR("vatsal Kevadiya<vhkevadiya15@gmail.com>");
MODULE_DESCRIPTION("I2C Device Driver with read/write operations");
MODULE_LICENSE("GPL");