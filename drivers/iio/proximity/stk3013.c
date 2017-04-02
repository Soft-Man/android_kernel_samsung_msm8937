/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/errno.h>
#include <linux/wakelock.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/regulator/consumer.h>
#include <linux/of_gpio.h>

#include <linux/sensor/sensors_core.h>
#include "stk3013.h"

///IIO additions...
//
#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/kfifo_buf.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/events.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>

#define DRIVER_VERSION  "3.10.0_ps_only_20150508"

/* Driver Settings */
#define STK_INT_PS_MODE 1	/* 1, 2, or 3 */

#define PROX_READ_NUM   40

/* Define Register Map */
#define STK_STATE_REG           0x00
#define STK_PSCTRL_REG          0x01
#define STK_LEDCTRL_REG         0x03
#define STK_INT_REG             0x04
#define STK_WAIT_REG            0x05
#define STK_THDH1_PS_REG        0x06
#define STK_THDH2_PS_REG        0x07
#define STK_THDL1_PS_REG        0x08
#define STK_THDL2_PS_REG        0x09
#define STK_FLAG_REG            0x10
#define STK_DATA1_PS_REG        0x11
#define STK_DATA2_PS_REG        0x12
#define STK_DATA1_OFFSET_REG    0x15
#define STK_DATA2_OFFSET_REG    0x16
#define STK_DATA1_IR_REG        0x17
#define STK_DATA2_IR_REG        0x18
#define STK_PDT_ID_REG          0x3E
#define STK_RSRVD_REG           0x3F
#define STK_SW_RESET_REG        0x80

#define STK_GSCTRL_REG          0x1A
#define STK_FLAG2_REG           0x1C

/* Define state reg */
#define STK_STATE_EN_IRS_SHIFT  7
#define STK_STATE_EN_AK_SHIFT   6
#define STK_STATE_EN_ASO_SHIFT  5
#define STK_STATE_EN_IRO_SHIFT  4
#define STK_STATE_EN_WAIT_SHIFT 2
#define STK_STATE_EN_PS_SHIFT   0

#define STK_STATE_EN_IRS_MASK   0x80
#define STK_STATE_EN_AK_MASK    0x40
#define STK_STATE_EN_ASO_MASK   0x20
#define STK_STATE_EN_IRO_MASK   0x10
#define STK_STATE_EN_WAIT_MASK  0x04
#define STK_STATE_EN_PS_MASK    0x01

/* Define PS ctrl reg */
#define STK_PS_PRS_SHIFT        6
#define STK_PS_GAIN_SHIFT       4
#define STK_PS_IT_SHIFT         0

#define STK_PS_PRS_MASK         0xC0
#define STK_PS_GAIN_MASK        0x30
#define STK_PS_IT_MASK          0x0F

/* Define LED ctrl reg */
#define STK_LED_IRDR_SHIFT      6
#define STK_LED_DT_SHIFT        0

#define STK_LED_IRDR_MASK       0xC0
#define STK_LED_DT_MASK         0x3F

/* Define interrupt reg */
#define STK_INT_CTRL_SHIFT      7
#define STK_INT_OUI_SHIFT       4
#define STK_INT_PS_SHIFT        0

#define STK_INT_CTRL_MASK       0x80
#define STK_INT_OUI_MASK        0x10
#define STK_INT_PS_MASK         0x07

/* Define flag reg */
#define STK_FLG_PSDR_SHIFT      6
#define STK_FLG_PSINT_SHIFT     4
#define STK_FLG_OUI_SHIFT       2
#define STK_FLG_IR_RDY_SHIFT    1
#define STK_FLG_NF_SHIFT        0

#define STK_FLG_PSDR_MASK       0x40
#define STK_FLG_PSINT_MASK      0x10
#define STK_FLG_OUI_MASK        0x04
#define STK_FLG_IR_RDY_MASK     0x02
#define STK_FLG_NF_MASK         0x01

#define VENDOR           "SENSORTEK"
#define CHIP_ID          "STK3013"
#define MODULE_NAME      "proximity_sensor"

#define STK3310SA_PID    0x17
#define STK3311SA_PID    0x1E
#define STK3311WV_PID    0x1D

#define PROXIMITY_CALIBRATION
#ifdef PROXIMITY_CALIBRATION
#define CALIBRATION_FILE_PATH   "/efs/FactoryApp/prox_cal"
#endif

#define STK3013_PROX_INFO_SHARED_MASK		(BIT(IIO_CHAN_INFO_SCALE))
#define STK3013_PROX_INFO_SEPARATE_MASK		(BIT(IIO_CHAN_INFO_RAW))

#define STK3013_PROX_CHANNEL()						\
{									\
	.type = IIO_PROXIMITY,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_PROXIMITY,					\
	.info_mask_separate = STK3013_PROX_INFO_SEPARATE_MASK,		\
	.info_mask_shared_by_type = STK3013_PROX_INFO_SHARED_MASK,	\
	.scan_index = STK3013_SCAN_PROX_CH,				\
	.scan_type = {						\
		.sign = 's',					\
		.realbits = 32,				\
		.storagebits = 32,				\
		.shift = 0,				\
	},							\
}


enum {
	STK3013_SCAN_PROX_CH,
	STK3013_SCAN_PROX_TIMESTAMP,
};

enum {
	OFF = 0,
	ON,
};

struct stk3013_data {
	struct i2c_client *client;
	struct stk3013_platform_data *pdata;
	int32_t irq;
	struct work_struct stk_work;
	struct workqueue_struct *stk_wq;
	uint16_t ir_code;
	uint8_t psctrl_reg;
	uint8_t ledctrl_reg;
	uint8_t state_reg;
	int int_pin;
	uint8_t wait_reg;
	uint8_t int_reg;
	uint16_t ps_thd_h;
	uint16_t ps_thd_l;
	uint16_t ps_default_thd_h;
	uint16_t ps_default_thd_l;
	uint16_t ps_cancel_thd_h;
	uint16_t ps_cancel_thd_l;
	uint16_t ps_cal_skip_adc;
	uint16_t ps_cal_fail_adc;
	uint16_t ps_default_offset;
	uint16_t ps_offset;
	unsigned int cal_result;
	struct mutex io_lock;
	struct input_dev *ps_input_dev;
	int32_t ps_distance_last;
	bool ps_enabled;
	bool re_enable_ps;
	struct wake_lock ps_wakelock;
	ktime_t ps_poll_delay;
	bool first_boot;
	atomic_t recv_reg;
	uint8_t pid;
	uint8_t p_wv_r_bd_with_co;
	struct regulator *vdd;
	struct regulator *vio;
	struct device *ps_dev;
	struct hrtimer prox_timer;
	ktime_t prox_poll_delay;
	struct workqueue_struct *prox_wq;
	struct work_struct work_prox;
	int avg[3];

	int prox_delay;
	struct wake_lock prx_wake_lock;
	struct iio_trigger *prox_trig;
	int16_t sampling_frequency_prox;
	atomic_t pseudo_irq_enable_prox;
	struct mutex lock;
	spinlock_t spin_lock;
	struct iio_dev *indio_dev_prox;
};

static int32_t stk3013_set_ps_thd_l(struct stk3013_data *ps_data,
					uint16_t thd_l);
static int32_t stk3013_set_ps_thd_h(struct stk3013_data *ps_data,
					uint16_t thd_h);
static int32_t stk3013_set_ps_offset(struct stk3013_data *ps_data,
					uint16_t ps_offset);
#ifdef PROXIMITY_CALIBRATION
static int check_calibration_offset(struct stk3013_data *ps_data);
#endif
static int32_t stk3013_init_all_setting(struct i2c_client *client,
				struct stk3013_platform_data *plat_data);

struct stk3013_sensor_data {
	struct stk3013_data *cdata;
};

static inline s64 stk3013_iio_get_boottime_ns(void)
{
	struct timespec ts;

	ts = ktime_to_timespec(ktime_get_boottime());

	return timespec_to_ns(&ts);
}

irqreturn_t stk3013_iio_pollfunc_store_boottime(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	pf->timestamp = stk3013_iio_get_boottime_ns();
	return IRQ_WAKE_THREAD;
}

static int stk3013_prox_data_rdy_trig_poll(struct iio_dev *indio_dev)
{
	struct stk3013_sensor_data *sdata = iio_priv(indio_dev);
	struct stk3013_data *stk3013_iio = sdata->cdata;
	unsigned long flags;

	spin_lock_irqsave(&stk3013_iio->spin_lock, flags);
	iio_trigger_poll(stk3013_iio->prox_trig);
	spin_unlock_irqrestore(&stk3013_iio->spin_lock, flags);
	return 0;
}

