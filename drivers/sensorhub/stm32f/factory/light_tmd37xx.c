/*
 *  Copyright (C) 2012, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
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
 */
#include "../ssp.h"

#define	VENDOR		"AMS"

#if defined(CONFIG_SENSORS_SSP_TMD3700)
#define	CHIP_ID		"TMD3700"
#else
#define	CHIP_ID		"TMD3725"
#endif

/*************************************************************************/
/* factory Sysfs                                                         */
/*************************************************************************/
static ssize_t light_vendor_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", VENDOR);
}

static ssize_t light_name_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", CHIP_ID);
}

static ssize_t light_lux_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%u,%u,%u,%u,%u,%u\n",
		data->buf[SENSOR_TYPE_LIGHT].r, data->buf[SENSOR_TYPE_LIGHT].g,
		data->buf[SENSOR_TYPE_LIGHT].b, data->buf[SENSOR_TYPE_LIGHT].w,
		data->buf[SENSOR_TYPE_LIGHT].a_time, data->buf[SENSOR_TYPE_LIGHT].a_gain);
}

static ssize_t light_data_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct ssp_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%u,%u,%u,%u,%u,%u\n",
		data->buf[SENSOR_TYPE_LIGHT].r, data->buf[SENSOR_TYPE_LIGHT].g,
		data->buf[SENSOR_TYPE_LIGHT].b, data->buf[SENSOR_TYPE_LIGHT].w,
		data->buf[SENSOR_TYPE_LIGHT].a_time, data->buf[SENSOR_TYPE_LIGHT].a_gain);
}

static DEVICE_ATTR(vendor, S_IRUGO, light_vendor_show, NULL);
static DEVICE_ATTR(name, S_IRUGO, light_name_show, NULL);
static DEVICE_ATTR(lux, S_IRUGO, light_lux_show, NULL);
static DEVICE_ATTR(raw_data, S_IRUGO, light_data_show, NULL);

static struct device_attribute *light_attrs[] = {
	&dev_attr_vendor,
	&dev_attr_name,
	&dev_attr_lux,
	&dev_attr_raw_data,
	NULL,
};

void initialize_light_factorytest(struct ssp_data *data)
{
	sensors_register(data->devices[SENSOR_TYPE_LIGHT], data, light_attrs, "light_sensor");
}

void remove_light_factorytest(struct ssp_data *data)
{
	sensors_unregister(data->devices[SENSOR_TYPE_LIGHT], light_attrs);
}
