#include <linux/module.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/of.h>

struct hdd_expander_data {
	struct i2c_client	*client;
	struct device		*hwmon_dev;
	struct mutex		update_lock;
	int			temperature;
	unsigned int		present_status;
};

#define REQUEST_DATA_SIZE (4)
#define RESPONSE_DATA_SIZE (3)
#define PRESENT_RESPONSE_DATA_SIZE (5)
#define REQUEST_FUNCTION_ID (0x0400)
#define MULTIPLIER (1000)
#define MAX_HDD_EXPANDER_SIZE (24)

struct mutex		g_inspect_update_lock;
static int g_hdd_expander_record[MAX_HDD_EXPANDER_SIZE] = {0};

/**
 * hdd_expander_send - Send to HDD Expander Command register
 * @values: 4-byte array which will be sent
 *
 * This writes to HDD Expander Command register, waits for its completion, then returns
 * negative errno else zero on success.
 */
static s32 hdd_expander_i2c_access(struct device *dev, struct i2c_msg *hdd_i2c_msg, int hdd_i2c_msg_num)
{
	struct hdd_expander_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	struct i2c_adapter *adapter = client->adapter;

	if (i2c_transfer(adapter, hdd_i2c_msg, hdd_i2c_msg_num) < 0)
		return -1;

	return 0;
}

static int hdd_expander_get_temperature(struct device *dev, ssize_t device_index)
{
	struct hdd_expander_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;

	struct i2c_msg hdd_i2c_msg[2] = {0};
	int hdd_i2c_msg_num = 0;
	u8 hdd_write_buf[REQUEST_DATA_SIZE] = {0};
	u8 hdd_read_buf[RESPONSE_DATA_SIZE] = {0};

	if (device_index < 0)
		goto abort;

	hdd_write_buf[1] = (REQUEST_FUNCTION_ID >> 8);
	hdd_write_buf[2] = REQUEST_FUNCTION_ID & 0xff;
	hdd_write_buf[3] = (u8) device_index;
	hdd_write_buf[0] = 0xff - (hdd_write_buf[1] + hdd_write_buf[2] + hdd_write_buf[3]);

	hdd_i2c_msg[hdd_i2c_msg_num].addr = client->addr;
	hdd_i2c_msg[hdd_i2c_msg_num].flags = 0;
	hdd_i2c_msg[hdd_i2c_msg_num].buf = hdd_write_buf;
	hdd_i2c_msg[hdd_i2c_msg_num].len = REQUEST_DATA_SIZE;
	hdd_i2c_msg_num+=1;
	hdd_i2c_msg[hdd_i2c_msg_num].addr = client->addr;
	hdd_i2c_msg[hdd_i2c_msg_num].flags = I2C_M_RD;
	hdd_i2c_msg[hdd_i2c_msg_num].buf = hdd_read_buf;
	hdd_i2c_msg[hdd_i2c_msg_num].len = RESPONSE_DATA_SIZE;
	hdd_i2c_msg_num+=1;

	if (hdd_expander_i2c_access(dev, hdd_i2c_msg, hdd_i2c_msg_num) < 0)
		goto abort;
	/*
		hdd_read_buf[]: response data -
			0 - checksum byte
			1 - completion code
				0x00: Success
				0x01: Fail
				0x02: Device does not exit
				0x03: Checksum error
			2 - Temperature for Disk
	*/
	if (hdd_read_buf[1] == 0x00)
		return (hdd_read_buf[2] * MULTIPLIER);
abort:
	return 0;
}

static struct hdd_expander_data *hdd_expander_update_temperature(struct device *dev, ssize_t device_index)
{
	struct hdd_expander_data *data = dev_get_drvdata(dev);
	int temp = -1;

	mutex_lock(&data->update_lock);

	if (temp < 0) {
		/* Sensor on HDD */
		temp = hdd_expander_get_temperature(dev, device_index);
	}

	if (0 <= temp)
		data->temperature = temp;

	mutex_unlock(&data->update_lock);
	return data;
}

static struct hdd_expander_data *hdd_expander_update_present_status(struct device *dev)
{
    struct hdd_expander_data *data = dev_get_drvdata(dev);
    struct i2c_client *client = data->client;
    struct i2c_msg hdd_i2c_msg[2] = {0};
    int hdd_i2c_msg_num = 0;
    u8 hdd_write_buf[REQUEST_DATA_SIZE] = {0};
    u8 hdd_read_buf[PRESENT_RESPONSE_DATA_SIZE] = {0};

    mutex_lock(&data->update_lock);