static int stk3013_i2c_read_data(struct i2c_client *client,
	unsigned char command, int length, unsigned char *values)
{
	uint8_t retry;
	int ret;
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 1,
			.buf = &command,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = values,
		},
	};

	for (retry = 0; retry < 5; retry++) {
		ret = i2c_transfer(client->adapter, msgs, 2);
		if (ret == 2)
			break;
	}

	if (retry >= 5) {
		SENSOR_ERR("i2c read fail, err=%d\n", ret);
		return -EIO;
	}
	return 0;
}

static int stk3013_i2c_write_data(struct i2c_client *client,
	unsigned char command, int length, unsigned char *values)
{
	int retry;
	int ret;
	unsigned char data[11];
	struct i2c_msg msg;
	int index;

	if (!client)
		return -EINVAL;
	else if (length >= 10) {
		SENSOR_ERR("length %d exceeds 10\n", length);
		return -EINVAL;
	}

	data[0] = command;
	for (index = 1; index <= length; index++)
		data[index] = values[index-1];

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = length+1;
	msg.buf = data;

	for (retry = 0; retry < 5; retry++) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret == 1)
			break;
	}

	if (retry >= 5) {
		SENSOR_ERR("i2c write fail, err=%d\n", ret);
		return -EIO;
	}
	return 0;
}

static int stk3013_i2c_smbus_read_byte_data(struct i2c_client *client,
				unsigned char command)
{
	unsigned char value;
	int ret;

	ret = stk3013_i2c_read_data(client, command, 1, &value);
	if (ret < 0)
		return ret;
	return value;
}

static int stk3013_i2c_smbus_write_byte_data(struct i2c_client *client,
				unsigned char command, unsigned char value)
{
	int ret;

	ret = stk3013_i2c_write_data(client, command, 1, &value);
	return ret;
}

static void stk3013_proc_plat_data(struct stk3013_data *ps_data,
				struct stk3013_platform_data *plat_data)
{
	uint8_t w_reg;

	ps_data->state_reg = plat_data->state_reg;
	ps_data->psctrl_reg = plat_data->psctrl_reg;
	ps_data->ledctrl_reg = plat_data->ledctrl_reg;
	if (ps_data->pid == STK3310SA_PID || ps_data->pid == STK3311SA_PID)
		ps_data->ledctrl_reg &= 0x3F;

	ps_data->wait_reg = plat_data->wait_reg;
	if (ps_data->wait_reg < 2) {
		SENSOR_INFO("wait_reg should be larger than 2, force to write 2\n");
		ps_data->wait_reg = 2;
	} else if (ps_data->wait_reg > 0xFF) {
		SENSOR_INFO("wait_reg should be less than 0xFF, force to write 0xFF\n");
		ps_data->wait_reg = 0xFF;
	}
	if (ps_data->ps_thd_h == 0 && ps_data->ps_thd_l == 0) {
		ps_data->ps_thd_h = plat_data->ps_thd_h;
		ps_data->ps_thd_l = plat_data->ps_thd_l;
		ps_data->ps_default_thd_h = plat_data->ps_thd_h;
		ps_data->ps_default_thd_l = plat_data->ps_thd_l;
		ps_data->ps_cancel_thd_h = plat_data->ps_cancel_thd_h;
		ps_data->ps_cancel_thd_l = plat_data->ps_cancel_thd_l;
		ps_data->ps_cal_skip_adc = plat_data->ps_cal_skip_adc;
		ps_data->ps_cal_fail_adc = plat_data->ps_cal_fail_adc;
		/*initialize the offset data*/
		ps_data->ps_default_offset = plat_data->ps_default_offset;
		ps_data->ps_offset = ps_data->ps_default_offset;
	}

	w_reg = 0;
	w_reg |= STK_INT_PS_MODE;

	ps_data->int_reg = w_reg;
}

static int32_t stk3013_init_all_reg(struct stk3013_data *ps_data)
{
	int32_t ret;

	ret = stk3013_i2c_smbus_write_byte_data(ps_data->client,
				STK_STATE_REG, ps_data->state_reg);
	if (ret < 0) {
		SENSOR_ERR("write i2c error\n");
		return ret;
	}
	ret = stk3013_i2c_smbus_write_byte_data(ps_data->client,
				STK_PSCTRL_REG, ps_data->psctrl_reg);
	if (ret < 0) {
		SENSOR_ERR("write i2c error\n");
		return ret;
	}
	ret = stk3013_i2c_smbus_write_byte_data(ps_data->client,
				STK_LEDCTRL_REG, ps_data->ledctrl_reg);
	if (ret < 0) {
		SENSOR_ERR("write i2c error\n");
		return ret;
	}
	ret = stk3013_i2c_smbus_write_byte_data(ps_data->client,
				STK_WAIT_REG, ps_data->wait_reg);
	if (ret < 0) {
		SENSOR_ERR("write i2c error\n");
		return ret;
	}
	stk3013_set_ps_thd_h(ps_data, ps_data->ps_thd_h);
	stk3013_set_ps_thd_l(ps_data, ps_data->ps_thd_l);
	stk3013_set_ps_offset(ps_data, ps_data->ps_default_offset);
	ret = stk3013_i2c_smbus_write_byte_data(ps_data->client,
					STK_INT_REG, ps_data->int_reg);
	if (ret < 0) {
		SENSOR_ERR("write i2c error\n");
		return ret;
	}

	return 0;
}

static int32_t stk3013_read_otp25(struct stk3013_data *ps_data)
{
	int32_t ret, otp25;

	ret = stk3013_i2c_smbus_write_byte_data(ps_data->client, 0x0, 0x2);
	if (ret < 0) {
		SENSOR_ERR("write i2c error\n");
		return ret;
	}

	ret = stk3013_i2c_smbus_write_byte_data(ps_data->client, 0x90, 0x25);
	if (ret < 0) {
		SENSOR_ERR("write i2c error\n");
		return ret;
	}

	ret = stk3013_i2c_smbus_write_byte_data(ps_data->client, 0x92, 0x82);
	if (ret < 0) {
		SENSOR_ERR("write i2c error\n");
		return ret;
	}
	usleep_range(1000, 5000);

	ret = stk3013_i2c_smbus_read_byte_data(ps_data->client, 0x91);
	if (ret < 0) {
		SENSOR_ERR("fail, ret=%d\n", ret);
		return ret;
	}
	otp25 = ret;

	ret = stk3013_i2c_smbus_write_byte_data(ps_data->client, 0x0, 0x0);
	if (ret < 0) {
		SENSOR_ERR("write i2c error\n");
		return ret;
	}
	SENSOR_INFO("otp25=0x%x\n", otp25);
	if (otp25 & 0x80)
		return 1;
	return 0;
}

static int32_t stk3013_check_pid(struct stk3013_data *ps_data)
{
	unsigned char value[2], pid_msb;
	int ret;

	ps_data->p_wv_r_bd_with_co = 0;

	ret = stk3013_i2c_read_data(ps_data->client,
				STK_PDT_ID_REG, 2, &value[0]);
	if (ret < 0) {
		SENSOR_ERR("fail, ret=%d\n", ret);
		return ret;
	}

	SENSOR_INFO("PID=0x%x, RID=0x%x\n", value[0], value[1]);
	ps_data->pid = value[0];

	if (value[0] == STK3311WV_PID)
		ps_data->p_wv_r_bd_with_co |= 0xb100;
	if (value[1] == 0xC3)
		ps_data->p_wv_r_bd_with_co |= 0xb010;

	if (stk3013_read_otp25(ps_data) == 1)
		ps_data->p_wv_r_bd_with_co |= 0xb001;
	SENSOR_INFO("p_wv_r_bd_with_co = 0x%x\n", ps_data->p_wv_r_bd_with_co);

	if (value[0] == 0) {
		SENSOR_ERR("PID=0x0, please make sure the chip is stk3013!\n");
		return -ENXIO;
	}

	pid_msb = value[0] & 0xF0;
	switch (pid_msb) {
	case 0x10:
	case 0x20:
	case 0x30:
		return 0;
	default:
		SENSOR_ERR("invalid PID(%#x)\n", value[0]);
		return -EPERM;
	}
	return 0;
}

static int32_t stk3013_software_reset(struct stk3013_data *ps_data)
{
	int32_t r;
	uint8_t w_reg;

	w_reg = 0x7F;
	r = stk3013_i2c_smbus_write_byte_data(ps_data->client,
						STK_WAIT_REG, w_reg);
	if (r < 0) {
		SENSOR_ERR("software reset: write i2c error, ret=%d\n", r);
		return r;
	}
	r = stk3013_i2c_smbus_read_byte_data(ps_data->client, STK_WAIT_REG);
	if (w_reg != r) {
		SENSOR_ERR("software reset: read-back value is not the same\n");
		return -EPERM;
	}

	r = stk3013_i2c_smbus_write_byte_data(ps_data->client,
						STK_SW_RESET_REG, 0);
	if (r < 0) {
		SENSOR_ERR("software reset: read error after reset\n");
		return r;
	}
	usleep_range(13000, 15000);
	return 0;
}

