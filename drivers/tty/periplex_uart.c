/*
** Significant of uart.c file :
** 1.Make multiple uart devices with the use of dtso and create the 
** ttyPERI* series into the /dev
** 2.Allow Write/read data in any specific uart(ttyPERI*) device
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/moduleparam.h> 
#include <linux/peripheral.h>

#include <asm/uaccess.h>
#include <asm/errno.h>

/* 
** header file through which device can communicate and generated 
*/
// #include "include/peripheral.h"

#define DRIVER_NAME "periplex-uart"

/* Debug flag (can be set via module parameter) */
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable or disable debug mode");

/* Macro to conditionally print debug info */
#define UART_DEBUG(fmt, ...) \
    do { if (debug) pr_info(fmt, ##__VA_ARGS__); } while (0)

/* 
** this functions is used in read for uart 
*/
int read_data_for_uart(struct periplex_device *dev, char *message, const int len)
{
    struct tty_driver *uart_driver = dev->data;
    int i;
    UART_DEBUG("uart read calling\n");
    UART_DEBUG("length is %d\n",len);
    for (i = 0; i < len; i++)
    {
        tty_insert_flip_char(uart_driver->ports[0], message[i], TTY_NORMAL);
        tty_flip_buffer_push(uart_driver->ports[0]);
    }
    return 0;
}

/* 
** this functions is used for getting baud_rate for uart device 
*/
int get_tty_baud_rate(struct tty_struct *tty)
{
    speed_t baud_rate = 0;
    if (tty && tty->termios.c_cflag & CBAUD)
    {
        baud_rate = tty_termios_baud_rate(&tty->termios);
    }
    return (int)baud_rate;
}

/* 
** uart open function 
*/
static int tty_open(struct tty_struct *tty, struct file *file)
{
    return 0;
}

/* 
** uart close function 
*/
static void tty_close(struct tty_struct *tty, struct file *file)
{
    return;
}

/* 
** uart write function 
*/
static int tty_fpga_write(struct tty_struct *tty, const unsigned char *buffer, 
                        int count)
{
    int periplex_id = tty->driver->name_base;
    set_periplex_data(periplex_id, count, (char *)buffer);
    return count;
}

/* 
** uart write-room function 
*/
static int tty_fpga_write_room(struct tty_struct *tty)
{
    return 1;
}

/* 
** uart set-termios function used for set the configurations 
*/
static void tty_fpga_set_termios(struct tty_struct *tty, struct ktermios *old)
{
    int periplex_id = tty->driver->name_base;
    int configuration = get_tty_baud_rate(tty);
    configuration = 50000000 / get_tty_baud_rate(tty);
    set_periplex_configuration(periplex_id, 0, configuration);
}

/* 
** initialize the opeations for uart 
*/
static const struct tty_operations serial_ops = {
    .open = tty_open,
    .write = tty_fpga_write,
    .write_room = tty_fpga_write_room,
    .set_termios = tty_fpga_set_termios,
    .close = tty_close,
};

/* 
** probe functions for device registration
*/
static int periplex_uart_probe(struct platform_device *pdev)
{
    int ret;
    int retval;
    int periplex_id;
    struct device *dev = &pdev->dev;
    struct device_node *np = dev->of_node;
    struct tty_driver *uart_driver;
    struct tty_port *uart_port;
    struct periplex_device *tty_d;

    tty_d = kzalloc(sizeof(struct periplex_device), GFP_KERNEL);
    if (!tty_d)
        return -ENOMEM;

    uart_port = kzalloc(sizeof(struct tty_port), GFP_KERNEL);
    if (!uart_port)
        return -ENOMEM;

    if (!np)
    {
        dev_err(dev, "No device tree node found\n");
        return -ENODEV;
    }
    retval = of_property_read_u32(np, "periplex-id", &periplex_id);
    if (retval)
    {
        dev_err(dev, "Failed to read periplex-id from device tree\n");
        return retval;
    }

    UART_DEBUG("periplex_id is %d", periplex_id);

    tty_port_init(uart_port);

    /* Allocate the uart driver */
    uart_driver = tty_alloc_driver(1, TTY_DRIVER_REAL_RAW);

    if (IS_ERR(uart_driver))
    {
        return PTR_ERR(uart_driver);
    }

    /* Initialize the uart driver */
    uart_driver->owner = THIS_MODULE;
    uart_driver->driver_name = "tty_periplex";
    uart_driver->name = "ttyPERI";
    uart_driver->name_base = periplex_id;
    uart_driver->major = 0;
    uart_driver->minor_start = 0;
    uart_driver->type = TTY_DRIVER_TYPE_SERIAL;
    uart_driver->subtype = SERIAL_TYPE_NORMAL;
    uart_driver->flags = TTY_DRIVER_REAL_RAW;
    uart_driver->init_termios = tty_std_termios;

    /* Assigning port to each multiple uart devices */
    tty_set_operations(uart_driver, &serial_ops);
    tty_port_link_device(uart_port, uart_driver, 0);

    /* Register the uart driver */
    ret = tty_register_driver(uart_driver);
    if (ret)
    {
        pr_info(KERN_ERR "failed to register tiny tty driver\n");
        return ret;
    }

    tty_d->pdev = pdev;
    tty_d->data = uart_driver;
    tty_d->get_periplex_data = read_data_for_uart;

    /* This is mandatory part to register device with periplex */
    platform_set_drvdata(pdev, tty_d);
    periplex_device_register(pdev, periplex_id);
    pr_info("ttyPERI are successfully inserted...\n");

    return ret;
}

/*
** remove functions for device unregistration
*/
static int periplex_uart_remove(struct platform_device *pdev)
{
    struct periplex_device *tty_d = platform_get_drvdata(pdev);
    struct tty_driver *uart_driver = tty_d->data;
    tty_unregister_driver(uart_driver);
    tty_driver_kref_put(uart_driver);
    tty_port_destroy(uart_driver->ports[0]);
    periplex_device_unregister(pdev);
    kfree(uart_driver->ports[0]);
    kfree(tty_d);
    pr_info("ttyPERI are removed successfully...\n");
    return 0;
}

/* 
** compitable property match with DTSO of uart 
*/
static struct of_device_id periplex_uart_dt_match[] = {
    {.compatible = "vicharak,periplex-uart"},
    {},
};
MODULE_DEVICE_TABLE(of, periplex_uart_dt_match);

static struct platform_driver periplex_uart_driver = {
    .probe = periplex_uart_probe,
    .remove = periplex_uart_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = periplex_uart_dt_match,
    },
};
module_platform_driver(periplex_uart_driver);

MODULE_ALIAS("platform:uart");
MODULE_AUTHOR("Vatsal Kevadiya<vhkevadiya15@gmail.com>");
MODULE_DESCRIPTION("UART Device Driver with read/write operations");
MODULE_LICENSE("GPL");