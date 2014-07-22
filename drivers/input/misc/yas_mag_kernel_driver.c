/*
 * Copyright (c) 2010-2013 Yamaha Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301, USA.
 */

#include <asm/atomic.h>
#include <linux/types.h>
#include <linux/i2c.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

//S [CCI]Ginger modified for MSM8226 DTS
#include <linux/input/yas.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
//E [CCI]Ginger modified for MSM8226 DTS

//[CCI] Ginger modified for factory test
//#undef SYSFS_PCBTEST
#define SYSFS_PCBTEST

#ifdef SYSFS_PCBTEST
#include "yas_pcb_test.h"
#include "yas_pcb_test.c"
#endif

#if YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS529
#include "yas_mag_driver-yas529.c"
#elif YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS530
#include "yas_mag_driver-yas53x.c"
#elif YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS532
#include "yas_mag_driver-yas53x.c"
#elif YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS533
#include "yas_mag_driver-yas53x.c"
#else
#include "yas_mag_driver-none.c"
#endif

#define GEOMAGNETIC_I2C_DEVICE_NAME	"geomagnetic"
#define GEOMAGNETIC_INPUT_NAME		"geomagnetic"
#define GEOMAGNETIC_INPUT_RAW_NAME	"geomagnetic_raw"
#undef GEOMAGNETIC_PLATFORM_API

#define ABS_STATUS			(ABS_BRAKE)
#define ABS_WAKE			(ABS_MISC)

#define ABS_RAW_DISTORTION		(ABS_THROTTLE)
#define ABS_RAW_THRESHOLD		(ABS_RUDDER)
#define ABS_RAW_SHAPE			(ABS_WHEEL)
#define ABS_RAW_MODE			(ABS_HAT0X)
#define ABS_RAW_GYRO_THRESHOLD		(ABS_HAT0Y)
#define ABS_RAW_REPORT			(ABS_GAS)

#define GEOMAGNETIC_RSTN_GPIO 64  //S [CCI]Ginger modified for MSM8226 DTS
//S [CCI] Ginger modified for factory test
#define CHECK_RANGE(X, MIN, MAX)    (X>=MIN && X<=MAX)?1:0
#define CHECK_GREATER(X, MIN)    (X>=MIN)?1:0
//E [CCI] Ginger modified for factory test

struct geomagnetic_data {
	struct input_dev *input_data;
	struct input_dev *input_raw;
	struct delayed_work work;
	struct semaphore driver_lock;
	struct semaphore multi_lock;
	atomic_t last_data[3];
	atomic_t last_raw[3];
	atomic_t last_status;
	atomic_t enable;
	int filter_enable;
	int filter_len;
	int32_t filter_noise[3];
	int32_t filter_threshold;
	int delay;
	int32_t mag_noise;
	int32_t gyro_noise;
	int32_t threshold;
	int32_t distortion[3];
	int32_t shape;
	int32_t ellipsoid_mode;
	struct yas_mag_offset driver_offset;
#if DEBUG
	int suspend;
#endif
#ifdef YAS_MAG_MANUAL_OFFSET
	struct yas_vector manual_offset;
#endif
	struct yas_matrix static_matrix;
	struct yas_matrix dynamic_matrix;
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	struct list_head devfile_list;
	struct list_head raw_devfile_list;
#endif
};

static struct i2c_client *this_client = {0};

#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE

#include <linux/miscdevice.h>
#define MAX_COUNT (64)
#define SENSOR_NAME "geomagnetic"
#define SENSOR_RAW_NAME "geomagnetic_raw"

struct sensor_device {
	struct list_head list;
	struct mutex lock;
	wait_queue_head_t waitq;
	struct input_event events[MAX_COUNT];
	int head, num_event;
};

static void
get_time_stamp(struct timeval *tv)
{
	struct timespec ts;
	ktime_get_ts(&ts);
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / 1000;
}

static void
mkev(struct input_event *ev, int type, int code, int value)
{
	struct timeval tv;
	get_time_stamp(&tv);
	ev->type = type;
	ev->code = code;
	ev->value = value;
	ev->time = tv;
}

static void
mkev_with_time(struct input_event *ev, int type, int code, int value,
	struct timeval *tv)
{
	ev->type = type;
	ev->code = code;
	ev->value = value;
	ev->time = *tv;
}

static void
sensor_enq(struct sensor_device *kdev, struct input_event *ev)
{
	int idx;

	idx = kdev->head + kdev->num_event;
	if (MAX_COUNT <= idx)
		idx -= MAX_COUNT;
	kdev->events[idx] = *ev;
	kdev->num_event++;
	if (MAX_COUNT < kdev->num_event) {
		kdev->num_event = MAX_COUNT;
		kdev->head++;
		if (MAX_COUNT <= kdev->head)
			kdev->head -= MAX_COUNT;
	}
}

static int
sensor_deq(struct sensor_device *kdev, struct input_event *ev)
{
	if (kdev->num_event == 0)
		return 0;

	*ev = kdev->events[kdev->head];
	kdev->num_event--;
	kdev->head++;
	if (MAX_COUNT <= kdev->head)
		kdev->head -= MAX_COUNT;
	return 1;
}

static void
sensor_event(struct list_head *devlist, struct input_event *ev, int num)
{
	struct sensor_device *kdev;
	int i;

	list_for_each_entry(kdev, devlist, list) {
		mutex_lock(&kdev->lock);
		for (i = 0; i < num; i++)
			sensor_enq(kdev, &ev[i]);
		mutex_unlock(&kdev->lock);
		wake_up_interruptible(&kdev->waitq);
	}
}

static ssize_t
sensor_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
	struct geomagnetic_data *data = i2c_get_clientdata(this_client);
	struct sensor_device *kdev;
	struct input_event ev[MAX_COUNT];
	int num, i;

	if (count < sizeof(struct input_event))
		return -EINVAL;
	num = count / sizeof(struct input_event);
	if (MAX_COUNT < num)
		num = MAX_COUNT;
	if (copy_from_user(ev, buf, num * sizeof(struct input_event)))
		return -EFAULT;

	list_for_each_entry(kdev, &data->devfile_list, list) {
		mutex_lock(&kdev->lock);
		for (i = 0; i < num; i++)
			sensor_enq(kdev, &ev[i]);
		mutex_unlock(&kdev->lock);
		wake_up_interruptible(&kdev->waitq);
	}

	return count;
}

static ssize_t
sensor_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
	struct sensor_device *kdev = f->private_data;
	int rt, num;
	struct input_event ev[MAX_COUNT];

	if (count < sizeof(struct input_event))
		return -EINVAL;

	rt = wait_event_interruptible(kdev->waitq, kdev->num_event != 0);
	if (rt)
		return rt;

	mutex_lock(&kdev->lock);
	for (num = 0; num < count / sizeof(struct input_event); num++)
		if (!sensor_deq(kdev, &ev[num]))
			break;
	mutex_unlock(&kdev->lock);

	if (copy_to_user(buf, ev, num * sizeof(struct input_event)))
		return -EFAULT;

	return num * sizeof(struct input_event);
}

static unsigned int
sensor_poll(struct file *f, struct poll_table_struct *wait)
{
	struct sensor_device *kdev = f->private_data;

	poll_wait(f, &kdev->waitq, wait);
	if (kdev->num_event != 0)
		return POLLIN | POLLRDNORM;

	return 0;
}

static int sensor_open(struct inode *inode, struct file *f)
{
	struct geomagnetic_data *data = i2c_get_clientdata(this_client);
	struct sensor_device *kdev;

	kdev = kzalloc(sizeof(struct sensor_device), GFP_KERNEL);
	if (!kdev)
		return -ENOMEM;

	mutex_init(&kdev->lock);
	init_waitqueue_head(&kdev->waitq);
	f->private_data = kdev;
	kdev->head = 0;
	kdev->num_event = 0;
	list_add(&kdev->list, &data->devfile_list);

	return 0;
}

static int sensor_release(struct inode *inode, struct file *f)
{
	struct sensor_device *kdev = f->private_data;

	list_del(&kdev->list);
	kfree(kdev);

	return 0;
}

static int sensor_raw_open(struct inode *inode, struct file *f)
{
	struct geomagnetic_data *data = i2c_get_clientdata(this_client);
	struct sensor_device *kdev;

	kdev = kzalloc(sizeof(struct sensor_device), GFP_KERNEL);
	if (!kdev)
		return -ENOMEM;

	mutex_init(&kdev->lock);
	init_waitqueue_head(&kdev->waitq);
	f->private_data = kdev;
	kdev->head = 0;
	kdev->num_event = 0;
	list_add(&kdev->list, &data->raw_devfile_list);

	return 0;
}

