#include <linux/module.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/of.h>

#define PROPLENG I2C_SMBUS_BLOCK_MAX

struct g50_gpu_data {
	struct i2c_client	*client;
	struct device		*hwmon_dev;
	struct mutex		update_lock;
	int			temperature;
	char			board_part_number	[PROPLENG];
	char			serial_number		[PROPLENG];
	char			marketing_name		[PROPLENG];
	char			gpu_part_number		[PROPLENG];
	char			firmware_version	[PROPLENG];
};

/**
 * g50_gpu_recv - Read GPU Data register
 * @values: Byte array into which data will be read
 *
 * This reads from GPU Data register, returning negative errno else number of
 * data bytes in the slave's response.
 */
static s32 g50_gpu_recv(struct device *dev, u8 *values)
{
	struct g50_gpu_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	return i2c_smbus_read_block_data(client, 0x5D, values);
}

/**
 * g50_gpu_send - Send to GPU Command register
 * @values: 4-byte array which will be sent
 *
 * This writes to GPU Command register, waits for its completion, then returns
 * negative errno else zero on success.
 */
static s32 g50_gpu_send(struct device *dev, const u8 *values)
{
	struct g50_gpu_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	u8 readbuff[I2C_SMBUS_BLOCK_MAX] = {0};
	int retry = 5;
	s32 ret;

	if (i2c_smbus_write_block_data(client, 0x5C, 4, values) < 0)
		return -1;

	do {
		mdelay(400);
		ret = i2c_smbus_read_block_data(client, 0x5C, readbuff);
		if (ret == 4 && readbuff[3] == 0x1F)
			return 0;
	} while (0 < --retry);

	return -2;
}

static int g50_gpu_get_temperature(struct device *dev, u8 type)
{
	u8 writebuff[I2C_SMBUS_BLOCK_MAX] = {0};
	u8 readbuff[I2C_SMBUS_BLOCK_MAX] = {0};
	int ret = -1;

	writebuff[0] = 0x02;
	writebuff[1] = type;
	writebuff[2] = 0x00;
	writebuff[3] = 0x80;

	if (g50_gpu_send(dev, writebuff))
		goto abort;

	if (g50_gpu_recv(dev, readbuff) < 0)
		goto abort;

	ret = readbuff[1] + (readbuff[2] << 8) + (readbuff[3] << 16);

abort:
	return ret;
}

static struct g50_gpu_data *g50_gpu_update_temperature(struct device *dev)
{
	struct g50_gpu_data *data = dev_get_drvdata(dev);
	int temp = -1;

	mutex_lock(&data->update_lock);

	if (temp < 0) {
		/* Sensor on primary GPU */
		temp = g50_gpu_get_temperature(dev, 0x00);
	}
	if (temp < 0) {
		/* Sensor monitoring memory temperature */
		temp = g50_gpu_get_temperature(dev, 0x05);
	}

	if (0 <= temp)
		data->temperature = temp;

	mutex_unlock(&data->update_lock);
	return data;
}

static char *g50_gpu_get_cpu_info(struct device *dev, char *property,
				  u8 type, u8 length)
{
	struct g50_gpu_data *data = dev_get_drvdata(dev);
	u8 writebuff[I2C_SMBUS_BLOCK_MAX] = {0};
	u8 readbuff[I2C_SMBUS_BLOCK_MAX] = {0};
	u8 offset = 0;

	mutex_lock(&data->update_lock);

	if (property[0] != '\0') {
		/* Abort because property does not change. */
		goto abort;
	}

	for (offset = 0; offset*4 < length; offset++) {
		writebuff[0] = 0x05;
		writebuff[1] = type;
		writebuff[2] = offset;
		writebuff[3] = 0x80;

		if (g50_gpu_send(dev, writebuff))
			goto abort;

		if (g50_gpu_recv(dev, readbuff + offset*4) < 0)
			goto abort;
	}

	/* NOTE we're assuming content in readbuff is string. */
	sprintf(property, "%s", readbuff);

abort:
	mutex_unlock(&data->update_lock);
	return property;
}

static char *g50_gpu_update_board_part_number(struct device *dev)
{
	struct g50_gpu_data *data = dev_get_drvdata(dev);
	return g50_gpu_get_cpu_info(dev, data->board_part_number, 0x00, 24);
}

static char *g50_gpu_update_serial_number(struct device *dev)
{
	struct g50_gpu_data *data = dev_get_drvdata(dev);
	return g50_gpu_get_cpu_info(dev, data->serial_number, 0x02, 16);
}

static char *g50_gpu_update_marketing_name(struct device *dev)
{
	struct g50_gpu_data *data = dev_get_drvdata(dev);
	return g50_gpu_get_cpu_info(dev, data->marketing_name, 0x03, 24);
}

