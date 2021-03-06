/*
 * FSI core driver
 *
 * Copyright (C) IBM Corporation 2016
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/fsi.h>
#include <linux/idr.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/delay.h>

#include "fsi-master.h"

#define DEBUG

#define FSI_N_SLAVES	4
#define FSI_BREAK	0xc0de0000

#define FSI_SLAVE_CONF_NEXT_MASK	0x80000000
#define FSI_SLAVE_CONF_SLOTS_MASK	0x00ff0000
#define FSI_SLAVE_CONF_SLOTS_SHIFT	16
#define FSI_SLAVE_CONF_VERSION_MASK	0x0000f000
#define FSI_SLAVE_CONF_VERSION_SHIFT	12
#define FSI_SLAVE_CONF_TYPE_MASK	0x00000ff0
#define FSI_SLAVE_CONF_TYPE_SHIFT	4
#define FSI_SLAVE_CONF_CRC_SHIFT	4
#define FSI_SLAVE_CONF_CRC_MASK		0x0000000f
#define FSI_SLAVE_CONF_DATA_BITS	28

#define FSI_PEEK_BASE			0x410
#define	FSI_SLAVE_BASE			0x800
#define	FSI_HUB_CONTROL			0x3400

#define	FSI_SLAVE_SMODE_DFLT		0xa0ff0100

#define FSI_IPOLL_PERIOD		msecs_to_jiffies(fsi_ipoll_period_ms)

#define	FSI_ENGID_HUB_MASTER		0x1c
#define	FSI_ENGID_HUB_LINK		0x1d
#define	FSI_HUB_LINK_OFFSET		0x80000
#define	FSI_MASTER_HUB_LINK_SIZE	0x80000
#define	FSI_HUB_MASTER_MAX_LINKS	8

#define	FSI_LINK_ENABLE_SETUP_TIME	10	/* in mS */

static const int engine_page_size = 0x400;
static struct task_struct *master_ipoll;
static unsigned int fsi_ipoll_period_ms = 100;

static DEFINE_IDA(master_ida);

struct fsi_slave {
	struct list_head	list_link;	/* Master's list of slaves */
	struct list_head	my_engines;
	struct device		dev;
	struct fsi_master	*master;
	int			link;
	uint8_t			id;
};

struct fsi_master_hub {
	struct fsi_master	master;
	struct fsi_slave	*slave;
	struct device		dev;
	uint32_t		control_regs;	/* slave-relative addr regs */
	uint32_t		base;		/* slave-relative addr of */
						/* master address space */
};

#define to_fsi_hub(d) container_of(d, struct fsi_master_hub, dev)
#define to_fsi_master_hub(d) container_of(d, struct fsi_master_hub, master)
#define to_fsi_slave(d) container_of(d, struct fsi_slave, dev)

static void fsi_master_unscan(struct fsi_master *master);
static int fsi_slave_read(struct fsi_slave *slave, uint32_t addr,
		void *val, size_t size);
static int fsi_slave_write(struct fsi_slave *slave, uint32_t addr,
		const void *val, size_t size);

/*
 * FSI slave engine control register offsets
 */
#define	FSI_SMODE		0x0	/* R/W: Mode register */
#define FSI_SI1M		0x18	/* R/W: IRQ mask */
#define FSI_SI1S		0x1C	/* R: IRQ status */
#define FSI_SRSIC0		0x68	/* R/W: Remote IRQ condition 0 */
#define FSI_SRSIC1		0x6C	/* R/W: Remote IRQ condition 1 */
#define FSI_SRSIM0		0x70	/* R/W: Remote IRQ mask 0 */
#define FSI_SRSIS0		0x78	/* R: Remote IRQ status 0 */

/*
 * SI1S, SI1M fields
 */
#define FSI_SI1_HUB_SRC		0x00100000	/* hub IRQ source */

/*
 * SMODE fields
 */
#define	FSI_SMODE_WSC		0x80000000	/* Warm start done */
#define	FSI_SMODE_ECRC		0x20000000	/* Hw CRC check */
#define	FSI_SMODE_SID_SHIFT	24		/* ID shift */
#define	FSI_SMODE_SID_MASK	3		/* ID Mask */
#define	FSI_SMODE_ED_SHIFT	20		/* Echo delay shift */
#define	FSI_SMODE_ED_MASK	0xf		/* Echo delay mask */
#define	FSI_SMODE_SD_SHIFT	16		/* Send delay shift */
#define	FSI_SMODE_SD_MASK	0xf		/* Send delay mask */
#define	FSI_SMODE_LBCRR_SHIFT	8		/* Clk ratio shift */
#define	FSI_SMODE_LBCRR_MASK	0xf		/* Clk ratio mask */

/*
 * SRSIS, SRSIM, SRSIC fields
 */
#define	FSI_SRSIX_IRQ1_MASK	0x00aaaaaa	/* SI1 IRQ sources */
#define	FSI_SRSIX_BITS_PER_LINK	8


/* FSI endpoint-device support */
int fsi_device_read(struct fsi_device *dev, uint32_t addr, void *val,
		size_t size)
{
	if (addr > dev->size || size > dev->size || addr > dev->size - size)
		return -EINVAL;

	return fsi_slave_read(dev->slave, dev->addr + addr, val, size);
}
EXPORT_SYMBOL_GPL(fsi_device_read);