static const struct file_operations sensor_fops = {
	.owner = THIS_MODULE,
	.open = sensor_open,
	.release = sensor_release,
	.write = sensor_write,
	.read = sensor_read,
	.poll = sensor_poll,
};

static struct miscdevice sensor_devfile = {
	.name = SENSOR_NAME,
	.fops = &sensor_fops,
	.minor = MISC_DYNAMIC_MINOR,
};

static const struct file_operations sensor_raw_fops = {
	.owner = THIS_MODULE,
	.open = sensor_raw_open,
	.release = sensor_release,
	.write = sensor_write,
	.read = sensor_read,
	.poll = sensor_poll,
};

static struct miscdevice sensor_raw_devfile = {
	.name = SENSOR_RAW_NAME,
	.fops = &sensor_raw_fops,
	.minor = MISC_DYNAMIC_MINOR,
};

#endif

static int
geomagnetic_i2c_open(void)
{
	return 0;
}

static int
geomagnetic_i2c_close(void)
{
	return 0;
}

#if YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS529
static int
geomagnetic_i2c_write(const uint8_t *buf, int len)
{
	if (i2c_master_send(this_client, buf, len) < 0)
		return -1;
#if DEBUG
	YLOGD(("[W] [%02x]\n", buf[0]));
#endif

	return 0;
}

static int
geomagnetic_i2c_read(uint8_t *buf, int len)
{
	if (i2c_master_recv(this_client, buf, len) < 0)
		return -1;

#if DEBUG
	if (len == 1) {
		YLOGD(("[R] [%02x]\n", buf[0]));
	} else if (len == 6) {
		YLOGD(("[R] "
		"[%02x%02x%02x%02x%02x%02x]\n",
		buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]));
	} else if (len == 8) {
		YLOGD(("[R] "
		"[%02x%02x%02x%02x%02x%02x%02x%02x]\n",
		buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6],
		buf[7]));
	} else if (len == 9) {
		YLOGD(("[R] "
		"[%02x%02x%02x%02x%02x%02x%02x%02x%02x]\n",
		buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
		buf[8]));
	} else if (len == 16) {
		YLOGD(("[R] "
		"[%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x"
		"%02x]\n",
		buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
		buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14],
		buf[15]));
	}
#endif

	return 0;
}

#else

static int
geomagnetic_i2c_write(uint8_t addr, const uint8_t *buf, int len)
{
	uint8_t tmp[16];

	if (sizeof(tmp) - 1 < len)
		return -1;

	tmp[0] = addr;
	memcpy(&tmp[1], buf, len);

	if (i2c_master_send(this_client, tmp, len + 1) < 0)
		return -1;
#if DEBUG
	YLOGD(("[W] addr[%02x] [%02x]\n", addr, buf[0]));
#endif

	return 0;
}

static int
geomagnetic_i2c_read(uint8_t addr, uint8_t *buf, int len)
{
	struct i2c_msg msg[2];
	int err;

	msg[0].addr = this_client->addr;
	msg[0].flags = 0;
	msg[0].len = 1;
	msg[0].buf = &addr;
	msg[1].addr = this_client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = len;
	msg[1].buf = buf;

	err = i2c_transfer(this_client->adapter, msg, 2);
	if (err != 2) {
		dev_err(&this_client->dev,
				"i2c_transfer() read error: "
				"slave_addr=%02x, reg_addr=%02x, err=%d\n",
				this_client->addr, addr, err);
		return err;
	}


#if DEBUG
	if (len == 1) {
		YLOGD(("[R] addr[%02x] [%02x]\n", addr, buf[0]));
	} else if (len == 6) {
		YLOGD(("[R] addr[%02x] "
		"[%02x%02x%02x%02x%02x%02x]\n",
		addr, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]));
	} else if (len == 8) {
		YLOGD(("[R] addr[%02x] "
		"[%02x%02x%02x%02x%02x%02x%02x%02x]\n",
		addr, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6],
		buf[7]));
	} else if (len == 9) {
		YLOGD(("[R] addr[%02x] "
		"[%02x%02x%02x%02x%02x%02x%02x%02x%02x]\n",
		addr, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6],
		buf[7], buf[8]));
	} else if (len == 16) {
		YLOGD(("[R] addr[%02x] "
		"[%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x"
		"%02x]\n",
		addr,
		buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
		buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14],
		buf[15]));
	}
#endif

	return 0;
}

#endif

static int
geomagnetic_lock(void)
{
	struct geomagnetic_data *data = NULL;
	int rt;

	if (this_client == NULL)
		return -1;

	data = i2c_get_clientdata(this_client);
	rt = down_interruptible(&data->driver_lock);
	if (rt < 0)
		up(&data->driver_lock);
	return rt;
}

static int
geomagnetic_unlock(void)
{
	struct geomagnetic_data *data = NULL;

	if (this_client == NULL)
		return -1;

	data = i2c_get_clientdata(this_client);
	up(&data->driver_lock);
	return 0;
}

static void
geomagnetic_msleep(int ms)
{
	msleep(ms);
}

static void
geomagnetic_current_time(int32_t *sec, int32_t *msec)
{
	struct timeval tv;

	do_gettimeofday(&tv);

	*sec = tv.tv_sec;
	*msec = tv.tv_usec / 1000;
}

static struct yas_mag_driver hwdriver = {
	.callback = {
		.lock		= geomagnetic_lock,
		.unlock		= geomagnetic_unlock,
		.device_open	= geomagnetic_i2c_open,
		.device_close	= geomagnetic_i2c_close,
		.device_read	= geomagnetic_i2c_read,
		.device_write	= geomagnetic_i2c_write,
		.msleep		= geomagnetic_msleep,
		.current_time	= geomagnetic_current_time,
	},
};

static int
geomagnetic_multi_lock(void)
{
	struct geomagnetic_data *data = NULL;
	int rt;

	if (this_client == NULL)
		return -1;

	data = i2c_get_clientdata(this_client);
	rt = down_interruptible(&data->multi_lock);
	if (rt < 0)
		up(&data->multi_lock);
	return rt;
}

static int
geomagnetic_multi_unlock(void)
{
	struct geomagnetic_data *data = NULL;

	if (this_client == NULL)
		return -1;

	data = i2c_get_clientdata(this_client);
	up(&data->multi_lock);
	return 0;
}

static int
geomagnetic_enable(struct geomagnetic_data *data)
{
	if (!atomic_cmpxchg(&data->enable, 0, 1))
		schedule_delayed_work(&data->work, 0);

	return 0;
}

static int
geomagnetic_disable(struct geomagnetic_data *data)
{
	if (atomic_cmpxchg(&data->enable, 1, 0))
		cancel_delayed_work_sync(&data->work);

	return 0;
}

/* Sysfs interface */
static ssize_t
geomagnetic_delay_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	struct input_dev *input_data = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_data);
	int delay;

	geomagnetic_multi_lock();

	delay = data->delay;

	geomagnetic_multi_unlock();

	return sprintf(buf, "%d\n", delay);
}

static ssize_t
geomagnetic_delay_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	struct input_dev *input_data = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_data);
	long value;

	if (hwdriver.set_delay == NULL)
		return -ENOTTY;
	if (strict_strtol(buf, 10, &value) < 0)
		return -EINVAL;

	geomagnetic_multi_lock();

	if (hwdriver.set_delay(value) == 0)
		data->delay = value;

	geomagnetic_multi_unlock();

	return count;
}

static ssize_t
geomagnetic_enable_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	struct input_dev *input_data = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_data);

	return sprintf(buf, "%d\n", atomic_read(&data->enable));
}

static ssize_t
geomagnetic_enable_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	struct input_dev *input_data = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_data);
	long value;
//S [CCI]Ginger modified for MSM8226 
        int gpio = 0;

        gpio = gpio_get_value(GEOMAGNETIC_RSTN_GPIO);
	printk("%s: [CCI]gpio_tlmm_config => pin(%d), GPIO_CFG_OUTPUT, HL=%d, GPIO_CFG_2MA\n", __FUNCTION__, GEOMAGNETIC_RSTN_GPIO, gpio);
	if(gpio == 0)
        {
	        if(gpio_request(GEOMAGNETIC_RSTN_GPIO,"geomagnetic-rstn") < 0)
	        {
		        printk(KERN_ERR "%s: [CCI]gpio_request geomagnetic-rstn", __FUNCTION__);
	        }
	        if(gpio_direction_output(GEOMAGNETIC_RSTN_GPIO, 1) < 0)
	        {
		        printk(KERN_ERR "%s: [CCI]gpio_direction_output geomagnetic-rstn", __FUNCTION__);
	        }
        }
