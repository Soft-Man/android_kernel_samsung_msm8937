/* abov_touchkey.c -- Linux driver for abov chip as touchkey
 *
 * Copyright (C) 2013 Samsung Electronics Co.Ltd
 * Author: Junkyeong Kim <jk0430.kim@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <asm/unaligned.h>
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/consumer.h>

#include <linux/sec_class.h>
#include <linux/async_initcall.h>

#ifdef CONFIG_OF
#include <linux/of_gpio.h>
#endif
#ifdef CONFIG_INPUT_BOOSTER
#include <linux/input/input_booster.h>
#endif

#ifdef CONFIG_TOUCHSCREEN_DUMP_MODE
#include <linux/sec_debug.h>
#endif

#define ABOV_TK_NAME "abov-ft1604"

/* registers */
#define ABOV_BTNSTATUS		0x00
#define ABOV_FW_VER		0x01
#define ABOV_PCB_VER		0x02
#define ABOV_COMMAND		0x03
#define ABOV_THRESHOLD		0x04
#define ABOV_SENS		0x05
#define ABOV_SETIDAC		0x06
#define ABOV_DIFFDATA		0x0A
#define ABOV_RAWDATA		0x0E
#define ABOV_VENDORID		0x12
#define ABOV_GLOVE		0x13
#define ABOV_MD_VER		0x14

/* command */
#define CMD_LED_ON		0x10
#define CMD_LED_OFF		0x20
#define CMD_DATA_UPDATE		0x40
#define CMD_LED_CTRL_ON		0x60
#define CMD_LED_CTRL_OFF	0x70
#define CMD_STOP_MODE		0x80
#define CMD_GLOVE_ON		0x20
#define CMD_GLOVE_OFF		0x10

#define ABOV_BOOT_DELAY		45
#define ABOV_RESET_DELAY	150

static struct device *sec_touchkey;

#define FW_VERSION 0x05
#define FW_CHECKSUM_H 0x9A
#define FW_CHECKSUM_L 0x5A

/* Force FW update if module# is different */
#undef FORCE_FW_UPDATE_DIFF_MODULE

/* Touchkey LED twinkle during booting in factory sw (in LCD detached status) */
#ifdef CONFIG_SEC_FACTORY
#define LED_TWINKLE_BOOTING
#endif


#define TK_FW_PATH_SDCARD "/sdcard/Firmware/TOUCHKEY/abov_fw.bin"

#ifdef LED_TWINKLE_BOOTING
static void led_twinkle_work(struct work_struct *work);
#endif

#define I2C_M_WR 0		/* for i2c */

enum {
	BUILT_IN = 0,
	SDCARD,
};


#define LIGHT_VERSION_PATH		"/efs/FactoryApp/tkey_light_version"
#define LIGHT_TABLE_PATH		"/efs/FactoryApp/tkey_light"
#define LIGHT_CRC_PATH			"/efs/FactoryApp/tkey_light_crc"
#define LIGHT_TABLE_PATH_LEN		50
#define LIGHT_VERSION_LEN		25
#define LIGHT_CRC_SIZE			10
#define LIGHT_DATA_SIZE			5
#define LIGHT_VOLTAGE_MIN_VAL		3000
#define LIGHT_TABLE_MAX			1

struct light_info {
	int octa_id;
	int vol_mv;
};

enum WINDOW_COLOR {
	WINDOW_COLOR_BLACK_UTYPE = 0,
	WINDOW_COLOR_BLACK,
	WINDOW_COLOR_WHITE,
	WINDOW_COLOR_GOLD,
	WINDOW_COLOR_SILVER,
	WINDOW_COLOR_GREEN,
	WINDOW_COLOR_BLUE,
	WINDOW_COLOR_PINKGOLD,
};

#define WINDOW_COLOR_DEFAULT		WINDOW_COLOR_WHITE

/* should be pair table item numbers and table size */
#define LIGHT_VERSION			160907

struct light_info tkey_light_voltage_table[LIGHT_TABLE_MAX] =
{
	/* octa id, voltage */
	{ WINDOW_COLOR_WHITE,		3300},
};
/**************************************************/

#ifdef CONFIG_SAMSUNG_LPM_MODE
extern int poweroff_charging;
#endif
extern int get_lcd_attached(char *mode);
extern unsigned int system_rev;
extern struct class *sec_class;
static int touchkey_keycode[] = { 0,
	KEY_RECENT, KEY_BACK,
};

struct abov_ft1604_info {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct abov_ft1604_devicetree_data *dtdata;
	struct mutex lock;
	struct pinctrl *pinctrl;

	const struct firmware *firm_data_bin;
	const u8 *firm_data_ums;
	char phys[32];
	long firm_size;
	int irq;
	u16 menu_s;
	u16 back_s;
	u16 menu_raw;
	u16 back_raw;
	int (*power) (bool on);
	int touchkey_count;
	u8 fw_update_state;
	u8 fw_ver;
	u8 md_ver;
	u8 checksum_h;
	u8 checksum_l;
	u8 fw_ver_bin;
	u8 md_ver_bin;	
	u8 checksum_h_bin;
	u8 checksum_l_bin;
	bool enabled;
#ifdef GLOVE_MODE
	bool glovemode;
#endif
	bool probe_done;
#ifdef LED_TWINKLE_BOOTING
	struct delayed_work led_twinkle_work;
	bool led_twinkle_check;
#endif
#ifdef CONFIG_INPUT_BOOSTER
	struct input_booster *tkey_booster;
#endif

	struct delayed_work efs_open_work;
	int light_version_efs;
	char light_version_full_efs[LIGHT_VERSION_LEN];
	char light_version_full_bin[LIGHT_VERSION_LEN];
	int light_table_crc;
};

struct abov_ft1604_devicetree_data {
	unsigned long irq_flag;
	int gpio_en;
	int gpio_int;
	int gpio_sda;
	int gpio_scl;
	int gpio_rst;
	int gpio_tkey_led_en;
	int vdd_io_alwayson;
	int bringup;
	struct regulator *vdd_io_vreg;
	struct regulator *avdd_vreg;
	struct regulator *vdd_led;
	const char *fw_name;
	int (*power) (struct abov_ft1604_devicetree_data *dtdata, bool on);
	int (*keyled) (bool on);
	bool reverse_key;
};


static int abov_tk_input_open(struct input_dev *dev);
static void abov_tk_input_close(struct input_dev *dev);

static int abov_tk_i2c_read_checksum(struct abov_ft1604_info *info);
static int efs_read_light_table_version(struct abov_ft1604_info *info);

static int abov_touchkey_led_status;
static int abov_touchled_cmd_reserved;

#ifdef GLOVE_MODE
static int abov_glove_mode_enable(struct i2c_client *client, u8 cmd)
{
	return i2c_smbus_write_byte_data(client, ABOV_GLOVE, cmd);
}
#endif

static void change_touch_key_led_voltage(struct device *dev, int vol_mv)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);	
	int ret;

	ret = regulator_set_voltage(info->dtdata->vdd_led, vol_mv * 1000, vol_mv * 1000);
	if (ret)
		input_err(true, dev, "%s: failed to set key led %d mv, %d\n",
				__func__, vol_mv, ret);
	
	input_info(true, dev, "%s: %dmV\n", __func__, vol_mv);
}

static ssize_t brightness_control(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);
	int data;

	if (sscanf(buf, "%d\n", &data) == 1)
		change_touch_key_led_voltage(&info->client->dev, data);
	else
		input_err(true, &info->client->dev, "%s Error\n", __func__);

	return size;
}

static int read_window_type(void)
{
	struct file *type_filp = NULL;
	mm_segment_t old_fs;
	int ret = 0;
	char window_type[10] = {0, };

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	type_filp = filp_open("/sys/class/lcd/panel/window_type", O_RDONLY, 0440);
	if (IS_ERR(type_filp)) {
		set_fs(old_fs);
		ret = PTR_ERR(type_filp);
		return ret;
	}

	ret = type_filp->f_op->read(type_filp, window_type,
			sizeof(window_type), &type_filp->f_pos);
	if (ret != 9 * sizeof(char)) {
		pr_err("%s touchkey %s: fd read fail\n", SECLOG, __func__);
		ret = -EIO;
		return ret;
	}

	filp_close(type_filp, current->files);
	set_fs(old_fs);

	if (window_type[1] < '0' || window_type[1] >= 'f')
		return -EAGAIN;

	ret = (window_type[1] - '0') & 0x0f;
	pr_info("%s touchkey %s: %d\n", SECLOG, __func__, ret);
	return ret;
}

static int efs_calculate_crc (struct abov_ft1604_info *info)
{
	struct file *temp_file = NULL;
	int crc = info->light_version_efs;
	mm_segment_t old_fs;
	char predefine_value_path[LIGHT_TABLE_PATH_LEN];
	int ret = 0, i;
	char temp_vol[LIGHT_CRC_SIZE] = {0, };
	int table_size;

	efs_read_light_table_version(info);
	table_size = (int)strlen(info->light_version_full_efs) - 8;

	for (i = 0; i < table_size; i++) {
		char octa_temp = info->light_version_full_efs[8 + i];
		int octa_temp_i;

		if (octa_temp >= 'A')
			octa_temp_i = octa_temp - 'A' + 0x0A;
		else
			octa_temp_i = octa_temp - '0';
		
		input_info(true, &info->client->dev, "%s: octa %d\n", __func__, octa_temp_i);

		snprintf(predefine_value_path, LIGHT_TABLE_PATH_LEN, "%s%d",
				LIGHT_TABLE_PATH, octa_temp_i);
		old_fs = get_fs();
		set_fs(KERNEL_DS);
		temp_file = filp_open(predefine_value_path, O_RDONLY, 0440);
		if (!IS_ERR(temp_file)) {
			temp_file->f_op->read(temp_file, temp_vol,
					sizeof(temp_vol), &temp_file->f_pos);
			filp_close(temp_file, current->files);
			if (kstrtoint(temp_vol, 0, &ret) < 0) {
				ret = -EIO;
			} else {
				crc += octa_temp_i;
				crc += ret;
				ret = 0;
			}
		}
		set_fs(old_fs);
	}

	if (!ret)
		ret = crc;

	return ret;
}