int fsi_device_write(struct fsi_device *dev, uint32_t addr, const void *val,
		size_t size)
{
	if (addr > dev->size || size > dev->size || addr > dev->size - size)
		return -EINVAL;

	return fsi_slave_write(dev->slave, dev->addr + addr, val, size);
}
EXPORT_SYMBOL_GPL(fsi_device_write);

int fsi_device_peek(struct fsi_device *dev, void *val)
{
	uint32_t addr = FSI_PEEK_BASE + ((dev->unit - 2) * sizeof(uint32_t));

	return fsi_slave_read(dev->slave, addr, val, sizeof(uint32_t));
}

static void fsi_device_release(struct device *_device)
{
	struct fsi_device *device = to_fsi_dev(_device);

	kfree(device);
}

static struct fsi_device *fsi_create_device(struct fsi_slave *slave)
{
	struct fsi_device *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return NULL;

	dev->dev.parent = &slave->dev;
	dev->dev.bus = &fsi_bus_type;
	dev->dev.release = fsi_device_release;

	return dev;
}

/* crc helpers */
static const uint8_t crc4_tab[] = {
	0x0, 0x7, 0xe, 0x9, 0xb, 0xc, 0x5, 0x2,
	0x1, 0x6, 0xf, 0x8, 0xa, 0xd, 0x4, 0x3,
};

uint8_t fsi_crc4(uint8_t c, uint64_t x, int bits)
{
	int i;

	/* Align to 4-bits */
	bits = (bits + 3) & ~0x3;

	/* Calculate crc4 over four-bit nibbles, starting at the MSbit */
	for (i = bits; i >= 0; i -= 4)
		c = crc4_tab[c ^ ((x >> i) & 0xf)];

	return c;
}
EXPORT_SYMBOL_GPL(fsi_crc4);

/* FSI slave support */

/* Encode slave local bus echo delay */
static inline uint32_t fsi_smode_echodly(int x)
{
	return (x & FSI_SMODE_ED_MASK) << FSI_SMODE_ED_SHIFT;
}

/* Encode slave local bus send delay */
static inline uint32_t fsi_smode_senddly(int x)
{
	return (x & FSI_SMODE_SD_MASK) << FSI_SMODE_SD_SHIFT;
}

/* Encode slave local bus clock rate ratio */
static inline uint32_t fsi_smode_lbcrr(int x)
{
	return (x & FSI_SMODE_LBCRR_MASK) << FSI_SMODE_LBCRR_SHIFT;
}

/* Encode slave ID */
static inline uint32_t fsi_smode_sid(int x)
{
	return (x & FSI_SMODE_SID_MASK) << FSI_SMODE_SID_SHIFT;
}

static int fsi_slave_read(struct fsi_slave *slave, uint32_t addr,
			void *val, size_t size)
{
	return slave->master->read(slave->master, slave->link,
			slave->id, addr, val, size);
}

static int fsi_slave_write(struct fsi_slave *slave, uint32_t addr,
			const void *val, size_t size)
{
	return slave->master->write(slave->master, slave->link,
			slave->id, addr, val, size);
}

/*
 * FSI hub master support
 *
 * A hub master increases the number of potential target devices that the
 * primary FSI master can access.  For each link a primary master supports
 * each of those links can in turn be chained to a hub master with multiple
 * hub links of its own.  Hubs differ from cascaded masters (cMFSI) in the
 * total addressable range per link -hubs having address ranges that are much
 * larger.  Hub masters also contain the registers that describe them
 * whereas cascaded masters are described by their parent.
 */
int hub_master_read(struct fsi_master *master, int linkno, uint8_t slave,
			uint32_t addr, void *val, size_t size)
{
	struct fsi_master_hub *hub = to_fsi_master_hub(master);

	addr += (linkno * FSI_MASTER_HUB_LINK_SIZE) + hub->base;
	return fsi_slave_read(hub->slave, addr, val, size);
}

int hub_master_write(struct fsi_master *master, int linkno, uint8_t slave,
			uint32_t addr, const void *val, size_t size)
{
	struct fsi_master_hub *hub = to_fsi_master_hub(master);

	addr += (linkno * FSI_MASTER_HUB_LINK_SIZE) + hub->base;
	return fsi_slave_write(hub->slave, addr, val, size);
}

int hub_master_break(struct fsi_master *master, int linkno)
{
	struct fsi_master_hub *hub = to_fsi_master_hub(master);
	uint32_t command;
	uint32_t break_offset = 0x4; /* hw workaround: hub links require a */
				     /* break to offset 4 instead of the */
				     /* non hub 0 offset. */
	uint32_t addr;

	command = FSI_BREAK;
	addr = (linkno * FSI_MASTER_HUB_LINK_SIZE) + hub->base;
	return fsi_slave_write(hub->slave, addr + break_offset, &command,
			sizeof(command));
}

int hub_master_link_enable(struct fsi_master *master, int linkno)
{
	struct fsi_master_hub *hub = to_fsi_master_hub(master);
	uint32_t menp = L_MSB_MASK(linkno);
	int rc;

	rc = fsi_slave_write(hub->slave, hub->control_regs + FSI_MSENP0, &menp,
				sizeof(menp));

	/*
	 * Wait for hw to finish enable - there is latency in logic setup
	 * before link operations like break, etc can be done
	 */
	mdelay(FSI_LINK_ENABLE_SETUP_TIME);

	return rc;
}

