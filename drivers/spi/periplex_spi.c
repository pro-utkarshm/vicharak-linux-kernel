/*
** Significant of spi.c file :
** 1.Make multiple spi chip with the use of dtso and create the
** spi* series into the /sys/class/spi_master.
** 2.Set SPI max-frequency and chip-select number to attach flash(for store data).
*/

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>

/*
** header file through which device can communicate and generated
*/
#include "include/peripheral.h"

#define DRIVER_NAME "periplex-spi"
#define MAX_FPGA_FREQ 100000000
#define MAX_CHUNK_SIZE 64

struct mutex spi_mutex;

/*
** Debug flag (can be set via module parameter)
*/
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable or disable debug mode");

/* Macro to conditionally print debug info */
#define SPI_DEBUG(fmt, ...)              \
    do                                   \
    {                                    \
        if (debug)                       \
            pr_info(fmt, ##__VA_ARGS__); \
    } while (0)

wait_queue_head_t spi_read_wait;
int spi_read_periplex_data_flag;

wait_queue_head_t spi_read_ack_wait;
int spi_read_ack_periplex_data_flag;

char *read_data_spi = NULL;
int read_length_spi = 0;

/*
** Struct periplex_spi_data - Runtime info holder for SPI driver
*/
struct periplex_spi_data
{
    struct spi_controller *controller;
    u32 periplex_id;
};

/*
** Get_clk_per_half_bit - use for calculation of clk
*/
static uint8_t get_clk_per_half_bit(u32 sclk_freq)
{
    return MAX_FPGA_FREQ / (2 * sclk_freq);
}

/*
** Periplex_spi_set_cs - handle to send the chip-select frame
*/
static void periplex_spi_set_cs(struct spi_device *spi, bool enable)
{
    struct spi_controller *ctlr = spi->controller;
    int config = 0;
    int periplex_id = 0;
    SPI_DEBUG("chip select set\n");
    config = (spi->chip_select << 8) + enable;
    periplex_id = *(int *)(ctlr->dev.driver_data);
    set_periplex_configuration(periplex_id, 1, config);
    // msleep(150);
}

/* 
** function use in read data for spi
*/
int read_data_for_spi(struct periplex_device *peri_dev, char *message, const int length)
{
    int ret = 0;

    if (!message || length <= 0)
    {
        pr_err("Invalid message or length\n");
        return -EINVAL;
    }

    mutex_lock(&spi_mutex);

    read_data_spi = kmalloc(length, GFP_KERNEL);
    if (!read_data_spi)
    {
        pr_err("Failed to allocate memory for read_data_spi\n");
        return -ENOMEM;
    }

    read_length_spi = length;

    if (memcpy(read_data_spi, message, length) == NULL)
    {
        pr_err("Unable to copy data to read_data_spi\n");
        kfree(read_data_spi);
        mutex_unlock(&spi_mutex);
        return -EFAULT;
    }

    spi_read_periplex_data_flag = 1;
    wake_up_interruptible(&spi_read_wait);

    // Wait for acknowledgment with timeout
    spi_read_ack_periplex_data_flag = 0;
    ret = wait_event_interruptible_timeout(spi_read_ack_wait,
                                           spi_read_ack_periplex_data_flag != 0,
                                           msecs_to_jiffies(5000)); // 5 second timeout

    if (ret == 0)
    {
        // Timeout occurred
        pr_err("Timed out waiting for acknowledgment\n");
        kfree(read_data_spi);
        mutex_unlock(&spi_mutex);
        return -ETIMEDOUT;
    }
    else if (ret < 0)
    {
        // Wait was interrupted by a signal
        pr_err("Wait interrupted with error: %d\n", ret);
        kfree(read_data_spi);
        mutex_unlock(&spi_mutex);
        return -ERESTARTSYS;
    }

    SPI_DEBUG("Acknowledgment received successfully\n");
    mutex_unlock(&spi_mutex);

    msleep(1);
    return 0;
}

/*
**periplex_spi_setup - callback to setup the controller
*/
static int periplex_spi_setup(struct spi_device *spi)
{
    /*       SPI CONFIG frame
          ┌─────────┬─────────────────┬────────┬───────┬────┬────┐
          │config   │       res       │clk per │ res   │cpol│cpha│
          │         │                 │half bit│       │    │    │
          └─────────┴─────────────────┴────────┴───────┴────┴────┘
          <--------> <---------------> <------> <-----> <--> <-->
            8 bits       16 bits        8 bits   6 bits   1    1
    */

    struct spi_controller *ctlr = spi->controller;
    uint8_t clk_per_half_bit = 0;
    int periplex_id = 0;
    int config = 0;

    periplex_id = *(int *)(ctlr->dev.driver_data);
    SPI_DEBUG("max-speed %d\n", spi->max_speed_hz);
    //  TODO: check if the device freq is greater than master freq or not
    clk_per_half_bit = get_clk_per_half_bit(spi->max_speed_hz);
    config = (clk_per_half_bit << 8) + (spi->mode);
    SPI_DEBUG("periplex-id: %d\n", periplex_id);
    set_periplex_configuration(periplex_id, 0, config);
    // msleep(50);
    SPI_DEBUG("configuration set\n");
    return 0;
}

static void transfer_buf(struct spi_controller *ctlr, const void *buf, int len, int offset, bool read_write)
{
    int periplex_id = *(int *)(ctlr->dev.driver_data);
    char *data;

    /* Allocate memory for this chunk */
    data = kmalloc(len + 1, GFP_KERNEL);
    if (!data)
    {
        pr_err("Failed to allocate memory for chunk\n");
        return;
    }

    data[0] = (read_write << 7) | len;

    /* Copy data for this chunk */
    if (memcpy(data + 1, buf + offset, len) == NULL)
    {
        pr_err("Not able to copy chunk data\n");
        kfree(data);
        return;
    }

    SPI_DEBUG("len is %d\n", len);
    if (len > 0)
    {
        set_periplex_data(periplex_id, len + 1, data);
    }
    kfree(data);
    // msleep(40);
}

/*
** periplex_spi_transfer_one_message - callback to transfer spi message
*/
static int periplex_spi_transfer_one_message(struct spi_controller *ctlr, struct spi_message *spi_msg)
{
    int ret = 0;
    struct spi_transfer *transfer;
    int rx_len = 0;
    int remaining_len = 0;
    int current_offset = 0;
    int chunk_size = 0;

    if (!ctlr || !spi_msg)
    {
        pr_err("Invalid controller or message\n");
        return -EINVAL;
    }

    periplex_spi_set_cs(spi_msg->spi, 0);

    list_for_each_entry(transfer, &spi_msg->transfers, transfer_list)
    {

        /* Initialize variables for chunked transfer */
        remaining_len = transfer->len;
        current_offset = 0;

        if (transfer->tx_buf && transfer->len != 0)
        {
            SPI_DEBUG("WRITE:Transfer Length: %d\n", transfer->len);

            /* Process data in chunks */
            while (remaining_len > 0)
            {
                chunk_size = min((int)transfer->len - current_offset, MAX_CHUNK_SIZE);
                /* Transfer current chunk */
                transfer_buf(ctlr, transfer->tx_buf, chunk_size, current_offset, 0);

                current_offset += chunk_size;
                remaining_len -= chunk_size;
            }
            current_offset = 0;
        }

        if (transfer->rx_buf)
        {
            SPI_DEBUG("READ:Transfer Length: %d\n", transfer->len);

            while (remaining_len > 0)
            {
                chunk_size = min((int)transfer->len - current_offset, MAX_CHUNK_SIZE);
                /* Transfer current chunk */
                transfer_buf(ctlr, transfer->rx_buf, chunk_size, current_offset, 1);

                while (rx_len < chunk_size)
                {
                    spi_read_periplex_data_flag = 0;

                    // Use timeout to avoid indefinite waiting
                    ret = wait_event_interruptible_timeout(
                        spi_read_wait,
                        spi_read_periplex_data_flag != 0,
                        msecs_to_jiffies(5000)); // 5 second timeout

                    if (ret == 0)
                    {
                        // Timeout occurred
                        pr_err("SPI: Timed out waiting for read data \n");
                        ret = -ETIMEDOUT;
                        goto exit_with_error;
                    }
                    else if (ret < 0)
                    {
                        // Wait was interrupted
                        pr_err("RX wait interrupted: %d\n", ret);
                        goto exit_with_error;
                    }

                    SPI_DEBUG("Data received, processing...\n");

                    // Safely copy data
                    if (transfer->rx_buf + rx_len + read_length_spi <=
                        transfer->rx_buf + transfer->len)
                    {
                        memcpy(transfer->rx_buf + rx_len, read_data_spi, read_length_spi);
                    }
                    else
                    {
                        pr_err("Buffer overflow prevented\n");
                        ret = -EOVERFLOW;
                        goto exit_with_error;
                    }

                    /* Update counters */
                    rx_len += read_length_spi;
                    current_offset += read_length_spi;
                    SPI_DEBUG("rx_len is %d\n", rx_len);
                    SPI_DEBUG("chunk_size is %d\n", chunk_size);
                    SPI_DEBUG("remaining length is %d", remaining_len);
                    read_length_spi = 0;
                    kfree(read_data_spi);
                    read_data_spi = NULL;

                    spi_read_ack_periplex_data_flag = 1;
                    wake_up_interruptible(&spi_read_ack_wait);
                }
                remaining_len -= chunk_size;
                rx_len = 0;
                /* Copy received data */
            }
            current_offset = 0;
            SPI_DEBUG("Reading complete\n");
        }

        spi_msg->actual_length += transfer->len;
    }

    periplex_spi_set_cs(spi_msg->spi, 1);
    spi_msg->status = 0;
    spi_finalize_current_message(ctlr);
    return 0;

exit_with_error:
    periplex_spi_set_cs(spi_msg->spi, 1);
    spi_msg->status = ret;
    spi_finalize_current_message(ctlr);
    return ret;
}

/*
** Called when a matched spi controller device is found
*/
static int probe_periplex_spi_controller(struct periplex_device *pdev)
{
    struct spi_controller *ctlr;
    int *periplex_id = kmalloc(sizeof(int), GFP_KERNEL);
    int ret;
    u32 num_cs, max_freq;

    init_waitqueue_head(&spi_read_ack_wait);
    init_waitqueue_head(&spi_read_wait);

    mutex_init(&spi_mutex);

    ctlr = spi_alloc_master(&pdev->dev, sizeof(struct periplex_spi_data));
    if (!ctlr)
    {
        dev_err(&pdev->dev, "Failed to allocate SPI master\n");
        kfree(periplex_id);
        return -ENOMEM;
    }

    // Initialize controller
    ctlr->mode_bits = SPI_CPOL | SPI_CPHA;
    ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
    ctlr->bus_num = -1;
    ret = device_property_read_u32(&pdev->dev, "periplex-id", periplex_id);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to read periplex-id from device tree for spi\n");
        kfree(periplex_id);
        return ret;
    }

    if (device_property_read_u32(&pdev->dev, "vicharak,peri-spi-cs-num", &num_cs))
    {
        num_cs = 1;
    }

    if (device_property_read_u32(&pdev->dev, "vicharak,peri-spi-max-freq", &max_freq))
    {
        max_freq = 25000000;
    }

    ctlr->dev.driver_data = periplex_id;
    ctlr->num_chipselect = num_cs;
    ctlr->max_speed_hz = max_freq;

    ctlr->transfer_one_message = periplex_spi_transfer_one_message;
    ctlr->set_cs = periplex_spi_set_cs;
    ctlr->setup = periplex_spi_setup;
    ctlr->dev.parent = &pdev->dev;
    ctlr->dev.of_node = pdev->dev.of_node;

    pdev->periplex_id = *periplex_id;
    pdev->get_periplex_data = read_data_for_spi;

    periplex_link_device(pdev);
    periplex_set_drvdata(pdev, ctlr);

    ret = spi_register_controller(ctlr);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to register SPI master\n");
        spi_controller_put(ctlr);
        return ret;
    }
    pr_info("Periplex SPI controller registered %d\n", *periplex_id);
    return 0;
}

/*
** called when device is removed from the system
*/
static int remove_periplex_spi_controller(struct periplex_device *pdev)
{
    struct spi_controller *ctlr = periplex_get_drvdata(pdev);
    int *periplex_id = (int *)ctlr->dev.driver_data;
    spi_unregister_controller(ctlr);
    kfree(periplex_id);

    dev_info(&pdev->dev, "spi controller driver removed\n");

    return 0;
}

struct of_device_id periplex_spi_dt_match[] = {
    {.compatible = "vicharak,periplex-spi"},
    {},
};
MODULE_DEVICE_TABLE(of, periplex_spi_dt_match);

struct periplex_driver periplex_spi_driver = {
    .probe = probe_periplex_spi_controller,
    .remove = remove_periplex_spi_controller,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = periplex_spi_dt_match}};

module_periplex_driver(periplex_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("shailparmar03");
MODULE_DESCRIPTION("SPI Controller Driver");