    hdd_write_buf[0] = 0xFC;
    hdd_write_buf[1] = 0x03;
    hdd_write_buf[2] = 0x00;
    hdd_write_buf[3] = 0x00;
    hdd_i2c_msg[hdd_i2c_msg_num].addr = client->addr;
    hdd_i2c_msg[hdd_i2c_msg_num].flags = 0;
    hdd_i2c_msg[hdd_i2c_msg_num].buf = hdd_write_buf;
    hdd_i2c_msg[hdd_i2c_msg_num].len = REQUEST_DATA_SIZE;
    hdd_i2c_msg_num+=1;
    hdd_i2c_msg[hdd_i2c_msg_num].addr = client->addr;
    hdd_i2c_msg[hdd_i2c_msg_num].flags = I2C_M_RD;
    hdd_i2c_msg[hdd_i2c_msg_num].buf = hdd_read_buf;
    hdd_i2c_msg[hdd_i2c_msg_num].len = PRESENT_RESPONSE_DATA_SIZE;
    hdd_i2c_msg_num+=1;

    data->present_status = 0;
    if (hdd_expander_i2c_access(dev, hdd_i2c_msg, hdd_i2c_msg_num) >= 0) {
        /*
            [0]: checksum Byte
            [1]: Completion Code
                - 0x00 : success
                - 0x01 : fail
                - 0x02 : Device does not exist
                - 0x03 : Checksum error
            [2]: present status for Disk 00~07
            [3]: present status for Disk 08~15
            [4]: present status for Disk 16~23
        */
        if (hdd_read_buf[1] == 0x00) {
            data->present_status = (hdd_read_buf[2]) | (hdd_read_buf[3] << 8) | (hdd_read_buf[4] << 16);
        } else {
            printk(KERN_INFO "%s: 0x%x  Error!!\n", __FUNCTION__, hdd_read_buf[1]);
        }
    }
    
    mutex_unlock(&data->update_lock);
    return data;
}

static ssize_t show_temp(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct hdd_expander_data *data =  NULL;
	ssize_t device_index = -1;

	if (da != NULL && da->attr.name != NULL) {
		sscanf(da->attr.name, "temp%d_input", &device_index);
	}

	mutex_lock(&g_inspect_update_lock);
	data = hdd_expander_update_temperature(dev, device_index);
	mutex_unlock(&g_inspect_update_lock);

	if (IS_ERR(data))
		return PTR_ERR(data);

	g_hdd_expander_record[device_index] = data->temperature;

	return sprintf(buf, "%d\n", data->temperature);
}

static ssize_t show_present_status(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct hdd_expander_data *data =  NULL;
	ssize_t device_index = -1;

	mutex_lock(&g_inspect_update_lock);
	data = hdd_expander_update_present_status(dev);
	mutex_unlock(&g_inspect_update_lock);

	if (IS_ERR(data))
		return PTR_ERR(data);

	return sprintf(buf, "%ld\n", data->present_status);
}

static ssize_t show_max_temp(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	int i;
	int max_temp = 0;
	for (i = 0; i < MAX_HDD_EXPANDER_SIZE; i++)
		if (max_temp < g_hdd_expander_record[i])
			max_temp = g_hdd_expander_record[i];
	return sprintf(buf, "%d\n", max_temp);
}

static SENSOR_DEVICE_ATTR(temp0_input, S_IRUGO, show_temp, NULL, 0);
static SENSOR_DEVICE_ATTR(temp1_input, S_IRUGO, show_temp, NULL, 1);
static SENSOR_DEVICE_ATTR(temp2_input, S_IRUGO, show_temp, NULL, 2);
static SENSOR_DEVICE_ATTR(temp3_input, S_IRUGO, show_temp, NULL, 3);
static SENSOR_DEVICE_ATTR(temp4_input, S_IRUGO, show_temp, NULL, 4);
static SENSOR_DEVICE_ATTR(temp5_input, S_IRUGO, show_temp, NULL, 5);
static SENSOR_DEVICE_ATTR(temp6_input, S_IRUGO, show_temp, NULL, 6);
static SENSOR_DEVICE_ATTR(temp7_input, S_IRUGO, show_temp, NULL, 7);
static SENSOR_DEVICE_ATTR(temp8_input, S_IRUGO, show_temp, NULL, 8);
static SENSOR_DEVICE_ATTR(temp9_input, S_IRUGO, show_temp, NULL, 9);
static SENSOR_DEVICE_ATTR(temp10_input, S_IRUGO, show_temp, NULL, 10);
static SENSOR_DEVICE_ATTR(temp11_input, S_IRUGO, show_temp, NULL, 11);
static SENSOR_DEVICE_ATTR(temp12_input, S_IRUGO, show_temp, NULL, 12);
static SENSOR_DEVICE_ATTR(temp13_input, S_IRUGO, show_temp, NULL, 13);
static SENSOR_DEVICE_ATTR(temp14_input, S_IRUGO, show_temp, NULL, 14);
static SENSOR_DEVICE_ATTR(temp15_input, S_IRUGO, show_temp, NULL, 15);
static SENSOR_DEVICE_ATTR(temp16_input, S_IRUGO, show_temp, NULL, 16);
static SENSOR_DEVICE_ATTR(temp17_input, S_IRUGO, show_temp, NULL, 17);
static SENSOR_DEVICE_ATTR(temp18_input, S_IRUGO, show_temp, NULL, 18);
static SENSOR_DEVICE_ATTR(temp19_input, S_IRUGO, show_temp, NULL, 19);
static SENSOR_DEVICE_ATTR(temp20_input, S_IRUGO, show_temp, NULL, 20);
static SENSOR_DEVICE_ATTR(temp21_input, S_IRUGO, show_temp, NULL, 21);
static SENSOR_DEVICE_ATTR(temp22_input, S_IRUGO, show_temp, NULL, 22);
static SENSOR_DEVICE_ATTR(temp23_input, S_IRUGO, show_temp, NULL, 23);
static SENSOR_DEVICE_ATTR(present_status, S_IRUGO, show_present_status, NULL, 23);
static SENSOR_DEVICE_ATTR(max_temp, S_IRUGO, show_max_temp, NULL, 0);

