/*
** Significance of the onewire.c file:
** 1. The periplex_onewire_search() function discovers all connected OneWire
**    slave devices and lists them under /sys/bus/w1/devices with their
**    unique addressable IDs.
** 2. Parameters of detected slave devices can be accessed, and the underlying
**    operations for reading or writing data are handled by the read_block(),
**    write_block(), and write_byte() functions.
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/moduleparam.h>
#include <linux/w1.h>
#include <linux/types.h>

/*
** header file through which device can communicate and generated
*/
#include <linux/peripheral.h>
// #include "include/peripheral.h"

#define DRIVER_NAME "periplex-onewire"

/* Debug flag (can be set via module parameter) */
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable or disable debug mode");

/* Macro to conditionally print debug info */
#define OW_DEBUG(fmt, ...)               \
    do                                   \
    {                                    \
        if (debug)                       \
            pr_info(fmt, ##__VA_ARGS__); \
    } while (0)

/* One-wire parameters */
u8 OW_RESET_PULSE = 1;
u8 OW_WRITE_PULSE = 2;
u8 OW_READ_PULSE = 3;
u8 OW_SEARCH_PULSE = 4;

/*
** waitqueue used for internal ow operations
*/
wait_queue_head_t wait_queue_ow_ioctl_search_data;
int wait_queue_flag_com_ow_search_data;

wait_queue_head_t wait_queue_ow_ack_ioctl_search_data;
int wait_queue_flag_com_ow_ack_search_data;

wait_queue_head_t wait_queue_ow_ioctl_read_data;
int wait_queue_flag_com_ow_read_data;

wait_queue_head_t wait_queue_ow_ack_ioctl_read_data;
int wait_queue_flag_com_ow_ack_read_data;

struct periplex_onewire
{
    struct w1_bus_master bus_master;
    struct periplex_device *pdev;
    char dev_id[64];
};

char *read_data_onewire = NULL;
int read_length_onewire = 0;

/*
** function use in read data for spi
*/
int read_data_for_onewire(struct periplex_device *peri_dev, char *message,
                          const int length)
{
    long timeout = msecs_to_jiffies(3000);
    int ret = 0;
    read_length_onewire = length;
    OW_DEBUG("reading for onewire..... ,length %d\n", length);

    read_data_onewire = kmalloc(length, GFP_KERNEL);
    if (!read_data_onewire)
    {
        pr_err("Failed to allocate memory for read_data_onewire\n");
        return -ENOMEM;
    }

    if (memcpy(read_data_onewire, message, length) == NULL)
    {
        pr_err("unable to copy data to read_data_onewire\n");
        kfree(read_data_onewire);
        return -EINVAL;
    }

    if (read_data_onewire[0] != 0)
    {
        // handle search part here
        wait_queue_flag_com_ow_search_data = 1;
        wake_up_interruptible(&wait_queue_ow_ioctl_search_data);

        wait_queue_flag_com_ow_ack_search_data = 0;
        ret = wait_event_interruptible_timeout(wait_queue_ow_ack_ioctl_search_data,
                                               wait_queue_flag_com_ow_ack_search_data != 0,
                                               timeout);

        if (ret < 0)
        {
            pr_err("one-wire:Wait interrupted with error: %d\n", ret);
            return ret;
        }
    }
    else
    {
        // handle read part here
        wait_queue_flag_com_ow_read_data = 1;
        wake_up_interruptible(&wait_queue_ow_ioctl_read_data);

        wait_queue_flag_com_ow_ack_read_data = 0;
        ret = wait_event_interruptible_timeout(wait_queue_ow_ack_ioctl_read_data,
                                               wait_queue_flag_com_ow_ack_read_data != 0,
                                               timeout);

        if (ret < 0)
        {
            pr_err("one-wire:Wait interrupted with error: %d\n", ret);
            return ret;
        }
    }

    return 0;
}

/*
** convert u8-array to u64
*/
static inline u64 convert_u8_array_to_u64_le(u8 buf[8])
{
    u64 result = 0;

    /* Combine bytes with bitwise operations (little-endian format) */
    result = ((u64)buf[7] << 56) |
             ((u64)buf[6] << 48) |
             ((u64)buf[5] << 40) |
             ((u64)buf[4] << 32) |
             ((u64)buf[3] << 24) |
             ((u64)buf[2] << 16) |
             ((u64)buf[1] << 8) |
             ((u64)buf[0]);

    return result;
}

/*
** Touch_bit function
*/
static u8 periplex_onewire_touch_bit(void *data, u8 bit)
{
    OW_DEBUG("touch-bit one-wire\n");
    return bit;
}

/*
** Reset the one-wire bus
*/
static u8 periplex_onewire_reset_bus(void *data)
{
    struct periplex_onewire *dev = data;
    u8 message[1] = {0};
    message[0] = OW_RESET_PULSE;
    set_periplex_data(dev->pdev->periplex_id, 1, (char *)message);
    OW_DEBUG("Resetting one-wire bus\n");
    return 0;
}

/*
** periple_onewire write block
*/
static void periplex_onewire_write_block(void *data, const u8 *buf, int len)
{
    struct periplex_onewire *dev = data;
    int i;
    u8 *message;
    u8 len_u8 = 0;
    OW_DEBUG("write one-wire\n");
    OW_DEBUG("length: %d\n", len);

    message = kmalloc(len + 1, GFP_KERNEL);
    if (!message)
    {
        OW_DEBUG("Failed to allocate memory\n");
        return;
    }

    /* Initialize the buffer to zero */
    memset(message, 0, len);
    len_u8 = (u8)len << 4;
    message[0] = ((len_u8 - 1) & 0xF0) | OW_WRITE_PULSE;

    // Print buffer contents
    if (buf && len > 0)
    {
        OW_DEBUG("buffer contents: ");
        for (i = 0; i < len; i++)
        {
            message[i + 1] = buf[i];
            OW_DEBUG("0x%02x ", buf[i]);
        }
        OW_DEBUG("\n");
    }
    set_periplex_data(dev->pdev->periplex_id, len + 1, (char *)message);
    kfree(message);
    return;
}

/*
** periplex_onewire read block
*/
static u8 periplex_onewire_read_block(void *data, u8 *buf, int len)
{
    int ret = 0;
    struct periplex_onewire *dev = data;
    u8 message[1] = {0};
    int i = 0;
    long timeout = msecs_to_jiffies(3000);
    int count = len;
    OW_DEBUG("read one-wire, len is %d\n", len);

    // wait for the actual read data
    message[0] = (((len - 1) << 4) | OW_READ_PULSE) & 0xFF;
    set_periplex_data(dev->pdev->periplex_id, 1, (char *)message);

    while (count > 0)
    {
        wait_queue_flag_com_ow_read_data = 0;
        ret = wait_event_interruptible_timeout(wait_queue_ow_ioctl_read_data,
                                               wait_queue_flag_com_ow_read_data != 0,
                                               timeout);

        if (ret == 0)
        {
            // Timeout occurred, handle accordingly
            if (read_data_onewire != NULL)
            {
                kfree(read_data_onewire);
                read_data_onewire = NULL;
            }
            pr_err("Timeout waiting for onewire read-data\n");
            return len;
        }
        else if (ret < 0)
        {
            if (read_data_onewire != NULL)
            {
                kfree(read_data_onewire);
                read_data_onewire = NULL;
            }
            pr_err("Wait interrupted for onewire read-data: %d\n", ret);
            return len;
        }

        if (buf && read_data_onewire)
        {
            buf[i] = read_data_onewire[1];
            OW_DEBUG("Read byte[%d]: 0x%02x\n", i, buf[i]);
            i++;
        }

        count--;
        kfree(read_data_onewire);
        read_data_onewire = NULL;

        wait_queue_flag_com_ow_ack_read_data = 1;
        wake_up_interruptible(&wait_queue_ow_ack_ioctl_read_data);
    }

    return len;
}

/*
** periplex_onewire write byte
*/
static void periplex_onewire_write_byte(void *data, u8 buf)
{
    struct periplex_onewire *dev = data;
    int write_length = 0;
    u8 message[2] = {0};
    message[0] = ((write_length << 4) | OW_WRITE_PULSE) & 0xFF;
    message[1] = buf & 0xFF;
    set_periplex_data(dev->pdev->periplex_id, 2, (char *)message);
    OW_DEBUG("periplex_onewire_write_byte : 0x%02x\n", buf);
    return;
}

/*
** periplex_onewire read byte
*/
static u8 periplex_onewire_read_byte(void *data)
{
    OW_DEBUG("periplex_onewire_read_byte\n");
    return 0;
}

/*
** periplex_onewire search
*/
static void periplex_onewire_search(void *data, struct w1_master *master,
                                    u8 search_type, w1_slave_found_callback callback)
{
    struct periplex_onewire *dev = data;
    int ret = 0;
    int count = 0;
    u8 message[4] = {0};
    u8 buf[8] = {0};
    u64 rom_code = 0;
    int index = 0;
    int write_length = 0;
    int device_count = 0;
    long timeout = msecs_to_jiffies(3000);
    message[0] = OW_RESET_PULSE;
    message[1] = ((write_length << 4) | OW_WRITE_PULSE) & 0xFF;
    message[2] = (search_type) & 0xFF;
    message[3] = (OW_SEARCH_PULSE) & 0xFF;

    set_periplex_data(dev->pdev->periplex_id, 4, (char *)message);
    OW_DEBUG("Searching one-wire bus, search_type=%02x\n", search_type);

    while (1)
    {
        wait_queue_flag_com_ow_search_data = 0;
        ret = wait_event_interruptible_timeout(wait_queue_ow_ioctl_search_data,
                                               wait_queue_flag_com_ow_search_data != 0,
                                               timeout);

        if (ret == 0)
        {
            // Timeout occurred, handle accordingly
            if (read_data_onewire != NULL)
            {
                kfree(read_data_onewire);
                read_data_onewire = NULL;
            }
            pr_err("Wait timed out for onewire search-data\n");
            return;
        }
        else if (ret < 0)
        {
            if (read_data_onewire != NULL)
            {
                kfree(read_data_onewire);
                read_data_onewire = NULL;
            }
            pr_err("Wait timed out for onewire search-data\n");
            return;
        }

        count++;
        index = read_data_onewire[0] & 0x0F;
        buf[index - 1] = read_data_onewire[1];

        if ((count % 8) == 0)
        {
            pr_info("inside if: count is %d\n", count);
            rom_code = convert_u8_array_to_u64_le(buf);
            pr_info("Found 1-Wire device with ROM ID: 0x%016llx\n", rom_code);

            // Only call the callback if the ROM code is valid (non-zero)
            if (rom_code != 0)
            {
                // Make sure the callback function exists
                if (callback)
                {
                    OW_DEBUG("Calling callback for ROM ID: 0x%016llx\n", rom_code);
                    callback(master, rom_code);
                    device_count++;
                }
                else
                {
                    pr_err("Callback function is NULL\n");
                }
            }
            else
            {
                pr_warn("Found zero ROM ID, ignoring device\n");
            }
            memset(buf, 0, sizeof(buf));
            pr_info("1-Wire search completed. Found %d devices\n", device_count);
        }

        pr_info("count is %d\n", count);
        kfree(read_data_onewire);
        read_data_onewire = NULL;

        wait_queue_flag_com_ow_ack_search_data = 1;
        wake_up_interruptible(&wait_queue_ow_ack_ioctl_search_data);
    }

    return;
}

static u8 periplex_onewire_triplet(void *data, u8 bit)
{

    OW_DEBUG("periplex_onewire_triplet %d\n", bit);
    return bit;
}

/*
** Probe function for device registration
*/
static int periplex_onewire_probe(struct periplex_device *pdev)
{
    struct periplex_onewire *ow_dev;
    int periplex_id;
    int ret;

    /* 	initialize ow internal wait queue */
    init_waitqueue_head(&wait_queue_ow_ioctl_search_data);
    init_waitqueue_head(&wait_queue_ow_ack_ioctl_search_data);
    init_waitqueue_head(&wait_queue_ow_ioctl_read_data);
    init_waitqueue_head(&wait_queue_ow_ack_ioctl_read_data);

    ow_dev = kzalloc(sizeof(struct periplex_onewire), GFP_KERNEL);
    if (!ow_dev)
        return -ENOMEM;

    if (device_property_read_u32(&pdev->dev, "periplex-id", &periplex_id))
    {
        dev_err(&pdev->dev, "Failed to read periplex-id from device tree for one-wire\n");
        ret = -EINVAL;
        goto cleanup_alloc;
    }

    /* Initialize one-wire bus master */
    ow_dev->bus_master.data = ow_dev;
    ow_dev->bus_master.touch_bit = periplex_onewire_touch_bit;
    ow_dev->bus_master.reset_bus = periplex_onewire_reset_bus;
    ow_dev->bus_master.write_block = periplex_onewire_write_block;
    ow_dev->bus_master.read_block = periplex_onewire_read_block;
    ow_dev->bus_master.write_byte = periplex_onewire_write_byte;
    ow_dev->bus_master.read_byte = periplex_onewire_read_byte;
    ow_dev->bus_master.search = periplex_onewire_search;
    ow_dev->bus_master.triplet = periplex_onewire_triplet;
    ow_dev->pdev = pdev;

    /* Set device ID */
    snprintf(ow_dev->dev_id, sizeof(ow_dev->dev_id), "owewire-%d", periplex_id);
    ow_dev->bus_master.dev_id = ow_dev->dev_id;

    /* Link with periplex subsystem */
    pdev->periplex_id = periplex_id;
    pdev->get_periplex_data = read_data_for_onewire;

    periplex_link_device(pdev);
    periplex_set_drvdata(pdev, ow_dev);

    /* Register with w1 subsystem */
    ret = w1_add_master_device(&ow_dev->bus_master);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to register w1 master device\n");
        goto cleanup_link;
    }

    pr_info("owewire successfully initialized\n");
    return 0;

cleanup_link:
    periplex_unlink_device(pdev);
cleanup_alloc:
    kfree(ow_dev);
    return ret;
}

/*
** Remove function for device unregistration
*/
static int periplex_onewire_remove(struct periplex_device *pdev)
{
    struct periplex_onewire *ow_dev = periplex_get_drvdata(pdev);

    w1_remove_master_device(&ow_dev->bus_master);
    periplex_unlink_device(pdev);
    kfree(ow_dev);

    pr_info("onewire device removed successfully\n");
    return 0;
}

/*
** Compatible property match with DTSO of one-wire
*/
static struct of_device_id periplex_onewire_dt_match[] = {
    {.compatible = "vicharak,periplex-onewire"},
    {},
};
MODULE_DEVICE_TABLE(of, periplex_onewire_dt_match);

static struct periplex_driver periplex_onewire_driver = {
    .probe = periplex_onewire_probe,
    .remove = periplex_onewire_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = periplex_onewire_dt_match,
    },
};
module_periplex_driver(periplex_onewire_driver);

MODULE_ALIAS("periplex:onewire");
MODULE_AUTHOR("vatsal Kevadiya<vhkevadiya15@gmail.com>");
MODULE_DESCRIPTION("One-Wire Device Driver with read/write operations");
MODULE_LICENSE("GPL");