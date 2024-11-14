#define PERIPHERAL_H
#define PERIPHERAL_H

#include <linux/platform_device.h>
#include <linux/export.h>

/* Common Structure for congigration frame(use in write case) */
struct kernel_config
{
    int peri_id;
    int configuration;
    uint8_t configuration_id;
};

/* Common Structure for data frame(use in write case) */
struct kernel_data
{
    int length;
    int peri_id;
    int minor;
};

/* common structure for read data (use in read case) */
struct read_buffer
{
    char *message;
    int length;
    int minor;
};

/* periplex device structure */
struct periplex_device
{
    struct platform_device *pdev;
    void *data;
    int (*get_periplex_data) (struct periplex_device *dev, 
    char *message, const int len);
};

/* use for set configurations */
void set_periplex_configuration(int peri_id, uint8_t config_id, 
        int configuration);

/* use for set data */
void set_periplex_data(int peri_id,int length, char*message);

/* use for register device to periplex */
int periplex_device_register(struct platform_device *pdev, 
        int periplex_id);

/* use for unregister device from periplex */
void periplex_device_unregister(struct platform_device *pdev);