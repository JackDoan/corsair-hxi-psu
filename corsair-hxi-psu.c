// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * corsair-cpro.c - Linux driver for Corsair HX 850i
 * Copyright (C) 2020 Jack Doan <me@jackdoan.com>
 * Copyright (C) 2020 Marius Zachmann <mail@mariuszachmann.de>
 * Copyright (C) 2017-2019  Sean Nelson <audiohacked@gmail.com>
 *
 * Supported devices:
 * Corsair HXi ATX power supplies (HX750i, HX850i, HX1000i and HX1200i)
 *
 * This driver uses hid reports to communicate with the device to allow hidraw
 * userspace drivers still being used. The device does not use report ids.
 * When using hidraw and this driver simultaneously, reports could be switched.
 *
 * Broadly speaking, this power supply communicates with a sort of
 * PMBUS-over-USBHID protocol.
 *
 * Support has been added for reading:
 *      * both temperature sensors
 *      * voltage, current, and power for 12V, 5V, 3.3V rails
 *      * input (wall) voltage
 *      * total input (wall) power
 * Supposedly, the PSU supports the following functionality that is not yet
 * supported by this driver:
 *      * changing the fan control mode from automatic to manual
 *      * setting fan speed
 *      * reading/writing different overcurrent-protection modes
 */

#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/types.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jack Doan <me@jackdoan.com>");

#define USB_VENDOR_ID_CORSAIR            0x1b1c
#define USB_PRODUCT_ID_CORSAIR_HX750i    0x1c05
#define USB_PRODUCT_ID_CORSAIR_HX850i    0x1c06
#define USB_PRODUCT_ID_CORSAIR_HX1000i    0x1c07
#define USB_PRODUCT_ID_CORSAIR_HX1200i    0x1c08

#define OUT_BUFFER_SIZE 63
#define IN_BUFFER_SIZE 16
#define LABEL_LENGTH 8
#define REQ_TIMEOUT 300
#define NUM_RAILS 4

enum hxi_sensor_id {
	SENSOR_12V = 0x0,
	SENSOR_5V = 0x1,
	SENSOR_3V = 0x2,
	UNSWITCHED = 0xFE // do not send a sensor switch msg
};

enum hxi_sensor_cmd {
	SIG_VOLTS = 0x8B,
	SIG_WALL_VOLTS = 0x88,
	SIG_AMPS = 0x8C,
	SIG_TEMPERATURE_1 = 0x8D,
	SIG_TEMPERATURE_2 = 0x8E,
	SIG_WATTS = 0x96,
	SIG_TOTAL_WATTS = 0xEE,
};

struct hxi_rail {
	enum hxi_sensor_id sensor;
	enum hxi_sensor_cmd volt_cmd;
	enum hxi_sensor_cmd amp_cmd;
	enum hxi_sensor_cmd power_cmd;
	char label[LABEL_LENGTH];
};

struct hxi_device {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct completion wait_input_report;
	struct mutex mutex; /* whenever buffer is used, lock before send_usb_cmd */
	u8 *buffer;
	struct hxi_rail rails[NUM_RAILS];
};

/* send command, check for error in response, response in hxi->buffer */
static int send_usb_cmd(struct hxi_device *hxi, u8 command, u8 b1, u8 b2)
{
	unsigned long t;
	int ret;

	memset(hxi->buffer, 0x00, OUT_BUFFER_SIZE);
	hxi->buffer[0] = command;
	hxi->buffer[1] = b1;
	hxi->buffer[2] = b2;

	reinit_completion(&hxi->wait_input_report);

	ret = hid_hw_output_report(hxi->hdev, hxi->buffer, OUT_BUFFER_SIZE);
	if (ret < 0) {
		goto exit;
	} else {
		ret = 0;
	}
	t = wait_for_completion_timeout(&hxi->wait_input_report, msecs_to_jiffies(REQ_TIMEOUT));
	if (!t) {
		ret = -ETIMEDOUT;
	}

	exit:
	return ret;
}

static int hxi_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
	struct hxi_device *hxi = hid_get_drvdata(hdev);

	/* only copy buffer when requested */
	if (completion_done(&hxi->wait_input_report)) {
		goto exit;
	}

	memcpy(hxi->buffer, data, min(IN_BUFFER_SIZE, size));
	complete(&hxi->wait_input_report);

	exit:
	return 0;
}

/*
 * Gets one of the two temperature sensors in the PSU
 * This code is different enough from the other sensors to justify pulling it out into it's own
 * function to improve readability.
 */
