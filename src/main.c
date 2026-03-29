/* src/main.c: HID Consumer Control (Board Agnostic) */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/services/dis.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "hog.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* GPIO for Buttons & LED using standard aliases */
static const struct gpio_dt_spec sw1 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec sw2 = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
static const struct gpio_dt_spec sw3 = GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios);
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static struct gpio_callback sw1_cb_data;
static struct gpio_callback sw2_cb_data;
static struct gpio_callback sw3_cb_data;

/* Timing constants */
#define LONG_PRESS_THRESHOLD K_SECONDS(3)

/* Consumer Control Usage IDs */
#define CONSUMER_VOL_UP     BIT(0)
#define CONSUMER_VOL_DOWN   BIT(1)
#define CONSUMER_PLAY_PAUSE BIT(2)

static struct bt_conn *current_conn;
static uint8_t pending_report;
static int64_t sw1_press_time;

/* Work items */
struct k_work report_work;
struct k_work unpair_work;
struct k_work_delayable led_success_work;

/* Advertising Data */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* LED Success indication */
static void led_success_handler(struct k_work *work)
{
	gpio_pin_set_dt(&led, 1);
	k_sleep(K_MSEC(1000));
	gpio_pin_set_dt(&led, 0);
}

/* Unpair and reset advertising */
static void unpair_handler(struct k_work *work)
{
	LOG_INF("Long press detected: Clearing all bonds and restarting advertising...");
	
	bt_le_adv_stop();

	if (current_conn) {
		bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		k_sleep(K_MSEC(500));
	}

	bt_unpair(BT_ID_DEFAULT, NULL);

	/* Flash LED to indicate clear */
	for (int i = 0; i < 3; i++) {
		gpio_pin_set_dt(&led, 1);
		k_sleep(K_MSEC(100));
		gpio_pin_set_dt(&led, 0);
		k_sleep(K_MSEC(100));
	}

	k_sleep(K_MSEC(500));
	bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	LOG_INF("Bonds cleared. Ready for fresh pairing with PIN 123456.");
}

/* Report work */
static void report_work_handler(struct k_work *work)
{
	if (!current_conn) {
		return;
	}

	uint8_t report = pending_report;
	LOG_INF("Sending HID Report: 0x%02x", report);
	
	hog_send_report(current_conn, report);
	k_sleep(K_MSEC(20));
	hog_send_report(current_conn, 0);
}

/* Button Handler */
void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	bool pressed = gpio_pin_get_dt(&sw1) > 0;

	if (pins & BIT(sw1.pin)) {
		if (pressed) {
			sw1_press_time = k_uptime_get();
		} else {
			/* Released */
			int64_t duration = k_uptime_get() - sw1_press_time;
			if (duration >= k_ticks_to_ms_near64(LONG_PRESS_THRESHOLD.ticks)) {
				k_work_submit(&unpair_work);
				return;
			}
			
			/* Normal SW1 action (Vol Up) */
			if (current_conn) {
				pending_report = CONSUMER_VOL_UP;
				k_work_submit(&report_work);
			}
		}
	} else if (pins & BIT(sw2.pin)) {
		if (pressed) return;
		if (current_conn) {
			pending_report = CONSUMER_VOL_DOWN;
			k_work_submit(&report_work);
		}
	} else if (pins & BIT(sw3.pin)) {
		if (pressed) return;
		if (current_conn) {
			pending_report = CONSUMER_PLAY_PAUSE;
			k_work_submit(&report_work);
		}
	}
}

/* Bluetooth Connection Management */
static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		LOG_ERR("Connection to %s failed (err %u)", addr, err);
		return;
	}

	LOG_INF("Connected to %s", addr);
	current_conn = bt_conn_ref(conn);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Disconnected from %s (reason %u)", addr, reason);
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	if (!err) {
		LOG_INF("Security level changed: level %u", level);
		if (level >= BT_SECURITY_L3) {
			k_work_schedule(&led_success_work, K_NO_WAIT);
		}
	} else {
		LOG_ERR("Security failed: level %u err %d", level, err);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	LOG_INF("##############################################");
	LOG_INF("# Passkey: %06u", passkey);
	LOG_INF("# PLEASE ENTER THIS PIN ON YOUR PHONE/PC");
	LOG_INF("##############################################");
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	LOG_INF("Pairing complete, bonded: %s", bonded ? "yes" : "no");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	LOG_ERR("Pairing failed (reason %d)", reason);
}

static struct bt_conn_auth_cb auth_cb_display = {
	.passkey_display = auth_passkey_display,
	.passkey_entry = NULL,
	.cancel = NULL,
};

static struct bt_conn_auth_info_cb auth_cb_info = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

static void bt_ready(int err)
{
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return;
	}

	LOG_INF("Bluetooth initialized");

	hog_init();

	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		err = settings_load();
		if (err) {
			LOG_ERR("Settings load failed (err %d)", err);
		} else {
			LOG_INF("Settings loaded successfully");
		}
	}

	/* Set fixed passkey */
	bt_passkey_set(123456);

	bt_bas_set_battery_level(100);

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return;
	}

	LOG_INF("Advertising successfully started");
}

int main(void)
{
	LOG_INF("Starting HID Remote Control (Fixed PIN)...");

	k_work_init(&report_work, report_work_handler);
	k_work_init(&unpair_work, unpair_handler);
	k_work_init_delayable(&led_success_work, led_success_handler);

	/* Init Buttons & LED */
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&sw1, GPIO_INPUT | GPIO_PULL_UP);
	gpio_pin_configure_dt(&sw2, GPIO_INPUT | GPIO_PULL_UP);
	gpio_pin_configure_dt(&sw3, GPIO_INPUT | GPIO_PULL_UP);

	gpio_pin_interrupt_configure_dt(&sw1, GPIO_INT_EDGE_BOTH);
	gpio_pin_interrupt_configure_dt(&sw2, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_pin_interrupt_configure_dt(&sw3, GPIO_INT_EDGE_TO_ACTIVE);

	gpio_init_callback(&sw1_cb_data, button_pressed, BIT(sw1.pin));
	gpio_init_callback(&sw2_cb_data, button_pressed, BIT(sw2.pin));
	gpio_init_callback(&sw3_cb_data, button_pressed, BIT(sw3.pin));

	gpio_add_callback(sw1.port, &sw1_cb_data);
	gpio_add_callback(sw2.port, &sw2_cb_data);
	gpio_add_callback(sw3.port, &sw3_cb_data);

	/* Initialize Settings Subsystem */
	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_subsys_init();
	}

	bt_conn_auth_cb_register(&auth_cb_display);
	bt_conn_auth_info_cb_register(&auth_cb_info);

	/* Initialize Bluetooth */
	bt_enable(bt_ready);

	return 0;
}
