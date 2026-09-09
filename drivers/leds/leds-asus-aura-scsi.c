// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ASUS Aura RGB over SCSI for ROG external NVMe enclosures
 * (e.g. ROG STRIX Arion, USB 0b05:1932).
 *
 * USB mass-storage device, no HID; the ENE LED controller is driven via
 * vendor SCSI commands. Matched by INQUIRY (vendor "ROG", model "ESD-S1C")
 * through the SCSI class interface so sd keeps owning the disk.
 *
 * Exposes a Dynamic Lighting class device (asus-aura-scsi-<H_C_T_L>:rgb:indicator),
 * providing hardware effect offload (Off, Static, Breathing, Strobe,
 * Spectrum Cycle, Rainbow, Direct streaming), speed, direction, palette,
 * and direct buffer streaming.
 */

#include <linux/bits.h>
#include <linux/blk-mq.h>
#include <linux/blk_types.h>
#include <linux/blkdev.h>
#include <linux/led-dynamic-lighting.h>
#include <linux/leds.h>
#include <linux/list.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_driver.h>

#define ASUS_SCSI_INQ_VENDOR		"ROG"
#define ASUS_SCSI_INQ_MODEL		"ESD-S1C"
#define ASUS_AURA_USB_VID		0x0b05
#define ASUS_AURA_USB_PID		0x1932

#define ENE_OPCODE			0xec
#define ENE_REG_MODE			0x8021
#define ENE_REG_SPEED			0x8022
#define ENE_REG_DIRECTION		0x8023
#define ENE_REG_APPLY			0x80a0
#define ENE_REG_COLORS			0x8160
#define ENE_REG_COLORS_DIRECT		0x8100
#define ENE_APPLY			0x01
#define ENE_SAVE			0xaa
#define ENE_CDB_LEN			16
#define ENE_TIMEOUT			(10 * HZ)

#define ASUS_AURA_SCSI_NUM_LEDS		4
#define ASUS_AURA_SCSI_COLOR_LEN	3
#define ASUS_AURA_SCSI_DIRECT_BUF_SIZE	(ASUS_AURA_SCSI_NUM_LEDS * ASUS_AURA_SCSI_COLOR_LEN)

struct asus_aura_zone {
	struct list_head		list;
	struct device			*class_dev;
	struct scsi_device		*sdev;
	struct led_classdev_dynamic	dldev;
	u8				colors[ASUS_AURA_SCSI_NUM_LEDS][ASUS_AURA_SCSI_COLOR_LEN];
	u8				current_mode;
	u8				current_speed;
	u8				current_direction;
};

static LIST_HEAD(asus_aura_list);
static DEFINE_MUTEX(asus_aura_list_lock);

static const struct usb_device_id asus_aura_usb_ids[] = {
	{
		.match_flags = USB_DEVICE_ID_MATCH_VENDOR |
			       USB_DEVICE_ID_MATCH_PRODUCT,
		.idVendor = ASUS_AURA_USB_VID,
		.idProduct = ASUS_AURA_USB_PID,
	},
	{ }
};
MODULE_DEVICE_TABLE(usb, asus_aura_usb_ids);

static void ene_build_cdb(u8 *cdb, u16 reg, u8 arg_count)
{
	memset(cdb, 0, ENE_CDB_LEN);
	cdb[0] = ENE_OPCODE;
	cdb[1] = 'A';
	cdb[2] = 'S';
	cdb[3] = (reg >> 8) & 0xff;
	cdb[4] = reg & 0xff;
	cdb[13] = arg_count;
}

static int ene_write(struct scsi_device *sdev, u16 reg,
		     const void *data, u8 arg_count)
{
	struct request *rq;
	struct scsi_cmnd *scmd;
	u8 cdb[ENE_CDB_LEN];
	int ret;

	ene_build_cdb(cdb, reg, arg_count);

	rq = scsi_alloc_request(sdev->request_queue, REQ_OP_DRV_OUT, 0);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	if (arg_count) {
		ret = blk_rq_map_kern(rq, (void *)data, arg_count, GFP_NOIO);
		if (ret)
			goto out;
	}

	scmd = blk_mq_rq_to_pdu(rq);
	scmd->cmd_len = ENE_CDB_LEN;
	memcpy(scmd->cmnd, cdb, ENE_CDB_LEN);
	scmd->allowed = 1;
	rq->timeout = ENE_TIMEOUT;
	rq->rq_flags |= RQF_QUIET;

	blk_execute_rq(rq, true);
	ret = scmd->result ? -EIO : 0;
out:
	blk_mq_free_request(rq);
	return ret;
}