static int32_t stk3013_set_ps_thd_l(struct stk3013_data *ps_data,
						uint16_t thd_l)
{
	unsigned char val[2];
	int ret;

	val[0] = (thd_l & 0xFF00) >> 8;
	val[1] = thd_l & 0x00FF;
	ret = stk3013_i2c_write_data(ps_data->client,
				STK_THDL1_PS_REG, 2, val);
	if (ret < 0)
		SENSOR_ERR("fail, ret=%d\n", ret);
	else
		ps_data->ps_thd_l = thd_l;

	SENSOR_INFO("thd_l=%d\n", thd_l);
	return ret;
}
static int32_t stk3013_set_ps_thd_h(struct stk3013_data *ps_data,
						uint16_t thd_h)
{
	unsigned char val[2];
	int ret;

	val[0] = (thd_h & 0xFF00) >> 8;
	val[1] = thd_h & 0x00FF;
	ret = stk3013_i2c_write_data(ps_data->client,
				STK_THDH1_PS_REG, 2, val);
	if (ret < 0)
		SENSOR_ERR("fail, ret=%d\n", ret);
	else
		ps_data->ps_thd_h = thd_h;

	SENSOR_INFO("thd_h=%d\n", thd_h);
	return ret;
}
static int32_t stk3013_set_ps_offset(struct stk3013_data *ps_data,
						uint16_t ps_offset)
{
	unsigned char val[2];
	int ret;

	val[0] = (ps_offset & 0xFF00) >> 8;
	val[1] = ps_offset & 0x00FF;

	ret = stk3013_i2c_write_data(ps_data->client,
			STK_DATA1_OFFSET_REG, 2, val);
	if (ret < 0)
		SENSOR_ERR("fail, ret=%d\n", ret);
	return ret;
}

static uint32_t stk3013_get_ps_reading(struct stk3013_data *ps_data)
{
	unsigned char value[2];
	int ret;

	ret = stk3013_i2c_read_data(ps_data->client,
			STK_DATA1_PS_REG, 2, &value[0]);
	if (ret < 0) {
		SENSOR_ERR("DATA1 fail, ret=%d\n", ret);
		return ret;
	}

	return (value[0]<<8) | value[1];
}

static int32_t stk3013_set_flag(struct stk3013_data *ps_data,
					uint8_t org_flag_reg, uint8_t clr)
{
	uint8_t w_flag;
	int ret;

	w_flag = org_flag_reg | (STK_FLG_PSINT_MASK | STK_FLG_OUI_MASK |
					STK_FLG_IR_RDY_MASK);
	w_flag &= (~clr);
	/*SENSOR_INFO(" org_flag_reg=0x%x, w_flag = 0x%x\n",
		org_flag_reg, w_flag);*/
	ret = stk3013_i2c_smbus_write_byte_data(ps_data->client,
						STK_FLAG_REG, w_flag);
	if (ret < 0)
		SENSOR_ERR("fail, ret=%d\n", ret);

	return ret;
}

static int32_t stk3013_get_flag(struct stk3013_data *ps_data)
{
	int ret;

	ret = stk3013_i2c_smbus_read_byte_data(ps_data->client,
						STK_FLAG_REG);
	if (ret < 0)
		SENSOR_ERR("fail, ret=%d\n", ret);
	return ret;
}

static int32_t stk3013_set_state(struct stk3013_data *ps_data, uint8_t state)
{
	int ret;

	ret = stk3013_i2c_smbus_write_byte_data(ps_data->client,
						STK_STATE_REG, state);
	if (ret < 0)
		SENSOR_ERR("fail, ret=%d\n", ret);

	return ret;
}

static int32_t stk3013_get_state(struct stk3013_data *ps_data)
{
	int ret;

	ret = stk3013_i2c_smbus_read_byte_data(ps_data->client, STK_STATE_REG);
	if (ret < 0)
		SENSOR_ERR("fail, ret=%d\n", ret);
	return ret;
}

static int32_t stk3013_enable_ps(struct stk3013_data *ps_data,
			uint8_t enable, uint8_t validate_reg)
{
	int32_t ret;
	uint8_t w_state_reg;
	uint8_t curr_ps_enable;
	uint32_t read_value;
	int32_t near_far_state;

	curr_ps_enable = ps_data->ps_enabled ? 1 : 0;
	if (curr_ps_enable == enable)
		return 0;

	if (enable) {
		msleep(20);
		ret = stk3013_init_all_setting(ps_data->client,
							ps_data->pdata);
		if (ret < 0) {
			SENSOR_ERR("init setting fail, ret=%d\n", ret);
			return ret;
		}
	}

	if (ps_data->first_boot == true)
		ps_data->first_boot = false;

	ret = stk3013_get_state(ps_data);
	if (ret < 0)
		return ret;
	w_state_reg = ret;

	w_state_reg &= ~(STK_STATE_EN_PS_MASK | STK_STATE_EN_WAIT_MASK | STK_STATE_EN_AK_MASK);
	if (enable)
		w_state_reg |= (STK_STATE_EN_PS_MASK | STK_STATE_EN_WAIT_MASK);

	ret = stk3013_set_state(ps_data, w_state_reg);
	if (ret < 0)
		return ret;
	ps_data->state_reg = w_state_reg;

	if (enable) {
#ifdef PROXIMITY_CALIBRATION
		check_calibration_offset(ps_data);
		stk3013_set_ps_offset(ps_data, ps_data->ps_offset);
#endif
		enable_irq(ps_data->irq);
		ps_data->ps_enabled = true;
		{
			usleep_range(4000, 5000);
			ret = stk3013_get_flag(ps_data);
			if (ret < 0)
				return ret;
			near_far_state = ret & STK_FLG_NF_MASK;
			ps_data->ps_distance_last = near_far_state;
			stk3013_prox_data_rdy_trig_poll(ps_data->indio_dev_prox);
			wake_lock_timeout(&ps_data->ps_wakelock, 3*HZ);
			read_value = stk3013_get_ps_reading(ps_data);
			SENSOR_INFO("ps input event=%d, ps code = %d\n",
					near_far_state, read_value);
		}
	} else {
		disable_irq(ps_data->irq);
		ps_data->ps_enabled = false;
	}
	return ret;
}

#ifdef PROXIMITY_CALIBRATION
static int proximity_store_calibration(struct device *dev, bool do_calib)
{
	struct stk3013_data *ps_data =  dev_get_drvdata(dev);
	struct file *cal_filp = NULL;
	mm_segment_t old_fs;
	unsigned char value[2];
	int ret;
	uint16_t temp[2];
	uint16_t offset_data = 0;

	SENSOR_INFO("start\n");

	if (do_calib) {
		ret = stk3013_i2c_read_data(ps_data->client,
				STK_DATA1_PS_REG, 2, &value[0]);
		if (ret < 0) {
			SENSOR_ERR("DATA1 fail, ret=%d\n", ret);
			return ret;
		}
		offset_data = ((value[0]<<8) | value[1]);
		SENSOR_INFO("ps_offset =  %d\n", offset_data);
		if (offset_data < ps_data->ps_cal_skip_adc) {
			SENSOR_INFO("skip calibration = %d\n", offset_data);
			ps_data->ps_offset = ps_data->ps_default_offset;
			ps_data->cal_result = 2;
		} else if (offset_data <= ps_data->ps_cal_fail_adc/*DO_CAL*/) {
			SENSOR_INFO("do calibration =  %d\n", offset_data);
			temp[0] = ps_data->ps_default_offset;
			ps_data->ps_offset = offset_data + ps_data->ps_default_offset;
			ret = stk3013_set_ps_offset(ps_data,
					ps_data->ps_offset);
			if (ret < 0) {
				SENSOR_ERR("calibration fail\n");
				ps_data->ps_default_offset = temp[0];
				ps_data->cal_result = 0;
			} else {
				stk3013_set_ps_thd_h(ps_data,
					ps_data->ps_cancel_thd_h);
				stk3013_set_ps_thd_l(ps_data,
					ps_data->ps_cancel_thd_l);
				ps_data->cal_result = 1;
			}
		} else {
			SENSOR_INFO("fail offset calibration = %d\n",
					offset_data);
			ps_data->ps_offset = ps_data->ps_default_offset;
		}
	} else {
		/*reset*/
		SENSOR_INFO("reset start\n");
		temp[0] = ps_data->ps_offset;
		temp[1] = ps_data->cal_result;
		ps_data->ps_offset = ps_data->ps_default_offset;
		ps_data->cal_result = 0;
		ret = stk3013_set_ps_offset(ps_data, ps_data->ps_offset);
		if (ret < 0) {
			SENSOR_ERR("calibration reset fail\n");
			ps_data->ps_default_offset = temp[0];
			ps_data->cal_result = temp[1];
		}
		SENSOR_INFO("ps_thd_h=%d, ps_thd_l=%d, ps_offset=%d\n",
				ps_data->ps_thd_h, ps_data->ps_thd_l,
				ps_data->ps_offset);
		stk3013_set_ps_thd_h(ps_data, ps_data->ps_default_thd_h);
		stk3013_set_ps_thd_l(ps_data, ps_data->ps_default_thd_l);
	}

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	cal_filp = filp_open(CALIBRATION_FILE_PATH,
		O_CREAT | O_TRUNC | O_WRONLY | O_SYNC, 0666);
	if (IS_ERR(cal_filp)) {
		SENSOR_ERR("Can't open calibration file\n");
		set_fs(old_fs);
		ret = PTR_ERR(cal_filp);
		return ret;
	}

	ret = cal_filp->f_op->write(cal_filp,
		(char *)&ps_data->ps_offset,
		sizeof(u16), &cal_filp->f_pos);
	if (ret != sizeof(u16)) {
		SENSOR_ERR("Can't write the cancel data to file\n");
		ret = -EIO;
	}

	filp_close(cal_filp, current->files);
	set_fs(old_fs);
	SENSOR_INFO("end\n");
	return ret;
}