static int efs_read_crc(struct abov_ft1604_info *info)
{
	struct file *temp_file = NULL;
	char crc[LIGHT_CRC_SIZE] = {0, };
	mm_segment_t old_fs;
	int ret = 0;
	
	old_fs = get_fs();
	set_fs(KERNEL_DS);
	temp_file = filp_open(LIGHT_CRC_PATH, O_RDONLY, 0440);
	if (IS_ERR(temp_file)) {
		ret = PTR_ERR(temp_file);
		input_info(true, &info->client->dev,
				"%s: failed to open efs file %d\n", __func__, ret);
	} else {
		temp_file->f_op->read(temp_file, crc, sizeof(crc), &temp_file->f_pos);
		filp_close(temp_file, current->files);
		if (kstrtoint(crc, 0, &ret) < 0)
			ret = -EIO;
	}
	set_fs(old_fs);

	return ret;
}


static bool check_light_table_crc(struct abov_ft1604_info *info)
{
	int crc_efs = efs_read_crc(info);

	if (info->light_version_efs == LIGHT_VERSION) {
		/* compare efs crc file with binary crc*/
		input_info(true, &info->client->dev,
				"%s: efs:%d, bin:%d\n",
				__func__, crc_efs, info->light_table_crc);
		if (crc_efs != info->light_table_crc)
			return false;
	}

	return true;
}

static int efs_write_light_table_crc(struct abov_ft1604_info *info, int crc_cal)
{
	struct file *temp_file = NULL;
	char crc[LIGHT_CRC_SIZE] = {0, };
	mm_segment_t old_fs;
	int ret = 0;

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	temp_file = filp_open(LIGHT_CRC_PATH, O_TRUNC | O_RDWR | O_CREAT, 0660);
	if (IS_ERR(temp_file)) {
		ret = PTR_ERR(temp_file);
		input_info(true, &info->client->dev,
				"%s: failed to open efs file %d\n", __func__, ret);
	} else {
		snprintf(crc, sizeof(crc), "%d", crc_cal);
		temp_file->f_op->write(temp_file, crc, sizeof(crc), &temp_file->f_pos);
		filp_close(temp_file, current->files);
		input_info(true, &info->client->dev, "%s: %s\n", __func__, crc);
	}
	set_fs(old_fs);
	return ret;
}

static int efs_write_light_table_version(struct abov_ft1604_info *info, char *full_version)
{
	struct file *temp_file = NULL;
	mm_segment_t old_fs;
	int ret = 0;

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	temp_file = filp_open(LIGHT_VERSION_PATH, O_TRUNC | O_RDWR | O_CREAT, 0660);
	if (IS_ERR(temp_file)) {
		ret = -ENOENT;
	} else {
		temp_file->f_op->write(temp_file, full_version,
				LIGHT_VERSION_LEN, &temp_file->f_pos);
		filp_close(temp_file, current->files);
		input_info(true, &info->client->dev, "%s: version = %s\n",
				__func__, full_version);
	}
	set_fs(old_fs);
	return ret;
}

static int efs_write_light_table(struct abov_ft1604_info *info, struct light_info table)
{
	struct file *type_filp = NULL;
	mm_segment_t old_fs;
	int ret = 0;
	char predefine_value_path[LIGHT_TABLE_PATH_LEN];
	char vol_mv[LIGHT_DATA_SIZE] = {0, };

	snprintf(predefine_value_path, LIGHT_TABLE_PATH_LEN,
			"%s%d", LIGHT_TABLE_PATH, table.octa_id);
	snprintf(vol_mv, sizeof(vol_mv), "%d", table.vol_mv);

	input_info(true, &info->client->dev, "%s: make %s\n", __func__, predefine_value_path);

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	type_filp = filp_open(predefine_value_path, O_TRUNC | O_RDWR | O_CREAT, 0660);
	if (IS_ERR(type_filp)) {
		set_fs(old_fs);
		ret = PTR_ERR(type_filp);
		input_err(true, &info->client->dev, "%s: open fail :%d\n",
			__func__, ret);
		return ret;
	}

	type_filp->f_op->write(type_filp, vol_mv, sizeof(vol_mv), &type_filp->f_pos);
	filp_close(type_filp, current->files);
	set_fs(old_fs);

	return ret;
}

static int efs_write(struct abov_ft1604_info *info)
{
	int ret = 0;
	int i, crc_cal;

	ret = efs_write_light_table_version(info, info->light_version_full_bin);
	if (ret < 0)
		return ret;
	info->light_version_efs = LIGHT_VERSION;

	for (i = 0; i < LIGHT_TABLE_MAX; i++) {
		ret = efs_write_light_table(info, tkey_light_voltage_table[i]);
		if (ret < 0)
			break;
	}
	if (ret < 0)
		return ret;

	crc_cal = efs_calculate_crc(info);
	if (crc_cal < 0)
		return crc_cal;

	ret = efs_write_light_table_crc(info, crc_cal);
	if (ret < 0)
		return ret;

	if (!check_light_table_crc(info))
		ret = -EIO;

	return ret;
}

static int pick_light_table_version(char* str)
{
	static char* str_addr;
	char* token = NULL;
	int ret = 0;
	
	if (str != NULL)
		str_addr = str;
	else if (str_addr == NULL)
		return 0;

	token = str_addr;
	while (true) {
		if (*str_addr == 'T') {
			*str_addr = '0';
		} else if (*str_addr == '.') {
			*str_addr = '\0';
			str_addr = str_addr + 1;
			break;
		} else if (str_addr == NULL) {
			break;
		}

		str_addr++;
	}

	if (kstrtoint(token + 1, 0, &ret) < 0)
		return 0;

	return ret;
}


static int efs_read_light_table_version(struct abov_ft1604_info *info)
{
	struct file *temp_file = NULL;
	char version[LIGHT_VERSION_LEN] = {0, };
	mm_segment_t old_fs;
	int ret = 0;
	
	old_fs = get_fs();
	set_fs(KERNEL_DS);
	temp_file = filp_open(LIGHT_VERSION_PATH, O_RDONLY, 0440);
	if (IS_ERR(temp_file)) {
		ret = PTR_ERR(temp_file);
	} else {
		temp_file->f_op->read(temp_file, version, sizeof(version), &temp_file->f_pos);
		filp_close(temp_file, current->files);
		input_info(true, &info->client->dev,
				"%s: table full version = %s\n", __func__, version);
		snprintf(info->light_version_full_efs,
				sizeof(info->light_version_full_efs), version);
		info->light_version_efs = pick_light_table_version(version);
		input_dbg(true, &info->client->dev,
				"%s: table version = %d\n", __func__, info->light_version_efs);
	}
	set_fs(old_fs);

	return ret;
}

static int efs_read_light_table(struct abov_ft1604_info *info, int octa_id)
{
	struct file *type_filp = NULL;
	mm_segment_t old_fs;
	char predefine_value_path[LIGHT_TABLE_PATH_LEN];
	char voltage[LIGHT_DATA_SIZE] = {0, };
	int ret;

	snprintf(predefine_value_path, LIGHT_TABLE_PATH_LEN,
		"%s%d", LIGHT_TABLE_PATH, octa_id);

	input_info(true, &info->client->dev, "%s: %s\n", __func__, predefine_value_path);

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	type_filp = filp_open(predefine_value_path, O_RDONLY, 0440);
	if (IS_ERR(type_filp)) {
		set_fs(old_fs);
		ret = PTR_ERR(type_filp);
		input_err(true, &info->client->dev,
				"%s: fail to open light data %d\n", __func__, ret);
		return ret;
	}

	type_filp->f_op->read(type_filp, voltage, sizeof(voltage), &type_filp->f_pos);
	filp_close(type_filp, current->files);
	set_fs(old_fs);

	if (kstrtoint(voltage, 0, &ret) < 0)
		return -EIO;

	return ret;
}

static int efs_read_light_table_with_default(struct abov_ft1604_info *info, int octa_id)
{
	bool set_default = false;
	int ret;

retry:
	if (set_default)
		octa_id = WINDOW_COLOR_DEFAULT;

	ret = efs_read_light_table(info, octa_id);
	if (ret < 0) {
		if (!set_default) {
			set_default = true;
			goto retry;
		}
	}

	return ret;
}

static bool need_update_light_table(struct abov_ft1604_info *info)
{
	/* Check version file exist*/
	if (efs_read_light_table_version(info) < 0) {
		return true;
	}

	/* Compare version */
	input_info(true, &info->client->dev,
			"%s: efs:%d, bin:%d\n", __func__,
			info->light_version_efs, LIGHT_VERSION);
	if (info->light_version_efs < LIGHT_VERSION)
		return true;

	/* Check CRC */
	if (!check_light_table_crc(info)) {
		input_info(true, &info->client->dev,
				"%s: crc is diffrent\n", __func__);
		return true;
	}

	return false;
}

static void touchkey_efs_open_work(struct work_struct *work)
{
	struct abov_ft1604_info *info =
			container_of(work, struct abov_ft1604_info, efs_open_work.work);
	int window_type;
	static int count = 0;
	int vol_mv;

	if (need_update_light_table(info)) {
		if (efs_write(info) < 0)
			goto out;
	}

	window_type = read_window_type();
	if (window_type < 0)
		goto out;

	vol_mv = efs_read_light_table_with_default(info, window_type);
	if (vol_mv >= LIGHT_VOLTAGE_MIN_VAL) {
		change_touch_key_led_voltage(&info->client->dev, vol_mv);
		input_info(true, &info->client->dev,
				"%s: read done for %d\n", __func__, window_type);
	} else {
		input_err(true, &info->client->dev,
				"%s: fail. voltage is %d\n", __func__, vol_mv);
	}
	return;

out:
	if (count < 50) {
		schedule_delayed_work(&info->efs_open_work, msecs_to_jiffies(2000));
		count++;
 	} else {
		input_err(true, &info->client->dev,
				"%s: retry %d times but can't check efs\n", __func__, count);
 	}
}