static int hub_master_init(struct fsi_master_hub *hub)
{
	int rc;
	uint32_t mver;
	struct fsi_master *master = &hub->master;

	master->read = hub_master_read;
	master->write = hub_master_write;
	master->send_break = hub_master_break;
	master->link_enable = hub_master_link_enable;

	/* Initialize the MFSI (hub master) engine */
	rc = fsi_slave_read(hub->slave, hub->control_regs + FSI_MVER, &mver,
				sizeof(mver));
	if (rc)
		return rc;

	mver = FSI_MRESP_RST_ALL_MASTER | FSI_MRESP_RST_ALL_LINK
			| FSI_MRESP_RST_MCR | FSI_MRESP_RST_PYE;
	rc = fsi_slave_write(hub->slave, hub->control_regs + FSI_MRESP0, &mver,
				sizeof(mver));
	if (rc)
		return rc;

	mver = FSI_MECTRL_EOAE | FSI_MECTRL_P8_AUTO_TERM;
	rc = fsi_slave_write(hub->slave, hub->control_regs + FSI_MECTRL, &mver,
				sizeof(mver));
	if (rc)
		return rc;

	mver = FSI_MMODE_EIP | FSI_MMODE_ECRC | FSI_MMODE_EPC
			| fsi_mmode_crs0(1) | fsi_mmode_crs1(1)
			| FSI_MMODE_P8_TO_LSB;
	rc = fsi_slave_write(hub->slave, hub->control_regs + FSI_MMODE, &mver,
				sizeof(mver));
	if (rc)
		return rc;

	mver = 0xffff0000;
	rc = fsi_slave_write(hub->slave, hub->control_regs + FSI_MDLYR, &mver,
				sizeof(mver));
	if (rc)
		return rc;

	mver = ~0;
	rc = fsi_slave_write(hub->slave, hub->control_regs + FSI_MSENP0, &mver,
				sizeof(mver));
	if (rc)
		return rc;

	/* Leave enabled long enough for master logic to set up */
	mdelay(FSI_LINK_ENABLE_SETUP_TIME);

	rc = fsi_slave_write(hub->slave, hub->control_regs + FSI_MCENP0, &mver,
				sizeof(mver));
	if (rc)
		return rc;

	rc = fsi_slave_read(hub->slave, hub->control_regs + FSI_MAEB, &mver,
				sizeof(mver));
	if (rc)
		return rc;

	mver = FSI_MRESP_RST_ALL_MASTER | FSI_MRESP_RST_ALL_LINK;
	rc = fsi_slave_write(hub->slave, hub->control_regs + FSI_MRESP0, &mver,
				sizeof(mver));
	if (rc)
		return rc;

	rc = fsi_slave_read(hub->slave, hub->control_regs + FSI_MLEVP0, &mver,
				sizeof(mver));
	if (rc)
		return rc;

	/* Reset the master bridge */
	mver = FSI_MRESB_RST_GEN;
	rc = fsi_slave_write(hub->slave, hub->control_regs + FSI_MRESB0, &mver,
				sizeof(mver));
	if (rc)
		return rc;

	mver = FSI_MRESB_RST_ERR;
	return fsi_slave_write(hub->slave, hub->control_regs + FSI_MRESB0,
				&mver, sizeof(mver));
}

static void hub_master_release(struct device *dev)
{
	struct fsi_master_hub *hub = to_fsi_hub(dev);

	kfree(hub);
}