static char *g50_gpu_update_gpu_part_number(struct device *dev)
{
	struct g50_gpu_data *data = dev_get_drvdata(dev);
	return g50_gpu_get_cpu_info(dev, data->gpu_part_number, 0x04, 16);
}

static char *g50_gpu_update_firmware_version(struct device *dev)
{
	struct g50_gpu_data *data = dev_get_drvdata(dev);
	return g50_gpu_get_cpu_info(dev, data->firmware_version, 0x08, 14);
}

static ssize_t show_temp(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct g50_gpu_data *data = g50_gpu_update_temperature(dev);

	if (IS_ERR(data))
		return PTR_ERR(data);

	return sprintf(buf, "%d\n", data->temperature);
}

static ssize_t show_string(struct device *dev, struct device_attribute *da,
			   char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	char *data = NULL;

	switch (attr->index) {
	case 1:
		data = g50_gpu_update_board_part_number(dev);
		break;
	case 2:
		data = g50_gpu_update_serial_number(dev);
		break;
	case 3:
		data = g50_gpu_update_marketing_name(dev);
		break;
	case 4:
		data = g50_gpu_update_gpu_part_number(dev);
		break;
	case 5:
		data = g50_gpu_update_firmware_version(dev);
		break;
	default:
		dev_err(dev, "G50_GPU: bad attribute index %d\n", attr->index);
		return sprintf(buf, "\n");
	}

	return sprintf(buf, "%s\n", data);
}

static SENSOR_DEVICE_ATTR(temperature, S_IRUGO, show_temp, NULL, 0);
static SENSOR_DEVICE_ATTR(board_part_number, S_IRUGO, show_string, NULL, 1);
static SENSOR_DEVICE_ATTR(serial_number, S_IRUGO, show_string, NULL, 2);
static SENSOR_DEVICE_ATTR(marketing_name, S_IRUGO, show_string, NULL, 3);
static SENSOR_DEVICE_ATTR(gpu_part_number, S_IRUGO, show_string, NULL, 4);
static SENSOR_DEVICE_ATTR(firmware_version, S_IRUGO, show_string, NULL, 5);

static struct attribute *g50_gpu_attrs[] = {
	&sensor_dev_attr_temperature.dev_attr.attr,
	&sensor_dev_attr_board_part_number.dev_attr.attr,
	&sensor_dev_attr_serial_number.dev_attr.attr,
	&sensor_dev_attr_marketing_name.dev_attr.attr,
	&sensor_dev_attr_gpu_part_number.dev_attr.attr,
	&sensor_dev_attr_firmware_version.dev_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(g50_gpu);

static int
g50_gpu_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct g50_gpu_data *data;

	data = devm_kzalloc(dev, sizeof(struct g50_gpu_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	i2c_set_clientdata(client, data);
	mutex_init(&data->update_lock);

	data->hwmon_dev = hwmon_device_register_with_groups(dev, client->name,
							    data,
							    g50_gpu_groups);
	if (IS_ERR(data->hwmon_dev))
		return PTR_ERR(data->hwmon_dev);

	dev_info(dev, "%s: sensor '%s'\n",
		 dev_name(data->hwmon_dev), client->name);

	return 0;
}

static int g50_gpu_remove(struct i2c_client *client)
{
	struct g50_gpu_data *data = i2c_get_clientdata(client);

	hwmon_device_unregister(data->hwmon_dev);
	return 0;
}

static const struct i2c_device_id g50_gpu_ids[] = {
	{ "g50_gpu", 0, },
	{ /* LIST END */ }
};
MODULE_DEVICE_TABLE(i2c, g50_gpu_ids);

static int g50_gpu_detect(struct i2c_client *new_client,
			       struct i2c_board_info *info)
{
	/* NOTE we're assuming device described in DTS is present. */
	struct i2c_adapter *adapter = new_client->adapter;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BLOCK_DATA))
		return -ENODEV;

	strlcpy(info->type, "g50_gpu", I2C_NAME_SIZE);

	return 0;
}

/* Addresses scanned */
static const unsigned short normal_i2c[] = { 0x4f, I2C_CLIENT_END };

static struct i2c_driver g50_gpu_driver = {
	.class		= I2C_CLASS_HWMON,
	.driver = {
		.name	= "g50_gpu",
	},
	.probe		= g50_gpu_probe,
	.remove		= g50_gpu_remove,
	.id_table	= g50_gpu_ids,
	.detect		= g50_gpu_detect,
	.address_list	= normal_i2c,
};

module_i2c_driver(g50_gpu_driver);

MODULE_LICENSE("GPL");