static int abov_tk_i2c_read(struct i2c_client *client,
		u8 reg, u8 *val, unsigned int len)
{
	struct abov_ft1604_info *info = i2c_get_clientdata(client);
	struct i2c_msg msg;
	int ret;
	int retry = 3;

	mutex_lock(&info->lock);
	msg.addr = client->addr;
	msg.flags = I2C_M_WR;
	msg.len = 1;
	msg.buf = &reg;
	while (retry--) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret >= 0)
			break;

		input_err(true, &client->dev, "%s fail(address set)(%d)\n",
			__func__, retry);
		usleep_range(10 * 1000, 10 * 1000);
	}
	if (ret < 0) {
		mutex_unlock(&info->lock);
		return ret;
	}
	retry = 3;
	msg.flags = 1;/*I2C_M_RD*/
	msg.len = len;
	msg.buf = val;
	while (retry--) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret >= 0) {
			mutex_unlock(&info->lock);
			return 0;
		}
		input_err(true, &client->dev, "%s fail(data read)(%d)\n",
			__func__, retry);
		usleep_range(10 * 1000, 10 * 1000);
	}
	mutex_unlock(&info->lock);
	return ret;
}

static int abov_tk_i2c_write(struct i2c_client *client,
		u8 reg, u8 *val, unsigned int len)
{
	struct abov_ft1604_info *info = i2c_get_clientdata(client);
	struct i2c_msg msg[1];
	unsigned char data[2];
	int ret;
	int retry = 3;

	mutex_lock(&info->lock);
	data[0] = reg;
	data[1] = *val;
	msg->addr = client->addr;
	msg->flags = I2C_M_WR;
	msg->len = 2;
	msg->buf = data;

	while (retry--) {
		ret = i2c_transfer(client->adapter, msg, 1);
		if (ret >= 0) {
			mutex_unlock(&info->lock);
			return 0;
		}
		input_err(true, &client->dev, "%s fail(%d)\n",
			__func__, retry);
		usleep_range(10 * 1000, 10 * 1000);
	}
	mutex_unlock(&info->lock);
	return ret;
}

static void release_all_fingers(struct abov_ft1604_info *info)
{
	struct i2c_client *client = info->client;
	int i;

	input_dbg(true, &client->dev, "[TK] %s\n", __func__);

	for (i = 1; i < info->touchkey_count; i++) {
		input_report_key(info->input_dev,
			touchkey_keycode[i], 0);
	}
	input_sync(info->input_dev);

#ifdef CONFIG_INPUT_BOOSTER
	if (info->tkey_booster && info->tkey_booster->dvfs_set)
		info->tkey_booster->dvfs_set(info->tkey_booster, 2);
#endif
}

static int abov_tk_reset_for_bootmode(struct abov_ft1604_info *info)
{
	if (gpio_get_value(info->dtdata->gpio_en)) {
		gpio_direction_output(info->dtdata->gpio_en, 0);
		msleep(50);
	}

	gpio_direction_output(info->dtdata->gpio_en, 1);

	return 0;
}

static void abov_tk_reset(struct abov_ft1604_info *info)
{
	struct i2c_client *client = info->client;

	if (info->enabled == false)
		return;

	input_info(true, &client->dev, "%s++\n", __func__);
	disable_irq_nosync(info->irq);

	info->enabled = false;

	release_all_fingers(info);

	abov_tk_reset_for_bootmode(info);
	msleep(ABOV_RESET_DELAY);

#ifdef GLOVE_MODE
	if (info->glovemode)
		abov_glove_mode_enable(client, CMD_GLOVE_ON);
#endif

	info->enabled = true;

	enable_irq(info->irq);
	input_info(true, &client->dev, "%s--\n", __func__);
}

static irqreturn_t abov_tk_interrupt(int irq, void *dev_id)
{
	struct abov_ft1604_info *info = dev_id;
	struct i2c_client *client = info->client;
	int ret, retry;
	u8 buf;
	bool press;
	int menu_data;
	int back_data;
	u8 menu_press;
	u8 back_press;

	ret = abov_tk_i2c_read(client, ABOV_BTNSTATUS, &buf, 1);
	if (ret < 0) {
		retry = 3;
		while (retry--) {
			input_err(true, &client->dev, "%s read fail(%d)\n",
				__func__, retry);
			ret = abov_tk_i2c_read(client, ABOV_BTNSTATUS, &buf, 1);
			if (ret == 0)
				break;
			else
				usleep_range(10 * 1000, 10 * 1000);
		}
		if (retry == 0) {
			abov_tk_reset(info);
			return IRQ_HANDLED;
		}
	}

	// added dual key mode concept for screen pinning(google)
	
	menu_data = buf & 0x03;
	back_data = (buf >> 2) & 0x03;

	if (info->dtdata->reverse_key) {
		u8 tmp;
		tmp = menu_data;
		menu_data = back_data;
		back_data = tmp;
	}

	menu_press = !(menu_data % 2);
	back_press = !(back_data % 2);

	press = (menu_data ? menu_press : 0) || (back_data ? back_press : 0);

	if (menu_data)
		input_report_key(info->input_dev,
			touchkey_keycode[1], menu_press);
	if (back_data)
		input_report_key(info->input_dev,
			touchkey_keycode[2], back_press);

#ifdef CONFIG_SAMSUNG_PRODUCT_SHIP
	input_info(true, &client->dev,
		"key %s%s ver0x%02x\n",
		menu_data ? (menu_press ? "P" : "R") : "",
		back_data ? (back_press ? "P" : "R") : "",
		info->fw_ver);
#else
	input_info(true, &client->dev,
		"%s%s%x ver0x%02x\n",
		menu_data ? (menu_press ? "menu P " : "menu R ") : "",
		back_data ? (back_press ? "back P " : "back R ") : "",
		buf, info->fw_ver);
#endif

#if 0 // old concept
	button = buf & 0x03;
	press = !!(buf & 0x8);

	if (press) {
		input_report_key(info->input_dev,
			touchkey_keycode[button], 0);
#ifdef CONFIG_SAMSUNG_PRODUCT_SHIP
		dev_notice(&client->dev,
			"key R\n");
#else
		dev_notice(&client->dev,
			"key R : %d(%d) ver0x%02x\n",
			touchkey_keycode[button], buf, info->fw_ver);
#endif
	} else {
		input_report_key(info->input_dev,
			touchkey_keycode[button], 1);
#ifdef CONFIG_SAMSUNG_PRODUCT_SHIP
		dev_notice(&client->dev,
			"key P\n");
#else
		dev_notice(&client->dev,
			"key P : %d(%d)\n",
			touchkey_keycode[button], buf);
#endif
	}
#endif
	input_sync(info->input_dev);

#ifdef CONFIG_INPUT_BOOSTER
	if (info->tkey_booster && info->tkey_booster->dvfs_set)
		info->tkey_booster->dvfs_set(info->tkey_booster, press);
#endif

	return IRQ_HANDLED;
}

#ifdef CONFIG_INPUT_BOOSTER
static ssize_t boost_level_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);
	int val, stage;
	struct dvfs value;

	if (!info->tkey_booster) {
		input_err(true, &info->client->dev,
			"%s: booster is NULL\n", __func__);
		return count;
	}

	sscanf(buf, "%d", &val);

	stage = 1 << val;

	if (!(info->tkey_booster->dvfs_stage & stage)) {
		input_info(true, &info->client->dev,
			"%s: wrong cmd %d\n", __func__, val);
		return count;
	}

	info->tkey_booster->dvfs_boost_mode = stage;
	input_info(true, &info->client->dev,
			"%s: dvfs_boost_mode = 0x%X\n",
			__func__, info->tkey_booster->dvfs_boost_mode);

	if (info->tkey_booster->dvfs_boost_mode == DVFS_STAGE_NONE) {
		if (info->tkey_booster->dvfs_set)
			info->tkey_booster->dvfs_set(info->tkey_booster, 2);
	} else if (info->tkey_booster->dvfs_boost_mode == DVFS_STAGE_SINGLE) {
		input_booster_get_default_setting("head", &value);
		info->tkey_booster->dvfs_freq = value.cpu_freq;
		input_info(true, &info->client->dev,
			"%s: boost_mode SINGLE, dvfs_freq = %d\n",
			__func__, info->tkey_booster->dvfs_freq);
	} else if (info->tkey_booster->dvfs_boost_mode == DVFS_STAGE_DUAL) {
		input_booster_get_default_setting("tail", &value);
		info->tkey_booster->dvfs_freq = value.cpu_freq;
		input_info(true, &info->client->dev,
			"%s: boost_mode DUAL, dvfs_freq = %d\n",
			__func__, info->tkey_booster->dvfs_freq);
	}

	return count;
}
#endif

static int touchkey_led_set(struct abov_ft1604_info *info, int data)
{
	u8 cmd;
	int ret;

	if (data == 1)
		cmd = CMD_LED_ON;
	else
		cmd = CMD_LED_OFF;

	if (!info->enabled)
		goto out;

	if (info->dtdata->gpio_tkey_led_en >= 0)
		gpio_direction_output(info->dtdata->gpio_tkey_led_en,data);

	if (info->dtdata->vdd_led > 0) {
		if(data == 1) {
			if(regulator_is_enabled(info->dtdata->vdd_led))
				ret = -1;
			else
				ret = regulator_enable(info->dtdata->vdd_led);
		} else if(data == 0)
			if(regulator_is_enabled(info->dtdata->vdd_led))
				ret = regulator_disable(info->dtdata->vdd_led);
			else
				ret = -1;
		else
			input_err(true, &info->client->dev, "%s data param is wrong value: %d", __func__, data);
		if(ret < 0)
			input_err(true, &info->client->dev, "%s fail to set vdd_led(%d), ret(%d)", __func__, data, ret);
	}

	ret = abov_tk_i2c_write(info->client, ABOV_BTNSTATUS, &cmd, 1);
	if (ret < 0) {
		input_err(true, &info->client->dev, "%s fail(%d)\n", __func__, ret);
		goto out;
	}

	return 0;
out:
	abov_touchled_cmd_reserved = 1;
	abov_touchkey_led_status = cmd;
	return 1;
}