static ssize_t proximity_calibration_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	bool do_calib;
	int err;

	if (sysfs_streq(buf, "1")) /* calibrate cancelation value */
		do_calib = true;
	else if (sysfs_streq(buf, "0")) /* reset cancelation value */
		do_calib = false;
	else {
		SENSOR_ERR("invalid value %d\n", *buf);
		return -EINVAL;
	}
	SENSOR_INFO("%d\n", do_calib);
	err = proximity_store_calibration(dev, do_calib);
	if (err < 0) {
		SENSOR_ERR("proximity_store_cancelation() failed\n");
		return err;
	}

	return size;
}

static ssize_t proximity_calibration_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct stk3013_data *ps_data =  dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%u,%u,%u\n",
		ps_data->ps_offset,
		(ps_data->ps_offset != ps_data->ps_default_offset) ? ps_data->ps_cancel_thd_h : ps_data->ps_thd_h,
		(ps_data->ps_offset != ps_data->ps_default_offset) ? ps_data->ps_cancel_thd_l : ps_data->ps_thd_l);
}

static ssize_t proximity_calibration_pass_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct stk3013_data *ps_data =  dev_get_drvdata(dev);

	SENSOR_INFO("result = %d\n", ps_data->cal_result);
	return snprintf(buf, PAGE_SIZE, "%u\n",
		ps_data->cal_result);
}
#endif

static ssize_t proximity_avg_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct stk3013_data *ps_data =  dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d,%d,%d\n", ps_data->avg[0],
		ps_data->avg[1], ps_data->avg[2]);
}
static void proximity_get_avg_val(struct stk3013_data *ps_data)
{
	int min = 0, max = 0, avg = 0;
	int i;
	uint32_t read_value;

	for (i = 0; i < PROX_READ_NUM; i++) {
		msleep(40);
		read_value = stk3013_get_ps_reading(ps_data);
		avg += read_value;

		if (!i)
			min = read_value;
		else if (read_value < min)
			min = read_value;

		if (read_value > max)
			max = read_value;
	}
	avg /= PROX_READ_NUM;

	ps_data->avg[0] = min;
	ps_data->avg[1] = avg;
	ps_data->avg[2] = max;
}

static ssize_t proximity_avg_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct stk3013_data *ps_data =  dev_get_drvdata(dev);
	bool new_value = false;

	if (sysfs_streq(buf, "1"))
		new_value = true;
	else if (sysfs_streq(buf, "0"))
		new_value = false;
	else {
		SENSOR_ERR("invalid value %d\n", *buf);
		return -EINVAL;
	}

	SENSOR_INFO("average enable = %d\n",  new_value);
	if (new_value) {
		if ((ps_data->ps_enabled ? 1 : 0) == OFF) {
			mutex_lock(&ps_data->io_lock);
			stk3013_enable_ps(ps_data, new_value, 1);
			mutex_unlock(&ps_data->io_lock);
		}
		hrtimer_start(&ps_data->prox_timer, ps_data->prox_poll_delay,
			HRTIMER_MODE_REL);
	} else if (!new_value) {
		hrtimer_cancel(&ps_data->prox_timer);
		cancel_work_sync(&ps_data->work_prox);
		if ((ps_data->ps_enabled ? 1 : 0) == OFF) {
			mutex_lock(&ps_data->io_lock);
			stk3013_enable_ps(ps_data, new_value, 0);
			mutex_unlock(&ps_data->io_lock);
		}
	}

	return size;
}

static ssize_t proximity_trim_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct stk3013_data *ps_data =  dev_get_drvdata(dev);

	SENSOR_INFO("trim: %d\n", ps_data->ps_default_offset);
	return snprintf(buf, PAGE_SIZE, "%u\n",
		ps_data->ps_default_offset);
}

static void stk3013_work_func_prox(struct work_struct *work)
{
	struct stk3013_data *ps_data = container_of(work,
		struct stk3013_data, work_prox);

	proximity_get_avg_val(ps_data);
}
/*
static enum hrtimer_restart stk3013_prox_timer_func(struct hrtimer *timer)
{
	struct stk3013_data *ps_data = container_of(timer,
		struct stk3013_data, prox_timer);

	queue_work(ps_data->prox_wq, &ps_data->work_prox);
	hrtimer_forward_now(&ps_data->prox_timer, ps_data->prox_poll_delay);
	return HRTIMER_RESTART;
}
*/
static ssize_t proximity_thresh_high_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int32_t ps_thd_h1_reg, ps_thd_h2_reg;
	struct stk3013_data *ps_data =  dev_get_drvdata(dev);

	ps_thd_h1_reg = stk3013_i2c_smbus_read_byte_data(ps_data->client,
							STK_THDH1_PS_REG);
	if (ps_thd_h1_reg < 0) {
		SENSOR_ERR("fail, err=0x%x", ps_thd_h1_reg);
		return -EINVAL;
	}
	ps_thd_h2_reg = stk3013_i2c_smbus_read_byte_data(ps_data->client,
							STK_THDH2_PS_REG);
	if (ps_thd_h2_reg < 0) {
		SENSOR_ERR("fail, err=0x%x", ps_thd_h2_reg);
		return -EINVAL;
	}
	ps_thd_h1_reg = ps_thd_h1_reg<<8 | ps_thd_h2_reg;
	SENSOR_INFO("thresh:0x%x",  ps_thd_h1_reg);
	return scnprintf(buf, PAGE_SIZE, "%d\n", ps_thd_h1_reg);
}

static ssize_t proximity_thresh_high_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct stk3013_data *ps_data =  dev_get_drvdata(dev);
	u16 value = 0;
	int ret;

	ret = kstrtou16(buf, 10, &value);
	if (ret < 0) {
		SENSOR_ERR("kstrtoul failed, ret=0x%x\n", ret);
		return ret;
	}
	SENSOR_INFO("thresh: %d\n",  value);
	stk3013_set_ps_thd_h(ps_data, value);
	return size;
}

static ssize_t proximity_thresh_low_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int32_t ps_thd_l1_reg, ps_thd_l2_reg;
	struct stk3013_data *ps_data = dev_get_drvdata(dev);

	ps_thd_l1_reg = stk3013_i2c_smbus_read_byte_data(ps_data->client,
							STK_THDL1_PS_REG);
	if (ps_thd_l1_reg < 0) {
		SENSOR_ERR("fail, err=0x%x", ps_thd_l1_reg);
		return -EINVAL;
	}
	ps_thd_l2_reg = stk3013_i2c_smbus_read_byte_data(ps_data->client,
							STK_THDL2_PS_REG);
	if (ps_thd_l2_reg < 0) {
		SENSOR_ERR("fail, err=0x%x", ps_thd_l2_reg);
		return -EINVAL;
	}
	ps_thd_l1_reg = ps_thd_l1_reg<<8 | ps_thd_l2_reg;

	return scnprintf(buf, PAGE_SIZE, "%d\n", ps_thd_l1_reg);
}

static ssize_t proximity_thresh_low_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct stk3013_data *ps_data =  dev_get_drvdata(dev);
	u16 value = 0;
	int ret;

	ret = kstrtou16(buf, 10, &value);
	if (ret < 0) {
		SENSOR_ERR("kstrtoul failed, ret=0x%x\n", ret);
		return ret;
	}
	SENSOR_INFO("thresh: %d\n", value);
	stk3013_set_ps_thd_l(ps_data, value);
	return size;
}