static struct attribute *hdd_expander_attrs[] = {
	&sensor_dev_attr_temp0_input.dev_attr.attr,
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	&sensor_dev_attr_temp2_input.dev_attr.attr,
	&sensor_dev_attr_temp3_input.dev_attr.attr,
	&sensor_dev_attr_temp4_input.dev_attr.attr,
	&sensor_dev_attr_temp5_input.dev_attr.attr,
	&sensor_dev_attr_temp6_input.dev_attr.attr,
	&sensor_dev_attr_temp7_input.dev_attr.attr,
	&sensor_dev_attr_temp8_input.dev_attr.attr,
	&sensor_dev_attr_temp9_input.dev_attr.attr,
	&sensor_dev_attr_temp10_input.dev_attr.attr,
	&sensor_dev_attr_temp11_input.dev_attr.attr,
	&sensor_dev_attr_temp12_input.dev_attr.attr,
	&sensor_dev_attr_temp13_input.dev_attr.attr,
	&sensor_dev_attr_temp14_input.dev_attr.attr,
	&sensor_dev_attr_temp15_input.dev_attr.attr,
	&sensor_dev_attr_temp16_input.dev_attr.attr,
	&sensor_dev_attr_temp17_input.dev_attr.attr,
	&sensor_dev_attr_temp18_input.dev_attr.attr,
	&sensor_dev_attr_temp19_input.dev_attr.attr,
	&sensor_dev_attr_temp20_input.dev_attr.attr,
	&sensor_dev_attr_temp21_input.dev_attr.attr,
	&sensor_dev_attr_temp22_input.dev_attr.attr,
	&sensor_dev_attr_temp23_input.dev_attr.attr,
	&sensor_dev_attr_present_status.dev_attr.attr,
	&sensor_dev_attr_max_temp.dev_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(hdd_expander);

static int
hdd_expander_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct hdd_expander_data *data;

	data = devm_kzalloc(dev, sizeof(struct hdd_expander_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->client = client;
	i2c_set_clientdata(client, data);

	mutex_init(&data->update_lock);
	mutex_init(&g_inspect_update_lock);

	data->hwmon_dev = hwmon_device_register_with_groups(dev, client->name,
								data,
								hdd_expander_groups);

	if (IS_ERR(data->hwmon_dev))
		return PTR_ERR(data->hwmon_dev);

	dev_info(dev, "%s: sensor '%s'\n",
		 dev_name(data->hwmon_dev), client->name);
	return 0;
}

static int hdd_expander_remove(struct i2c_client *client)
{
	struct hdd_expander_data *data = i2c_get_clientdata(client);
	hwmon_device_unregister(data->hwmon_dev);
	return 0;
}

static const struct i2c_device_id hdd_expander_ids[] = {
	{ "hdd_expander", 0, },
	{ /* LIST END */ }
};
MODULE_DEVICE_TABLE(i2c, hdd_expander_ids);

static int hdd_expander_detect(struct i2c_client *new_client,
				   struct i2c_board_info *info)
{
	/* NOTE we're assuming device described in DTS is present. */
	struct i2c_adapter *adapter = new_client->adapter;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BLOCK_DATA))
		return -ENODEV;

	strlcpy(info->type, "hdd_expander", I2C_NAME_SIZE);

	return 0;
}

/* Addresses scanned */
static const unsigned short normal_i2c[] = { I2C_CLIENT_END };

static struct i2c_driver hdd_expander_driver = {
	.class		= I2C_CLASS_HWMON,
	.driver = {
		.name	= "hdd_expander",
	},
	.probe		= hdd_expander_probe,
	.remove		= hdd_expander_remove,
	.id_table	= hdd_expander_ids,
	.detect		= hdd_expander_detect,
	.address_list	= normal_i2c,
};

module_i2c_driver(hdd_expander_driver);

MODULE_AUTHOR("FXN-KS-BMC");
MODULE_LICENSE("GPL");