static int asus_aura_sync_hardware(struct asus_aura_zone *zone, bool save_flash)
{
	struct scsi_device *sdev = zone->sdev;
	u8 colors[ASUS_AURA_SCSI_DIRECT_BUF_SIZE];
	u8 mode = zone->current_mode;
	u8 speed = zone->current_speed;
	u8 dir = zone->current_direction;
	u8 apply = ENE_APPLY;
	u8 save = ENE_SAVE;
	int i, ret;

	if (!scsi_device_online(sdev))
		return -ENODEV;

	/* Mode must be written first or hardware ignores sequence */
	ret = ene_write(sdev, ENE_REG_MODE, &mode, 1);
	if (ret)
		return ret;

	/* Convert RGB to ENE wire order: R, B, G */
	for (i = 0; i < ASUS_AURA_SCSI_NUM_LEDS; i++) {
		colors[i * 3 + 0] = zone->colors[i][0];
		colors[i * 3 + 1] = zone->colors[i][2];
		colors[i * 3 + 2] = zone->colors[i][1];
	}

	ret = ene_write(sdev, ENE_REG_COLORS, colors, sizeof(colors));
	if (ret)
		return ret;

	ret = ene_write(sdev, ENE_REG_COLORS_DIRECT, colors, sizeof(colors));
	if (ret)
		return ret;

	ret = ene_write(sdev, ENE_REG_SPEED, &speed, 1);
	if (ret)
		return ret;

	ret = ene_write(sdev, ENE_REG_DIRECTION, &dir, 1);
	if (ret)
		return ret;

	ret = ene_write(sdev, ENE_REG_APPLY, &apply, 1);
	if (ret)
		return ret;

	if (save_flash) {
		ret = ene_write(sdev, ENE_REG_APPLY, &save, 1);
		if (ret)
			return ret;
	}

	return 0;
}

static int asus_aura_set_effect(struct led_classdev_dynamic *ldev,
				enum dl_effect_mode mode)
{
	struct asus_aura_zone *zone = ldev->driver_data;
	u8 hw_mode;

	switch (mode) {
	case DL_EFFECT_OFF:
		hw_mode = 0;
		break;
	case DL_EFFECT_STATIC:
	case DL_EFFECT_DIRECT:
		hw_mode = 1;
		break;
	case DL_EFFECT_BREATHING:
		hw_mode = 2;
		break;
	case DL_EFFECT_STROBE:
		hw_mode = 3;
		break;
	case DL_EFFECT_SPECTRUM_CYCLE:
		hw_mode = 4;
		break;
	case DL_EFFECT_RAINBOW:
		hw_mode = 5;
		break;
	default:
		return -EINVAL;
	}

	zone->current_mode = hw_mode;
	/* Persist the selected effect; speed/palette/direct stay RAM-only. */
	return asus_aura_sync_hardware(zone, true);
}

static int asus_aura_set_speed(struct led_classdev_dynamic *ldev,
			       unsigned int speed)
{
	struct asus_aura_zone *zone = ldev->driver_data;

	if (speed > 4)
		return -EINVAL;

	zone->current_speed = speed;
	return asus_aura_sync_hardware(zone, false);
}

static int asus_aura_set_direction(struct led_classdev_dynamic *ldev,
				   enum dl_direction direction)
{
	struct asus_aura_zone *zone = ldev->driver_data;

	switch (direction) {
	case DL_DIRECTION_RIGHT:
		zone->current_direction = 0;
		break;
	case DL_DIRECTION_LEFT:
		zone->current_direction = 1;
		break;
	default:
		return -EINVAL;
	}

	return asus_aura_sync_hardware(zone, false);
}

static int asus_aura_set_palette(struct led_classdev_dynamic *ldev,
				 const struct dl_rgb *palette,
				 unsigned int num_entries)
{
	struct asus_aura_zone *zone = ldev->driver_data;
	unsigned int i;

	if (!palette || num_entries == 0)
		return -EINVAL;

	for (i = 0; i < ASUS_AURA_SCSI_NUM_LEDS; i++) {
		const struct dl_rgb *c = &palette[min_t(unsigned int, i, num_entries - 1)];

		zone->colors[i][0] = c->r;
		zone->colors[i][1] = c->g;
		zone->colors[i][2] = c->b;
	}

	return asus_aura_sync_hardware(zone, false);
}