static ssize_t proximity_state_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct stk3013_data *ps_data =  dev_get_drvdata(dev);
	uint32_t read_value;

	read_value = stk3013_get_ps_reading(ps_data);
	return scnprintf(buf, PAGE_SIZE, "%d\n", read_value);
}

static ssize_t stk3013_vendor_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", VENDOR);
}

static ssize_t stk3013_name_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", CHIP_ID);
}

#ifdef PROXIMITY_CALIBRATION
static DEVICE_ATTR(prox_cal, S_IRUGO | S_IWUSR | S_IWGRP,
	proximity_calibration_show, proximity_calibration_store);
static DEVICE_ATTR(prox_offset_pass, S_IRUGO, proximity_calibration_pass_show,
	NULL);
#endif
static DEVICE_ATTR(prox_avg, S_IRUGO | S_IWUSR | S_IWGRP,
	proximity_avg_show, proximity_avg_store);
static DEVICE_ATTR(prox_trim, S_IRUSR | S_IRGRP,
	proximity_trim_show, NULL);
static DEVICE_ATTR(thresh_high, S_IRUGO | S_IWUSR | S_IWGRP,
	proximity_thresh_high_show, proximity_thresh_high_store);
static DEVICE_ATTR(thresh_low, S_IRUGO | S_IWUSR | S_IWGRP,
	proximity_thresh_low_show, proximity_thresh_low_store);
static DEVICE_ATTR(state, S_IRUGO, proximity_state_show, NULL);
static DEVICE_ATTR(raw_data, S_IRUGO, proximity_state_show, NULL);
static DEVICE_ATTR(vendor, S_IRUSR | S_IRGRP, stk3013_vendor_show, NULL);
static DEVICE_ATTR(name, S_IRUSR | S_IRGRP, stk3013_name_show, NULL);

static struct device_attribute *prox_sensor_attrs[] = {
#ifdef PROXIMITY_CALIBRATION
	&dev_attr_prox_cal,
	&dev_attr_prox_offset_pass,
#endif
	&dev_attr_prox_avg,
	&dev_attr_prox_trim,
	&dev_attr_thresh_high,
	&dev_attr_thresh_low,
	&dev_attr_state,
	&dev_attr_raw_data,
	&dev_attr_vendor,
	&dev_attr_name,
	NULL,
};
/*
static ssize_t proximity_enable_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int32_t ret;
	struct stk3013_data *ps_data =  dev_get_drvdata(dev);

	ret = stk3013_get_state(ps_data);
	if (ret < 0)
		return ret;
	ret = (ret & STK_STATE_EN_PS_MASK) ? 1 : 0;

	return scnprintf(buf, PAGE_SIZE, "%d\n", ret);
}

static ssize_t proximity_enable_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct stk3013_data *ps_data = dev_get_drvdata(dev);
	uint8_t en;

	if (sysfs_streq(buf, "1"))
		en = 1;
	else if (sysfs_streq(buf, "0"))
		en = 0;
	else {
		SENSOR_ERR("invalid value %d\n", *buf);
		return -EINVAL;
	}
	SENSOR_INFO("Enable PS : %d\n", en);
	mutex_lock(&ps_data->io_lock);
	stk3013_enable_ps_fac(dev, en, 1);
	mutex_unlock(&ps_data->io_lock);
	return size;
}
*/
/*
static DEVICE_ATTR(enable, S_IRUGO | S_IWUSR | S_IWGRP,
	proximity_enable_show, proximity_enable_store);

static struct attribute *proximity_sysfs_attrs[] = {
	&dev_attr_enable.attr,
	NULL
};

static struct attribute_group proximity_attribute_group = {
	.attrs = proximity_sysfs_attrs,
};
*/
static const struct iio_chan_spec stk3013_prox_channels[] = {
	STK3013_PROX_CHANNEL(),
	IIO_CHAN_SOFT_TIMESTAMP(STK3013_SCAN_PROX_TIMESTAMP)
};

static int stk3013_read_raw_prox(struct iio_dev *indio_dev,
	struct iio_chan_spec const *chan, int *val, int *val2, long mask)
{
	struct stk3013_sensor_data *sdata = iio_priv(indio_dev);
	struct stk3013_data *stk3013_iio = sdata->cdata;
	int ret = -EINVAL;

	if (chan->type != IIO_PROXIMITY)
		return -EINVAL;

	pr_info(" %s\n", __func__);
	mutex_lock(&stk3013_iio->lock);

	switch (mask) {
	case 0:
		ret = IIO_VAL_INT;
		break;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = 1000;
		ret = IIO_VAL_INT_PLUS_MICRO;
		break;
	}

	mutex_unlock(&stk3013_iio->lock);

	return ret;
}

static const struct iio_info stk3013_prox_info= {

	.read_raw = &stk3013_read_raw_prox,
	.driver_module = THIS_MODULE,

};

static enum hrtimer_restart stk3013_prox_timer_func(struct hrtimer *timer)
{
	struct stk3013_data *stk3013_iio
			= container_of(timer, struct stk3013_data, prox_timer);
	queue_work(stk3013_iio->prox_wq, &stk3013_iio->work_prox);
	hrtimer_forward_now(&stk3013_iio->prox_timer,
			stk3013_iio->prox_poll_delay);
	return HRTIMER_RESTART;
}

static const struct iio_buffer_setup_ops stk3013_prox_buffer_setup_ops = {
	.postenable = &iio_triggered_buffer_postenable,
	.predisable = &iio_triggered_buffer_predisable,
};


static int stk3013_prox_probe_buffer(struct iio_dev *indio_dev)
{
	int ret;
	struct iio_buffer *buffer;

	buffer = iio_kfifo_allocate(indio_dev);

	if (!buffer) {
		ret = -ENOMEM;
		goto error_ret;
	}

	buffer->scan_timestamp = true;
	indio_dev->buffer = buffer;
	indio_dev->setup_ops = &stk3013_prox_buffer_setup_ops;
	indio_dev->modes |= INDIO_BUFFER_TRIGGERED;

	ret = iio_buffer_register(indio_dev, indio_dev->channels,
			indio_dev->num_channels);
	if (ret)
		goto error_free_buf;
	iio_scan_mask_set(indio_dev, indio_dev->buffer, STK3013_SCAN_PROX_CH);

	pr_err("%s is success\n", __func__);
	return 0;

error_free_buf:
	iio_kfifo_free(indio_dev->buffer);
error_ret:
	return ret;
}

static int stk3013_prox_pseudo_irq_enable(struct iio_dev *indio_dev)
{
	struct stk3013_sensor_data *sdata = iio_priv(indio_dev);
	struct stk3013_data *stk3013_iio = sdata->cdata;

	if (!atomic_cmpxchg(&stk3013_iio->pseudo_irq_enable_prox, 0, 1)) {
		mutex_lock(&stk3013_iio->lock);
		stk3013_enable_ps(stk3013_iio, 1, 1);
		mutex_unlock(&stk3013_iio->lock);
	}

	return 0;
}

static int stk3013_prox_pseudo_irq_disable(struct iio_dev *indio_dev)
{
	struct stk3013_sensor_data *sdata = iio_priv(indio_dev);
	struct stk3013_data *stk3013_iio = sdata->cdata;

	if (atomic_cmpxchg(&stk3013_iio->pseudo_irq_enable_prox, 1, 0)) {
		mutex_lock(&stk3013_iio->lock);
		stk3013_enable_ps(stk3013_iio, 0, 1);
		mutex_unlock(&stk3013_iio->lock);
	}
	return 0;
}

static int stk3013_prox_set_pseudo_irq(struct iio_dev *indio_dev, int enable)
{
	if (enable)
		stk3013_prox_pseudo_irq_enable(indio_dev);
	else
		stk3013_prox_pseudo_irq_disable(indio_dev);
	return 0;
}

static int stk3013_data_prox_rdy_trigger_set_state(struct iio_trigger *trig,
		bool state)
{
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trig);
	stk3013_prox_set_pseudo_irq(indio_dev, state);
	return 0;
}

static const struct iio_trigger_ops stk3013_prox_trigger_ops = {
	.owner = THIS_MODULE,
	.set_trigger_state = &stk3013_data_prox_rdy_trigger_set_state,
};

static irqreturn_t stk3013_prox_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct stk3013_sensor_data *sdata = iio_priv(indio_dev);
	struct stk3013_data *stk3013_iio = sdata->cdata;

	int len = 0;
	int ret = 0;
	int32_t *data;

	data = (int32_t *) kmalloc(indio_dev->scan_bytes, GFP_KERNEL);
	if (data == NULL)
		goto done;

	if (!bitmap_empty(indio_dev->active_scan_mask, indio_dev->masklength)) {
		/* TODO : data update */
		if (!stk3013_iio->ps_distance_last)
			*data = 0;
		else
			*data = 1;
	}

	len = 4;

	/* Guaranteed to be aligned with 8 byte boundary */
	if (indio_dev->scan_timestamp)
		*(s64 *)((u8 *)data + ALIGN(len, sizeof(s64))) = pf->timestamp;
	ret = iio_push_to_buffers(indio_dev, (u8 *)data);
	if (ret < 0)
		pr_err("%s, iio_push buffer failed = %d\n",
			__func__, ret);
	kfree(data);