static ssize_t touchkey_led_control(struct device *dev,
		 struct device_attribute *attr, const char *buf,
		 size_t count)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int data;
	int ret;

	ret = sscanf(buf, "%d", &data);
	if (ret != 1) {
		input_err(true, &client->dev, "%s: cmd read err\n", __func__);
		return count;
	}

	if (!(data == 0 || data == 1)) {
		input_err(true, &client->dev, "%s: wrong command(%d)\n",
			__func__, data);
		return count;
	}

#ifdef LED_TWINKLE_BOOTING
	if (info->led_twinkle_check == 1){
		info->led_twinkle_check = 0;
		cancel_delayed_work(&info->led_twinkle_work);
	}
#endif

	if (touchkey_led_set(info, data))
		return count;

	msleep(20);

	abov_touchled_cmd_reserved = 0;
	input_info(true, &client->dev, "%s data(%d)\n",__func__,data);

	return count;
}

static ssize_t touchkey_threshold_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	u8 r_buf;
	int ret;

	ret = abov_tk_i2c_read(client, ABOV_THRESHOLD, &r_buf, 1);
	if (ret < 0) {
		input_err(true, &client->dev, "%s fail(%d)\n", __func__, ret);
		r_buf = 0;
	}
	return sprintf(buf, "%d\n", r_buf);
}

static void get_diff_data(struct abov_ft1604_info *info)
{
	struct i2c_client *client = info->client;
	u8 r_buf[4];
	int ret;

	ret = abov_tk_i2c_read(client, ABOV_DIFFDATA, r_buf, 4);
	if (ret < 0) {
		input_err(true, &client->dev, "%s fail(%d)\n", __func__, ret);
		info->menu_s = 0;
		info->back_s = 0;
		return;
	}

	info->menu_s = (r_buf[0] << 8) | r_buf[1];
	info->back_s = (r_buf[2] << 8) | r_buf[3];
}

static ssize_t touchkey_menu_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);

	get_diff_data(info);

	return sprintf(buf, "%d\n", info->menu_s);
}

static ssize_t touchkey_back_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);

	get_diff_data(info);

	return sprintf(buf, "%d\n", info->back_s);
}

static void get_raw_data(struct abov_ft1604_info *info)
{
	struct i2c_client *client = info->client;
	u8 r_buf[4];
	int ret;

	ret = abov_tk_i2c_read(client, ABOV_RAWDATA, r_buf, 4);
	if (ret < 0) {
		input_err(true, &client->dev, "%s fail(%d)\n", __func__, ret);
		info->menu_raw = 0;
		info->back_raw = 0;
		return;
	}

	info->menu_raw = (r_buf[0] << 8) | r_buf[1];
	info->back_raw = (r_buf[2] << 8) | r_buf[3];
}

static ssize_t touchkey_menu_raw_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);

	get_raw_data(info);

	return sprintf(buf, "%d\n", info->menu_raw);
}

static ssize_t touchkey_back_raw_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);

	get_raw_data(info);

	return sprintf(buf, "%d\n", info->back_raw);
}

static ssize_t bin_fw_ver(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;

	input_dbg(true, &client->dev, "fw version bin : 0x%x\n", info->fw_ver_bin);

	return sprintf(buf, "0x%02x\n", info->fw_ver_bin);
}

static int get_tk_fw_version(struct abov_ft1604_info *info, bool bootmode)
{
	struct i2c_client *client = info->client;
	u8 buf;
	int ret;
	int retry = 3;

	ret = abov_tk_i2c_read(client, ABOV_FW_VER, &buf, 1);
	if (ret < 0) {
		while (retry--) {
			input_err(true, &client->dev, "%s read fail(%d)\n",
				__func__, retry);
			if (!bootmode)
				abov_tk_reset(info);
			else
				return -1;
			ret = abov_tk_i2c_read(client, ABOV_FW_VER, &buf, 1);
			if (ret == 0)
				break;
		}
		if (retry == 0)
			return -1;
	}

	info->fw_ver = buf;

	retry = 3;
	ret = abov_tk_i2c_read(client, ABOV_MD_VER, &buf, 1);
	if (ret < 0) {
		while (retry--) {
			input_err(true, &client->dev, "%s read fail(%d)\n",
				__func__, retry);
			if (!bootmode)
				abov_tk_reset(info);
			else
				return -1;
			ret = abov_tk_i2c_read(client, ABOV_MD_VER, &buf, 1);
			if (ret == 0)
				break;
		}
		if (retry == 0)
			return -1;
	}

	info->md_ver = buf;
	input_info(true, &client->dev, "%s : fw = 0x%x, md = 0x%x\n",
		__func__, info->fw_ver, info->md_ver);
	return 0;
}

static ssize_t read_fw_ver(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int ret;

	ret = get_tk_fw_version(info, false);
	if (ret < 0) {
		input_err(true, &client->dev, "%s read fail\n", __func__);
		info->fw_ver = 0;
	}

	abov_tk_i2c_read_checksum(info);

	return sprintf(buf, "0x%02x\n", info->fw_ver);
}

static int abov_load_fw_kernel(struct abov_ft1604_info *info)
{
	struct i2c_client *client = info->client;
	int ret = 0;

	ret = request_firmware(&info->firm_data_bin,
		info->dtdata->fw_name, &client->dev);
	if (ret) {
		input_err(true, &client->dev,
			"%s request_firmware fail. \n", __func__);
		return ret;
	}
	info->firm_size = info->firm_data_bin->size;	
	info->fw_ver_bin = info->firm_data_bin->data[5];
	info->md_ver_bin = info->firm_data_bin->data[1];
	input_info(true, &client->dev, "%s : fw = 0x%x, md = 0x%x\n",
		__func__, info->fw_ver_bin, info->md_ver_bin);
	
	info->checksum_h_bin = info->firm_data_bin->data[8];
	info->checksum_l_bin = info->firm_data_bin->data[9];

	return ret;
}

static int abov_load_fw(struct abov_ft1604_info *info, u8 cmd)
{
	struct i2c_client *client = info->client;
	struct file *fp;
	mm_segment_t old_fs;
	long fsize, nread;
	int ret = 0;

	switch(cmd) {
	case BUILT_IN:
		break;

	case SDCARD:
		old_fs = get_fs();
		set_fs(get_ds());
		fp = filp_open(TK_FW_PATH_SDCARD, O_RDONLY, S_IRUSR);
		if (IS_ERR(fp)) {
			input_err(true, &client->dev,
				"%s %s open error\n", __func__, TK_FW_PATH_SDCARD);
			ret = -ENOENT;
			goto fail_sdcard_open;
		}

		fsize = fp->f_path.dentry->d_inode->i_size;
		info->firm_data_ums = kzalloc((size_t)fsize, GFP_KERNEL);
		if (!info->firm_data_ums) {
			input_err(true, &client->dev,
				"%s fail to kzalloc for fw\n", __func__);
			ret = -ENOMEM;
			goto fail_sdcard_kzalloc;
		}

		nread = vfs_read(fp,
			(char __user *)info->firm_data_ums, fsize, &fp->f_pos);
		if (nread != fsize) {
			input_err(true, &client->dev,
				"%s fail to vfs_read file\n", __func__);
			ret = -EINVAL;
			goto fail_sdcard_size;
		}
		filp_close(fp, current->files);
		set_fs(old_fs);
		info->firm_size = nread;
		break;

	default:
		ret = -1;
		break;
	}
	input_info(true, &client->dev, "fw_size : %lu\n", info->firm_size);
	input_info(true, &client->dev, "%s success\n", __func__);
	return ret;

fail_sdcard_size:
	kfree(&info->firm_data_ums);
fail_sdcard_kzalloc:
	filp_close(fp, current->files);
fail_sdcard_open:
	set_fs(old_fs);
	return ret;
}

static int abov_tk_check_busy(struct abov_ft1604_info *info)
{
	int ret, count = 0;
	unsigned char val = 0x00;

	do {
		ret = i2c_master_recv(info->client, &val, sizeof(val));

		if (val) {
			count++;
		} else {
			break;
		}

	} while(1);

	if (count > 1000)
		input_err(true, &info->client->dev, "%s: busy %d\n", __func__, count);
	return ret;
}

static int abov_tk_i2c_read_checksum(struct abov_ft1604_info *info)
{
	unsigned char data[6] = {0xAC, 0x9E, 0x10, 0x00, 0x3F, 0xFF};
	unsigned char checksum[5] = {0, };
	int ret;
	unsigned char reg = 0x00;

	i2c_master_send(info->client, data, 6);

	usleep_range(5 * 1000, 5 * 1000);

	abov_tk_check_busy(info);

	ret = abov_tk_i2c_read(info->client, reg, checksum, 5);

	input_info(true, &info->client->dev, "%s: ret:%d [%X][%X][%X][%X][%X]\n",
			__func__, ret, checksum[0], checksum[1], checksum[2]
			, checksum[3], checksum[4]);
	info->checksum_h = checksum[3];
	info->checksum_l = checksum[4];
	return 0;
}

static int abov_tk_fw_write(struct abov_ft1604_info *info, unsigned char *addrH,
						unsigned char *addrL, unsigned char *val)
{
	int length = 36, ret = 0;
	unsigned char data[36];

	data[0] = 0xAC;
	data[1] = 0x7A;
	memcpy(&data[2], addrH, 1);
	memcpy(&data[3], addrL, 1);
	memcpy(&data[4], val, 32);

	ret = i2c_master_send(info->client, data, length);
	if (ret != length) {
		input_err(true, &info->client->dev, "%s: write fail[%x%x], %d\n", __func__, *addrH, *addrL, ret);
		return ret;
	}

	usleep_range(2 * 1000, 2 * 1000);

	abov_tk_check_busy(info);

	return 0;
}

static int abov_tk_fw_mode_enter(struct abov_ft1604_info *info)
{
	unsigned char data[3] = {0xAC, 0x5B, 0x2D};
	int ret = 0;

	ret = i2c_master_send(info->client, data, 3);
	if (ret != 3) {
		input_err(true, &info->client->dev, "%s: write fail\n", __func__);
		return -1;
	}

	return 0;

}