static int fsi_slave_scan(struct fsi_slave *slave)
{
	uint32_t engine_addr;
	uint32_t conf;
	int rc, i;
	uint8_t si1s_bit = 1;
	uint8_t conf_link_count = 0;
	struct fsi_master_hub *hub;

	INIT_LIST_HEAD(&slave->my_engines);

	/*
	 * scan engines
	 *
	 * We keep the peek mode and slave engines for the core; so start
	 * at the third slot in the configuration table. We also need to
	 * skip the chip ID entry at the start of the address space.
	 */
	engine_addr = engine_page_size * 3;
	for (i = 2; i < engine_page_size / sizeof(uint32_t); i++) {
		uint8_t slots, version, type, crc;
		struct fsi_device *dev;

		rc = fsi_slave_read(slave, (i + 1) * sizeof(conf),
				&conf, sizeof(conf));
		if (rc) {
			dev_warn(&slave->dev,
				"error reading slave registers\n");
			return -1;
		}

		crc = fsi_crc4(0, conf >> FSI_SLAVE_CONF_CRC_SHIFT,
				FSI_SLAVE_CONF_DATA_BITS);
		if (crc != (conf & FSI_SLAVE_CONF_CRC_MASK)) {
			dev_warn(&slave->dev,
				"crc error in slave register at 0x%04x\n",
				i);
			return -1;
		}

		slots = (conf & FSI_SLAVE_CONF_SLOTS_MASK)
			>> FSI_SLAVE_CONF_SLOTS_SHIFT;
		version = (conf & FSI_SLAVE_CONF_VERSION_MASK)
			>> FSI_SLAVE_CONF_VERSION_SHIFT;
		type = (conf & FSI_SLAVE_CONF_TYPE_MASK)
			>> FSI_SLAVE_CONF_TYPE_SHIFT;

		switch (type) {
		case 0:
			/*
			 * Unused address areas are marked by a zero type
			 * value; this skips the defined address areas
			 */
			break;

		case FSI_ENGID_HUB_MASTER:
			hub = kzalloc(sizeof(*hub), GFP_KERNEL);
			if (!hub)
				return -ENOMEM;

			device_initialize(&hub->dev);
			dev_set_name(&hub->dev, "hub@%02x", slave->master->idx);
			hub->dev.release = hub_master_release;
			hub->master.dev = &hub->dev;
			hub->master.dev->parent = &slave->dev;
			dev_set_drvdata(&hub->dev, hub);
			rc = device_add(&hub->dev);
			if (rc)
				return rc;

			hub->base = FSI_HUB_LINK_OFFSET;
			hub->control_regs = engine_addr;
			hub->slave = slave;
			rc = hub_master_init(hub);

			break;

		case FSI_ENGID_HUB_LINK:
			conf_link_count++;

			break;

		default:
			if (slots == 0)
				break;

			/* create device */
			dev = fsi_create_device(slave);
			if (!dev)
				return -ENOMEM;

			dev->slave = slave;
			dev->engine_type = type;
			dev->version = version;
			dev->unit = i;
			dev->addr = engine_addr;
			dev->size = slots * engine_page_size;
			dev->si1s_bit = si1s_bit++;

			dev_info(&slave->dev,
			"engine[%i]: type %x, version %x, addr %x size %x\n",
					dev->unit, dev->engine_type, version,
					dev->addr, dev->size);

			device_initialize(&dev->dev);
			dev_set_name(&dev->dev, "%02x:%02x:%02x:%02x",
					slave->master->idx, slave->link,
					slave->id, i - 2);

			rc = device_add(&dev->dev);
			if (rc) {
				dev_warn(&slave->dev, "add failed: %d\n", rc);
				put_device(&dev->dev);
				continue;
			}
			list_add(&dev->link, &slave->my_engines);
		}

		engine_addr += slots * engine_page_size;

		if (!(conf & FSI_SLAVE_CONF_NEXT_MASK))
			break;
	}

	if (hub) {
		hub->master.n_links = conf_link_count / 2;
		fsi_master_register(&hub->master);
	}

	return 0;
}

static void fsi_slave_release(struct device *dev)
{
	struct fsi_slave *slave = to_fsi_slave(dev);

	kfree(slave);
}

static uint32_t set_smode_defaults(struct fsi_master *master)
{
	return FSI_SMODE_WSC | FSI_SMODE_ECRC
		| fsi_smode_echodly(0xf) | fsi_smode_senddly(0xf)
		| fsi_smode_lbcrr(1);
}

static int fsi_slave_set_smode(struct fsi_master *master, int link, int id)
{
	uint32_t smode = set_smode_defaults(master);

	smode |= fsi_smode_sid(id);
	return master->write(master, link, 3, FSI_SLAVE_BASE + FSI_SMODE,
				&smode, sizeof(smode));
}

static ssize_t fsi_slave_sysfs_raw_read(struct file *file,
		struct kobject *kobj, struct bin_attribute *attr, char *buf,
		loff_t off, size_t count)
{
	struct fsi_slave *slave = to_fsi_slave(kobj_to_dev(kobj));
	int rc;

	if (count != 4 || off & 0x3)
		return -EINVAL;

	if (off > 0xfffffffc || off < 0)
		return -EINVAL;

	rc = fsi_slave_read(slave, off, buf, 4);

	return rc ? rc : count;
}

static ssize_t fsi_slave_sysfs_raw_write(struct file *file,
		struct kobject *kobj, struct bin_attribute *attr,
		char *buf, loff_t off, size_t count)
{
	struct fsi_slave *slave = to_fsi_slave(kobj_to_dev(kobj));
	int rc;

	if (count != 4 || off & 0x3)
		return -EINVAL;

	if (off > 0xfffffffc || off < 0)
		return -EINVAL;

	rc = fsi_slave_write(slave, off, buf, 4);

	return rc ? rc : count;
}


static struct bin_attribute fsi_slave_raw_attr = {
	.attr = {
		.name = "raw",
		.mode = S_IRUSR | S_IWUSR,
	},
	.size = 0,
	.read = fsi_slave_sysfs_raw_read,
	.write = fsi_slave_sysfs_raw_write,
};

static int fsi_slave_irq_clear(struct fsi_slave *slave)
{
	uint32_t clear = ~0;
	int rc;

	rc = fsi_slave_write(slave, FSI_SLAVE_BASE + FSI_SRSIC0, &clear,
				sizeof(clear));
	if (rc) {
		dev_dbg(&slave->dev, "Failed on write to SRSIC0\n");
		return rc;
	}
	return fsi_slave_write(slave, FSI_SLAVE_BASE + FSI_SRSIC1, &clear,
				sizeof(clear));
}