static int asus_aura_direct_write(struct led_classdev_dynamic *ldev,
				  const u8 *buffer, size_t size)
{
	struct asus_aura_zone *zone = ldev->driver_data;
	struct scsi_device *sdev = zone->sdev;
	u8 colors[ASUS_AURA_SCSI_DIRECT_BUF_SIZE];
	u8 mode = 1;
	u8 apply = ENE_APPLY;
	int i, ret;

	if (size != ASUS_AURA_SCSI_DIRECT_BUF_SIZE)
		return -EINVAL;

	if (!scsi_device_online(sdev))
		return -ENODEV;

	for (i = 0; i < ASUS_AURA_SCSI_NUM_LEDS; i++) {
		u8 r = buffer[i * 3 + 0];
		u8 g = buffer[i * 3 + 1];
		u8 b = buffer[i * 3 + 2];

		zone->colors[i][0] = r;
		zone->colors[i][1] = g;
		zone->colors[i][2] = b;

		colors[i * 3 + 0] = r;
		colors[i * 3 + 1] = b;
		colors[i * 3 + 2] = g;
	}

	ret = ene_write(sdev, ENE_REG_MODE, &mode, 1);
	if (ret)
		return ret;

	ret = ene_write(sdev, ENE_REG_COLORS, colors, sizeof(colors));
	if (ret)
		return ret;

	ret = ene_write(sdev, ENE_REG_COLORS_DIRECT, colors, sizeof(colors));
	if (ret)
		return ret;

	/* Apply to RAM only without wearing flash during streaming */
	return ene_write(sdev, ENE_REG_APPLY, &apply, 1);
}

static int asus_aura_brightness_set_blocking(struct led_classdev *cdev,
					     enum led_brightness brightness)
{
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);
	struct asus_aura_zone *zone = ldev->driver_data;

	guard(mutex)(&ldev->lock);

	if (brightness == LED_OFF) {
		u8 mode = 0;
		u8 apply = ENE_APPLY;
		int ret;

		ret = ene_write(zone->sdev, ENE_REG_MODE, &mode, 1);
		if (ret)
			return ret;
		return ene_write(zone->sdev, ENE_REG_APPLY, &apply, 1);
	}

	return asus_aura_sync_hardware(zone, false);
}

static const struct led_dynamic_ops asus_aura_dynamic_ops = {
	.set_effect = asus_aura_set_effect,
	.set_speed = asus_aura_set_speed,
	.set_direction = asus_aura_set_direction,
	.set_palette = asus_aura_set_palette,
	.direct_write = asus_aura_direct_write,
};

static bool asus_aura_sdev_match(struct scsi_device *sdev)
{
	return !strncmp(sdev->vendor, ASUS_SCSI_INQ_VENDOR,
			strlen(ASUS_SCSI_INQ_VENDOR)) &&
	       !strncmp(sdev->model, ASUS_SCSI_INQ_MODEL,
			strlen(ASUS_SCSI_INQ_MODEL));
}

