/*
 * drivers/soc/samsung/sec_param.c
 *
 * COPYRIGHT(C) 2011-2016 Samsung Electronics Co., Ltd. All Right Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/sec_param.h>
#include <linux/file.h>
#include <linux/syscalls.h>
#include <linux/delay.h>

#define PARAM_RD	0
#define PARAM_WR	1

#define SEC_PARAM_FILE_SIZE	0xC00000		/* 4 MB */
#define SEC_PARAM_FILE_OFFSET (SEC_PARAM_FILE_SIZE - 0x100000)
#define MAX_PARAM_BUFFER	128

static DEFINE_MUTEX(sec_param_mutex);

/* single global instance */
struct sec_param_data *param_data;
struct sec_param_data_s sched_sec_param_data;

static char param_name[MAX_PARAM_BUFFER];
static __init int get_param_name(char *str)
{
	if (likely(str))
		return snprintf(param_name, sizeof(param_name),
			"/dev/block/platform/soc/%s/by-name/param", str);

	pr_err("sec_param: failed to get path from androidboot.bootdevice\n");
	return 0;

}
__setup("androidboot.bootdevice=", get_param_name);

static void param_sec_operation(struct work_struct *work)
{
	/* Read from PARAM(parameter) partition  */
	struct file *filp;
	mm_segment_t fs;
	int ret;
	struct sec_param_data_s *sched_param_data =
		container_of(work, struct sec_param_data_s, sec_param_work);

	int flag = (sched_param_data->direction == PARAM_WR)
			? (O_RDWR | O_SYNC) : O_RDONLY;

	pr_debug("%s %p %x %d %d\n", __func__, sched_param_data->value,
			sched_param_data->offset, sched_param_data->size,
			sched_param_data->direction);

	if (param_name[0] == '\0') {
		pr_err("sec_param: failed to find cmdline\n");
		return;
	}
	filp = filp_open(param_name, flag, 0);

	if (IS_ERR(filp)) {
		pr_err("%s: filp_open failed. (%ld)\n",
				__func__, PTR_ERR(filp));
		complete(&sched_sec_param_data.work);
		return;
	}

	fs = get_fs();
	set_fs(get_ds());

	ret = filp->f_op->llseek(filp, sched_param_data->offset, SEEK_SET);
	if (unlikely(ret < 0)) {
		pr_err("%s FAIL LLSEEK\n", __func__);
		goto param_sec_debug_out;
	}

	if (sched_param_data->direction == PARAM_RD)
		filp->f_op->read(filp,
				(char __user *)sched_param_data->value,
				sched_param_data->size, &filp->f_pos);
	else if (sched_param_data->direction == PARAM_WR)
		filp->f_op->write(filp,
				(char __user *)sched_param_data->value,
				sched_param_data->size, &filp->f_pos);

param_sec_debug_out:
	set_fs(fs);
	filp_close(filp, NULL);
	complete(&sched_sec_param_data.work);
}

bool sec_open_param(void)
{
	pr_info("%s start \n",__func__);

	if (param_data != NULL)
		return true;

	mutex_lock(&sec_param_mutex);

	param_data = kmalloc(sizeof(struct sec_param_data), GFP_KERNEL);

	sched_sec_param_data.value=param_data;
	sched_sec_param_data.offset=SEC_PARAM_FILE_OFFSET;
	sched_sec_param_data.size=sizeof(struct sec_param_data);
	sched_sec_param_data.direction=PARAM_RD;

	schedule_work(&sched_sec_param_data.sec_param_work);
	wait_for_completion(&sched_sec_param_data.work);

	mutex_unlock(&sec_param_mutex);

	pr_info("%s end \n",__func__);

	return true;
}

bool sec_write_param(void)
{
	pr_info("%s start\n",__func__);

	mutex_lock(&sec_param_mutex);

	sched_sec_param_data.value=param_data;
	sched_sec_param_data.offset=SEC_PARAM_FILE_OFFSET;
	sched_sec_param_data.size=sizeof(struct sec_param_data);
	sched_sec_param_data.direction=PARAM_WR;

	schedule_work(&sched_sec_param_data.sec_param_work);
	wait_for_completion(&sched_sec_param_data.work);

	mutex_unlock(&sec_param_mutex);

	pr_info("%s end\n",__func__);

	return true;
}