static int abov_tk_fw_update(struct abov_ft1604_info *info, u8 cmd)
{
	int ret, ii = 0;
	int count;
	unsigned short address;
	unsigned char addrH, addrL;
	unsigned char data[32] = {0, };


	input_err(true, &info->client->dev, "%s:1\n", __func__);

	count = info->firm_size / 32;
	address = 0x1000;

	gpio_direction_output(info->dtdata->gpio_en, 0);
	msleep(30);
	gpio_direction_output(info->dtdata->gpio_en, 1);
	usleep_range(ABOV_BOOT_DELAY * 1000, ABOV_BOOT_DELAY * 1000);

	input_err(true, &info->client->dev, "%s:2\n", __func__);

	ret = abov_tk_fw_mode_enter(info);

	input_err(true, &info->client->dev, "%s:3\n", __func__);

	msleep(1100);
	
	for (ii = 1; ii < count; ii++) {

		addrH = (unsigned char)((address >> 8) & 0xFF);
		addrL = (unsigned char)(address & 0xFF);
		if (cmd == BUILT_IN)
			memcpy(data, &info->firm_data_bin->data[ii * 32], 32);
		else if (cmd == SDCARD)
			memcpy(data, &info->firm_data_ums[ii * 32], 32);

		ret = abov_tk_fw_write(info, &addrH, &addrL, data);
		if (ret < 0) {
			input_err(true, &info->client->dev, "%s: err, no device : %d\n", __func__, ret);
			return ret;
		}
		usleep_range(3 * 1000, 3 * 1000);

		abov_tk_check_busy(info);

		address += 0x20;

		memset(data, 0, 32);
	}

	input_err(true, &info->client->dev, "%s:4\n", __func__);
	ret = abov_tk_i2c_read_checksum(info);

	input_err(true, &info->client->dev, "%s:5\n", __func__);


	gpio_direction_output(info->dtdata->gpio_en, 0);
	msleep(30);
	gpio_direction_output(info->dtdata->gpio_en, 1);	
	msleep(100);

	return ret;


}


static void abov_release_fw(struct abov_ft1604_info *info, u8 cmd)
{
	switch(cmd) {
	case BUILT_IN:
		release_firmware(info->firm_data_bin);
		break;

	case SDCARD:
		kfree(info->firm_data_ums);
		break;

	default:
		break;
	}
}

static int abov_flash_fw(struct abov_ft1604_info *info, bool probe, u8 cmd)
{
	struct i2c_client *client = info->client;
	int retry = 2;
	int ret;
	int block_count;
	const u8 *fw_data;

	ret = get_tk_fw_version(info, probe);
	if (ret)
		info->fw_ver = 0;

	ret = abov_load_fw(info, cmd);
	if (ret) {
		input_err(true, &client->dev,
			"%s fw load fail\n", __func__);
		return ret;
	}

	switch(cmd) {
	case BUILT_IN:
		fw_data = info->firm_data_bin->data;
		break;

	case SDCARD:
		fw_data = info->firm_data_ums;
		break;

	default:
		return -1;
		break;
	}

	block_count = (int)(info->firm_size / 32);

	while (retry--) {
		ret = abov_tk_fw_update(info, cmd);
		if (ret < 0)
			break;

		if (cmd == BUILT_IN) {
/*
			if ((info->checksum_h != info->checksum_h_bin) ||
				(info->checksum_l != info->checksum_l_bin)) {
				dev_err(&client->dev,
					"%s checksum fail.(0x%x,0x%x),(0x%x,0x%x) retry:%d\n",
					__func__, info->checksum_h, info->checksum_l,
					info->checksum_h_bin, info->checksum_l_bin, retry);
				ret = -1;
				continue;
			}
*/
		}
		abov_tk_reset_for_bootmode(info);
		msleep(ABOV_RESET_DELAY);
		ret = get_tk_fw_version(info, true);
		if (ret) {
			input_err(true, &client->dev, "%s fw version read fail\n", __func__);
			ret = -1;
			continue;
		}

		if (info->fw_ver == 0) {
			input_err(true, &client->dev, "%s fw version fail (0x%x)\n",
				__func__, info->fw_ver);
			ret = -1;
			continue;
		}

		if ((cmd == BUILT_IN) && (info->fw_ver != info->fw_ver_bin)) {
			input_err(true, &client->dev, "%s fw version fail 0x%x, 0x%x\n",
				__func__, info->fw_ver, info->fw_ver_bin);
			ret = -1;
			continue;
		}
		ret = 0;
		break;
	}

	abov_release_fw(info, cmd);

	return ret;
}

static ssize_t touchkey_fw_update(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int ret;
	u8 cmd;

	switch(*buf) {
	case 's':
	case 'S':
		cmd = BUILT_IN;
		break;
	case 'i':
	case 'I':
		cmd = SDCARD;
		break;
	default:
		info->fw_update_state = 2;
		goto touchkey_fw_update_out;
	}

	info->fw_update_state = 1;
	disable_irq(info->irq);
	info->enabled = false;

	if(cmd == BUILT_IN){
		ret = abov_load_fw_kernel(info);
		if (ret) {
			input_err(true, &client->dev,
				"failed to abov_load_fw_kernel (%d)\n", ret);
		} else {
			input_err(true, &client->dev,
				"fw version read success (%d)\n", ret);
		}
	}
	ret = abov_flash_fw(info, false, cmd);
#ifdef GLOVE_MODE
	if (info->glovemode)
		abov_glove_mode_enable(client, CMD_GLOVE_ON);
#endif
	info->enabled = true;
	enable_irq(info->irq);
	if (ret) {
		input_err(true, &client->dev, "%s fail\n", __func__);
//		info->fw_update_state = 2;
		info->fw_update_state = 0;

	} else {
		input_info(true, &client->dev, "%s success\n", __func__);
		info->fw_update_state = 0;
	}

touchkey_fw_update_out:
	dev_dbg(&client->dev, "%s : %d\n", __func__, info->fw_update_state);

	return count;
}

static ssize_t touchkey_fw_update_status(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int count = 0;

	input_info(true, &client->dev, "%s : %d\n", __func__, info->fw_update_state);

	if (info->fw_update_state == 0)
		count = sprintf(buf, "PASS\n");
	else if (info->fw_update_state == 1)
		count = sprintf(buf, "Downloading\n");
	else if (info->fw_update_state == 2)
		count = sprintf(buf, "Fail\n");

	return count;
}

#ifdef GLOVE_MODE
static ssize_t abov_glove_mode(struct device *dev,
	 struct device_attribute *attr, const char *buf, size_t count)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);
	struct i2c_client *client = info->client;
	int scan_buffer;
	int ret;
	u8 cmd;

	ret = sscanf(buf, "%d", &scan_buffer);
	if (ret != 1) {
		input_err(true, &client->dev, "%s: cmd read err\n", __func__);
		return count;
	}

	if (!(scan_buffer == 0 || scan_buffer == 1)) {
		input_err(true, &client->dev, "%s: wrong command(%d)\n",
			__func__, scan_buffer);
		return count;
	}

	if (!info->enabled)
		return count;

	if (info->glovemode == scan_buffer) {
		input_info(true, &client->dev, "%s same command(%d)\n",
			__func__, scan_buffer);
		return count;
	}

	if (scan_buffer == 1) {
		input_info(true, &client->dev, "%s glove mode\n", __func__);
		cmd = CMD_GLOVE_ON;
	} else {
		input_info(true, &client->dev, "%s normal mode\n", __func__);
		cmd = CMD_GLOVE_OFF;
	}

	ret = abov_glove_mode_enable(client, cmd);
	if (ret < 0) {
		input_err(true, &client->dev, "%s fail(%d)\n", __func__, ret);
		return count;
	}

	info->glovemode = scan_buffer;

	return count;
}

static ssize_t abov_glove_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", info->glovemode);
}
#endif

static ssize_t touchkey_light_version_read(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);
	int count;
	int crc_cal, crc_efs;

	if (efs_read_light_table_version(info) < 0) {
		count = sprintf(buf, "NG");
		goto out;
	} else {
		if (info->light_version_efs == LIGHT_VERSION) {
			if (!check_light_table_crc(info)) {
				count = sprintf(buf, "NG_CS");
				goto out;
			}
		} else {
			crc_cal = efs_calculate_crc(info);
			crc_efs = efs_read_crc(info);
			input_info(true, &info->client->dev,
					"CRC compare: efs[%d], bin[%d]\n",
					crc_cal, crc_efs);
			if (crc_cal != crc_efs) {
				count = sprintf(buf, "NG_CS");
				goto out;
			}
		}
	}

	count = sprintf(buf, "%s,%s",
			info->light_version_full_efs,
			info->light_version_full_bin);
out:
	input_info(true, &info->client->dev, "%s: %s\n", __func__, buf);
	return count;
}

static ssize_t touchkey_light_update(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);
	int ret;
	int vol_mv;
	int window_type = read_window_type();

	ret = efs_write(info);
	if (ret < 0) {
		input_err(true, &info->client->dev, "%s: fail %d\n", __func__, ret);
		return -EIO;
	}

	vol_mv = efs_read_light_table_with_default(info, window_type);
	if (vol_mv >= LIGHT_VOLTAGE_MIN_VAL) {
		change_touch_key_led_voltage(&info->client->dev, vol_mv);
		input_info(true, &info->client->dev,
				"%s: read done for %d\n", __func__, window_type);
	} else {
		input_err(true, &info->client->dev,
				"%s: fail. voltage is %d\n", __func__, vol_mv);
	}

	return size;
}

static ssize_t touchkey_light_id_compare(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);
	int count, ret;
	int window_type = read_window_type();
	int crc_cal, crc_efs;

	if (window_type < 0) {
		input_err(true, &info->client->dev,
				"%s: window_type:%d, NG\n", __func__, window_type);
		return sprintf(buf, "NG");
	}

	ret = efs_read_light_table(info, window_type);
	if (ret < 0) {
		count = sprintf(buf, "NG");
	} else {
		crc_cal = efs_calculate_crc(info);
		crc_efs = efs_read_crc(info);
		input_info(true, &info->client->dev,
				"EFS CRC compare: efs[%d], bin[%d]\n",
				crc_cal, crc_efs);
		if (crc_cal != crc_efs) {
			count = sprintf(buf, "NG_CS");
		}else{
			count = sprintf(buf, "OK");
		}	
	}

	input_info(true, &info->client->dev,
			"%s: window_type:%d, %s\n", __func__, window_type, buf);
	return count;
}