done:
	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

static int stk3013_prox_probe_trigger(struct iio_dev *indio_dev)
{
	int ret;
	struct stk3013_sensor_data *sdata = iio_priv(indio_dev);
	struct stk3013_data *stk3013_iio = sdata->cdata;

	indio_dev->pollfunc = iio_alloc_pollfunc(&stk3013_iio_pollfunc_store_boottime,
			&stk3013_prox_trigger_handler, IRQF_ONESHOT, indio_dev,
			"%s_consumer%d", indio_dev->name, indio_dev->id);

	if (indio_dev->pollfunc == NULL) {
		ret = -ENOMEM;
		goto error_ret;
	}
	stk3013_iio->prox_trig = iio_trigger_alloc("%s-dev%d",
			indio_dev->name,
			indio_dev->id);
	if (!stk3013_iio->prox_trig) {
		ret = -ENOMEM;
		goto error_dealloc_pollfunc;
	}
	stk3013_iio->prox_trig->dev.parent = &stk3013_iio->client->dev;
	stk3013_iio->prox_trig->ops = &stk3013_prox_trigger_ops;
	iio_trigger_set_drvdata(stk3013_iio->prox_trig, indio_dev);
	ret = iio_trigger_register(stk3013_iio->prox_trig);
	if (ret)
		goto error_free_trig;

	pr_err("%s is success\n", __func__);
	return 0;

error_free_trig:
	iio_trigger_free(stk3013_iio->prox_trig);
error_dealloc_pollfunc:
	iio_dealloc_pollfunc(indio_dev->pollfunc);
error_ret:
	pr_info("%s is failed\n", __func__);
	return ret;
}


static void stk3013_prox_remove_trigger(struct iio_dev *indio_dev)
{
	struct stk3013_sensor_data *sdata = iio_priv(indio_dev);
	struct stk3013_data *stk3013_iio = sdata->cdata;

	iio_trigger_unregister(stk3013_iio->prox_trig);
	iio_trigger_free(stk3013_iio->prox_trig);
	iio_dealloc_pollfunc(indio_dev->pollfunc);
}

static void stk3013_prox_remove_buffer(struct iio_dev *indio_dev)
{
	iio_buffer_unregister(indio_dev);
	iio_kfifo_free(indio_dev->buffer);
}

static int stk3013_prox_register(struct stk3013_data *stk3013_iio)
{
	struct stk3013_sensor_data *prox_p;
	int err;
	int ret = -ENODEV;

	/* iio device register - prox*/
	stk3013_iio->indio_dev_prox  = iio_device_alloc(sizeof(*prox_p));
	if (!stk3013_iio->indio_dev_prox ) {
		pr_err("%s, iio_dev_prox alloc failed\n", __func__);
		err = -ENOMEM;
		goto err_prox_register_failed;
	}

	prox_p = iio_priv(stk3013_iio->indio_dev_prox );
	prox_p->cdata = stk3013_iio;

	stk3013_iio->indio_dev_prox->name = CHIP_ID;
	stk3013_iio->indio_dev_prox->dev.parent = &stk3013_iio->client->dev;
	stk3013_iio->indio_dev_prox->info = &stk3013_prox_info;
	stk3013_iio->indio_dev_prox->channels = stk3013_prox_channels;
	stk3013_iio->indio_dev_prox->num_channels = ARRAY_SIZE(stk3013_prox_channels);
	stk3013_iio->indio_dev_prox->modes = INDIO_DIRECT_MODE;

	stk3013_iio->sampling_frequency_prox = 5;
	stk3013_iio->prox_delay = MSEC_PER_SEC / stk3013_iio->sampling_frequency_prox;

	/* wake lock init for proximity sensor */
	wake_lock_init(&stk3013_iio->prx_wake_lock, WAKE_LOCK_SUSPEND, "prx_wake_lock");

	/* probe buffer */
	err = stk3013_prox_probe_buffer(stk3013_iio->indio_dev_prox);
	if (err) {
		pr_err("%s, prox probe buffer failed\n", __func__);
		goto error_free_prox_dev;
	}

	/* probe trigger */
	err = stk3013_prox_probe_trigger(stk3013_iio->indio_dev_prox);
	if (err) {
		pr_err("%s, prox probe trigger failed\n", __func__);
		goto error_remove_prox_buffer;
	}
	/* iio device register */
	err = iio_device_register(stk3013_iio->indio_dev_prox);
	if (err) {
		pr_err("%s, prox iio register failed\n", __func__);
		goto error_remove_prox_trigger;
	}

	/* For factory test mode, we use timer to get average proximity data. */
	/* prox_timer settings. we poll for light values using a timer. */
	hrtimer_init(&stk3013_iio->prox_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	stk3013_iio->prox_poll_delay = ns_to_ktime(2000 * NSEC_PER_MSEC);/*2 sec*/
	stk3013_iio->prox_timer.function = stk3013_prox_timer_func;

	/* the timer just fires off a work queue request.  we need a thread
	   to read the i2c (can be slow and blocking). */
	stk3013_iio->prox_wq = create_singlethread_workqueue("stk3013_prox_wq");
	if (!stk3013_iio->prox_wq) {
		ret = -ENOMEM;
		pr_err("[SENSOR] %s: could not create prox workqueue\n",
			__func__);
		goto err_create_prox_workqueue;
	}
	/* this is the thread function we run on the work queue */
	INIT_WORK(&stk3013_iio->work_prox, stk3013_work_func_prox);

	return 0;

err_create_prox_workqueue:
	iio_device_unregister(stk3013_iio->indio_dev_prox);
error_remove_prox_trigger:
	stk3013_prox_remove_trigger(stk3013_iio->indio_dev_prox);
error_remove_prox_buffer:
	stk3013_prox_remove_buffer(stk3013_iio->indio_dev_prox);
error_free_prox_dev:
	iio_device_free(stk3013_iio->indio_dev_prox);
	wake_lock_destroy(&stk3013_iio->prx_wake_lock);
err_prox_register_failed:
	return err;
}



static void stk_work_func(struct work_struct *work)
{
	uint32_t read_value;
#if ((STK_INT_PS_MODE != 0x03) && (STK_INT_PS_MODE != 0x02))
	int32_t ret;
	uint8_t disable_flag = 0;
	int32_t org_flag_reg;
#endif/* #if ((STK_INT_PS_MODE != 0x03) && (STK_INT_PS_MODE != 0x02)) */
	struct stk3013_data *ps_data = container_of(work,
				struct stk3013_data, stk_work);
	int32_t near_far_state;

#if (STK_INT_PS_MODE == 0x03)
	near_far_state = gpio_get_value(ps_data->int_pin);
#elif (STK_INT_PS_MODE == 0x02)
	near_far_state = !(gpio_get_value(ps_data->int_pin));
#endif

#if ((STK_INT_PS_MODE == 0x03) || (STK_INT_PS_MODE == 0x02))
	ps_data->ps_distance_last = near_far_state;
	stk3013_prox_data_rdy_trig_poll(ps_data->indio_dev_prox);
	wake_lock_timeout(&ps_data->ps_wakelock, 3 * HZ);
	read_value = stk3013_get_ps_reading(ps_data);
	SENSOR_INFO("ps input event %d cm, ps code = %d\n",
			near_far_state, read_value);
#else
	/* mode 0x01 or 0x04 */
	org_flag_reg = stk3013_get_flag(ps_data);
	if (org_flag_reg < 0)
		goto err_i2c_rw;
	if (org_flag_reg & STK_FLG_PSINT_MASK) {
		disable_flag |= STK_FLG_PSINT_MASK;
		near_far_state = (org_flag_reg & STK_FLG_NF_MASK) ? 1 : 0;
		ps_data->ps_distance_last = near_far_state;
		read_value = stk3013_get_ps_reading(ps_data);

#ifdef CONFIG_SEC_FACTORY
		SENSOR_INFO("FACTORY: near/far=%d, ps code = %d\n",
				near_far_state, read_value);
#else
		SENSOR_INFO("near/far=%d, ps code = %d\n",
				near_far_state, read_value);
		if ((near_far_state == 0 && read_value >= ps_data->ps_thd_h)
			|| (near_far_state == 1 && read_value <= ps_data->ps_thd_l))
#endif
		{
			stk3013_prox_data_rdy_trig_poll(ps_data->indio_dev_prox);
			wake_lock_timeout(&ps_data->ps_wakelock, 3 * HZ);
		}
	}

	if (disable_flag) {
		ret = stk3013_set_flag(ps_data, org_flag_reg, disable_flag);
		if (ret < 0)
			goto err_i2c_rw;
	}
#endif
	usleep_range(1000, 2000);
	goto exit;

err_i2c_rw:
	msleep(30);
exit:
	enable_irq(ps_data->irq);
}

static irqreturn_t stk_oss_irq_handler(int irq, void *data)
{
	struct stk3013_data *pData = data;

	disable_irq_nosync(irq);
	queue_work(pData->stk_wq, &pData->stk_work);
	return IRQ_HANDLED;
}

static int32_t stk3013_init_all_setting(struct i2c_client *client,
				struct stk3013_platform_data *plat_data)
{
	int32_t ret;
	struct stk3013_data *ps_data = i2c_get_clientdata(client);

	ret = stk3013_software_reset(ps_data);
	if (ret < 0)
		return ret;

	ret = stk3013_check_pid(ps_data);
	if (ret < 0)
		return ret;

	stk3013_proc_plat_data(ps_data, plat_data);
	ret = stk3013_init_all_reg(ps_data);
	if (ret < 0)
		return ret;
	ps_data->ps_enabled = false;
	ps_data->re_enable_ps = false;
	ps_data->ir_code = 0;
	ps_data->first_boot = true;

	atomic_set(&ps_data->recv_reg, 0);
	ps_data->ps_distance_last = 1;
	return 0;
}

static int stk3013_setup_irq(struct i2c_client *client)
{
	int irq, ret = -EIO;
	struct stk3013_data *ps_data = i2c_get_clientdata(client);

	irq = gpio_to_irq(ps_data->int_pin);

	SENSOR_INFO("int pin #=%d, irq=%d\n", ps_data->int_pin, irq);

	if (irq <= 0) {
		SENSOR_ERR("irq number is not specified, irq=%d, int pin=%d\n",
				irq, ps_data->int_pin);
		return irq;
	}
	ps_data->irq = irq;
	ret = gpio_request(ps_data->int_pin, "stk-int");
	if (ret < 0) {
		SENSOR_ERR("gpio_request, err=%d", ret);
		return ret;
	}
	ret = gpio_direction_input(ps_data->int_pin);
	if (ret < 0) {
		SENSOR_ERR("gpio_direction_input, err=%d", ret);
		return ret;
	}

#if ((STK_INT_PS_MODE == 0x03) || (STK_INT_PS_MODE == 0x02))
	ret = request_any_context_irq(irq, stk_oss_irq_handler,
			IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
			"proximity_int", ps_data);
#else
	ret = request_any_context_irq(irq, stk_oss_irq_handler,
			IRQF_TRIGGER_LOW, "proximity_int", ps_data);
#endif
	if (ret < 0) {
		SENSOR_WARN("request_any_context_irq(%d) failed for (%d)\n",
				irq, ret);
		goto err_request_any_context_irq;
	}
	disable_irq(irq);

	return 0;
err_request_any_context_irq:
	gpio_free(ps_data->int_pin);
	return ret;
}

static int stk3013_suspend(struct device *dev)
{
	struct stk3013_data *ps_data = dev_get_drvdata(dev);
	int ret;
	struct i2c_client *client = to_i2c_client(dev);

	SENSOR_INFO("\n");
	mutex_lock(&ps_data->io_lock);

	if (ps_data->ps_enabled) {
		if (device_may_wakeup(&client->dev)) {
			ret = enable_irq_wake(ps_data->irq);
			if (ret)
				SENSOR_WARN("set_irq_wake(%d) failed(%d)\n",
						ps_data->irq, ret);
		} else {
			SENSOR_ERR("not support wakeup source");
		}
	}

	mutex_unlock(&ps_data->io_lock);

	return 0;
}

static int stk3013_resume(struct device *dev)
{
	struct stk3013_data *ps_data = dev_get_drvdata(dev);
	int ret;
	struct i2c_client *client = to_i2c_client(dev);

	SENSOR_INFO("\n");

	mutex_lock(&ps_data->io_lock);
	if (ps_data->ps_enabled) {
		if (device_may_wakeup(&client->dev)) {
			ret = disable_irq_wake(ps_data->irq);
			if (ret)
				SENSOR_WARN("disable_irq_wake(%d) fail(%d)\n",
						ps_data->irq, ret);
		}
	}

	mutex_unlock(&ps_data->io_lock);

	return 0;
}

static const struct dev_pm_ops stk3013_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(stk3013_suspend, stk3013_resume)
};