static int get_temperature(struct hxi_device *hxi, int channel)
{
	int ret;
	u8 cmd = SIG_TEMPERATURE_1;
	if (channel == 1) {
		cmd = SIG_TEMPERATURE_2;
	}

	mutex_lock(&hxi->mutex);
	ret = send_usb_cmd(hxi, 0x03, cmd, 0);
	if (ret) {
		ret = -ENODATA;
	} else {
		ret = (hxi->buffer[2] << 8) + hxi->buffer[3];
	}
	mutex_unlock(&hxi->mutex);

	return ret;
}

/*
 * The PSU reports voltage/current/power measurements in this 16-bit
 * floating-point format as described in the
 * PMBUS spec, v1.2, Part II, section 8.3.1
 *
 * There is already code for this in the PMBus section of hwmon, but I didn't
 * want this module to depend on any of the PMBus components because PMBus is
 * so heavily tied in with I2C, and this is a USBHID driver.
 */
static int decode_corsair_float(u16 input)
{
	int ret;
	int exponent = input >> 11;
	int fraction = input & 2047;

	if (exponent > 15) {
		exponent = -(32 - exponent);
	}
	if (fraction > 1023) {
		fraction = -(2048 - fraction);
	}
	if (fraction & 1) {
		fraction++;
	}
	/* scale to milli-units */
	fraction = fraction * 1000;
	if (exponent < 0) {
		ret = fraction >> ((~exponent) + 1);
	} else {
		ret = fraction << exponent;
	}
	return ret;
}

/*
 * Requests and returns single data values depending on channel.
 * To maintain PMBUS-like behavior, the PSU switches "channels" when taking
 * measurements from the 12V/5V/3.3V busses. Other sensors that have standard
 * PMBUS commands are "unswitched"
 */
static int get_data(struct hxi_device *hxi, enum hxi_sensor_id sensor, enum hxi_sensor_cmd sig)
{
	int ret;
	mutex_lock(&hxi->mutex);
	switch (sensor) {
	case SENSOR_12V:
	case SENSOR_5V:
	case SENSOR_3V:
		ret = send_usb_cmd(hxi, 0x2, 0x0, sensor);
		break;
	case UNSWITCHED:
		ret = 0;
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	if (ret) {
		goto out_unlock;
	}
	switch (sig) {
	case SIG_VOLTS:
	case SIG_AMPS:
	case SIG_WATTS:
	case SIG_TOTAL_WATTS:
	case SIG_WALL_VOLTS:
		ret = send_usb_cmd(hxi, 0x3, sig, 0);
		break;
	default:
		ret = -1;
		break;
	}
	if (ret) {
		goto out_unlock;
	}
	/*
	 * Note that this is different byte order from temperature.
	 * Thanks, PMBus.
	 */
	ret = (hxi->buffer[3] << 8) + hxi->buffer[2];

	out_unlock:
	mutex_unlock(&hxi->mutex);
	return decode_corsair_float((u16) ret);
}

static int hxi_read_string(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, const char **str)
{
	int ret;
	struct hxi_device *hxi = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_in:
	case hwmon_curr:
	case hwmon_power:
		switch (attr) {
		case hwmon_in_label: /* this includes current as well */
		case hwmon_power_label:
			*str = hxi->rails[channel].label;
			ret = 0;
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

static int hxi_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int chan, long *val)
{
	struct hxi_device *hxi = dev_get_drvdata(dev);
	int ret = -EOPNOTSUPP;
	int data;

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			data = get_temperature(hxi, chan);
			if (data < 0) {
				ret = -ENODATA;
				goto exit;
			}
			*val = data;
			ret = 0;
			break;
		default:
			break;
		}
		break;
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			data = get_data(hxi, hxi->rails[chan].sensor, hxi->rails[chan].volt_cmd);
			if (data < 0) {
				ret = -ENODATA;
				goto exit;
			}
			*val = data;
			ret = 0;
			break;
		default:
			break;
		}
		break;
	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input:
			data = get_data(hxi, hxi->rails[chan].sensor, hxi->rails[chan].amp_cmd);
			if (data < 0) {
				ret = -ENODATA;
				goto exit;
			}
			*val = data;
			ret = 0;
			break;
		default:
			break;
		}
		break;
	case hwmon_power:
		switch (attr) {
		case hwmon_power_input:
			data = get_data(hxi, hxi->rails[chan].sensor, hxi->rails[chan].power_cmd);
			if (data < 0) {
				ret = -ENODATA;
				goto exit;
			}
			/*power is reported in uW, more scaling needed */
			*val = data * 1000;
			ret = 0;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	exit:
	return ret;
}