static char* tokenizer(char* str, char delim)
{
	static char* str_addr;
	char* token = NULL;
	
	if (str != NULL)
		str_addr = str;
	else if (str_addr == NULL)
		return 0;

	token = str_addr;
	while (true) {
		if (*str_addr == delim) {
			*str_addr = '\0';
			str_addr = str_addr + 1;
			break;
		} else if (str_addr == NULL) {
			break;
		}
		str_addr++;
	}

	return token;
}

static ssize_t touchkey_light_table_write(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct abov_ft1604_info *info = dev_get_drvdata(dev);
	struct light_info table[16];
	int ret;
	int vol_mv;
	int window_type;
	char *full_version;
	char data[150] = {0, };
	int i, crc, crc_cal;
	char *octa_id;
	int table_size = 0;

	snprintf(data, sizeof(data), buf);

	input_info(true, &info->client->dev, "%s: %s\n",
			__func__, data);

	full_version = tokenizer(data, ',');
	if (!full_version)
		return -EIO;

	table_size = (int)strlen(full_version) - 8;
	input_info(true, &info->client->dev, "%s: version:%s size:%d\n",
			__func__, full_version, table_size);

	if (kstrtoint(tokenizer(NULL, ','), 0, &crc))
		return -EIO;

	input_info(true, &info->client->dev, "%s: crc:%d\n",
			__func__, crc);

	for (i = 0; i < table_size; i++) {
		octa_id = tokenizer(NULL, '_');
		if (octa_id[0] >= 'A')
			table[i].octa_id = octa_id[0] - 'A' + 0x0A;
		else
			table[i].octa_id = octa_id[0] - '0';
		if (table[i].octa_id < 0 || table[i].octa_id > 0x0F)
			break;
		if (kstrtoint(tokenizer(NULL, ','), 0, &table[i].vol_mv))
			break;
	}

	if (!table_size) {
		input_err(true, &info->client->dev, "%s: no data in table\n", __func__);
		return -ENODATA;
	}

	for (i = 0; i < table_size; i++) {
		input_info(true, &info->client->dev, "%s: [%d] %X - %dmv\n",
				__func__, i, table[i].octa_id, table[i].vol_mv);
	}

	/* write efs */
	ret = efs_write_light_table_version(info, full_version);
	if (ret < 0) {
		input_err(true, &info->client->dev,
				"%s: failed to write table ver %s. %d\n",
				__func__, full_version, ret);
		return ret;
	}

	info->light_version_efs = pick_light_table_version(full_version);

	for (i = 0; i < table_size; i++) {
		ret = efs_write_light_table(info, table[i]);
		if (ret < 0)
			break;
	}
	if (ret < 0) {
		input_err(true, &info->client->dev,
				"%s: failed to write table%d. %d\n",
				__func__, i, ret);
		return ret;
	}

	ret = efs_write_light_table_crc(info, crc);
	if (ret < 0) {
		input_err(true, &info->client->dev,
				"%s: failed to write table crc. %d\n",
				__func__, ret);
		return ret;
	}

	crc_cal = efs_calculate_crc(info);
	input_info(true, &info->client->dev,
			"%s: efs crc:%d, caldulated crc:%d\n",
			__func__, crc, crc_cal);
	if (crc_cal != crc)
		return -EIO;

	window_type = read_window_type();
	vol_mv = efs_read_light_table_with_default(info, window_type);
	if (vol_mv >= LIGHT_VOLTAGE_MIN_VAL) {
		change_touch_key_led_voltage(&info->client->dev, vol_mv);
		input_info(true, &info->client->dev,
				"%s: read done for %d\n", __func__, window_type);
	} else {
		input_err(true, &info->client->dev,
				"%s: fail. voltage is %d\n", __func__, vol_mv);
	}

	return size;
}

static DEVICE_ATTR(touchkey_threshold, S_IRUGO, touchkey_threshold_show, NULL);
static DEVICE_ATTR(brightness, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
			touchkey_led_control);
static DEVICE_ATTR(touchkey_recent, S_IRUGO, touchkey_menu_show, NULL);
static DEVICE_ATTR(touchkey_back, S_IRUGO, touchkey_back_show, NULL);
static DEVICE_ATTR(touchkey_recent_raw, S_IRUGO, touchkey_menu_raw_show, NULL);
static DEVICE_ATTR(touchkey_back_raw, S_IRUGO, touchkey_back_raw_show, NULL);
static DEVICE_ATTR(touchkey_firm_version_phone, S_IRUGO, bin_fw_ver, NULL);
static DEVICE_ATTR(touchkey_firm_version_panel, S_IRUGO, read_fw_ver, NULL);
static DEVICE_ATTR(touchkey_firm_update, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
			touchkey_fw_update);
static DEVICE_ATTR(touchkey_firm_update_status, S_IRUGO | S_IWUSR | S_IWGRP,
			touchkey_fw_update_status, NULL);
#ifdef GLOVE_MODE
static DEVICE_ATTR(glove_mode, S_IRUGO | S_IWUSR | S_IWGRP,
			abov_glove_mode_show, abov_glove_mode);
#endif
#ifdef CONFIG_INPUT_BOOSTER
static DEVICE_ATTR(boost_level,
		   S_IWUSR | S_IWGRP, NULL, boost_level_store);
#endif
static DEVICE_ATTR(touchkey_brightness, S_IRUGO | S_IWUSR | S_IWGRP, NULL, brightness_control);
static DEVICE_ATTR(touchkey_light_version, S_IRUGO, touchkey_light_version_read, NULL);
static DEVICE_ATTR(touchkey_light_update, S_IWUSR | S_IWGRP, NULL, touchkey_light_update);
static DEVICE_ATTR(touchkey_light_id_compare, S_IRUGO, touchkey_light_id_compare, NULL);
static DEVICE_ATTR(touchkey_light_table_write, S_IWUSR | S_IWGRP, NULL, touchkey_light_table_write);

static struct attribute *sec_touchkey_attributes[] = {
	&dev_attr_touchkey_threshold.attr,
	&dev_attr_brightness.attr,
	&dev_attr_touchkey_recent.attr,
	&dev_attr_touchkey_back.attr,
	&dev_attr_touchkey_recent_raw.attr,
	&dev_attr_touchkey_back_raw.attr,
	&dev_attr_touchkey_firm_version_phone.attr,
	&dev_attr_touchkey_firm_version_panel.attr,
	&dev_attr_touchkey_firm_update.attr,
	&dev_attr_touchkey_firm_update_status.attr,
#ifdef GLOVE_MODE
	&dev_attr_glove_mode.attr,
#endif
#ifdef CONFIG_INPUT_BOOSTER
	&dev_attr_boost_level.attr,
#endif
	&dev_attr_touchkey_brightness.attr,
	&dev_attr_touchkey_light_version.attr,
	&dev_attr_touchkey_light_update.attr,
	&dev_attr_touchkey_light_id_compare.attr,
	&dev_attr_touchkey_light_table_write.attr,
	NULL,
};

static struct attribute_group sec_touchkey_attr_group = {
	.attrs = sec_touchkey_attributes,
};

static int abov_tk_fw_check(struct abov_ft1604_info *info)
{
	struct i2c_client *client = info->client;
	int ret;
	bool force = false;

	if (info->dtdata->bringup)
		return 0;

	ret = get_tk_fw_version(info, true);
	if (ret) {
		input_err(true, &client->dev,
			"%s: i2c fail...[%d], addr[%d]\n",
			__func__, ret, info->client->addr);
#ifdef LED_TWINKLE_BOOTING
		/* regard I2C fail & LCD attached status as no TKEY device */
		if (get_lcd_attached("GET") == 0) {
			input_err(true, &client->dev,
				"%s : LCD is not attached\n", __func__);
				return ret;
		}
#endif
	}
	ret = abov_load_fw_kernel(info);
	if (ret) {
		input_err(true, &client->dev,
			"failed to abov_load_fw_kernel (%d)\n", ret);
	} else {
		input_err(true, &client->dev,
			"fw version read success (%d)\n", ret);
	}

	if (info->md_ver != info->md_ver_bin) {
		input_err(true, &client->dev,
			"MD version is different.(IC %x, BN %x). Do force FW update\n",
			info->md_ver, info->md_ver_bin);
		force = true;
	}

	input_info(true, &client->dev, "version IC:0x%x, Bin:0x%x\n",info->fw_ver, info->fw_ver_bin);

	if (info->fw_ver != info->fw_ver_bin || info->fw_ver > 0xf0 || force == true) {
		input_err(true, &client->dev, "excute tk firmware update (0x%x -> 0x%x)\n",
			info->fw_ver, info->fw_ver_bin);
		ret = abov_flash_fw(info, true, BUILT_IN);
		if (ret) {
			input_err(true, &client->dev,
				"failed to abov_flash_fw (%d)\n", ret);
		} else {
			input_err(true, &client->dev,
				"fw update success\n");
		}
	}

	return ret;
}

static int abov_power(struct abov_ft1604_devicetree_data *dtdata, bool on)
{
	int ret = 0;

	// 1.8V on,off control.
	if(!(dtdata->vdd_io_alwayson)){
		if(dtdata->vdd_io_vreg) {
			if (on)
				ret = regulator_enable(dtdata->vdd_io_vreg);
			else
				ret = regulator_disable(dtdata->vdd_io_vreg);
		}
		else
			pr_err("%s %s: iovdd reg NULL!! \n", SECLOG, __func__);

		if(ret)
			pr_err("%s %s: iovdd reg %s fail\n",
				SECLOG, __func__, on ? "enable" : "disable");
	}

	if (dtdata->gpio_tkey_led_en >= 0) {
                gpio_direction_output(dtdata->gpio_tkey_led_en, on);
                pr_info("[TKEY] %s: %s: %d\n", __func__, on ? "on" : "off",
                        gpio_get_value(dtdata->gpio_tkey_led_en));
        }
	return ret;
}