static int stk3013_parse_dt(struct device *dev,
			struct stk3013_platform_data *pdata)
{
	int rc;
	struct device_node *np = dev->of_node;
	u32 temp_val;

	if (!pdata)
		return -ENOMEM;

	pdata->int_pin = of_get_named_gpio_flags(np, "stk,irq-gpio", 0,
						&pdata->int_flags);
	if (pdata->int_pin < 0) {
		dev_err(dev, "Unable to read irq-gpio\n");
		return pdata->int_pin;
	}

	rc = of_property_read_u32(np, "stk,transmittance", &temp_val);
	if (!rc)
		pdata->transmittance = temp_val;
	else {
		dev_err(dev, "Unable to read transmittance\n");
		return rc;
	}

	rc = of_property_read_u32(np, "stk,state-reg", &temp_val);
	if (!rc)
		pdata->state_reg = temp_val;
	else {
		dev_err(dev, "Unable to read state-reg\n");
		return rc;
	}

	rc = of_property_read_u32(np, "stk,psctrl-reg", &temp_val);
	if (!rc)
		pdata->psctrl_reg = (u8)temp_val;
	else {
		dev_err(dev, "Unable to read psctrl-reg\n");
		return rc;
	}
/*
	rc = of_property_read_u32(np, "stk,alsctrl-reg", &temp_val);
	if (!rc)
		pdata->alsctrl_reg = (u8)temp_val;
	else {
		dev_err(dev, "Unable to read alsctrl-reg\n");
		return rc;
	}
*/
	rc = of_property_read_u32(np, "stk,ledctrl-reg", &temp_val);
	if (!rc)
		pdata->ledctrl_reg = (u8)temp_val;
	else {
		dev_err(dev, "Unable to read ledctrl-reg\n");
		return rc;
	}

	rc = of_property_read_u32(np, "stk,wait-reg", &temp_val);
	if (!rc)
		pdata->wait_reg = (u8)temp_val;
	else {
		dev_err(dev, "Unable to read wait-reg\n");
		return rc;
	}

	rc = of_property_read_u32(np, "stk,ps-thd-h", &temp_val);
	if (!rc)
		pdata->ps_thd_h = (u16)temp_val;
	else {
		dev_err(dev, "Unable to read ps-thd-h\n");
		return rc;
	}

	rc = of_property_read_u32(np, "stk,ps-thd-l", &temp_val);
	if (!rc)
		pdata->ps_thd_l = (u16)temp_val;
	else {
		dev_err(dev, "Unable to read ps-thd-l\n");
		return rc;
	}

	rc = of_property_read_u32(np, "stk,ps-cancel-thd-h", &temp_val);
	if (!rc)
		pdata->ps_cancel_thd_h = (u16)temp_val;
	else {
		dev_err(dev, "Unable to read ps-cancel-thd-h\n");
		return rc;
	}

	rc = of_property_read_u32(np, "stk,ps-cancel-thd-l", &temp_val);
	if (!rc)
		pdata->ps_cancel_thd_l = (u16)temp_val;
	else {
		dev_err(dev, "Unable to read ps-cancel-thd-l\n");
		return rc;
	}

	rc = of_property_read_u32(np, "stk,ps-cal-skip-adc", &temp_val);
	if (!rc)
		pdata->ps_cal_skip_adc = (u16)temp_val;
	else {
		dev_err(dev, "Unable to read ps-cal-skip-adc\n");
		return rc;
	}

	rc = of_property_read_u32(np, "stk,ps-cal-fail-adc", &temp_val);
	if (!rc)
		pdata->ps_cal_fail_adc = (u16)temp_val;
	else {
		dev_err(dev, "Unable to read ps-cal-fail-adc\n");
		return rc;
	}

	rc = of_property_read_u32(np, "stk,ps-default-offset", &temp_val);
	if (!rc)
		pdata->ps_default_offset = (u16)temp_val;
	else {
		dev_err(dev, "Unable to read ps-default-offset\n");
		return rc;
	}

	return 0;
}