//E [CCI]Ginger modified for MSM8226 

	if (hwdriver.set_enable == NULL)
		return -ENOTTY;
	if (strict_strtol(buf, 10, &value) < 0)
		return -EINVAL;
	value = !!value;

	if (geomagnetic_multi_lock() < 0)
		return count;
	if (hwdriver.set_enable(value) == 0) {
		if (value)
			geomagnetic_enable(data);
		else
			geomagnetic_disable(data);
	}

	geomagnetic_multi_unlock();

	return count;
}

static ssize_t
geomagnetic_filter_enable_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct input_dev *input_data = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_data);
	int filter_enable;

	geomagnetic_multi_lock();

	filter_enable = data->filter_enable;

	geomagnetic_multi_unlock();

	return sprintf(buf, "%d\n", filter_enable);
}

static ssize_t
geomagnetic_filter_enable_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct input_dev *input_data = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_data);
	long value;

	if (hwdriver.set_filter_enable == NULL)
		return -ENOTTY;
	if (strict_strtol(buf, 10, &value) < 0)
		return -EINVAL;

	if (geomagnetic_multi_lock() < 0)
		return count;

	if (hwdriver.set_filter_enable(value) == 0)
		data->filter_enable = !!value;

	geomagnetic_multi_unlock();

	return count;
}

static ssize_t
geomagnetic_filter_len_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	struct input_dev *input_data = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_data);
	int filter_len;

	geomagnetic_multi_lock();

	filter_len = data->filter_len;

	geomagnetic_multi_unlock();

	return sprintf(buf, "%d\n", filter_len);
}

static ssize_t
geomagnetic_filter_len_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	struct input_dev *input_data = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_data);
	struct yas_mag_filter filter;
	long value;

	if (hwdriver.get_filter == NULL || hwdriver.set_filter == NULL)
		return -ENOTTY;
	if (strict_strtol(buf, 10, &value) < 0)
		return -EINVAL;

	if (geomagnetic_multi_lock() < 0)
		return count;

	hwdriver.get_filter(&filter);
	filter.len = value;
	if (hwdriver.set_filter(&filter) == 0)
		data->filter_len = value;

	geomagnetic_multi_unlock();

	return count;
}

static ssize_t
geomagnetic_filter_noise_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	int rt;

	geomagnetic_multi_lock();

	rt = sprintf(buf, "%d %d %d\n", data->filter_noise[0],
			data->filter_noise[1], data->filter_noise[2]);

	geomagnetic_multi_unlock();

	return rt;
}

static ssize_t
geomagnetic_filter_noise_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct yas_mag_filter filter;
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	int32_t noise[3];

	geomagnetic_multi_lock();

	sscanf(buf, "%d %d %d", &noise[0], &noise[1], &noise[2]);
	hwdriver.get_filter(&filter);
	memcpy(filter.noise, noise, sizeof(filter.noise));
	if (hwdriver.set_filter(&filter) == 0)
		memcpy(data->filter_noise, noise, sizeof(data->filter_noise));

	geomagnetic_multi_unlock();

	return count;
}

static ssize_t
geomagnetic_filter_threshold_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct input_dev *input_data = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_data);
	int32_t filter_threshold;

	geomagnetic_multi_lock();

	filter_threshold = data->filter_threshold;

	geomagnetic_multi_unlock();

	return sprintf(buf, "%d\n", filter_threshold);
}

static ssize_t
geomagnetic_filter_threshold_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct input_dev *input_data = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_data);
	struct yas_mag_filter filter;
	long value;

	if (hwdriver.get_filter == NULL || hwdriver.set_filter == NULL)
		return -ENOTTY;
	if (strict_strtol(buf, 10, &value) < 0)
		return -EINVAL;

	if (geomagnetic_multi_lock() < 0)
		return count;

	hwdriver.get_filter(&filter);
	filter.threshold = value;
	if (hwdriver.set_filter(&filter) == 0)
		data->filter_threshold = value;

	geomagnetic_multi_unlock();

	return count;
}

static ssize_t
geomagnetic_position_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	if (hwdriver.get_position == NULL)
		return -ENOTTY;
	return sprintf(buf, "%d\n", hwdriver.get_position());
}

static ssize_t
geomagnetic_position_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	long value;

	if (hwdriver.set_position == NULL)
		return -ENOTTY;
	if (strict_strtol(buf, 10, &value) < 0)
		return -EINVAL;

	hwdriver.set_position(value);

	return count;
}

static ssize_t
geomagnetic_data_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	struct input_dev *input_data = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_data);
	int rt;

	rt = sprintf(buf, "%d %d %d\n", atomic_read(&data->last_data[0]),
			atomic_read(&data->last_data[1]),
			atomic_read(&data->last_data[2]));

	return rt;
}

static ssize_t
geomagnetic_status_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	struct input_dev *input_data = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_data);
	int rt;

	rt = sprintf(buf, "%d\n", atomic_read(&data->last_status));

	return rt;
}

static ssize_t
geomagnetic_status_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	struct input_dev *input_data = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_data);
	static int16_t cnt = 1;
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	struct input_event ev;
#endif
	int accuracy = 0;
	int code = 0;
	int value = 0;

	geomagnetic_multi_lock();

	sscanf(buf, "%d", &accuracy);
	if (0 <= accuracy && accuracy <= 3)
		atomic_set(&data->last_status, accuracy);
	code |= YAS_REPORT_CALIB_OFFSET_CHANGED;
	value = (cnt++ << 16) | (code);

#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	mkev(&ev, EV_ABS, ABS_RAW_REPORT, value);
	sensor_event(&data->raw_devfile_list, &ev, 1);
#else
	input_report_abs(data->input_raw, ABS_RAW_REPORT, value);
	input_sync(data->input_raw);
#endif

	geomagnetic_multi_unlock();

	return count;
}

static ssize_t
geomagnetic_wake_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	struct input_dev *input_data = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_data);
	static int16_t cnt = 1;
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	struct input_event ev[1];
	mkev(ev, EV_ABS, ABS_WAKE, cnt++);
	sensor_event(&data->devfile_list, ev, 1);
#else
	input_report_abs(data->input_data, ABS_WAKE, cnt++);
	input_sync(data->input_data);
#endif

	return count;
}

//S [CCI]Ginger modified for factory test
uint8_t g_compass_product_id=0;
static ssize_t
geomagnetic_ping(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	hwdriver.set_enable(1);
	return sprintf(buf, "0x2e:0x%02x\n", g_compass_product_id);
}
//E [CCI]Ginger modified for factory test

#if DEBUG

static int geomagnetic_suspend(struct i2c_client *client, pm_message_t mesg);
static int geomagnetic_resume(struct i2c_client *client);

static ssize_t
geomagnetic_debug_suspend_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct input_dev *input = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input);

	return sprintf(buf, "%d\n", data->suspend);
}

static ssize_t
geomagnetic_debug_suspend_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	long suspend;

	if (strict_strtol(buf, 10, &suspend) < 0)
		return -EINVAL;
	if (suspend) {
		pm_message_t msg;
		memset(&msg, 0, sizeof(msg));
		geomagnetic_suspend(this_client, msg);
	} else {
		geomagnetic_resume(this_client);
	}

	return count;
}

#endif /* DEBUG */

static DEVICE_ATTR(delay, S_IRUGO|S_IWUSR|S_IWGRP,
	geomagnetic_delay_show, geomagnetic_delay_store);
static DEVICE_ATTR(enable, S_IRUGO|S_IWUSR|S_IWGRP,
	geomagnetic_enable_show, geomagnetic_enable_store);
static DEVICE_ATTR(filter_enable, S_IRUGO|S_IWUSR|S_IWGRP,
	geomagnetic_filter_enable_show,
	geomagnetic_filter_enable_store);
static DEVICE_ATTR(filter_len, S_IRUGO|S_IWUSR|S_IWGRP,
	geomagnetic_filter_len_show, geomagnetic_filter_len_store);
static DEVICE_ATTR(filter_threshold, S_IRUGO|S_IWUSR|S_IWGRP,
	geomagnetic_filter_threshold_show,
	geomagnetic_filter_threshold_store);
static DEVICE_ATTR(filter_noise, S_IRUGO|S_IWUSR|S_IWGRP,
	geomagnetic_filter_noise_show, geomagnetic_filter_noise_store);