static int fsi_slave_init(struct fsi_master *master,
		int link, uint8_t slave_id)
{
	struct fsi_slave *slave;
	uint32_t chip_id;
	int rc;
	uint8_t crc;

	/*
	 * todo: Due to CFAM hardware issues related to BREAK commands we're
	 * limited to only one CFAM per link.  Once issues are resolved this
	 * restriction can be removed.
	 */
	if (slave_id > 0)
		return 0;

	rc = fsi_slave_set_smode(master, link, slave_id);
	if (rc) {
		dev_warn(master->dev, "can't set smode on slave:%02x:%02x %d\n",
				link, slave_id, rc);
		return -ENODEV;
	}

	rc = master->read(master, link, slave_id, 0, &chip_id, sizeof(chip_id));
	if (rc) {
		dev_warn(master->dev, "can't read slave %02x:%02x: %d\n",
				link, slave_id, rc);
		return -ENODEV;
	}
	crc = fsi_crc4(0, chip_id >> FSI_SLAVE_CONF_CRC_SHIFT,
			FSI_SLAVE_CONF_DATA_BITS);
	if (crc != (chip_id & FSI_SLAVE_CONF_CRC_MASK)) {
		dev_warn(master->dev, "slave %02x:%02x invalid chip id CRC!\n",
				link, slave_id);
		return -EIO;
	}

	pr_debug("fsi: found chip %08x at %02x:%02x:%02x\n",
			master->idx, chip_id, link, slave_id);

	/* we can communicate with a slave; create devices and scan */
	slave = kzalloc(sizeof(*slave), GFP_KERNEL);
	if (!slave)
		return -ENOMEM;

	slave->master = master;
	slave->id = slave_id;
	slave->dev.parent = master->dev;
	slave->dev.release = fsi_slave_release;
	slave->link = link;

	dev_set_name(&slave->dev, "slave@%02x:%02x", link, slave_id);
	rc = device_register(&slave->dev);
	if (rc < 0) {
		dev_warn(master->dev, "failed to create slave device: %d\n",
				rc);
		put_device(&slave->dev);
		return rc;
	}

	rc = device_create_bin_file(&slave->dev, &fsi_slave_raw_attr);
	if (rc)
		dev_warn(&slave->dev, "failed to create raw attr: %d\n", rc);

	list_add(&slave->list_link, &master->my_slaves);
	rc = fsi_slave_scan(slave);
	if (rc)
		return rc;

	return fsi_slave_irq_clear(slave);
}

/* FSI master support */

static int fsi_master_link_enable(struct fsi_master *master, int link)
{
	if (master->link_enable)
		return master->link_enable(master, link);

	return 0;
}

/*
 * Issue a break command on this link
 */
static int fsi_master_break(struct fsi_master *master, int link)
{
	if (master->send_break)
		return master->send_break(master, link);

	return 0;
}

void fsi_master_handle_error(struct fsi_master *master, uint32_t addr)
{
	uint32_t smode = FSI_SLAVE_SMODE_DFLT;
	static atomic_t in_err_cleanup = ATOMIC_INIT(-1);

	if (!atomic_inc_and_test(&in_err_cleanup))
		return;

	fsi_master_break(master, 0);
	udelay(200);
	master->write(master, 0, 0, FSI_SLAVE_BASE + FSI_SMODE, &smode,
			sizeof(smode));
	smode = FSI_MRESB_RST_GEN | FSI_MRESB_RST_ERR;
	master->write(master, 0, 0, FSI_HUB_CONTROL + FSI_MRESB0, &smode,
			sizeof(smode));

	if (addr > FSI_HUB_LINK_OFFSET) {
		smode = FSI_BREAK;
		master->write(master, 0, 0, 0x100004, &smode, sizeof(smode));
		smode = FSI_SLAVE_SMODE_DFLT;
		master->write(master, 0, 0, 0x100800, &smode, sizeof(smode));
	}

	atomic_set(&in_err_cleanup, -1);
}
EXPORT_SYMBOL(fsi_master_handle_error);

static int fsi_master_scan(struct fsi_master *master)
{
	int link, slave_id, rc;
	uint32_t smode;

	if (!master->slave_list) {
		INIT_LIST_HEAD(&master->my_slaves);
		master->slave_list = true;
	}

	for (link = 0; link < master->n_links; link++) {
		rc = fsi_master_link_enable(master, link);
		if (rc) {
			dev_dbg(master->dev,
				"enable link:%d failed with:%d\n", link, rc);
			continue;
		}
		rc = fsi_master_break(master, link);
		if (rc) {
			dev_dbg(master->dev,
				"Break to link:%d failed with:%d\n", link, rc);
			continue;
		}

		/*
		 * Verify can read slave at default ID location. If fails
		 * there must be nothing on other end of link
		 */
		rc = master->read(master, link, 3, FSI_SLAVE_BASE + FSI_SMODE,
				&smode, sizeof(smode));
		if (rc) {
			dev_dbg(master->dev,
				"Read link:%d smode default id failed:%d\n",
				link, rc);
			continue;
		}

		for (slave_id = 0; slave_id < FSI_N_SLAVES; slave_id++)
			fsi_slave_init(master, link, slave_id);

	}

	return 0;
}

static ssize_t fsi_ipoll_period_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE - 1, "%u\n", fsi_ipoll_period_ms);
}

static ssize_t fsi_ipoll_period_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	int rc;
	unsigned long val = 0;

	rc = kstrtoul(buf, 0, &val);

	if (val > 1 && val < 10000)
	fsi_ipoll_period_ms = val;

	return count;
}

