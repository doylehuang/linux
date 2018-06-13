#include <linux/module.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/bitops.h>

struct pex9797_g2_data {
    struct i2c_client   *client;
    struct device       *hwmon_dev;
    struct mutex        update_lock;
    unsigned int        nvme_hdd_present_status;
};

#define STATION_SIZE (4)
#define NVME_PRESNET_I2C_WRITE_SIZE (4)
#define NVME_PRESNET_I2C_READ_SIZE (4)
#define PEX9797_NVME_PRESENT_REG (0x80)

u8 g_pex9797_port_config[] = {
    14, //NVMe 0
     4, //NVMe 1
     5, //NVMe 2
     6, //NVMe 3
     7, //NVMe 4
    15, //NVMe 5
    12, //NVMe 6
    13, //NVMe 7
    20, //NVMe 8
    23, //NVMe 9
     8, //NVMe 10
    11, //NVMe 11
    16, //NVMe 12
    19, //NVMe 13
    22, //NVMe 14
     9, //NVMe 15
    10, //NVMe 16
    17, //NVMe 17
    18, //NVMe 18
    21, //NVMe 19
};

/**
 * pex9797_g2_i2c_access - Send to I2C Command register
 */
static s32 pex9797_g2_i2c_access(struct device *dev, struct i2c_msg *pex9797_i2c_msg, int pex9797_i2c_msg_num)
{
    struct pex9797_g2_data *data = dev_get_drvdata(dev);
    struct i2c_client *client = data->client;
    struct i2c_adapter *adapter = client->adapter;

    if (i2c_transfer(adapter, pex9797_i2c_msg, pex9797_i2c_msg_num) < 0)
        return -1;

    return 0;
}

static int pex9797_get_nvme_present_status(struct device *dev, u8 pex9797_port)
{
    struct pex9797_g2_data *data = dev_get_drvdata(dev);
    struct i2c_client *client = data->client;
    struct i2c_msg pex9797_i2c_msg[2] = {0};
    int pex9797_i2c_msg_num = 0;
    u8 pex9797_write_buf[NVME_PRESNET_I2C_WRITE_SIZE] = {0};
    u8 pex9797_read_buf[NVME_PRESNET_I2C_READ_SIZE] = {0};
    u8 station_select = (u8)(pex9797_port / STATION_SIZE);
    u8 port_select = pex9797_port % STATION_SIZE;
    
    pex9797_write_buf[0] = 0x04; //bit0~2 command: read register
    pex9797_write_buf[1] = (port_select & BIT(1)) >> 1; // bit 0: Port Select Bit1
    pex9797_write_buf[1] |= (station_select) << 2; // bit 2~4: Station selection
    pex9797_write_buf[2] = 0x3C;
    pex9797_write_buf[2] |= (port_select & BIT(0)) << 7; // bit 7: Port Select Bit0
    pex9797_write_buf[3] = (PEX9797_NVME_PRESENT_REG >> 2); // pex9797 register address bits[9:2]

    pex9797_i2c_msg[pex9797_i2c_msg_num].addr = client->addr;
    pex9797_i2c_msg[pex9797_i2c_msg_num].flags = 0;
    pex9797_i2c_msg[pex9797_i2c_msg_num].buf = pex9797_write_buf;
    pex9797_i2c_msg[pex9797_i2c_msg_num].len = sizeof(pex9797_write_buf);
    pex9797_i2c_msg_num+=1;
    pex9797_i2c_msg[pex9797_i2c_msg_num].addr = client->addr;
    pex9797_i2c_msg[pex9797_i2c_msg_num].flags = I2C_M_RD;
    pex9797_i2c_msg[pex9797_i2c_msg_num].buf = pex9797_read_buf;
    pex9797_i2c_msg[pex9797_i2c_msg_num].len = sizeof(pex9797_read_buf);
    pex9797_i2c_msg_num+=1;

    if (pex9797_g2_i2c_access(dev, pex9797_i2c_msg, pex9797_i2c_msg_num) < 0)
        return 0;

    return (pex9797_read_buf[1] & BIT(6)); // return bit22 of read bytes means: nvme hdd device present
}

static struct pex9797_g2_data *pex9797_nvme_hdd_update_present_status(struct device *dev)
{
    struct pex9797_g2_data *data = dev_get_drvdata(dev);
    int i;
    int resp;
    mutex_lock(&data->update_lock);
    data->nvme_hdd_present_status = 0;
    for (i = 0; i < sizeof(g_pex9797_port_config); i++) {
        resp = pex9797_get_nvme_present_status(dev, g_pex9797_port_config[i]);
        if (resp > 0) {
            data->nvme_hdd_present_status |= (1 << i);
        }
    }
    mutex_unlock(&data->update_lock);
    return data;
}

static ssize_t show_nvme_hdd_present_status(struct device *dev, struct device_attribute *da,
             char *buf)
{
    struct pex9797_g2_data *data = NULL;
    data = pex9797_nvme_hdd_update_present_status(dev);
    if (IS_ERR(data))
        return PTR_ERR(data);
    return sprintf(buf, "%d\n", data->nvme_hdd_present_status);
}

static SENSOR_DEVICE_ATTR(nvme_hdd_present_status, S_IRUGO, show_nvme_hdd_present_status, NULL, 0);
static struct attribute *pex9797_g2_attrs[] = {
    &sensor_dev_attr_nvme_hdd_present_status.dev_attr.attr,
    NULL
};
ATTRIBUTE_GROUPS(pex9797_g2);

static int
pex9797_g2_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    struct device *dev = &client->dev;
    struct pex9797_g2_data *data;

    data = devm_kzalloc(dev, sizeof(struct pex9797_g2_data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->client = client;
    i2c_set_clientdata(client, data);
    mutex_init(&data->update_lock);
    data->hwmon_dev = hwmon_device_register_with_groups(dev, client->name,
                                data,
                                pex9797_g2_groups);
    if (IS_ERR(data->hwmon_dev))
        return PTR_ERR(data->hwmon_dev);

    dev_info(dev, "%s: sensor '%s'\n",
         dev_name(data->hwmon_dev), client->name);

    return 0;
}

static int pex9797_g2_remove(struct i2c_client *client)
{
    struct pex9797_g2_data *data = i2c_get_clientdata(client);

    hwmon_device_unregister(data->hwmon_dev);
    return 0;
}

static const struct i2c_device_id pex9797_g2_ids[] = {
    { "pex9797_g2", 0, },
    { /* LIST END */ }
};
MODULE_DEVICE_TABLE(i2c, pex9797_g2_ids);

static int pex9797_g2_detect(struct i2c_client *new_client,
                   struct i2c_board_info *info)
{
    /* NOTE we're assuming device described in DTS is present. */
    struct i2c_adapter *adapter = new_client->adapter;

    if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BLOCK_DATA))
        return -ENODEV;

    strlcpy(info->type, "pex9797_g2", I2C_NAME_SIZE);

    return 0;
}

static struct i2c_driver pex9797_g2_driver = {
    .class      = I2C_CLASS_HWMON,
    .driver = {
        .name   = "pex9797_g2",
    },
    .probe      = pex9797_g2_probe,
    .remove     = pex9797_g2_remove,
    .id_table   = pex9797_g2_ids,
    .detect     = pex9797_g2_detect,
    .address_list   = NULL,
};

module_i2c_driver(pex9797_g2_driver);

MODULE_AUTHOR("FXN-KS-BMC");
MODULE_LICENSE("GPL");