static int hxi_write(struct device *dev, enum hwmon_sensor_types type,
		     u32 attr, int channel, long val)
{
	return -EOPNOTSUPP;
}

static umode_t hxi_is_visible(const void *data, enum hwmon_sensor_types type, u32 attr, int channel)
{
	return 0444;
}

static const struct hwmon_ops hxi_hwmon_ops = {
	.is_visible = hxi_is_visible,
	.read = hxi_read,
	.read_string = hxi_read_string,
	.write = hxi_write,
};

static const struct hwmon_channel_info *hxi_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT,
			   HWMON_T_INPUT
	),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL
	),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL
	),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL
	),
	NULL
};

static const struct hwmon_chip_info hxi_chip_info = {
	.ops = &hxi_hwmon_ops,
	.info = hxi_info,
};

static int hxi_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct hxi_device *hxi;
	int ret;
	int i;

	hxi = devm_kzalloc(&hdev->dev, sizeof(*hxi), GFP_KERNEL);
	if (!hxi) {
		ret = -ENOMEM;
		goto exit;
	}

	hxi->buffer = devm_kmalloc(&hdev->dev, OUT_BUFFER_SIZE, GFP_KERNEL);
	if (!hxi->buffer) {
		ret = -ENOMEM;
		goto exit;
	}

	for (i = 0; i < NUM_RAILS - 1; i++) {
		hxi->rails[i].volt_cmd = SIG_VOLTS;
		hxi->rails[i].amp_cmd = SIG_AMPS;
		hxi->rails[i].power_cmd = SIG_WATTS;
	}

	hxi->rails[0].sensor = SENSOR_12V;
	hxi->rails[1].sensor = SENSOR_5V;
	hxi->rails[2].sensor = SENSOR_3V;
	hxi->rails[3].sensor = UNSWITCHED;
	hxi->rails[3].volt_cmd = SIG_WALL_VOLTS;
	hxi->rails[3].power_cmd = SIG_TOTAL_WATTS;

	strncpy(hxi->rails[0].label, "12V", LABEL_LENGTH);
	strncpy(hxi->rails[1].label, "5V", LABEL_LENGTH);
	strncpy(hxi->rails[2].label, "3V", LABEL_LENGTH);
	strncpy(hxi->rails[3].label, "Wall", LABEL_LENGTH);

	ret = hid_parse(hdev);
	if (ret) {
		goto exit;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		goto exit;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		goto out_hw_stop;
	}

	hxi->hdev = hdev;
	hid_set_drvdata(hdev, hxi);
	mutex_init(&hxi->mutex);
	init_completion(&hxi->wait_input_report);

	hid_device_io_start(hdev);

	hxi->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, "hxipsu",
							 hxi, &hxi_chip_info, 0);
	if (IS_ERR(hxi->hwmon_dev)) {
		ret = (int) PTR_ERR(hxi->hwmon_dev);
		goto out_hw_close;
	}

	ret = 0;
	goto exit;

	out_hw_close:
	hid_hw_close(hdev);
	out_hw_stop:
	hid_hw_stop(hdev);
	exit:
	return ret;
}

static void hxi_remove(struct hid_device *hdev)
{
	struct hxi_device *hxi = hid_get_drvdata(hdev);
	hwmon_device_unregister(hxi->hwmon_dev);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id hxi_devices[] = {
	{HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_PRODUCT_ID_CORSAIR_HX750i)},
	{HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_PRODUCT_ID_CORSAIR_HX850i)},
	{HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_PRODUCT_ID_CORSAIR_HX1000i)},
	{HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_PRODUCT_ID_CORSAIR_HX1200i)},
	{}
};

static struct hid_driver hxi_driver = {
	.name = "corsair-hxi",
	.id_table = hxi_devices,
	.probe = hxi_probe,
	.remove = hxi_remove,
	.raw_event = hxi_raw_event,
};

MODULE_DEVICE_TABLE(hid, hxi_devices);

static int __init hxi_init(void)
{
	return hid_register_driver(&hxi_driver);
}

static void __exit hxi_exit(void)
{
	hid_unregister_driver(&hxi_driver);
}

/*
 * When compiling this driver as built-in, hwmon initcalls will get called
 * before the hid driver and this driver would fail to register.
 * late_initcall solves this.
 */
late_initcall(hxi_init);

module_exit(hxi_exit);