DEVICE_ATTR(fsi_ipoll_period, S_IRUGO | S_IWUSR, fsi_ipoll_period_show,
		fsi_ipoll_period_store);

static int fsi_unregister_hubs(struct device *dev, void *data)
{
	struct fsi_master_hub *hub = dev_get_drvdata(dev);

	if (!hub)
		return 0;

	ida_simple_remove(&master_ida, hub->master.idx);
	hub->master.idx = -1;
	device_remove_file(dev, &dev_attr_fsi_ipoll_period);
	fsi_master_unscan(&hub->master);
	device_del(dev);

	return 0;
}

static void fsi_master_unscan(struct fsi_master *master)
{
	struct fsi_slave *slave, *slave_tmp;
	struct fsi_device *fsi_dev, *fsi_dev_tmp;

	if (!master->slave_list)
		return;

	list_for_each_entry_safe(slave, slave_tmp, &master->my_slaves,
							list_link) {
		list_del(&slave->list_link);
		list_for_each_entry_safe(fsi_dev, fsi_dev_tmp,
					&slave->my_engines, link) {
			list_del(&fsi_dev->link);
			device_del(&fsi_dev->dev);
			put_device(&fsi_dev->dev);
		}
		/* Remove any hub masters */
		device_for_each_child(&slave->dev, NULL, fsi_unregister_hubs);
		device_unregister(&slave->dev);
	}
	master->slave_list = false;
}

/* TODO: Add support for hub links 4-7 */
static int next_hublink_source(struct fsi_slave *slave, uint32_t srsis)
{
	int index;

	if (!slave)
		return -EINVAL;

	if (!(srsis & FSI_SRSIX_IRQ1_MASK)) {
		dev_dbg(&slave->dev, "Unexpected IRQ source SRSIS:0x%08x\n",
			srsis);
		return -EINVAL;
	}

	/*
	 * TODO: add a fair scheduler to ensure we don't favor lower
	 * hublink IRQ sources over others
	 */
	index = __clz(srsis);
	dev_dbg(&slave->dev, "SRSIS:0x%08x index:%d\n", srsis, index);
	return index / FSI_SRSIX_BITS_PER_LINK;
}

static int __fsi_dev_irq(struct device *dev, void *data);

static int __fsi_hub_slave_irq(struct device *dev, void *data)
{
	int rc;
	struct fsi_slave *hub_slave = to_fsi_slave(dev);
	uint32_t si1s;

	if (!hub_slave) {
		dev_dbg(dev, "Could not find hub slave\n");
		return -ENODEV;
	}

	rc = fsi_slave_read(hub_slave, FSI_SLAVE_BASE + FSI_SI1S, &si1s,
			sizeof(si1s));
	if (rc) {
		dev_dbg(dev, "Fail on read of hub slave si1s\n");
		return rc;
	}

	if (!si1s)
		return 0;

	return device_for_each_child(dev, &si1s, __fsi_dev_irq);
}

static int __fsi_dev_irq(struct device *dev, void *data)
{
	uint32_t *si1s = data, srsis;
	struct fsi_device *fsi_dev = to_fsi_dev(dev);
	struct fsi_slave *slave;
	struct fsi_master_hub *hub;
	int rc, hublink;

	if (!fsi_dev || !si1s) {
		dev_dbg(dev, "Invalid input: %p %p\n", fsi_dev, si1s);
		return -EINVAL;
	}

	if (*si1s & (0x80000000 >> fsi_dev->si1s_bit) && fsi_dev->irq_handler) {
		fsi_dev->irq_handler(0, &fsi_dev->dev);
		return 1;
	}

	if (!(*si1s & FSI_SI1_HUB_SRC)) {
		dev_dbg(dev, "IRQ not from a hub source\n");
		return 0;
	}

	hub = dev_get_drvdata(dev);
	if (!hub) {
		dev_dbg(dev, "Not a hub device\n");
		return 0;
	}

	/* Scan the hub links for the source of IRQ */
	slave = to_fsi_slave(dev->parent);
	if (!slave) {
		dev_dbg(dev, "Could not retrieve device's slave\n");
		return -ENODEV;
	}

	rc = fsi_slave_read(slave, FSI_SLAVE_BASE + FSI_SRSIS0, &srsis,
			sizeof(srsis));
	if (rc) {
		dev_dbg(&slave->dev, "Failed to read SRSIS0\n");
		return rc;
	}
	if (srsis) {
		hublink = next_hublink_source(slave, srsis);

		if (!hub->master.dev)
			return 0;

		device_for_each_child(dev, &hublink, __fsi_hub_slave_irq);

		/* Clear out the interrupting condition */
		srsis = 0xff000000 >> (hublink * FSI_SRSIX_BITS_PER_LINK);
		rc =  fsi_slave_write(slave, FSI_SLAVE_BASE + FSI_SRSIC0,
					&srsis, sizeof(srsis));
		if (rc) {
			dev_dbg(&slave->dev, "Failed to clear out SRSIC\n");
			return rc;
		}
	} else {
		dev_dbg(&slave->dev, "SI1S HUB src but no SRSIS0 bits!\n");
		return -EINVAL;
	}

	return 1;
}