static DEVICE_ATTR(data, S_IRUGO, geomagnetic_data_show, NULL);
static DEVICE_ATTR(status, S_IRUGO|S_IWUSR|S_IWGRP, geomagnetic_status_show,
		geomagnetic_status_store);
static DEVICE_ATTR(wake, S_IWUSR|S_IWGRP, NULL, geomagnetic_wake_store);
static DEVICE_ATTR(position, S_IRUGO|S_IWUSR,
	geomagnetic_position_show, geomagnetic_position_store);
#if DEBUG
static DEVICE_ATTR(debug_suspend, S_IRUGO|S_IWUSR,
	geomagnetic_debug_suspend_show, geomagnetic_debug_suspend_store);
#endif /* DEBUG */
//[CCI]Ginger modified for factory test
static DEVICE_ATTR(ping, S_IRUGO|S_IWUSR|S_IWGRP, geomagnetic_ping, NULL);

static struct attribute *geomagnetic_attributes[] = {
	&dev_attr_delay.attr,
	&dev_attr_enable.attr,
	&dev_attr_filter_enable.attr,
	&dev_attr_filter_len.attr,
	&dev_attr_filter_threshold.attr,
	&dev_attr_filter_noise.attr,
	&dev_attr_data.attr,
	&dev_attr_status.attr,
	&dev_attr_wake.attr,
	&dev_attr_position.attr,
	&dev_attr_ping.attr,  //[CCI]Ginger modified for factory test
#if DEBUG
	&dev_attr_debug_suspend.attr,
#endif /* DEBUG */
	NULL
};

static struct attribute_group geomagnetic_attribute_group = {
	.attrs = geomagnetic_attributes
};

static ssize_t
geomagnetic_raw_data_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	struct input_dev *input_data = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_data);
	int rt;

	rt = sprintf(buf, "%d %d %d\n", atomic_read(&data->last_raw[0]),
			atomic_read(&data->last_raw[1]),
			atomic_read(&data->last_raw[2]));

	return rt;
}

static ssize_t
geomagnetic_raw_gyro_threshold_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	int mag_noise, gyro_noise;

	geomagnetic_multi_lock();

	mag_noise = data->mag_noise;
	gyro_noise = data->gyro_noise;

	geomagnetic_multi_unlock();

	return sprintf(buf, "%d %d\n", mag_noise, gyro_noise);
}

static ssize_t
geomagnetic_raw_gyro_threshold_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	static int cnt = 1;
	int32_t mag_noise = 0, gyro_noise = 0;
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	struct input_event ev[1];
#endif

	sscanf(buf, "%d %d", &mag_noise, &gyro_noise);
	if (mag_noise < 0 || SHRT_MAX < mag_noise
			|| gyro_noise < 0 || SHRT_MAX < gyro_noise)
		return -EINVAL;

	geomagnetic_multi_lock();

	data->mag_noise = mag_noise;
	data->gyro_noise = gyro_noise;
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	mkev(ev, EV_ABS, ABS_RAW_GYRO_THRESHOLD, cnt++);
	sensor_event(&data->raw_devfile_list, ev, 1);
#else
	input_report_abs(data->input_raw, ABS_RAW_GYRO_THRESHOLD, cnt++);
	input_sync(data->input_raw);
#endif

	geomagnetic_multi_unlock();

	return count;
}

static ssize_t
geomagnetic_raw_threshold_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	int threshold;

	geomagnetic_multi_lock();

	threshold = data->threshold;

	geomagnetic_multi_unlock();

	return sprintf(buf, "%d\n", threshold);
}

static ssize_t
geomagnetic_raw_threshold_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	static int cnt = 1;
	long value;
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	struct input_event ev[1];
#endif

	if (strict_strtol(buf, 10, &value) < 0)
		return -EINVAL;
	if (value < 0 || 2 <  value)
		return -EINVAL;

	geomagnetic_multi_lock();

	data->threshold = value;
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	mkev(ev, EV_ABS, ABS_RAW_THRESHOLD, cnt++);
	sensor_event(&data->raw_devfile_list, ev, 1);
#else
	input_report_abs(data->input_raw, ABS_RAW_THRESHOLD, cnt++);
	input_sync(data->input_raw);
#endif

	geomagnetic_multi_unlock();

	return count;
}

static ssize_t
geomagnetic_raw_distortion_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	int rt;

	geomagnetic_multi_lock();

	rt = sprintf(buf, "%d %d %d\n", data->distortion[0],
			data->distortion[1], data->distortion[2]);

	geomagnetic_multi_unlock();

	return rt;
}

static ssize_t
geomagnetic_raw_distortion_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	int32_t distortion[3];
	static int32_t cnt = 1;
	int i;
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	struct input_event ev[1];
#endif

	sscanf(buf, "%d %d %d", &distortion[0], &distortion[1], &distortion[2]);
	if (distortion[0] <= 0 || distortion[1] <= 0 || distortion[2] <= 0)
		return -EINVAL;

	geomagnetic_multi_lock();

	for (i = 0; i < 3; i++)
		data->distortion[i] = distortion[i];
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	mkev(ev, EV_ABS, ABS_RAW_DISTORTION, cnt++);
	sensor_event(&data->raw_devfile_list, ev, 1);
#else
	input_report_abs(data->input_raw, ABS_RAW_DISTORTION, cnt++);
	input_sync(data->input_raw);
#endif

	geomagnetic_multi_unlock();

	return count;
}

static ssize_t
geomagnetic_raw_shape_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	int shape;

	geomagnetic_multi_lock();

	shape = data->shape;

	geomagnetic_multi_unlock();

	return sprintf(buf, "%d\n", shape);
}

static ssize_t
geomagnetic_raw_shape_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	long value;
	static int cnt = 1;
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	struct input_event ev[1];
#endif

	if (strict_strtol(buf, 10, &value) < 0)
		return -EINVAL;
	if (value < 0 || 1 < value)
		return -EINVAL;

	geomagnetic_multi_lock();

	data->shape = value;
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	mkev(ev, EV_ABS, ABS_RAW_SHAPE, cnt++);
	sensor_event(&data->raw_devfile_list, ev, 1);
#else
	input_report_abs(data->input_raw, ABS_RAW_SHAPE, cnt++);
	input_sync(data->input_raw);
#endif

	geomagnetic_multi_unlock();

	return count;
}

static ssize_t
geomagnetic_raw_offsets_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	struct yas_mag_offset offset;
	int accuracy;

	geomagnetic_multi_lock();

	offset = data->driver_offset;
	accuracy = atomic_read(&data->last_status);

	geomagnetic_multi_unlock();

	return sprintf(buf, "%d %d %d %d %d %d %d\n", offset.hard_offset[0],
			offset.hard_offset[1], offset.hard_offset[2],
			offset.calib_offset.v[0], offset.calib_offset.v[1],
			offset.calib_offset.v[2], accuracy);
}

static ssize_t
geomagnetic_raw_offsets_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	struct yas_mag_offset offset;
	int32_t hard_offset[3];
	int i, accuracy;

	geomagnetic_multi_lock();

	sscanf(buf, "%d %d %d %d %d %d %d", &hard_offset[0], &hard_offset[1],
			&hard_offset[2], &offset.calib_offset.v[0],
			&offset.calib_offset.v[1], &offset.calib_offset.v[2],
			&accuracy);
	if (0 <= accuracy && accuracy <= 3) {
		for (i = 0; i < 3; i++)
			offset.hard_offset[i] = (int8_t)hard_offset[i];
		if (hwdriver.set_offset(&offset) == 0) {
			atomic_set(&data->last_status, accuracy);
			data->driver_offset = offset;
		}
	}

	geomagnetic_multi_unlock();
	YLOGD(("geomagnetic_raw_offsets_store[%d][%d][%d][%d][%d][%d][%d]\n",
				hard_offset[0], hard_offset[1], hard_offset[2],
				offset.calib_offset.v[0],
				offset.calib_offset.v[1],
				offset.calib_offset.v[2],
				accuracy));

	return count;
}

#ifdef YAS_MAG_MANUAL_OFFSET
static ssize_t
geomagnetic_raw_manual_offsets_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	struct yas_vector offset;

	geomagnetic_multi_lock();

	offset = data->manual_offset;

	geomagnetic_multi_unlock();

	return sprintf(buf, "%d %d %d\n", offset.v[0], offset.v[1],
			offset.v[2]);
}

