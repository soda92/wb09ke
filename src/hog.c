/** @file
 *  @brief HoG Service (Consumer Control)
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include "hog.h"

enum {
	HIDS_REMOTE_WAKE = BIT(0),
	HIDS_NORMALLY_CONNECTABLE = BIT(1),
};

struct hids_info {
	uint16_t version; /* version number of base USB HID Specification */
	uint8_t code; /* country HID Device hardware is localized for. */
	uint8_t flags;
} __packed;

struct hids_report {
	uint8_t id; /* report id */
	uint8_t type; /* report type */
} __packed;

static struct hids_info info = {
	.version = 0x0111,
	.code = 0x00,
	.flags = HIDS_NORMALLY_CONNECTABLE,
};

enum {
	HIDS_INPUT = 0x01,
	HIDS_OUTPUT = 0x02,
	HIDS_FEATURE = 0x03,
};

static struct hids_report input_report = {
	.id = 0x01,
	.type = HIDS_INPUT,
};

static uint8_t report_val;
static uint8_t ctrl_point;

/* Consumer Control Report Map */
static const uint8_t report_map[] = {
	0x05, 0x0c, /* Usage Page (Consumer Devices) */
	0x09, 0x01, /* Usage (Consumer Control) */
	0xa1, 0x01, /* Collection (Application) */
	0x85, 0x01, /* Report ID (1) */
	0x15, 0x00, /* Logical Minimum (0) */
	0x25, 0x01, /* Logical Maximum (1) */
	0x75, 0x01, /* Report Size (1) */
	0x95, 0x03, /* Report Count (3) */
	0x09, 0xe9, /* Usage (Volume Up) */
	0x09, 0xea, /* Usage (Volume Down) */
	0x09, 0xcd, /* Usage (Play/Pause) */
	0x81, 0x02, /* Input (Data, Variable, Absolute) */
	0x95, 0x05, /* Report Count (5) - Padding */
	0x81, 0x01, /* Input (Constant) */
	0xc0        /* End Collection */
};

static ssize_t read_info(struct bt_conn *conn,
			  const struct bt_gatt_attr *attr, void *buf,
			  uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
				 sizeof(struct hids_info));
}

static ssize_t read_report_map(struct bt_conn *conn,
				const struct bt_gatt_attr *attr, void *buf,
				uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, report_map,
				 sizeof(report_map));
}

static ssize_t read_report(struct bt_conn *conn,
			    const struct bt_gatt_attr *attr, void *buf,
			    uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
				 sizeof(uint8_t));
}

static void input_report_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	printk("Notifications %sabled\n", value == BT_GATT_CCC_NOTIFY ? "en" : "dis");
}

static ssize_t read_report_ref(struct bt_conn *conn,
				const struct bt_gatt_attr *attr, void *buf,
				uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
				 sizeof(struct hids_report));
}

static ssize_t write_ctrl_point(struct bt_conn *conn,
				 const struct bt_gatt_attr *attr,
				 const void *buf, uint16_t len, uint16_t offset,
				 uint8_t flags)
{
	uint8_t *value = attr->user_data;

	if (offset + len > sizeof(uint8_t)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	memcpy(value + offset, buf, len);

	return len;
}

/* HID Service Declaration */
BT_GATT_SERVICE_DEFINE(hog_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_info, NULL, &info),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_report_map, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ_ENCRYPT,
			       read_report, NULL, &report_val),
	BT_GATT_CCC(input_report_ccc_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
	BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ,
			   read_report_ref, NULL, &input_report),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT,
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, write_ctrl_point,
			       &ctrl_point),
);

void hog_init(void)
{
}

/* We use attribute index 6 (the value characteristic of the Report) for notifications */
void hog_send_report(struct bt_conn *conn, uint8_t report)
{
	bt_gatt_notify(conn, &hog_svc.attrs[6], &report, sizeof(report));
	k_sleep(K_MSEC(20));
	report = 0;
	bt_gatt_notify(conn, &hog_svc.attrs[6], &report, sizeof(report));
}