static int __fsi_slave_irq(struct device *dev, void *data)
{
	return device_for_each_child(dev, data, __fsi_dev_irq);
}

static void fsi_master_irq(struct fsi_master *master, int link, uint32_t si1s)
{
	device_for_each_child(master->dev, &si1s, __fsi_slave_irq);
}

static int fsi_master_ipoll(void *data)
{
	int rc;
	uint32_t si1s;
	unsigned long elapsed = 0;
	unsigned long previous_jiffies = jiffies;
	struct fsi_master *master = data;

	while (!kthread_should_stop()) {
		if (!master->ipoll)
			goto done;

		/* Ignore errors for now */
		rc = master->read(master, 0, 0, FSI_SLAVE_BASE + FSI_SI1S,
				  &si1s, sizeof(uint32_t));
		if (rc)
			goto done;

		if (si1s & master->ipoll)
			fsi_master_irq(master, 0, si1s);
done:
		elapsed = jiffies - previous_jiffies;
		if (elapsed < FSI_IPOLL_PERIOD) {
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout(FSI_IPOLL_PERIOD - elapsed);
		}
		previous_jiffies = jiffies;
	}

	return 0;
}

int fsi_master_register(struct fsi_master *master)
{
	if (!master || !master->dev)
		return -EINVAL;

	master->idx = ida_simple_get(&master_ida, 0, INT_MAX, GFP_KERNEL);
	master->slave_list = false;
	get_device(master->dev);
	fsi_master_scan(master);
	device_create_file(master->dev, &dev_attr_fsi_ipoll_period);
	return 0;
}
EXPORT_SYMBOL_GPL(fsi_master_register);

void fsi_master_unregister(struct fsi_master *master)
{
	if (!master || !master->dev || master->idx < 0)
		return;

	ida_simple_remove(&master_ida, master->idx);
	master->idx = -1;
	device_remove_file(master->dev, &dev_attr_fsi_ipoll_period);
	fsi_master_unscan(master);
	put_device(master->dev);
	if (master_ipoll) {
		kthread_stop(master_ipoll);
		master_ipoll = NULL;
	}
}
EXPORT_SYMBOL_GPL(fsi_master_unregister);

/*
 * TODO: move this to master->start_ipoll( ) -each master may have its
 * own way of doing this
 */
int fsi_master_start_ipoll(struct fsi_master *master)
{
	if (master_ipoll) {
		dev_err(master->dev, "Already polling for irqs\n");
		return -EALREADY;
	}
	master_ipoll = kthread_create(fsi_master_ipoll, master,
				"fsi_master_ipoll");
	if (IS_ERR(master_ipoll)) {
		dev_err(master->dev, "Couldn't create ipoll thread rc:%d\n",
			(int)PTR_ERR(master_ipoll));
		return PTR_ERR(master_ipoll);
	}
	wake_up_process(master_ipoll);

	return 0;
}
EXPORT_SYMBOL_GPL(fsi_master_start_ipoll);

/* FSI core & Linux bus type definitions */

static int fsi_bus_match(struct device *dev, struct device_driver *drv)
{
	struct fsi_device *fsi_dev = to_fsi_dev(dev);
	struct fsi_driver *fsi_drv = to_fsi_drv(drv);
	const struct fsi_device_id *id;

	if (!fsi_drv->id_table)
		return 0;

	for (id = fsi_drv->id_table; id->engine_type; id++) {
		if (id->engine_type != fsi_dev->engine_type)
			continue;
		if (id->version == FSI_VERSION_ANY ||
				id->version == fsi_dev->version)
			return 1;
	}

	return 0;
}

int fsi_driver_register(struct fsi_driver *fsi_drv)
{
	if (!fsi_drv)
		return -EINVAL;
	if (!fsi_drv->id_table)
		return -EINVAL;

	return driver_register(&fsi_drv->drv);
}
EXPORT_SYMBOL_GPL(fsi_driver_register);

void fsi_driver_unregister(struct fsi_driver *fsi_drv)
{
	driver_unregister(&fsi_drv->drv);
}
EXPORT_SYMBOL_GPL(fsi_driver_unregister);

static uint32_t link_to_srsim_mask(int link)
{
	return ((0x80000000 >> 6) >> FSI_SRSIX_BITS_PER_LINK*link);
}

static uint32_t link_to_msiep_mask(int link)
{
	return (0xf0000000 >> (FSI_MSIEP_BITS_PER_LINK*link));
}

static int set_si1m(struct fsi_slave *slave, uint32_t mask, int on)
{
	int rc;
	uint32_t si1m;

	rc = fsi_slave_read(slave, FSI_SLAVE_BASE + FSI_SI1M, &si1m,
			sizeof(si1m));
	if (rc) {
		dev_dbg(&slave->dev, "Failed to read SI1M\n");
		return rc;
	}

	if (on)
		si1m |= mask;
	else
		si1m &= ~mask;

	return fsi_slave_write(slave, FSI_SLAVE_BASE + FSI_SI1M, &si1m,
			sizeof(si1m));
}

static int set_upstream_irq_masks(struct fsi_master *master,
				struct fsi_slave *slave, int on)
{
	struct fsi_slave *upstream_slave;
	struct fsi_master *upstream_master;
	uint32_t mask, si1m;
	int rc;