bool sec_get_param(enum sec_param_index index, void *value)
{
	int ret;

	ret = sec_open_param();
	if (!ret)
		return ret;

	switch (index) {
	case param_index_debuglevel:
		memcpy(value, &(param_data->debuglevel), sizeof(unsigned int));
		break;
	case param_index_uartsel:
		memcpy(value, &(param_data->uartsel), sizeof(unsigned int));
		break;
	case param_rory_control:
		memcpy(value, &(param_data->rory_control),
				sizeof(unsigned int));
		break;
	case param_index_movinand_checksum_done:
		memcpy(value, &(param_data->movinand_checksum_done),
				sizeof(unsigned int));
		break;
	case param_index_movinand_checksum_pass:
		memcpy(value, &(param_data->movinand_checksum_pass),
				sizeof(unsigned int));
		break;
#ifdef CONFIG_GSM_MODEM_SPRD6500
	case param_update_cp_bin:
		memcpy(value, &(param_data->update_cp_bin),
				sizeof(unsigned int));
		printk(KERN_INFO "param_data.update_cp_bin :[%d]!!",
				param_data->update_cp_bin);
		break;
#endif
#ifdef CONFIG_SEC_MONITOR_BATTERY_REMOVAL
	case param_index_normal_poweroff:
		memcpy(&(param_data->normal_poweroff), value,
				sizeof(unsigned int));
		break;
#endif
#ifdef CONFIG_BARCODE_PAINTER
	case param_index_barcode_info:
		memcpy(value, param_data->param_barcode_info,
				sizeof(param_data->param_barcode_info));
		break;
#endif
#ifdef CONFIG_WIRELESS_CHARGER_HIGH_VOLTAGE
	case param_index_wireless_charging_mode:
		memcpy(value, &(param_data->wireless_charging_mode),
				sizeof(unsigned int));
		break;
#endif
#ifdef CONFIG_MUIC_HV
	case param_index_afc_disable:
		memcpy(value, &(param_data->afc_disable), sizeof(unsigned int));
		break;
#endif
	case param_index_cp_reserved_mem:
		memcpy(value, &(param_data->cp_reserved_mem),
				sizeof(unsigned int));
		break;
	default:
		return false;
	}

	return true;
}
EXPORT_SYMBOL(sec_get_param);

bool sec_set_param(enum sec_param_index index, void *value)
{
	int ret;

	ret = sec_open_param();
	if (!ret)
		return ret;

	switch (index) {
	case param_index_debuglevel:
		memcpy(&(param_data->debuglevel),
				value, sizeof(unsigned int));
		break;
	case param_index_uartsel:
		memcpy(&(param_data->uartsel),
				value, sizeof(unsigned int));
		break;
	case param_rory_control:
		memcpy(&(param_data->rory_control),
				value, sizeof(unsigned int));
		break;
	case param_index_movinand_checksum_done:
		memcpy(&(param_data->movinand_checksum_done),
				value, sizeof(unsigned int));
		break;
	case param_index_movinand_checksum_pass:
		memcpy(&(param_data->movinand_checksum_pass),
				value, sizeof(unsigned int));
		break;
#ifdef CONFIG_GSM_MODEM_SPRD6500
	case param_update_cp_bin:
		memcpy(&(param_data->update_cp_bin),
				value, sizeof(unsigned int));
		break;
#endif
#ifdef CONFIG_SEC_MONITOR_BATTERY_REMOVAL
	case param_index_normal_poweroff:
		memcpy(&(param_data->normal_poweroff),
				value, sizeof(unsigned int));
		break;
#endif
#ifdef CONFIG_BARCODE_PAINTER
	case param_index_barcode_info:
		memcpy(param_data->param_barcode_info,
				value, sizeof(param_data->param_barcode_info));
		break;
#endif
#ifdef CONFIG_WIRELESS_CHARGER_HIGH_VOLTAGE
	case param_index_wireless_charging_mode:
		memcpy(&(param_data->wireless_charging_mode),
				value, sizeof(unsigned int));
		break;
#endif
#ifdef CONFIG_MUIC_HV
	case param_index_afc_disable:
		memcpy(&(param_data->afc_disable),
				value, sizeof(unsigned int));
		break;
#endif
	case param_index_cp_reserved_mem:
		memcpy(&(param_data->cp_reserved_mem),
				value, sizeof(unsigned int));
		break;
	default:
		return false;
	}

	ret = sec_write_param();

	return ret;
}
EXPORT_SYMBOL(sec_set_param);

static int __init sec_param_work_init(void)
{
	pr_info("%s: start\n", __func__);

	sched_sec_param_data.offset=0;
	sched_sec_param_data.direction=0;
	sched_sec_param_data.size=0;
	sched_sec_param_data.value=NULL;

	init_completion(&sched_sec_param_data.work);
	INIT_WORK(&sched_sec_param_data.sec_param_work, param_sec_operation);

	pr_info("%s: end\n", __func__);

	return 0;
}

static void __exit sec_param_work_exit(void)
{
	cancel_work_sync(&sched_sec_param_data.sec_param_work);
	pr_info("%s: exit\n", __func__);
}

module_init(sec_param_work_init);
module_exit(sec_param_work_exit);