#ifdef PROXIMITY_CALIBRATION
static int check_calibration_offset(struct stk3013_data *ps_data)
{
	struct file *cal_filp = NULL;
	mm_segment_t old_fs;
	uint16_t file_offset_data;
	int ret;

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	cal_filp = filp_open(CALIBRATION_FILE_PATH, O_RDONLY, 0);
	if (IS_ERR(cal_filp)) {
		ret = PTR_ERR(cal_filp);
		if (ret != -ENOENT)
			SENSOR_ERR("Can't open calibration file\n");
		set_fs(old_fs);
		ps_data->ps_offset = ps_data->ps_default_offset;
		SENSOR_ERR("Can't open calibration file 2(%d) ps_offset =%d\n",
					ret, ps_data->ps_offset);
		return ret;
	}

	ret = cal_filp->f_op->read(cal_filp,
		(char *)&file_offset_data,
		sizeof(u16), &cal_filp->f_pos);
	if (ret != sizeof(u16)) {
		SENSOR_ERR("Can't read the cal data from file\n");
		ret = -EIO;
	}

	if(file_offset_data < ps_data->ps_cal_skip_adc)
		goto exit;

	if (file_offset_data != ps_data->ps_offset)
		ps_data->ps_offset = file_offset_data;
	if (ps_data->ps_offset != ps_data->ps_default_offset) {
		stk3013_set_ps_thd_h(ps_data, ps_data->ps_cancel_thd_h);
		stk3013_set_ps_thd_l(ps_data, ps_data->ps_cancel_thd_l);
	}

exit:
	SENSOR_INFO("file_offset = %d, ps_offset = %d, default_offset = %d\n",
		file_offset_data, ps_data->ps_offset,
		ps_data->ps_default_offset);

	filp_close(cal_filp, current->files);
	set_fs(old_fs);

	return ret;
}
#endif

static int stk3013_set_wq(struct stk3013_data *ps_data)
{
	ps_data->stk_wq = create_singlethread_workqueue("stk_wq");
	INIT_WORK(&ps_data->stk_work, stk_work_func);

	return 0;
}

static int stk3013_regulator_onoff(struct device *dev, bool onoff)

{
	struct stk3013_data *ps_data = dev_get_drvdata(dev);
	int ret;

	SENSOR_INFO("%s\n", (onoff) ? "on" : "off");

	if (!ps_data->vdd || IS_ERR(ps_data->vdd)) {
		SENSOR_INFO("VDD get regulator\n");
		ps_data->vdd = devm_regulator_get(dev, "stk,vdd");
		if (IS_ERR(ps_data->vdd)) {
			SENSOR_ERR("cannot get vdd\n");
			return -ENOMEM;
		}
#ifdef CONFIG_SEC_ON7XLTE_CHN
		regulator_set_voltage(ps_data->vdd, 2800000,2800000);
#else
		regulator_set_voltage(ps_data->vdd, 3300000,3300000);
#endif
	}

	if (!ps_data->vio || IS_ERR(ps_data->vio)) {
		SENSOR_INFO("VIO get regulator\n");
		ps_data->vio = devm_regulator_get(dev, "stk,vio");
		if (IS_ERR(ps_data->vio)) {
			SENSOR_ERR("cannot get vio\n");
			devm_regulator_put(ps_data->vdd);
			return -ENOMEM;
		}
		regulator_set_voltage(ps_data->vio, 1800000, 1800000);
	}

	if (onoff) {
		ret = regulator_enable(ps_data->vdd);
		if (ret)
			SENSOR_ERR("Failed to enable vdd.\n");
		msleep(20);

		ret = regulator_enable(ps_data->vio);
		if (ret)
			SENSOR_ERR("Failed to enable vio.\n");
		msleep(20);
	} else {
		ret = regulator_disable(ps_data->vdd);
		if (ret)
			SENSOR_ERR("Failed to disable vdd.\n");
		msleep(20);

		ret = regulator_disable(ps_data->vio);
		if (ret)
			SENSOR_ERR("Failed to disable vio.\n");
		msleep(20);
	}
	return 0;
}

static int stk3013_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	int ret = -ENODEV;
	struct stk3013_data *ps_data;
	struct stk3013_platform_data *plat_data;

	SENSOR_INFO("driver version = %s\n", DRIVER_VERSION);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		SENSOR_ERR("No Support for I2C_FUNC_I2C\n");
		return ret;
	}

	ps_data = kzalloc(sizeof(struct stk3013_data), GFP_KERNEL);
	if (!ps_data) {
		SENSOR_ERR("failed to allocate stk3013_data\n");
		return -ENOMEM;
	}

	ps_data->client = client;
	i2c_set_clientdata(client, ps_data);
	mutex_init(&ps_data->io_lock);
	wake_lock_init(&ps_data->ps_wakelock, WAKE_LOCK_SUSPEND,
			"stk_input_wakelock");

	if (client->dev.of_node) {
		SENSOR_INFO("with device tree\n");
		plat_data = devm_kzalloc(&client->dev,
			sizeof(struct stk3013_platform_data), GFP_KERNEL);
		if (!plat_data) {
			dev_err(&client->dev, "Failed to allocate memory\n");
			ret = -ENOMEM;
			goto err_als_input_allocate;
		}
		ret = stk3013_parse_dt(&client->dev, plat_data);
		if (ret) {
			SENSOR_ERR("stk3013_parse_dt ret=%d\n", ret);
			goto err_als_input_allocate;
		}
	} else {
		SENSOR_INFO("with platform data\n");
		plat_data = client->dev.platform_data;
	}
	if (!plat_data) {
		SENSOR_ERR("no stk3013 platform data!\n");
		ret = -ENOMEM;
		goto err_als_input_allocate;
	}

	stk3013_regulator_onoff(&client->dev, ON);
	ps_data->int_pin = plat_data->int_pin;
	ps_data->pdata = plat_data;

	stk3013_set_wq(ps_data);
	ret = stk3013_init_all_setting(client, plat_data);
	if (ret < 0)
		goto err_init_all_setting;

	ret = stk3013_prox_register(ps_data);
	if (ret < 0)
		goto err_init_all_setting;

	spin_lock_init(&ps_data->spin_lock);
	mutex_init(&ps_data->lock);

	ret = stk3013_setup_irq(client);
	if (ret < 0)
		goto err_stk3013_setup_irq;
	device_init_wakeup(&client->dev, true);

	hrtimer_init(&ps_data->prox_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ps_data->prox_poll_delay = ns_to_ktime(2000 * NSEC_PER_MSEC);/*2 sec*/
	ps_data->prox_timer.function = stk3013_prox_timer_func;

	/* the timer just fires off a work queue request.  we need a thread
	   to read the i2c (can be slow and blocking). */
	ps_data->prox_wq = create_singlethread_workqueue("stk3013_prox_wq");
	if (!ps_data->prox_wq) {
		ret = -ENOMEM;
		SENSOR_ERR("could not create prox workqueue\n");
		goto err_create_prox_workqueue;
	}
	/* this is the thread function we run on the work queue */
	INIT_WORK(&ps_data->work_prox, stk3013_work_func_prox);

	ret = sensors_register(ps_data->ps_dev, ps_data,
				prox_sensor_attrs, MODULE_NAME);
	if (ret) {
		SENSOR_ERR("cound not register proximity sensor device(%d)\n",
			ret);
		goto prox_sensor_register_failed;
	}

	sensors_create_symlink(&ps_data->indio_dev_prox->dev.kobj,
				"proximity_sensor");

	SENSOR_INFO("success\n");
	return 0;

prox_sensor_register_failed:
	destroy_workqueue(ps_data->prox_wq);
err_create_prox_workqueue:
err_stk3013_setup_irq:
	free_irq(ps_data->irq, ps_data);
	gpio_free(ps_data->int_pin);
err_init_all_setting:
	destroy_workqueue(ps_data->stk_wq);
err_als_input_allocate:
	wake_lock_destroy(&ps_data->ps_wakelock);
	mutex_destroy(&ps_data->io_lock);
	kfree(ps_data);
	return ret;
}


static int stk3013_remove(struct i2c_client *client)
{
	SENSOR_INFO("\n");
	return 0;
}

static const struct i2c_device_id stk_ps_id[] = {
	{ "stk_ps", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, stk_ps_id);

static struct of_device_id stk_match_table[] = {
	{ .compatible = "stk,stk3013", },
	{ },
};

static struct i2c_driver stk_ps_driver = {
	.driver = {
	.name = CHIP_ID,
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = stk_match_table,
#endif
		.pm = &stk3013_pm_ops,
	},
	.probe = stk3013_probe,
	.remove = stk3013_remove,
	.id_table = stk_ps_id,
};

static int __init stk3013_init(void)
{
	int ret;

	ret = i2c_add_driver(&stk_ps_driver);
	if (ret)
		i2c_del_driver(&stk_ps_driver);

	return ret;
}

static void __exit stk3013_exit(void)
{
	i2c_del_driver(&stk_ps_driver);
}

module_init(stk3013_init);
module_exit(stk3013_exit);
MODULE_AUTHOR("Samsung Electronics");
MODULE_DESCRIPTION("Sensortek stk3013 Proximity Sensor driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