static ssize_t
geomagnetic_raw_manual_offsets_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	struct yas_vector offset;

	geomagnetic_multi_lock();

	sscanf(buf, "%d %d %d", &offset.v[0], &offset.v[1], &offset.v[2]);
	if (hwdriver.set_manual_offset(&offset) == 0)
		data->manual_offset = offset;

	geomagnetic_multi_unlock();

	return count;
}
#endif

static ssize_t
geomagnetic_raw_static_matrix_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	struct yas_matrix matrix;

	geomagnetic_multi_lock();

	matrix = data->static_matrix;

	geomagnetic_multi_unlock();

	return sprintf(buf, "%d %d %d %d %d %d %d %d %d\n", matrix.matrix[0],
			matrix.matrix[1], matrix.matrix[2], matrix.matrix[3],
			matrix.matrix[4], matrix.matrix[5], matrix.matrix[6],
			matrix.matrix[7], matrix.matrix[8]);
}

static ssize_t
geomagnetic_raw_static_matrix_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	struct yas_matrix matrix;

	geomagnetic_multi_lock();

	sscanf(buf, "%d %d %d %d %d %d %d %d %d", &matrix.matrix[0],
			&matrix.matrix[1], &matrix.matrix[2],
			&matrix.matrix[3], &matrix.matrix[4], &matrix.matrix[5],
			&matrix.matrix[6], &matrix.matrix[7],
			&matrix.matrix[8]);
	if (hwdriver.set_static_matrix(&matrix) == 0)
		data->static_matrix = matrix;

	geomagnetic_multi_unlock();

	return count;
}

static ssize_t
geomagnetic_raw_dynamic_matrix_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	struct yas_matrix matrix;

	geomagnetic_multi_lock();

	matrix = data->dynamic_matrix;

	geomagnetic_multi_unlock();

	return sprintf(buf, "%d %d %d %d %d %d %d %d %d\n", matrix.matrix[0],
			matrix.matrix[1], matrix.matrix[2], matrix.matrix[3],
			matrix.matrix[4], matrix.matrix[5], matrix.matrix[6],
			matrix.matrix[7], matrix.matrix[8]);
}

static ssize_t
geomagnetic_raw_dynamic_matrix_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	struct yas_matrix matrix;

	geomagnetic_multi_lock();

	sscanf(buf, "%d %d %d %d %d %d %d %d %d", &matrix.matrix[0],
			&matrix.matrix[1], &matrix.matrix[2],
			&matrix.matrix[3], &matrix.matrix[4], &matrix.matrix[5],
			&matrix.matrix[6], &matrix.matrix[7],
			&matrix.matrix[8]);
	if (hwdriver.set_dynamic_matrix(&matrix) == 0)
		data->dynamic_matrix = matrix;

	geomagnetic_multi_unlock();

	return count;
}

static ssize_t
geomagnetic_raw_ellipsoid_mode_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	int ellipsoid_mode;

	geomagnetic_multi_lock();

	ellipsoid_mode = data->ellipsoid_mode;

	geomagnetic_multi_unlock();

	return sprintf(buf, "%d\n", ellipsoid_mode);
}

static ssize_t
geomagnetic_raw_ellipsoid_mode_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct input_dev *input_raw = to_input_dev(dev);
	struct geomagnetic_data *data = input_get_drvdata(input_raw);
	long value;
	static int cnt = 1;
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	struct input_event ev[1];
#endif

	if (strict_strtol(buf, 10, &value) < 0)
		return -EINVAL;
	if (value < 0 || 4 < value)
		return -EINVAL;

	geomagnetic_multi_lock();

	data->ellipsoid_mode = value;
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	mkev(ev, EV_ABS, ABS_RAW_MODE, cnt++);
	sensor_event(&data->raw_devfile_list, ev, 1);
#else
	input_report_abs(data->input_raw, ABS_RAW_MODE, cnt++);
	input_sync(data->input_raw);
#endif

	geomagnetic_multi_unlock();

	return count;
}

#ifdef SYSFS_PCBTEST

static int
pcbtest_i2c_write(uint8_t slave, uint8_t addr, const uint8_t *buf, int len)
{
	(void) slave;
	return geomagnetic_i2c_write(addr, buf, len);
}

static int
pcbtest_i2c_read(uint8_t slave, uint8_t addr, uint8_t *buf, int len)
{
	(void) slave;
	return geomagnetic_i2c_read(addr, buf, len);
}

static struct yas_pcb_test pcbtest = {
	.callback = {
		.power_on	= NULL,
		.power_off	= NULL,
		.i2c_open	= geomagnetic_i2c_open,
		.i2c_close	= geomagnetic_i2c_close,
		.i2c_read	= pcbtest_i2c_read,
		.i2c_write	= pcbtest_i2c_write,
		.msleep		= geomagnetic_msleep,
		.read_intpin	= NULL,
	},
};


static int noise_test_initialized;
//S [CCI] Ginger modified for factory test
int checkresult(int err1, int id, int err3, int err4, int x, int y1, int y2, int err5, int dir, int err6, int sx,
			int sy, int err7, int ohx, int ohy, int ohz, int err2)
{
	
	if(err1)
	    return 0;
	if(id != 0x02)
	    return 0;
	if(err3)
	    return 0;
	if(err4)
	    return 0;
	if(!CHECK_RANGE(x, -30, 30))
	    return 0;
	if(!CHECK_RANGE(y1, -30, 30))
	    return 0;
	if(!CHECK_RANGE(y2, -30, 30))
	    return 0;
	if(err5)
	    return 0;
	if(!CHECK_RANGE(dir, 0, 359))
	    return 0;
	if(err6)
	    return 0;
	if(!CHECK_GREATER(sx,17))
	    return 0;
	if(!CHECK_GREATER(sy,22))
	    return 0;
	if(err7)
	    return 0;
	//if(!CHECK_RANGE(ohx, -600, 600))
	//    return 0;
	//if(!CHECK_RANGE(ohy, -600, 600))
	//    return 0;
	//if(!CHECK_RANGE(ohz, -600, 600))
	//    return 0;
	if(err2)
	    return 0;
	    
	return 1;
}
//E [CCI] Ginger modified for factory test
static ssize_t
geomagnetic_raw_self_test_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int id, x, y1, y2, dir, sx, sy, ohx, ohy, ohz;
	int err1, err2, err3, err4, err5, err6, err7;
	int res = 0;//[CCI] Ginger modified for factory test

	if (noise_test_initialized)
		pcbtest.power_off();
	err1 = pcbtest.power_on_and_device_check(&id);
	err3 = pcbtest.initialization();
	err4 = pcbtest.offset_control_measurement_and_set_offset_register(&x,
			&y1, &y2);
	err5 = pcbtest.direction_measurement(&dir);
	err6
	= pcbtest.sensitivity_measurement_of_magnetic_sensor_by_test_coil(&sx,
			&sy);
	err7 = pcbtest.magnetic_field_level_check(&ohx, &ohy, &ohz);
	err2 = pcbtest.power_off();
	noise_test_initialized = 0;
//S [CCI] Ginger modified for factory test
	res = checkresult(err1, id, err3, err4, x, y1, y2, err5, dir, err6, sx,
			sy, err7, ohx, ohy, ohz, err2);
	return sprintf(buf,
			"%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\nself_test: %s\n",
			err1, id, err3, err4, x, y1, y2, err5, dir, err6, sx,
			sy, err7, ohx, ohy, ohz, err2, (res)?"Pass":"Fail");
//E [CCI] Ginger modified for factory test
}

static ssize_t
geomagnetic_raw_self_test_noise_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int id, x, y1, y2, dir, hx0, hy0, hz0;
	int err8;

	if (!noise_test_initialized) {
		pcbtest.power_on_and_device_check(&id);
		pcbtest.initialization();
		pcbtest.offset_control_measurement_and_set_offset_register(&x,
				&y1, &y2);
		noise_test_initialized = 1;
	}
	pcbtest.direction_measurement(&dir);
	err8 = pcbtest.noise_level_check(&hx0, &hy0, &hz0);
	return sprintf(buf, "%d %d %d %d\n", err8, hx0, hy0, hz0);
}

static DEVICE_ATTR(self_test, S_IRUSR, geomagnetic_raw_self_test_show, NULL);
static DEVICE_ATTR(self_test_noise, S_IRUSR,
		geomagnetic_raw_self_test_noise_show, NULL);

#endif

static DEVICE_ATTR(data_raw, S_IRUGO, geomagnetic_raw_data_show, NULL);
static DEVICE_ATTR(threshold, S_IRUGO|S_IWUSR,
	geomagnetic_raw_threshold_show, geomagnetic_raw_threshold_store);