static int abov_pinctrl_configure(struct abov_ft1604_info *info, 
							bool active)
{
	struct pinctrl_state *set_state;
	int retval;

	if (active) {
		set_state =
			pinctrl_lookup_state(info->pinctrl,
						"touchkey_active");
		if (IS_ERR(set_state)) {
			input_err(true, &info->client->dev, "%s: cannot get ts pinctrl active state\n", __func__);
			return PTR_ERR(set_state);
		}
	} else {
		set_state =
			pinctrl_lookup_state(info->pinctrl,
						"touchkey_suspend");
		if (IS_ERR(set_state)) {
			input_err(true, &info->client->dev, "%s: cannot get gpiokey pinctrl sleep state\n", __func__);
			return PTR_ERR(set_state);
		}
	}
	retval = pinctrl_select_state(info->pinctrl, set_state);
	if (retval) {
		input_err(true, &info->client->dev, "%s: cannot set ts pinctrl active state\n", __func__);
		return retval;
	}

	input_info(true, &info->client->dev, "%s %s\n",
			__func__, active ? "ACTIVE" : "SUSPEND");

	return 0;
}

static int abov_gpio_reg_init(struct device *dev,
			struct abov_ft1604_devicetree_data *dtdata)
{
	int ret = 0;

	if (dtdata->gpio_rst > 0) {
		ret = gpio_request(dtdata->gpio_rst, "tkey_gpio_rst");
		if(ret < 0){
			input_err(true, dev, "unable to request gpio_rst\n");
			return ret;
		}
	}
	ret = gpio_request(dtdata->gpio_int, "tkey_gpio_int");
	if(ret < 0){
		input_err(true, dev, "unable to request gpio_int\n");
		return ret;
	}

	ret = gpio_request(dtdata->gpio_en, "tkey_gpio_en");
	if(ret < 0){
		input_err(true, dev, "unable to request gpio_en\n");
		return ret;
	}

	if (dtdata->gpio_tkey_led_en >= 0) {
	ret = gpio_request(dtdata->gpio_tkey_led_en, "gpio_tkey_led_en");
	if(ret < 0){
		input_err(true, dev, "unable to request gpio_tkey_led_en. ignoring\n");
		ret = 0;
	}
	}

	dtdata->vdd_led = regulator_get(dev, "vdd_led");
	if (IS_ERR_OR_NULL(dtdata->vdd_led)) {
		dtdata->vdd_led = NULL;
		input_err(true, dev, "vdd_led is not used for PMIC LDO.\n");
	} else {
		regulator_set_voltage(dtdata->vdd_led, 3300000, 3300000);		
	}
	
	dtdata->vdd_io_vreg = devm_regulator_get(dev, "vddo");
	if (IS_ERR(dtdata->vdd_io_vreg)){
		dtdata->vdd_io_vreg = NULL;
		input_err(true, dev, "dtdata->vdd_io_vreg get error, ignoring\n");
	} else {
		regulator_set_voltage(dtdata->vdd_io_vreg, 1800000, 1800000);

		// 1.8V always on.
		if(dtdata->vdd_io_alwayson){
			ret = regulator_enable(dtdata->vdd_io_vreg);
			if (ret) {
				input_err(true, dev, "[TKEY] %s: iovdd reg enable fail\n",
					__func__);
			}
		}

	}

	dtdata->power = abov_power;

	return ret;
}

#ifdef CONFIG_OF
static int abov_parse_dt(struct device *dev,
			struct abov_ft1604_devicetree_data *dtdata)
{
	struct device_node *np = dev->of_node;

	dtdata->gpio_rst = of_get_named_gpio(np, "abov,rst-gpio", 0);
	if(dtdata->gpio_rst < 0){
		input_err(true, dev, "unable to get gpio_rst\n");
	}

	dtdata->gpio_en = of_get_named_gpio(np, "abov,tkey_en-gpio", 0);
	if(dtdata->gpio_en < 0){
		input_err(true, dev, "unable to get gpio_en\n");
		return dtdata->gpio_en;
	}

	dtdata->gpio_int = of_get_named_gpio(np, "abov,irq-gpio", 0);
	if(dtdata->gpio_int < 0){
		input_err(true, dev, "unable to get gpio_int\n");
		return dtdata->gpio_int;
	}

	dtdata->gpio_scl = of_get_named_gpio(np, "abov,scl-gpio", 0);
	if(dtdata->gpio_scl < 0){
		input_err(true, dev, "unable to get gpio_scl\n");
		return dtdata->gpio_scl;
	}

	dtdata->gpio_sda = of_get_named_gpio(np, "abov,sda-gpio", 0);
	if(dtdata->gpio_sda < 0){
		input_err(true, dev, "unable to get gpio_sda\n");
		return dtdata->gpio_sda;
	}

	dtdata->bringup = of_property_read_bool(np, "abov,bringup");
	if (dtdata->bringup < 0)
		dtdata->bringup = 0;

	dtdata->vdd_io_alwayson = of_property_read_bool(np, "abov,vddo_alwayson");
	if(dtdata->vdd_io_alwayson < 0){
		input_err(true, dev, "unable to get vdd_io_alwayson\n");
		dtdata->vdd_io_alwayson = 0;
	}

	dtdata->gpio_tkey_led_en = of_get_named_gpio(np, "abov,tkey_led_en-gpio", 0);
	if(dtdata->gpio_tkey_led_en < 0){
		input_err(true, dev, "unable to get gpio_tkey_led_en...ignoring\n");
	}

	dtdata->reverse_key = of_property_read_bool(np, "abov,reverse_key");
	if(dtdata->reverse_key < 0){
		dtdata->reverse_key = false;
	}

	of_property_read_string(np, "abov,firmware_name", &dtdata->fw_name);

	input_info(true, dev, "%s: gpio_en:%d, gpio_int:%d, gpio_scl:%d,"
		" gpio_sda:%d, gpio_led_en:%d, vdd_io:%d, reverse_key:%d, fw_name: %s\n",
			__func__, dtdata->gpio_en, dtdata->gpio_int, dtdata->gpio_scl,
			dtdata->gpio_sda, dtdata->gpio_tkey_led_en, dtdata->vdd_io_alwayson,
			dtdata->reverse_key, dtdata->fw_name);

	return 0;
}
#else
static int abov_parse_dt(struct device *dev,
			struct abov_ft1604_devicetree_data *dtdata)
{
	return -ENODEV;
}
#endif

static int abov_tk_probe(struct i2c_client *client,
				  const struct i2c_device_id *id)
{
	struct abov_ft1604_info *info;
	struct input_dev *input_dev;
	int ret = 0;
	int i;
	char tmp[2] = {0, };

	input_err(true, &client->dev, "%s++\n", __func__);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		input_err(true, &client->dev,
			"i2c_check_functionality fail\n");
		return -EIO;
	}

#ifndef LED_TWINKLE_BOOTING
	if (get_lcd_attached("GET") == 0) {
		input_err(true, &client->dev,
			"%s : LCD is not attached\n", __func__);
		return -ENODEV;
	}
#endif

	info = kzalloc(sizeof(struct abov_ft1604_info), GFP_KERNEL);
	if (!info) {
		input_err(true, &client->dev, "Failed to allocate memory\n");
		ret = -ENOMEM;
		goto err_alloc;
	}

	input_dev = input_allocate_device();
	if (!input_dev) {
		input_err(true, &client->dev,
			"Failed to allocate memory for input device\n");
		ret = -ENOMEM;
		goto err_input_alloc;
	}

	info->client = client;
	info->input_dev = input_dev;
	info->probe_done = false;

	if (client->dev.of_node) {
		struct abov_ft1604_devicetree_data *dtdata;
		dtdata = devm_kzalloc(&client->dev,
			sizeof(struct abov_ft1604_devicetree_data), GFP_KERNEL);
		if (!dtdata) {
			input_err(true, &client->dev, "Failed to allocate memory\n");
			ret = -ENOMEM;
			goto err_config;
		}

		ret = abov_parse_dt(&client->dev, dtdata);
		if (ret)
			goto err_config;

		info->dtdata = dtdata;
	} else
		info->dtdata = client->dev.platform_data;

	if (info->dtdata == NULL) {
		input_err(true, &client->dev, "failed to get platform data\n");
		goto err_config;
	}

	/* Get pinctrl if target uses pinctrl */
	info->pinctrl = devm_pinctrl_get(&client->dev);
	if (IS_ERR(info->pinctrl)) {
		if (PTR_ERR(info->pinctrl) == -EPROBE_DEFER)
			goto err_config;

		input_err(true, &client->dev, "%s: Target does not use pinctrl\n", __func__);
		info->pinctrl = NULL;
	}

	if (info->pinctrl) {
		ret = abov_pinctrl_configure(info, true);
		if (ret)
			input_err(true, &client->dev, "%s: cannot set ts pinctrl active state\n", __func__);
	}

	ret = abov_gpio_reg_init(&client->dev, info->dtdata);
	if(ret){
		input_err(true, &client->dev, "failed to init reg\n");
		goto pwr_config;
	}
	if (info->dtdata->power)
		info->dtdata->power(info->dtdata, true);

	info->irq = -1;
	mutex_init(&info->lock);

	abov_tk_reset_for_bootmode(info);
	msleep(ABOV_RESET_DELAY);

	info->touchkey_count = sizeof(touchkey_keycode) / sizeof(int);
	i2c_set_clientdata(client, info);

	ret = abov_tk_fw_check(info);
	if (ret) {
		input_err(true, &client->dev,
			"failed to firmware check (%d)\n", ret);
		goto err_reg_input_dev;
	}

	snprintf(info->phys, sizeof(info->phys),
		 "%s/input0", dev_name(&client->dev));
	input_dev->name = "sec_touchkey";
	input_dev->phys = info->phys;
	input_dev->id.bustype = BUS_HOST;
	input_dev->dev.parent = &client->dev;
	input_dev->open = abov_tk_input_open;
	input_dev->close = abov_tk_input_close;

	set_bit(EV_KEY, input_dev->evbit);
	set_bit(KEY_RECENT, input_dev->keybit);
	set_bit(KEY_BACK, input_dev->keybit);
	set_bit(EV_LED, input_dev->evbit);
	set_bit(LED_MISC, input_dev->ledbit);
	input_set_drvdata(input_dev, info);

	ret = input_register_device(input_dev);
	if (ret) {
		input_err(true, &client->dev, "failed to register input dev (%d)\n",
			ret);
		goto err_reg_input_dev;
	}

