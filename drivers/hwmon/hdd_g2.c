#include <linux/module.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/of.h>

#define MAX_HDD_RECORD_COUNT (10)
#define MAX_HDD_TEMP_COUNT (5)

struct hdd_g2_data {
	struct i2c_client	*client;
	struct device		*hwmon_dev;
	struct mutex		update_lock;
	int			temperature;
};

struct hdd_record_data {
	u8 client_addr;
	int record_temp[MAX_HDD_TEMP_COUNT];
};

#define HDD_MICRON_SLAVE_ADDR (0x53)
#define HDD_MICRON_MUX_SELECTION (1)
#define HDD_MICRON_VENDOR_ID (0x1344)
#define HDD_PM963_SLAVE_ADDR (0x6a)
#define HDD_PM963_I2C_RETRY (10)
#define HDD_PM963_I2C_RETRY_DELAY_MS (10)



struct mutex		inspect_update_lock;
static struct hdd_record_data  g_hdd_record[MAX_HDD_RECORD_COUNT] = {0};
static int g_hdd_record_size = 0;

/**
 * hdd_g2_i2c_access - Send to I2C Command register
 */
static void update_hdd_record(struct hdd_g2_data *data, int record_temp_index, int temp)
{
	struct i2c_client *client = data->client;
	int i = 0;
	int record_index = -1;
	for (i = 0; i < g_hdd_record_size; i++) {
		if (g_hdd_record[i].client_addr == client->addr) {
			record_index = i;
			break;
		}
	}
	if ((record_index == -1) && (g_hdd_record_size < MAX_HDD_RECORD_COUNT)) {
		record_index = g_hdd_record_size;
		g_hdd_record[record_index].client_addr = client->addr;
		g_hdd_record_size+=1;
	}
	if ((record_index != -1) && (record_temp_index < MAX_HDD_TEMP_COUNT))
		g_hdd_record[record_index].record_temp[record_temp_index] = temp;
}

static int get_hdd_max_temp_record(struct hdd_g2_data *data)
{
	struct i2c_client *client = data->client;
	int i = 0;
	int max_temp = 0;
	int record_index = -1;
	for (i = 0; i < g_hdd_record_size; i++) {
		if (g_hdd_record[i].client_addr == client->addr) {
			record_index = i;
			break;
		}
	}
	if (record_index != -1) {
		for (i = 0; i < MAX_HDD_TEMP_COUNT; i++)
			if (max_temp < g_hdd_record[record_index].record_temp[i])
				max_temp = g_hdd_record[record_index].record_temp[i];
	}
	return max_temp;
}

static s32 hdd_g2_i2c_access(struct device *dev, struct i2c_msg *hdd_i2c_msg, int hdd_i2c_msg_num)
{
	struct hdd_g2_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	struct i2c_adapter *adapter = client->adapter;

	if (i2c_transfer(adapter, hdd_i2c_msg, hdd_i2c_msg_num) < 0)
		return -1;

	return 0;
}

static int hdd_seletion(struct device *dev, u8 mux)
{
	struct hdd_g2_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	struct i2c_msg hdd_i2c_msg[2] = {0};
	int hdd_i2c_msg_num = 0;
	u8 hdd_write_buf[] = {mux};

	hdd_i2c_msg[hdd_i2c_msg_num].addr = client->addr;
	hdd_i2c_msg[hdd_i2c_msg_num].flags = 0;
	hdd_i2c_msg[hdd_i2c_msg_num].buf = hdd_write_buf;
	hdd_i2c_msg[hdd_i2c_msg_num].len = sizeof(hdd_write_buf);
	hdd_i2c_msg_num+=1;

	return hdd_g2_i2c_access(dev, hdd_i2c_msg, hdd_i2c_msg_num);
}

static int hdd_deseletion(struct device *dev)
{
	struct hdd_g2_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	struct i2c_msg hdd_i2c_msg[2] = {0};
	int hdd_i2c_msg_num = 0;
	u8 hdd_write_buf[] = {0x00};

	hdd_i2c_msg[hdd_i2c_msg_num].addr = client->addr;
	hdd_i2c_msg[hdd_i2c_msg_num].flags = 0;
	hdd_i2c_msg[hdd_i2c_msg_num].buf = hdd_write_buf;
	hdd_i2c_msg[hdd_i2c_msg_num].len = sizeof(hdd_write_buf);
	hdd_i2c_msg_num+=1;

	return hdd_g2_i2c_access(dev, hdd_i2c_msg, hdd_i2c_msg_num);
}

static int debug_print = 0;