static DEVICE_ATTR(distortion, S_IRUGO|S_IWUSR,
	geomagnetic_raw_distortion_show, geomagnetic_raw_distortion_store);
static DEVICE_ATTR(shape, S_IRUGO|S_IWUSR,
	geomagnetic_raw_shape_show, geomagnetic_raw_shape_store);
static DEVICE_ATTR(offsets, S_IRUGO|S_IWUSR,
	geomagnetic_raw_offsets_show, geomagnetic_raw_offsets_store);
static DEVICE_ATTR(gyro_threshold, S_IRUGO|S_IWUSR,
	geomagnetic_raw_gyro_threshold_show,
	geomagnetic_raw_gyro_threshold_store);
#ifdef YAS_MAG_MANUAL_OFFSET
static DEVICE_ATTR(manual_offsets, S_IRUGO|S_IWUSR,
	geomagnetic_raw_manual_offsets_show,
	geomagnetic_raw_manual_offsets_store);
#endif
static DEVICE_ATTR(static_matrix, S_IRUGO|S_IWUSR,
	geomagnetic_raw_static_matrix_show,
	geomagnetic_raw_static_matrix_store);
static DEVICE_ATTR(dynamic_matrix, S_IRUGO|S_IWUSR,
	geomagnetic_raw_dynamic_matrix_show,
	geomagnetic_raw_dynamic_matrix_store);
static DEVICE_ATTR(ellipsoid_mode, S_IRUGO|S_IWUSR,
	geomagnetic_raw_ellipsoid_mode_show,
	geomagnetic_raw_ellipsoid_mode_store);

static struct attribute *geomagnetic_raw_attributes[] = {
#ifdef SYSFS_PCBTEST
	&dev_attr_self_test.attr,
	&dev_attr_self_test_noise.attr,
#endif
	&dev_attr_data_raw.attr,
	&dev_attr_gyro_threshold.attr,
	&dev_attr_threshold.attr,
	&dev_attr_distortion.attr,
	&dev_attr_shape.attr,
	&dev_attr_offsets.attr,
#ifdef YAS_MAG_MANUAL_OFFSET
	&dev_attr_manual_offsets.attr,
#endif
	&dev_attr_static_matrix.attr,
	&dev_attr_dynamic_matrix.attr,
	&dev_attr_ellipsoid_mode.attr,
	NULL
};

static struct attribute_group geomagnetic_raw_attribute_group = {
	.attrs = geomagnetic_raw_attributes
};

/* Interface Functions for Lower Layer */

static int
geomagnetic_work(struct yas_mag_data *magdata)
{
	struct geomagnetic_data *data = i2c_get_clientdata(this_client);
	uint32_t time_delay_ms = 100;
	static int cnt;
	int rt, i, accuracy;
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	struct input_event ev[5];
	struct timeval tv;
#endif

	if (hwdriver.measure == NULL || hwdriver.get_offset == NULL)
		return time_delay_ms;

	rt = hwdriver.measure(magdata, &time_delay_ms);
	if (rt < 0) {
		YLOGE(("measure failed[%d]\n", rt));
		return 100;
	}
	YLOGD(("[%d] xy1y2 [%d][%d][%d] raw[%d][%d][%d]\n",
				rt,
				magdata->xy1y2.v[0], magdata->xy1y2.v[1],
				magdata->xy1y2.v[2], magdata->raw.v[0],
				magdata->raw.v[1], magdata->raw.v[2]));

	accuracy = atomic_read(&data->last_status);

	if ((rt & YAS_REPORT_OVERFLOW_OCCURED)
			|| (rt & YAS_REPORT_HARD_OFFSET_CHANGED)
			|| (rt & YAS_REPORT_CALIB_OFFSET_CHANGED)) {
		static uint16_t count = 1;
		int code = 0;
		int value = 0;

		hwdriver.get_offset(&data->driver_offset);
		if (rt & YAS_REPORT_OVERFLOW_OCCURED) {
			atomic_set(&data->last_status, 0);
			accuracy = 0;
		}

		/* report event */
		code |= (rt & YAS_REPORT_OVERFLOW_OCCURED);
		code |= (rt & YAS_REPORT_HARD_OFFSET_CHANGED);
		code |= (rt & YAS_REPORT_CALIB_OFFSET_CHANGED);
		value = (count++ << 16) | (code);
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
		mkev(ev, EV_ABS, ABS_RAW_REPORT, value);
		sensor_event(&data->raw_devfile_list, ev, 1);
#else
		input_report_abs(data->input_raw, ABS_RAW_REPORT, value);
#endif
	}

	if (rt & YAS_REPORT_DATA) {
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
		get_time_stamp(&tv);
		mkev_with_time(&ev[0], EV_ABS, ABS_X, magdata->xyz.v[0], &tv);
		mkev_with_time(&ev[1], EV_ABS, ABS_Y, magdata->xyz.v[1], &tv);
		mkev_with_time(&ev[2], EV_ABS, ABS_Z, magdata->xyz.v[2], &tv);
		mkev_with_time(&ev[3], EV_ABS, ABS_STATUS, accuracy, &tv);
		mkev_with_time(&ev[4], EV_SYN, 0, 0, &tv);
		sensor_event(&data->devfile_list, ev, 5);
#else
		/* report magnetic data in [nT] */
		input_report_abs(data->input_data, ABS_X, magdata->xyz.v[0]);
		input_report_abs(data->input_data, ABS_Y, magdata->xyz.v[1]);
		input_report_abs(data->input_data, ABS_Z, magdata->xyz.v[2]);
		if (atomic_read(&data->last_data[0]) == magdata->xyz.v[0]
				&& atomic_read(&data->last_data[1])
				== magdata->xyz.v[1]
				&& atomic_read(&data->last_data[2])
				== magdata->xyz.v[2]) {
			input_report_abs(data->input_data, ABS_RUDDER, cnt++);
		}
		input_report_abs(data->input_data, ABS_STATUS, accuracy);
		input_sync(data->input_data);
		YLOGD(("input_report_abs accuracy[%d]\n", accuracy));
#endif

		for (i = 0; i < 3; i++)
			atomic_set(&data->last_data[i], magdata->xyz.v[i]);
	}

	if (rt & YAS_REPORT_CALIB) {
#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
		get_time_stamp(&tv);
		mkev_with_time(&ev[0], EV_ABS, ABS_X, magdata->raw.v[0], &tv);
		mkev_with_time(&ev[1], EV_ABS, ABS_Y, magdata->raw.v[1], &tv);
		mkev_with_time(&ev[2], EV_ABS, ABS_Z, magdata->raw.v[2], &tv);
		mkev_with_time(&ev[3], EV_SYN, 0, 0, &tv);
		sensor_event(&data->raw_devfile_list, ev, 4);
#else
		/* report raw magnetic data */
		input_report_abs(data->input_raw, ABS_X, magdata->raw.v[0]);
		input_report_abs(data->input_raw, ABS_Y, magdata->raw.v[1]);
		input_report_abs(data->input_raw, ABS_Z, magdata->raw.v[2]);
		input_sync(data->input_raw);
#endif
		for (i = 0; i < 3; i++)
			atomic_set(&data->last_raw[i], magdata->raw.v[i]);
	}

	return time_delay_ms;

}

static void
geomagnetic_input_work_func(struct work_struct *work)
{
	struct geomagnetic_data *data
		= container_of((struct delayed_work *)work,
			struct geomagnetic_data, work);
	uint32_t delay;
	struct yas_mag_data magdata;

	delay = geomagnetic_work(&magdata);
	if (delay > 0)
		schedule_delayed_work(&data->work, msecs_to_jiffies(delay) + 1);
	else
		schedule_delayed_work(&data->work, 0);
}

static int
geomagnetic_suspend(struct i2c_client *client, pm_message_t mesg)
{
	struct geomagnetic_data *data = i2c_get_clientdata(client);

	if (atomic_read(&data->enable))
		cancel_delayed_work_sync(&data->work);
#if DEBUG
	data->suspend = 1;
#endif

	return 0;
}

static int
geomagnetic_resume(struct i2c_client *client)
{
	struct geomagnetic_data *data = i2c_get_clientdata(client);

	if (atomic_read(&data->enable))
		schedule_delayed_work(&data->work, 0);

#if DEBUG
	data->suspend = 0;
#endif

	return 0;
}