	if (master->idx <= 0)
		return 0;

	upstream_slave = to_fsi_slave(slave->master->dev->parent);
	if (!upstream_slave) {
		dev_dbg(&slave->dev, "No upstream slave found\n");
		return -ENODEV;
	}

	rc = fsi_slave_read(upstream_slave, FSI_SLAVE_BASE + FSI_SRSIM0, &si1m,
			sizeof(si1m));
	if (rc) {
		dev_dbg(&slave->dev, "Failed to read SRSIM0\n");
		return rc;
	}

	mask = link_to_srsim_mask(slave->link);
	if (on)
		si1m |= mask;
	else
		si1m &= ~mask;
	rc = fsi_slave_write(upstream_slave, FSI_SLAVE_BASE + FSI_SRSIM0, &si1m,
			sizeof(si1m));
	if (rc) {
		dev_dbg(&slave->dev, "Failed to write SRSIM0\n");
		return rc;
	}

	upstream_master = upstream_slave->master;
	if (!upstream_master) {
		dev_dbg(&upstream_slave->dev, "Cannot find master\n");
		return -ENODEV;
	}

	rc = upstream_master->read(upstream_master, 0, 0,
				FSI_HUB_CONTROL + FSI_MSIEP0, &si1m,
				sizeof(si1m));
	if (rc) {
		dev_dbg(&upstream_slave->dev,
			"Could not read master's MSIEP\n");
		return rc;
	}

	/* TODO: merge this into above on/off check */
	mask = link_to_msiep_mask(slave->link);
	if (on) {
		upstream_master->ipoll |= FSI_SI1_HUB_SRC;
		si1m |= mask;
	} else {
		upstream_master->ipoll &= ~FSI_SI1_HUB_SRC;
		si1m &= ~mask;
	}

	rc = upstream_master->write(upstream_master, 0, 0,
				FSI_HUB_CONTROL + FSI_MSIEP0, &si1m,
				sizeof(si1m));
	if (rc) {
		dev_dbg(&upstream_slave->dev,
			"Failed to write to master's MSIEP\n");
		return rc;
	}
	si1m = 0xd0040410;
	rc = upstream_master->write(upstream_master, 0, 0,
				FSI_HUB_CONTROL + FSI_MMODE, &si1m,
				sizeof(si1m));
	if (rc) {
		dev_dbg(&upstream_slave->dev,
			"Failed to set hub I POLL\n");
	}

	si1m = FSI_SI1_HUB_SRC;
	rc = upstream_master->write(upstream_master, 0, 0,
				FSI_SLAVE_BASE + FSI_SI1M, &si1m,
				sizeof(si1m));
	if (rc) {
		dev_dbg(&upstream_slave->dev,
			"Failed to set hub mask in SI1M\n");
	}

	return set_si1m(upstream_slave, FSI_SI1_HUB_SRC, on);
}

int fsi_enable_irq(struct fsi_device *dev)
{
	int rc;
	u32 si1m;
	u32 bit = 0x80000000 >> dev->si1s_bit;
	struct fsi_master *master = dev->slave->master;
	struct fsi_slave *slave = dev->slave;
	int link = slave->link;

	if (!dev->irq_handler)
		return -EINVAL;

	rc = master->read(master, link, 0, FSI_SLAVE_BASE + FSI_SI1M, &si1m,
			sizeof(u32));
	if (rc) {
		dev_err(master->dev, "couldn't read si1m:%d\n", rc);
		return rc;
	}

	si1m |= bit;
	rc = master->write(master, link, 0, FSI_SLAVE_BASE + FSI_SI1M, &si1m,
			sizeof(u32));
	if (rc) {
		dev_err(master->dev, "couldn't write si1m:%d\n", rc);
		return rc;
	}

	master->ipoll |= bit;
	return set_upstream_irq_masks(master, slave, 1);
}
EXPORT_SYMBOL_GPL(fsi_enable_irq);

void fsi_disable_irq(struct fsi_device *dev)
{
	int rc;
	u32 si1m;
	u32 bits = ~(0x80000000 >> dev->si1s_bit);
	struct fsi_master *master = dev->slave->master;
	struct fsi_slave *slave = dev->slave;
	int link = dev->slave->link;

	master->ipoll &= bits;

	rc = master->read(master, link, 0, FSI_SLAVE_BASE + FSI_SI1M, &si1m,
			sizeof(u32));
	if (rc) {
		dev_err(master->dev, "couldn't read si1m:%d\n", rc);
		return;
	}

	si1m &= bits;
	rc = master->write(master, link, 0, FSI_SLAVE_BASE + FSI_SI1M, &si1m,
			sizeof(u32));
	if (rc) {
		dev_err(master->dev, "couldn't write si1m:%d\n", rc);
		return;
	}

	if (!master->ipoll)
		set_upstream_irq_masks(master, slave, 0);
}

struct bus_type fsi_bus_type = {
	.name		= "fsi",
	.match		= fsi_bus_match,
};
EXPORT_SYMBOL_GPL(fsi_bus_type);

static int fsi_init(void)
{
	return bus_register(&fsi_bus_type);
}

static void fsi_exit(void)
{
	bus_unregister(&fsi_bus_type);
}

module_init(fsi_init);
module_exit(fsi_exit);