static int hdd_micron_get_temperature(struct device *dev, u8 type)
{
	struct hdd_g2_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;

	struct i2c_msg hdd_i2c_msg[2] = {0};
	int hdd_i2c_msg_num = 0;
	u8 hdd_write_buf[] = {0x4d};
	u8 hdd_read_buf[I2C_SMBUS_BLOCK_MAX] = {0};
	int ret = 0;
	u16 vendor_id = 0;

	if (hdd_seletion(dev, HDD_MICRON_MUX_SELECTION) < 0)
		goto micron_abort;

	hdd_i2c_msg[hdd_i2c_msg_num].addr = HDD_MICRON_SLAVE_ADDR;
	hdd_i2c_msg[hdd_i2c_msg_num].flags = 0;
	hdd_i2c_msg[hdd_i2c_msg_num].buf = hdd_write_buf;
	hdd_i2c_msg[hdd_i2c_msg_num].len = sizeof(hdd_write_buf);
	hdd_i2c_msg_num+=1;
	hdd_i2c_msg[hdd_i2c_msg_num].addr = HDD_MICRON_SLAVE_ADDR;
	hdd_i2c_msg[hdd_i2c_msg_num].flags = I2C_M_RD;
	hdd_i2c_msg[hdd_i2c_msg_num].buf = hdd_read_buf;
	hdd_i2c_msg[hdd_i2c_msg_num].len = sizeof(hdd_read_buf);
	hdd_i2c_msg_num+=1;

	if (hdd_g2_i2c_access(dev, hdd_i2c_msg, hdd_i2c_msg_num) < 0)
		goto micron_abort;

	vendor_id = hdd_read_buf[4] + (hdd_read_buf[5] << 8);
	if (vendor_id != HDD_MICRON_VENDOR_ID)
		goto micron_abort;

	ret = hdd_read_buf[8] + (hdd_read_buf[9] << 8);

micron_abort:

	hdd_deseletion(dev);
	return ret;
}

static struct hdd_g2_data *hdd_micron_update_temperature(struct device *dev)
{
	struct hdd_g2_data *data = dev_get_drvdata(dev);

	mutex_lock(&inspect_update_lock);
	mutex_lock(&data->update_lock);

	data->temperature = hdd_micron_get_temperature(dev, 0x00);

	mutex_unlock(&data->update_lock);
	mutex_unlock(&inspect_update_lock);
	return data;
}

static ssize_t show_hdd_micron_temp(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct hdd_g2_data *data = hdd_micron_update_temperature(dev);

	if (IS_ERR(data))
		return PTR_ERR(data);

	update_hdd_record(data, 0, data->temperature);

	return sprintf(buf, "%d\n", data->temperature);
}

static int hdd_pm963_get_temperature(struct device *dev, ssize_t device_index)
{
	struct hdd_g2_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;

	struct i2c_msg hdd_i2c_msg[2] = {0};
	int hdd_i2c_msg_num = 0;
	u8 hdd_write_buf[] = {0x00};
	u8 hdd_read_buf[I2C_SMBUS_BLOCK_MAX] = {0};
	int ret = 0;
	u16 retry = 0;

hdd_pm963_get_temperature_retry:

	if (hdd_seletion(dev, (u8) (1 << device_index)) < 0)
		goto pm963_abort;

	hdd_i2c_msg[hdd_i2c_msg_num].addr = HDD_PM963_SLAVE_ADDR;
	hdd_i2c_msg[hdd_i2c_msg_num].flags = 0;
	hdd_i2c_msg[hdd_i2c_msg_num].buf = hdd_write_buf;
	hdd_i2c_msg[hdd_i2c_msg_num].len = sizeof(hdd_write_buf);
	hdd_i2c_msg_num+=1;
	hdd_i2c_msg[hdd_i2c_msg_num].addr = HDD_PM963_SLAVE_ADDR;
	hdd_i2c_msg[hdd_i2c_msg_num].flags = I2C_M_RD;
	hdd_i2c_msg[hdd_i2c_msg_num].buf = hdd_read_buf;
	hdd_i2c_msg[hdd_i2c_msg_num].len = sizeof(hdd_read_buf);
	hdd_i2c_msg_num+=1;

	if (hdd_g2_i2c_access(dev, hdd_i2c_msg, hdd_i2c_msg_num) < 0)
		goto pm963_abort;

	ret = hdd_read_buf[3];
pm963_abort:

	hdd_deseletion(dev);
	if (ret == 0 && retry < HDD_PM963_I2C_RETRY)
	{
		mdelay(HDD_PM963_I2C_RETRY_DELAY_MS);
		memset(hdd_i2c_msg, 0, sizeof(hdd_i2c_msg));
		hdd_i2c_msg_num = 0;
		retry+=1;
		goto hdd_pm963_get_temperature_retry;
	}

	if (ret == 0xff) //no device
		return 0;

	return ret;
}

