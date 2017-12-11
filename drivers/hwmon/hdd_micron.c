#include <linux/module.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/of.h>

struct hdd_micron_data {
	struct i2c_client	*client;
	struct device		*hwmon_dev;
	struct mutex		update_lock;
	int			temperature;
};

#define HDD_SLAVE_ADDR (0x53)

struct mutex		inspect_update_lock;

/**
 * hdd_micron_send - Send to GPU Command register
 * @values: 4-byte array which will be sent
 *
 * This writes to GPU Command register, waits for its completion, then returns
 * negative errno else zero on success.
 */
static s32 hdd_micron_i2c_access(struct device *dev, struct i2c_msg *hdd_i2c_msg, int hdd_i2c_msg_num)
{
	struct hdd_micron_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	struct i2c_adapter *adapter = client->adapter;

	if (i2c_transfer(adapter, hdd_i2c_msg, hdd_i2c_msg_num) < 0)
		return -1;

	return 0;
}

static int hdd_seletion(struct device *dev)
{
	struct hdd_micron_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	struct i2c_msg hdd_i2c_msg[2] = {0};
	int hdd_i2c_msg_num = 0;
	u8 hdd_write_buf[] = {0x01};

	hdd_i2c_msg[hdd_i2c_msg_num].addr = client->addr;
	hdd_i2c_msg[hdd_i2c_msg_num].flags = 0;
	hdd_i2c_msg[hdd_i2c_msg_num].buf = hdd_write_buf;
	hdd_i2c_msg[hdd_i2c_msg_num].len = sizeof(hdd_write_buf);
	hdd_i2c_msg_num+=1;

	return hdd_micron_i2c_access(dev, hdd_i2c_msg, hdd_i2c_msg_num);
}

static int hdd_deseletion(struct device *dev)
{
	struct hdd_micron_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	struct i2c_msg hdd_i2c_msg[2] = {0};
	int hdd_i2c_msg_num = 0;
	u8 hdd_write_buf[] = {0x00};

	hdd_i2c_msg[hdd_i2c_msg_num].addr = client->addr;
	hdd_i2c_msg[hdd_i2c_msg_num].flags = 0;
	hdd_i2c_msg[hdd_i2c_msg_num].buf = hdd_write_buf;
	hdd_i2c_msg[hdd_i2c_msg_num].len = sizeof(hdd_write_buf);
	hdd_i2c_msg_num+=1;

	return hdd_micron_i2c_access(dev, hdd_i2c_msg, hdd_i2c_msg_num);
}

static int debug_print = 0;

static int hdd_micron_get_temperature(struct device *dev, u8 type)
{
	struct hdd_micron_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;

	struct i2c_msg hdd_i2c_msg[2] = {0};
	int hdd_i2c_msg_num = 0;
	u8 hdd_write_buf[] = {0x4d};
	u8 hdd_read_buf[I2C_SMBUS_BLOCK_MAX] = {0};
	int ret = 0;

	if (hdd_seletion(dev) < 0)
		goto abort;

	hdd_i2c_msg[hdd_i2c_msg_num].addr = HDD_SLAVE_ADDR;
	hdd_i2c_msg[hdd_i2c_msg_num].flags = 0;
	hdd_i2c_msg[hdd_i2c_msg_num].buf = hdd_write_buf;
	hdd_i2c_msg[hdd_i2c_msg_num].len = sizeof(hdd_write_buf);
	hdd_i2c_msg_num+=1;
	hdd_i2c_msg[hdd_i2c_msg_num].addr = HDD_SLAVE_ADDR;
	hdd_i2c_msg[hdd_i2c_msg_num].flags = I2C_M_RD;
	hdd_i2c_msg[hdd_i2c_msg_num].buf = hdd_read_buf;
	hdd_i2c_msg[hdd_i2c_msg_num].len = sizeof(hdd_read_buf);
	hdd_i2c_msg_num+=1;

	if (hdd_micron_i2c_access(dev, hdd_i2c_msg, hdd_i2c_msg_num) < 0)
		goto abort;

	ret = hdd_read_buf[8] + (hdd_read_buf[9] << 8);

abort:

	hdd_deseletion(dev);
	return ret;
}

static struct hdd_micron_data *hdd_micron_update_temperature(struct device *dev)
{
	struct hdd_micron_data *data = dev_get_drvdata(dev);
	int temp = -1;

	mutex_lock(&inspect_update_lock);
	mutex_lock(&data->update_lock);

	if (temp < 0) {
		/* Sensor on HDD */
		temp = hdd_micron_get_temperature(dev, 0x00);
	}

	if (0 <= temp)
		data->temperature = temp;

	mutex_unlock(&data->update_lock);
	mutex_unlock(&inspect_update_lock);
	return data;
}

static ssize_t show_temp(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct hdd_micron_data *data = hdd_micron_update_temperature(dev);

	if (IS_ERR(data))
		return PTR_ERR(data);

	return sprintf(buf, "%d\n", data->temperature);
}

static SENSOR_DEVICE_ATTR(temp1_input, S_IRUGO, show_temp, NULL, 0);

static struct attribute *hdd_micron_attrs[] = {
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(hdd_micron);

static int
hdd_micron_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct hdd_micron_data *data;

	data = devm_kzalloc(dev, sizeof(struct hdd_micron_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	i2c_set_clientdata(client, data);
	mutex_init(&data->update_lock);

	mutex_init(&inspect_update_lock);

	data->hwmon_dev = hwmon_device_register_with_groups(dev, client->name,
								data,
								hdd_micron_groups);
	if (IS_ERR(data->hwmon_dev))
		return PTR_ERR(data->hwmon_dev);

	dev_info(dev, "%s: sensor '%s'\n",
		 dev_name(data->hwmon_dev), client->name);

	return 0;
}

static int hdd_micron_remove(struct i2c_client *client)
{
	struct hdd_micron_data *data = i2c_get_clientdata(client);

	hwmon_device_unregister(data->hwmon_dev);
	return 0;
}

static const struct i2c_device_id hdd_micron_ids[] = {
	{ "hdd_micron", 0, },
	{ /* LIST END */ }
};
MODULE_DEVICE_TABLE(i2c, hdd_micron_ids);

static int hdd_micron_detect(struct i2c_client *new_client,
				   struct i2c_board_info *info)
{
	/* NOTE we're assuming device described in DTS is present. */
	struct i2c_adapter *adapter = new_client->adapter;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BLOCK_DATA))
		return -ENODEV;

	strlcpy(info->type, "hdd_micron", I2C_NAME_SIZE);

	return 0;
}

/* Addresses scanned */
static const unsigned short normal_i2c[] = { 0x53, I2C_CLIENT_END };

static struct i2c_driver hdd_micron_driver = {
	.class		= I2C_CLASS_HWMON,
	.driver = {
		.name	= "hdd_micron",
	},
	.probe		= hdd_micron_probe,
	.remove		= hdd_micron_remove,
	.id_table	= hdd_micron_ids,
	.detect		= hdd_micron_detect,
	.address_list	= normal_i2c,
};

module_i2c_driver(hdd_micron_driver);

MODULE_LICENSE("GPL");