#ifdef CONFIG_INPUT_BOOSTER
	info->tkey_booster = input_booster_allocate(INPUT_BOOSTER_ID_TKEY);
	if (!info->tkey_booster) {
		input_err(true, &client->dev,
			"%s [ERROR] failed to allocate input booster\n", __func__);
		goto err_alloc_booster_failed;
	}
#endif

	info->enabled = true;

	if (!info->dtdata->irq_flag) {
		input_err(true, &client->dev, "no irq_flag\n");
		ret = request_threaded_irq(client->irq, NULL, abov_tk_interrupt,
			IRQF_TRIGGER_LOW | IRQF_ONESHOT, ABOV_TK_NAME, info);
	} else {
		ret = request_threaded_irq(client->irq, NULL, abov_tk_interrupt,
			info->dtdata->irq_flag, ABOV_TK_NAME, info);
	}
	if (ret < 0) {
		input_err(true, &client->dev, "Failed to register interrupt\n");
		goto err_req_irq;
	}
	info->irq = client->irq;

	sec_touchkey = sec_device_create(0, info, "sec_touchkey");
	if (IS_ERR(sec_touchkey))
		input_err(true, &client->dev,
		"Failed to create device for the touchkey sysfs\n");

	ret = sysfs_create_group(&sec_touchkey->kobj,
		&sec_touchkey_attr_group);
	if (ret)
		input_err(true, &client->dev, "Failed to create sysfs group\n");

	ret = sysfs_create_link(&sec_touchkey->kobj,
		&info->input_dev->dev.kobj, "input");
	if (ret < 0) {
		input_err(true, &info->client->dev,
			"%s: Failed to create input symbolic link\n",
			__func__);
	}
	if(info->dtdata->gpio_tkey_led_en >= 0)
	gpio_direction_output(info->dtdata->gpio_tkey_led_en, 0);

#ifdef LED_TWINKLE_BOOTING
	if (get_lcd_attached("GET") == 0) {
		input_err(true, &client->dev,
			"%s : LCD is not connected. so start LED twinkle \n", __func__);
		INIT_DELAYED_WORK(&info->led_twinkle_work, led_twinkle_work);
		info->led_twinkle_check =  1;
		schedule_delayed_work(&info->led_twinkle_work, msecs_to_jiffies(400));
	}
#endif

	INIT_DELAYED_WORK(&info->efs_open_work, touchkey_efs_open_work);

	info->light_table_crc = LIGHT_VERSION;
	sprintf(info->light_version_full_bin, "T%d.", LIGHT_VERSION);
	for (i = 0; i < LIGHT_TABLE_MAX; i++) {
		info->light_table_crc += tkey_light_voltage_table[i].octa_id;
		info->light_table_crc += tkey_light_voltage_table[i].vol_mv;
		snprintf(tmp, 2, "%X", tkey_light_voltage_table[i].octa_id);
		strncat(info->light_version_full_bin, tmp, 1);
	}
	input_info(true, &client->dev, "%s: light version of kernel : %s\n",
			__func__, info->light_version_full_bin);

	schedule_delayed_work(&info->efs_open_work, msecs_to_jiffies(2000));

	input_err(true, &client->dev, "%s done\n", __func__);
	info->probe_done = true;

	return 0;

err_req_irq:
#ifdef CONFIG_INPUT_BOOSTER
	input_booster_free(info->tkey_booster);
	info->tkey_booster = NULL;
err_alloc_booster_failed:
#endif
	input_unregister_device(input_dev);
err_reg_input_dev:
	mutex_destroy(&info->lock);
pwr_config:
err_config:
	input_free_device(input_dev);
err_input_alloc:
	kfree(info);
err_alloc:
	if (ret >= 0)
		ret = -ENODEV;
	input_err(true, &client->dev, "%s failed %d\n", __func__, ret);
	return ret;

}

#ifdef LED_TWINKLE_BOOTING
static void led_twinkle_work(struct work_struct *work)
{
	struct abov_ft1604_info *info = container_of(work, struct abov_ft1604_info,
						led_twinkle_work.work);
	static bool led_on = 1;
	static int count = 0;
	input_err(true, &info->client->dev, "%s, on=%d, c=%d\n",__func__, led_on, count++ );

	if(info->led_twinkle_check == 1){
		touchkey_led_set(info, led_on);

		if (led_on)
			led_on = 0;
		else
			led_on = 1;

		schedule_delayed_work(&info->led_twinkle_work, msecs_to_jiffies(400));
	}
	else {
		if(led_on == 0)
			touchkey_led_set(info, 0);
	}
}
#endif

static int abov_tk_remove(struct i2c_client *client)
{
	struct abov_ft1604_info *info = i2c_get_clientdata(client);

	if (info->enabled)
		info->dtdata->power(info->dtdata, false);

	info->enabled = false;
	if (info->irq >= 0)
		free_irq(info->irq, info);
	input_unregister_device(info->input_dev);
	input_free_device(info->input_dev);
#ifdef CONFIG_INPUT_BOOSTER
	input_booster_free(info->tkey_booster);
	info->tkey_booster = NULL;
#endif
	kfree(info);

	return 0;
}

static void abov_tk_shutdown(struct i2c_client *client)
{
	struct abov_ft1604_info *info = i2c_get_clientdata(client);
	u8 cmd = CMD_LED_OFF;

	info->enabled = false;

	cancel_delayed_work(&info->efs_open_work);
	abov_tk_i2c_write(client, ABOV_BTNSTATUS, &cmd, 1);
}

static int abov_tk_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct abov_ft1604_info *info = i2c_get_clientdata(client);

	if (!info->enabled)
		return 0;

	input_info(true, &info->client->dev, "%s: users=%d\n", __func__,
		   info->input_dev->users);

	disable_irq(info->irq);
	info->enabled = false;
	release_all_fingers(info);

	if (info->dtdata->power)
		info->dtdata->power(info->dtdata, false);

	if (gpio_get_value(info->dtdata->gpio_en))
		gpio_direction_output(info->dtdata->gpio_en, 0);

	abov_pinctrl_configure(info, 0);

	return 0;
}

static int abov_tk_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct abov_ft1604_info *info = i2c_get_clientdata(client);
	u8 led_data;

	if (!info->probe_done)
		return 0;

	if (info->enabled)
		return 0;

	input_info(true, &info->client->dev, "%s: users=%d\n", __func__,
		   info->input_dev->users);

	abov_pinctrl_configure(info, 1);

	gpio_direction_output(info->dtdata->gpio_en, 1);

	if (info->dtdata->power)
		info->dtdata->power(info->dtdata, true);
	else
		get_tk_fw_version(info, true);

	msleep(ABOV_RESET_DELAY);

	info->enabled = true;

#ifdef GLOVE_MODE
	if (info->glovemode)
		abov_glove_mode_enable(client, CMD_GLOVE_ON);
#endif

	if (abov_touchled_cmd_reserved && abov_touchkey_led_status == CMD_LED_ON ) {
		abov_touchled_cmd_reserved = 0;
		led_data=abov_touchkey_led_status;

		if(info->dtdata->gpio_tkey_led_en >= 0)
			gpio_direction_output(info->dtdata->gpio_tkey_led_en,1);
		abov_tk_i2c_write(client, ABOV_BTNSTATUS, &led_data, 1);

		input_info(true, &info->client->dev, "%s: LED reserved %s\n",
			__func__, (led_data == CMD_LED_ON) ? "on" : "off");
	}
	enable_irq(info->irq);

	return 0;
}

static int abov_tk_input_open(struct input_dev *dev)
{
	struct abov_ft1604_info *info = input_get_drvdata(dev);

	//gpio_direction_input(info->dtdata->gpio_scl);
	//gpio_direction_input(info->dtdata->gpio_sda);

	abov_tk_resume(&info->client->dev);

	return 0;
}
static void abov_tk_input_close(struct input_dev *dev)
{
	struct abov_ft1604_info *info = input_get_drvdata(dev);

#ifdef LED_TWINKLE_BOOTING
	info->led_twinkle_check = 0;
#endif
	abov_tk_suspend(&info->client->dev);
	//gpio_set_value(info->dtdata->gpio_scl, 1);
	//gpio_set_value(info->dtdata->gpio_sda, 1);
}

static const struct i2c_device_id abov_tk_id[] = {
	{ABOV_TK_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, abov_tk_id);

#ifdef CONFIG_OF
static struct of_device_id abov_match_table[] = {
	{ .compatible = "abov,mc96ft16xx",},
	{ },
};
#else
#define abov_match_table NULL
#endif

static struct i2c_driver abov_tk_driver = {
	.probe = abov_tk_probe,
	.remove = abov_tk_remove,
	.shutdown = abov_tk_shutdown,
	.driver = {
		   .name = ABOV_TK_NAME,
		   .of_match_table = abov_match_table,
	},
	.id_table = abov_tk_id,
};

static int __init touchkey_init(void)
{
	pr_err("%s %s: abov,mc96ft16xx\n", SECLOG, __func__);
#if defined(CONFIG_SAMSUNG_LPM_MODE)
	if (poweroff_charging) {
		pr_notice("%s %s : LPM Charging Mode!!\n", SECLOG, __func__);
		return 0;
	}
#endif

	return i2c_add_driver(&abov_tk_driver);
}

static void __exit touchkey_exit(void)
{
	i2c_del_driver(&abov_tk_driver);
}

module_init_async(touchkey_init);
module_exit(touchkey_exit);

/* Module information */
MODULE_AUTHOR("Samsung Electronics");
MODULE_DESCRIPTION("Touchkey driver for Abov MF16xx chip");
MODULE_LICENSE("GPL");