static int asus_aura_add(struct device *dev)
{
	struct scsi_device *sdev = to_scsi_device(dev->parent);
	struct asus_aura_zone *zone;
	char hctl[32];
	int ret;

	if (!asus_aura_sdev_match(sdev))
		return 0;

	ret = scsi_device_get(sdev);
	if (ret)
		return ret;

	zone = kzalloc_obj(*zone, GFP_KERNEL);
	if (!zone) {
		scsi_device_put(sdev);
		return -ENOMEM;
	}

	zone->class_dev = dev;
	zone->sdev = sdev;
	zone->current_mode = 1;
	zone->current_speed = 2;
	zone->current_direction = 0;

	/* Default ROG red #a60000 */
	zone->colors[0][0] = 166;
	zone->colors[0][1] = 0;
	zone->colors[0][2] = 0;

	zone->colors[1][0] = 0;
	zone->colors[1][1] = 0;
	zone->colors[1][2] = 0;

	zone->colors[2][0] = 166;
	zone->colors[2][1] = 0;
	zone->colors[2][2] = 0;

	zone->colors[3][0] = 0;
	zone->colors[3][1] = 0;
	zone->colors[3][2] = 0;

	strscpy(hctl, dev_name(&sdev->sdev_gendev), sizeof(hctl));
	strreplace(hctl, ':', '_');

	zone->dldev.cdev.name = kasprintf(GFP_KERNEL, "asus-aura-scsi-%s:rgb:indicator", hctl);
	if (!zone->dldev.cdev.name) {
		scsi_device_put(sdev);
		kfree(zone);
		return -ENOMEM;
	}

	zone->dldev.cdev.max_brightness = 255;
	zone->dldev.cdev.brightness = 255;
	zone->dldev.cdev.brightness_set_blocking = asus_aura_brightness_set_blocking;

	zone->dldev.ops = &asus_aura_dynamic_ops;
	zone->dldev.driver_data = zone;
	zone->dldev.zone_type = "segment_strip";
	zone->dldev.led_count = ASUS_AURA_SCSI_NUM_LEDS;
	zone->dldev.max_speed = 4;
	zone->dldev.speed = 2;
	zone->dldev.direction = DL_DIRECTION_RIGHT;
	zone->dldev.supported_directions = BIT(DL_DIRECTION_RIGHT) | BIT(DL_DIRECTION_LEFT);
	zone->dldev.max_palette_entries = ASUS_AURA_SCSI_NUM_LEDS;
	zone->dldev.supported_effects = BIT(DL_EFFECT_OFF) |
					BIT(DL_EFFECT_STATIC) |
					BIT(DL_EFFECT_BREATHING) |
					BIT(DL_EFFECT_STROBE) |
					BIT(DL_EFFECT_SPECTRUM_CYCLE) |
					BIT(DL_EFFECT_RAINBOW) |
					BIT(DL_EFFECT_DIRECT);
	zone->dldev.current_effect = DL_EFFECT_STATIC;

	mutex_lock(&asus_aura_list_lock);
	list_add(&zone->list, &asus_aura_list);
	mutex_unlock(&asus_aura_list_lock);

	ret = led_classdev_dynamic_register(&sdev->sdev_gendev, &zone->dldev);
	if (ret) {
		mutex_lock(&asus_aura_list_lock);
		list_del(&zone->list);
		mutex_unlock(&asus_aura_list_lock);
		kfree(zone->dldev.cdev.name);
		scsi_device_put(sdev);
		kfree(zone);
		return ret;
	}

	if (zone->dldev.palette) {
		zone->dldev.palette[0].r = 166;
		zone->dldev.palette[0].g = 0;
		zone->dldev.palette[0].b = 0;

		zone->dldev.palette[1].r = 0;
		zone->dldev.palette[1].g = 0;
		zone->dldev.palette[1].b = 0;

		zone->dldev.palette[2].r = 166;
		zone->dldev.palette[2].g = 0;
		zone->dldev.palette[2].b = 0;

		zone->dldev.palette[3].r = 0;
		zone->dldev.palette[3].g = 0;
		zone->dldev.palette[3].b = 0;

		zone->dldev.num_palette_entries = ASUS_AURA_SCSI_NUM_LEDS;
	}

	return 0;
}

static void asus_aura_remove(struct device *dev)
{
	struct asus_aura_zone *zone = NULL, *tmp;

	mutex_lock(&asus_aura_list_lock);
	list_for_each_entry(tmp, &asus_aura_list, list) {
		if (tmp->class_dev == dev) {
			list_del(&tmp->list);
			zone = tmp;
			break;
		}
	}
	mutex_unlock(&asus_aura_list_lock);

	if (!zone)
		return;

	led_classdev_dynamic_unregister(&zone->dldev);
	kfree(zone->dldev.cdev.name);
	scsi_device_put(zone->sdev);
	kfree(zone);
}

static struct class_interface asus_aura_interface = {
	.add_dev	= asus_aura_add,
	.remove_dev	= asus_aura_remove,
};

static int __init asus_aura_init(void)
{
	return scsi_register_interface(&asus_aura_interface);
}

static void __exit asus_aura_exit(void)
{
	scsi_unregister_interface(&asus_aura_interface);
}

module_init(asus_aura_init);
module_exit(asus_aura_exit);

MODULE_DESCRIPTION("ASUS Aura RGB Dynamic Lighting driver for ROG NVMe enclosures");
MODULE_AUTHOR("Liang Haowen <nbg2974@gmail.com>");
MODULE_AUTHOR("Marco Scardovi <scardracs@disroot.org>");
MODULE_LICENSE("GPL");