//S [CCI]Ginger modified for MSM8226 DTS
static void mag_sensor_power_on(struct i2c_client *client)
{
	static struct regulator *reg_l19;
	static struct regulator *reg_lvs1;
	int error;

printk(KERN_INFO "%s: mag power on start\n", __func__);

	//get power and set voltage level
	reg_l19 = regulator_get(&client->dev, "vdd");
	if (IS_ERR(reg_l19)) {
		printk("[CCI]%s: Regulator get failed vdd rc=%ld\n", __FUNCTION__, PTR_ERR(reg_l19));
	}
	if (regulator_count_voltages(reg_l19) > 0) {
		error = regulator_set_voltage(reg_l19,  2850000, 2850000);
		if (error) {
			printk("[CCI]%s: regulator set_vtg vdd failed rc=%d\n", __FUNCTION__, error);
		}
	}

	reg_lvs1 = regulator_get(&client->dev,"vddio");
	if (IS_ERR(reg_lvs1)){
		printk("[CCI]could not get vddio lvs1, rc = %ld\n", PTR_ERR(reg_lvs1));
		}

	//enable power

	error = regulator_set_optimum_mode(reg_l19, 100000);
	if (error < 0) {
		printk("[CCI]%s: Regulator vdd set_opt failed rc=%d\n", __FUNCTION__, error);
		regulator_put(reg_l19);
	}

	error = regulator_enable(reg_l19);
	if (error) {
		printk("[CCI]%s: Regulator vdd enable failed rc=%d\n", __FUNCTION__, error);
		regulator_put(reg_l19);
	}

	error = regulator_enable(reg_lvs1);
	if (error) {
		printk("[CCI]%s: enable vddio lvs1 failed, rc=%d\n", __FUNCTION__, error);
		regulator_put(reg_lvs1);
	}

printk(KERN_INFO "%s: mag power on end\n", __func__);

	error = gpio_tlmm_config(GPIO_CFG(GEOMAGNETIC_RSTN_GPIO, 0, GPIO_CFG_OUTPUT, GPIO_CFG_PULL_UP, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	if(error < 0)
	{
		printk(KERN_ERR "%s: [CCI]gpio_tlmm_config geomagnetic-rstn, err=%d", __FUNCTION__, error);
	}
	error = gpio_request(GEOMAGNETIC_RSTN_GPIO,"geomagnetic-rstn");
	if(error < 0)
	{
		printk(KERN_ERR "%s: [CCI]gpio_request geomagnetic-rstn, err=%d", __FUNCTION__, error);
	}
	error = gpio_direction_output(GEOMAGNETIC_RSTN_GPIO, 1);
	if(error < 0)
	{
		printk(KERN_ERR "%s: [CCI]gpio_direction_output geomagnetic-rstn, err=%d", __FUNCTION__, error);
	}
	//gpio_set_value(GEOMAGNETIC_RSTN_GPIO,1);
	//printk("%s: [CCI]gpio_tlmm_config => pin(%d), GPIO_CFG_OUTPUT, HL=%d, GPIO_CFG_2MA\n", __FUNCTION__, GEOMAGNETIC_RSTN_GPIO, gpio_get_value(GEOMAGNETIC_RSTN_GPIO));

	mdelay(3);// delay 3 ms for power ready when issue first I2C command
	
}
//E [CCI]Ginger modified for MSM8226 DTS

static int
geomagnetic_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct geomagnetic_data *data = NULL;
	struct input_dev *input_data = NULL, *input_raw = NULL;
	int rt, sysfs_created = 0, sysfs_raw_created = 0;
	int data_registered = 0, raw_registered = 0, i;
	struct yas_mag_filter filter;

//S [CCI]Ginger modified for MSM8226 DTS
	mag_sensor_power_on(client);
	printk("[CCI]%s: geomagnetic_probe start ---\n", __FUNCTION__);
//E [CCI]Ginger modified for MSM8226 DTS

	i2c_set_clientdata(client, NULL);
	data = kzalloc(sizeof(struct geomagnetic_data), GFP_KERNEL);
	if (data == NULL) {
		rt = -ENOMEM;
		goto err;
	}
	data->ellipsoid_mode = YAS_DEFAULT_MAGCALIB_MODE;
	data->threshold = YAS_DEFAULT_MAGCALIB_THRESHOLD;
	data->distortion[0] = YAS_DEFAULT_MAGCALIB_DISTORTION_1;
	data->distortion[1] = YAS_DEFAULT_MAGCALIB_DISTORTION_2;
	data->distortion[2] = YAS_DEFAULT_MAGCALIB_DISTORTION_3;
	data->shape = 0;
	data->mag_noise = YAS_DEFAULT_GYROCALIB_DISTORTION_MAG;
	data->gyro_noise = YAS_DEFAULT_GYROCALIB_DISTORTION_GYRO;
	atomic_set(&data->enable, 0);
	for (i = 0; i < 3; i++)
		atomic_set(&data->last_data[i], 0);
	for (i = 0; i < 3; i++)
		atomic_set(&data->last_raw[i], 0);
	atomic_set(&data->last_status, 0);
	INIT_DELAYED_WORK(&data->work, geomagnetic_input_work_func);
	sema_init(&data->driver_lock, 1);
	sema_init(&data->multi_lock, 1);

	input_data = input_allocate_device();
	if (input_data == NULL) {
		rt = -ENOMEM;
		YLOGE(("geomagnetic_probe: Failed to allocate input_data "
					"device\n"));
		goto err;
	}

	input_data->name = GEOMAGNETIC_INPUT_NAME;
	input_data->id.bustype = BUS_I2C;
	set_bit(EV_ABS, input_data->evbit);
	input_set_abs_params(input_data, ABS_X, INT_MIN, INT_MAX, 0, 0);
	input_set_abs_params(input_data, ABS_Y, INT_MIN, INT_MAX, 0, 0);
	input_set_abs_params(input_data, ABS_Z, INT_MIN, INT_MAX, 0, 0);
	input_set_abs_params(input_data, ABS_RUDDER, INT_MIN, INT_MAX, 0, 0);
	input_set_abs_params(input_data, ABS_STATUS, 0, 3, 0, 0);
	input_set_abs_params(input_data, ABS_WAKE, INT_MIN, INT_MAX, 0, 0);
	input_data->dev.parent = &client->dev;

	rt = input_register_device(input_data);
	if (rt) {
		YLOGE(("geomagnetic_probe: Unable to register input_data "
					"device: %s\n", input_data->name));
		goto err;
	}
	data_registered = 1;

	rt = sysfs_create_group(&input_data->dev.kobj,
			&geomagnetic_attribute_group);
	if (rt) {
		YLOGE(("geomagnetic_probe: sysfs_create_group failed[%s]\n",
					input_data->name));
		goto err;
	}
	//S [CCI]Ginger modified for factory test
	rt = sysfs_create_group(&client->dev.kobj,
			&geomagnetic_attribute_group);
	if (rt) {
		printk("[CCI]geomagnetic_probe: sysfs_create_dev group failed[%s]\n",
					input_data->name);
	}
	//E [CCI]Ginger modified for factory test
	sysfs_created = 1;

	input_raw = input_allocate_device();
	if (input_raw == NULL) {
		rt = -ENOMEM;
		YLOGE(("geomagnetic_probe: Failed to allocate input_raw "
					"device\n"));
		goto err;
	}

	input_raw->name = GEOMAGNETIC_INPUT_RAW_NAME;
	input_raw->id.bustype = BUS_I2C;
	set_bit(EV_ABS, input_raw->evbit);
	input_set_abs_params(input_raw, ABS_X, INT_MIN, INT_MAX, 0, 0);
	input_set_abs_params(input_raw, ABS_Y, INT_MIN, INT_MAX, 0, 0);
	input_set_abs_params(input_raw, ABS_Z, INT_MIN, INT_MAX, 0, 0);
	input_set_abs_params(input_raw, ABS_RAW_DISTORTION, 0, INT_MAX, 0, 0);
	input_set_abs_params(input_raw, ABS_RAW_THRESHOLD, 0, 2, 0, 0);
	input_set_abs_params(input_raw, ABS_RAW_SHAPE, 0, 1, 0, 0);
	input_set_abs_params(input_raw, ABS_RAW_MODE, 0, 4, 0, 0);
	input_set_abs_params(input_raw, ABS_RAW_GYRO_THRESHOLD, INT_MIN,
			INT_MAX, 0, 0);
	input_set_abs_params(input_raw, ABS_RAW_REPORT, INT_MIN, INT_MAX, 0, 0);
	input_raw->dev.parent = &client->dev;

	rt = input_register_device(input_raw);
	if (rt) {
		YLOGE(("geomagnetic_probe: Unable to register input_raw device:"
					" %s\n", input_raw->name));
		goto err;
	}
	raw_registered = 1;

	rt = sysfs_create_group(&input_raw->dev.kobj,
			&geomagnetic_raw_attribute_group);
	if (rt) {
		YLOGE(("geomagnetic_probe: sysfs_create_group failed[%s]\n",
					input_data->name));
		goto err;
	}
//[CCI]S Ginger modified for factory test
#ifdef SYSFS_PCBTEST
        rt = device_create_file(&client->dev, &dev_attr_self_test);
	if (rt) {
		printk("[CCI]geomagnetic_probe: device_create_file dev_attr_self_test failed[%s]\n",
					input_data->name);
	}
#endif
//[CCI]E Ginger modified for factory test
	sysfs_raw_created = 1;

	this_client = client;
	data->input_raw = input_raw;
	data->input_data = input_data;
	input_set_drvdata(input_data, data);
	input_set_drvdata(input_raw, data);
	i2c_set_clientdata(client, data);

	rt = yas_mag_driver_init(&hwdriver);
	if (rt < 0) {
		YLOGE(("yas_mag_driver_init failed[%d]\n", rt));
		goto err;
	}
	if (hwdriver.init != NULL) {
		rt = hwdriver.init();
		if (rt < 0) {
			YLOGE(("hwdriver.init() failed[%d]\n", rt));
			goto err;
		}
	}
	if (hwdriver.set_position != NULL) {
		if (hwdriver.set_position(
					CONFIG_INPUT_YAS_MAGNETOMETER_POSITION)
				< 0) {
			YLOGE(("hwdriver.set_position() failed[%d]\n", rt));
			goto err;
		}
	}
	if (hwdriver.get_offset != NULL) {
		if (hwdriver.get_offset(&data->driver_offset) < 0) {
			YLOGE(("hwdriver get_driver_state failed\n"));
			goto err;
		}
	}
	if (hwdriver.get_delay != NULL)
		data->delay = hwdriver.get_delay();
	if (hwdriver.set_filter_enable != NULL) {
		/* default to enable */
		if (hwdriver.set_filter_enable(1) == 0)
			data->filter_enable = 1;
	}
	if (hwdriver.get_filter != NULL) {
		if (hwdriver.get_filter(&filter) < 0) {
			YLOGE(("hwdriver get_filter failed\n"));
			goto err;
		}
		data->filter_len = filter.len;
		for (i = 0; i < 3; i++)
			data->filter_noise[i] = filter.noise[i];
		data->filter_threshold = filter.threshold;
	}
	if (hwdriver.get_static_matrix != NULL)
		hwdriver.get_static_matrix(&data->static_matrix);
	if (hwdriver.get_dynamic_matrix != NULL)
		hwdriver.get_dynamic_matrix(&data->dynamic_matrix);
#ifdef SYSFS_PCBTEST
	rt = yas_pcb_test_init(&pcbtest);
	if (rt < 0) {
		YLOGE(("yas_pcb_test_init failed[%d]\n", rt));
		goto err;
	}
#endif

#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	INIT_LIST_HEAD(&data->devfile_list);
	INIT_LIST_HEAD(&data->raw_devfile_list);
	if (misc_register(&sensor_devfile) < 0)
		goto err;
	if (misc_register(&sensor_raw_devfile) < 0) {
		misc_deregister(&sensor_devfile);
		goto err;
	}
#endif

	printk("[CCI]%s: geomagnetic_probe end ---\n", __FUNCTION__);

	return 0;

err:
	if (data != NULL) {
		if (input_raw != NULL) {
			if (sysfs_raw_created) {
				sysfs_remove_group(&input_raw->dev.kobj,
					&geomagnetic_raw_attribute_group);
			}
			if (raw_registered)
				input_unregister_device(input_raw);
			else
				input_free_device(input_raw);
		}
		if (input_data != NULL) {
			if (sysfs_created) {
				sysfs_remove_group(&input_data->dev.kobj,
					&geomagnetic_attribute_group);
			}
			if (data_registered)
				input_unregister_device(input_data);
			else
				input_free_device(input_data);
		}
		kfree(data);
	}

	return rt;
}

static int
geomagnetic_remove(struct i2c_client *client)
{
	struct geomagnetic_data *data = i2c_get_clientdata(client);

#ifdef YAS_SENSOR_KERNEL_DEVFILE_INTERFACE
	misc_deregister(&sensor_devfile);
	misc_deregister(&sensor_raw_devfile);
#endif
	if (data != NULL) {
		geomagnetic_disable(data);
		if (hwdriver.term != NULL)
			hwdriver.term();

		input_unregister_device(data->input_raw);
		sysfs_remove_group(&data->input_data->dev.kobj,
				&geomagnetic_attribute_group);
		sysfs_remove_group(&data->input_raw->dev.kobj,
				&geomagnetic_raw_attribute_group);
		input_unregister_device(data->input_data);
		kfree(data);
	}

	return 0;
}

/* I2C Device Driver */
static struct i2c_device_id geomagnetic_idtable[] = {
	{GEOMAGNETIC_I2C_DEVICE_NAME, 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, geomagnetic_idtable);

//S [CCI]Ginger modified for MSM8226 DTS
static struct of_device_id geomagnetic_dts_table[] = {
		{ .compatible  = "qcom,geomagnetic",},
		{ },
};
//E [CCI]Ginger modified for MSM8226 DTS

static struct i2c_driver geomagnetic_i2c_driver = {
	.driver = {
		.name		= GEOMAGNETIC_I2C_DEVICE_NAME,
		.owner		= THIS_MODULE,
		.of_match_table = geomagnetic_dts_table,  //[CCI]Ginger modified for MSM8226 DTS
	},

	.id_table		= geomagnetic_idtable,
	.probe			= geomagnetic_probe,
	.remove			= geomagnetic_remove,
	.suspend		= geomagnetic_suspend,
	.resume			= geomagnetic_resume,
};

static int __init
geomagnetic_init(void)
{
	return i2c_add_driver(&geomagnetic_i2c_driver);
}

static void __exit
geomagnetic_term(void)
{
	i2c_del_driver(&geomagnetic_i2c_driver);
}

#ifdef GEOMAGNETIC_PLATFORM_API
static int
geomagnetic_api_enable(int enable)
{
	struct geomagnetic_data *data = i2c_get_clientdata(this_client);
	int rt;

	if (geomagnetic_multi_lock() < 0)
		return -1;
	enable = !!enable;

	rt = hwdriver.set_enable(enable);
	if (rt == 0) {
		atomic_set(&data->enable, enable);
		if (enable)
			rt = hwdriver.set_delay(20);
	}

	geomagnetic_multi_unlock();

	return rt;
}

int
geomagnetic_api_resume(void)
{
	return geomagnetic_api_enable(1);
}
EXPORT_SYMBOL(geomagnetic_api_resume);

int
geomagnetic_api_suspend(void)
{
	return geomagnetic_api_enable(0);
}
EXPORT_SYMBOL(geomagnetic_api_suspend);

int
geomagnetic_api_read(int *xyz, int *raw, int *xy1y2, int *accuracy)
{
	struct geomagnetic_data *data = i2c_get_clientdata(this_client);
	struct yas_mag_data magdata;
	int i;

	geomagnetic_work(&magdata);
	if (xyz != NULL)
		for (i = 0; i < 3; i++)
			xyz[i] = magdata.xyz.v[i];
	if (raw != NULL)
		for (i = 0; i < 3; i++)
			raw[i] = magdata.raw.v[i];
	if (xy1y2 != NULL)
		for (i = 0; i < 3; i++)
			xy1y2[i] = magdata.xy1y2.v[i];
	if (accuracy != NULL)
		*accuracy = atomic_read(&data->last_status);

	return 0;
}
EXPORT_SYMBOL(geomagnetic_api_read);
#endif

module_init(geomagnetic_init);
module_exit(geomagnetic_term);

MODULE_AUTHOR("Yamaha Corporation");
#if YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS529
MODULE_DESCRIPTION("YAS529 Geomagnetic Sensor Driver");
#elif YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS530
MODULE_DESCRIPTION("YAS530 Geomagnetic Sensor Driver");
#elif YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS532
MODULE_DESCRIPTION("YAS532 Geomagnetic Sensor Driver");
#elif YAS_MAG_DRIVER == YAS_MAG_DRIVER_YAS533
MODULE_DESCRIPTION("YAS533 Geomagnetic Sensor Driver");
#endif
MODULE_LICENSE("GPL");
MODULE_VERSION("4.18.910");