static struct hdd_g2_data *hdd_pm963_update_temperature(struct device *dev, ssize_t device_index)
{
	struct hdd_g2_data *data = dev_get_drvdata(dev);

	if (device_index < 0) {
		data->temperature = 0;
		return data;
	}

	mutex_lock(&inspect_update_lock);
	mutex_lock(&data->update_lock);

	data->temperature = hdd_pm963_get_temperature(dev, device_index);

	mutex_unlock(&data->update_lock);
	mutex_unlock(&inspect_update_lock);
	return data;
}



static ssize_t show_hdd_pm963_temp(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct hdd_g2_data *data = NULL;
	ssize_t device_index = -1;

	if (da != NULL && da->attr.name != NULL) {
		sscanf(da->attr.name, "pm963_temp%d_input", &device_index);
		device_index -= 1;
	}

	data = hdd_pm963_update_temperature(dev, device_index);

	if (IS_ERR(data))
		return PTR_ERR(data);

	update_hdd_record(data, device_index+1, data->temperature);

	return sprintf(buf, "%d\n", data->temperature);
}

static ssize_t show_hdd_max_temp(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct hdd_g2_data *data = dev_get_drvdata(dev);
	if (IS_ERR(data))
		return PTR_ERR(data);
	return sprintf(buf, "%d\n", get_hdd_max_temp_record(data));
}



static SENSOR_DEVICE_ATTR(micron_temp1_input, S_IRUGO, show_hdd_micron_temp, NULL, 0);
static SENSOR_DEVICE_ATTR(pm963_temp1_input, S_IRUGO, show_hdd_pm963_temp, NULL, 0);
static SENSOR_DEVICE_ATTR(pm963_temp2_input, S_IRUGO, show_hdd_pm963_temp, NULL, 0);
static SENSOR_DEVICE_ATTR(pm963_temp3_input, S_IRUGO, show_hdd_pm963_temp, NULL, 0);
static SENSOR_DEVICE_ATTR(pm963_temp4_input, S_IRUGO, show_hdd_pm963_temp, NULL, 0);
static SENSOR_DEVICE_ATTR(hdd_max_temp, S_IRUGO, show_hdd_max_temp, NULL, 0);



static struct attribute *hdd_g2_attrs[] = {
	&sensor_dev_attr_micron_temp1_input.dev_attr.attr,
	&sensor_dev_attr_pm963_temp1_input.dev_attr.attr,
	&sensor_dev_attr_pm963_temp2_input.dev_attr.attr,
	&sensor_dev_attr_pm963_temp3_input.dev_attr.attr,
	&sensor_dev_attr_pm963_temp4_input.dev_attr.attr,
	&sensor_dev_attr_hdd_max_temp.dev_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(hdd_g2);

static int
hdd_g2_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct hdd_g2_data *data;

	data = devm_kzalloc(dev, sizeof(struct hdd_g2_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	i2c_set_clientdata(client, data);
	mutex_init(&data->update_lock);

	mutex_init(&inspect_update_lock);

	data->hwmon_dev = hwmon_device_register_with_groups(dev, client->name,
								data,
								hdd_g2_groups);
	if (IS_ERR(data->hwmon_dev))
		return PTR_ERR(data->hwmon_dev);

	dev_info(dev, "%s: sensor '%s'\n",
		 dev_name(data->hwmon_dev), client->name);

	return 0;
}

static int hdd_g2_remove(struct i2c_client *client)
{
	struct hdd_g2_data *data = i2c_get_clientdata(client);

	hwmon_device_unregister(data->hwmon_dev);
	return 0;
}

static const struct i2c_device_id hdd_g2_ids[] = {
	{ "hdd_g2", 0, },
	{ /* LIST END */ }
};
MODULE_DEVICE_TABLE(i2c, hdd_g2_ids);

static int hdd_g2_detect(struct i2c_client *new_client,
				   struct i2c_board_info *info)
{
	/* NOTE we're assuming device described in DTS is present. */
	struct i2c_adapter *adapter = new_client->adapter;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BLOCK_DATA))
		return -ENODEV;

	strlcpy(info->type, "hdd_g2", I2C_NAME_SIZE);

	return 0;
}

static struct i2c_driver hdd_g2_driver = {
	.class		= I2C_CLASS_HWMON,
	.driver = {
		.name	= "hdd_g2",
	},
	.probe		= hdd_g2_probe,
	.remove		= hdd_g2_remove,
	.id_table	= hdd_g2_ids,
	.detect		= hdd_g2_detect,
	.address_list	= NULL,
};

module_i2c_driver(hdd_g2_driver);

MODULE_AUTHOR("FXN-KS-BMC");
MODULE_LICENSE("GPL");

