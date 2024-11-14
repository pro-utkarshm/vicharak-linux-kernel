#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/moduleparam.h>
#include <linux/peripheral.h>

/* 
** header file through which device can communicate and generated 
*/
// #include "include/peripheral.h"

#define DRIVER_NAME "periplex-spi"
#define MAX_FPGA_FREQ 100000000

/*
** Debug flag (can be set via module parameter)
*/
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable or disable debug mode");

#define SPI_DEBUG(fmt, ...)              \
    do                                   \
    {                                    \
        if (debug)                       \
            pr_info(fmt, ##__VA_ARGS__); \
    } while (0)

wait_queue_head_t spi_read_wait;
int spi_read_periplex_data_flag;

char read_data_spi[256];

/*
struct periplex_spi_data - Runtime info holder for SPI driver
*/
struct periplex_spi_data
{
    struct spi_controller *controller;
    u32 periplex_id;
};

static uint8_t get_clk_per_half_bit(u32 sclk_freq)
{
    return MAX_FPGA_FREQ / (2 * sclk_freq);
}

static void periplex_spi_set_cs(struct spi_device *spi, bool enable)
{
    struct spi_controller *ctlr = spi->controller;
    int config = 0;
    int periplex_id = *(int *)(ctlr->dev.driver_data);
    config = (spi->chip_select << 8) + enable;
    set_periplex_configuration(periplex_id, 1, config);
}

int read_data_for_spi(struct periplex_device *peri_dev, char *message, const int length)
{
    read_data_spi[0] = length;

    if (memcpy(read_data_spi + 1, message, length) == NULL)
    {
        pr_err("unable to copy data to read_data_spi\n");
        return -EINVAL;
    }

    spi_read_periplex_data_flag = 1;
    wake_up_interruptible(&spi_read_wait);
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
    int periplex_id = *(int *)(ctlr->dev.driver_data);
    uint8_t clk_per_half_bit;
    int config = 0;

    //  TODO: check if the device freq is greater than master freq or not
    clk_per_half_bit = get_clk_per_half_bit(spi->max_speed_hz);
    config = (clk_per_half_bit << 8) + (spi->mode);
    SPI_DEBUG("periplex-id: %d\n", periplex_id);
    set_periplex_configuration(periplex_id, 0, config);
    SPI_DEBUG("configuration set\n");

    return 0;
}

static void transfer_buf(struct spi_controller *ctlr, const void *buf, int len)
{
    /*   SPI DATA frame
         ┬─────────┬─────────┬─────────┬─────────┐
         │Length N │Data 0   │------   │Data N-1 │
         │         │         │         │         │
         ┴─────────┴─────────┴─────────┴─────────┘
         <-------> <--------------------------->
          8 bits            N*8  bits
    */

    int periplex_id = *(int *)(ctlr->dev.driver_data);
    int i = 0;
    char data[256];

    memcpy(data, buf, len);

    for (i = 0; i < len; i++)
        SPI_DEBUG("%02x\n", *(char *)(buf + i));
    SPI_DEBUG("transfering buffer\n");
    set_periplex_data(periplex_id, len, data);
}

/*
** periplex_spi_transfer_one_message - callback to transfer spi message
*/
static int periplex_spi_transfer_one_message(struct spi_controller *ctlr, struct spi_message *spi_msg)
{
    struct spi_transfer *transfer;
    int rx_len = 0;

    periplex_spi_set_cs(spi_msg->spi, 0);
    list_for_each_entry(transfer, &spi_msg->transfers, transfer_list)
    {
        SPI_DEBUG("Transfer Length: %d\n", transfer->len);
        if (transfer->tx_buf)
        {
            transfer_buf(ctlr, transfer->tx_buf, transfer->len);

            spi_read_periplex_data_flag = 0;
            wait_event_interruptible(spi_read_wait,
                                     spi_read_periplex_data_flag != 0);
        }

        // Can it be optimized?
        if (transfer->rx_buf)
        {
            SPI_DEBUG("reading data\n");
            transfer_buf(ctlr, transfer->rx_buf, transfer->len);

            while (rx_len < transfer->len)
            {
                spi_read_periplex_data_flag = 0;
                wait_event_interruptible(spi_read_wait,
                                         spi_read_periplex_data_flag != 0);
                if (memcpy(transfer->rx_buf + rx_len, read_data_spi + 1,
                           transfer->len - rx_len < read_data_spi[0]
                               ? transfer->len - rx_len
                               : read_data_spi[0]) == NULL)
                {
                    pr_err("unable to copy data to read buffer\n");
                }

                rx_len += read_data_spi[0];
            }
        }
        spi_msg->actual_length += transfer->len;
    }

    periplex_spi_set_cs(spi_msg->spi, 1);
    spi_msg->status = 0;
    spi_finalize_current_message(ctlr);
    return 0;
}

/*
** Called when a matched spi controller device is found
*/
static int probe_periplex_spi_controller(struct platform_device *pdev)
{
    struct spi_controller *ctlr;
    struct periplex_device *peri_spi_dev;
    int *periplex_id = kmalloc(sizeof(int), GFP_KERNEL);
    int ret;
    u32 num_cs, max_freq;

    pr_info("SPI controller detected\n");
    init_waitqueue_head(&spi_read_wait);

    peri_spi_dev = kzalloc(sizeof(struct periplex_device), GFP_KERNEL);
    if (!peri_spi_dev)
    {
        kfree(periplex_id);
        return -ENOMEM;
    }

    ctlr = spi_alloc_master(&pdev->dev, sizeof(struct periplex_spi_data));
    if (!ctlr)
    {
        dev_err(&pdev->dev, "Failed to allocate SPI master\n");
        kfree(peri_spi_dev);
        kfree(periplex_id);
        return -ENOMEM;
    }

    // Initialize controller
    ctlr->mode_bits = SPI_CPOL | SPI_CPHA;
    ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
    ctlr->bus_num = pdev->id;
    ret = device_property_read_u32(&pdev->dev, "periplex-id", periplex_id);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to read periplex-id from device tee\n");
        kfree(peri_spi_dev);
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
    ctlr->dev.of_node = pdev->dev.of_node;

    peri_spi_dev->pdev = pdev;
    peri_spi_dev->get_periplex_data = read_data_for_spi;
    peri_spi_dev->data = ctlr;

    platform_set_drvdata(pdev, peri_spi_dev);
    pr_info("Periplex SPI controller registered\n");
    periplex_device_register(pdev, *periplex_id);

    ret = spi_register_controller(ctlr);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to register SPI master\n");
        spi_controller_put(ctlr);
        periplex_device_unregister(pdev);
        return ret;
    }
    return 0;
}

/*
** called when device is removed from the system
*/
static int remove_periplex_spi_controller(struct platform_device *pdev)
{
    struct periplex_device *peri_spi_dev = platform_get_drvdata(pdev);
    struct spi_controller *ctlr = peri_spi_dev->data;
    int *periplex_id = (int *)ctlr->dev.driver_data;
    spi_unregister_controller(ctlr);
    periplex_device_unregister(pdev);
    kfree(periplex_id);
    kfree(peri_spi_dev);

    dev_info(&pdev->dev, "spi controller driver removed\n");

    return 0;
}

struct of_device_id periplex_spi_dt_match[] = {
    {.compatible = "vicharak,periplex-spi"},
    {},
};
MODULE_DEVICE_TABLE(of, periplex_spi_dt_match);

struct platform_driver periplex_spi_driver = {
    .probe = probe_periplex_spi_controller,
    .remove = remove_periplex_spi_controller,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = periplex_spi_dt_match}};

module_platform_driver(periplex_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("shailparmar03");
MODULE_DESCRIPTION("SPI Controller Driver");