/* src/main.c: HID Consumer Control for WB09 (Just Works / Pairing Mode) */
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

/* GPIO for Buttons & LED */
static const struct gpio_dt_spec sw1 = GPIO_DT_SPEC_GET(DT_NODELABEL(user_button_1), gpios);
static const struct gpio_dt_spec sw2 = GPIO_DT_SPEC_GET(DT_NODELABEL(user_button_2), gpios);
static const struct gpio_dt_spec sw3 = GPIO_DT_SPEC_GET(DT_NODELABEL(user_button_3), gpios);
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static struct gpio_callback sw1_cb_data;
static struct gpio_callback sw2_cb_data;
static struct gpio_callback sw3_cb_data;

/* Timing constants */
#define PAIRING_MODE_DURATION K_MINUTES(2)
#define LONG_PRESS_THRESHOLD K_SECONDS(3)
#define BLINK_INTERVAL K_MSEC(300)

/* Consumer Control Usage IDs */
#define CONSUMER_VOL_UP     BIT(0)
#define CONSUMER_VOL_DOWN   BIT(1)
#define CONSUMER_PLAY_PAUSE BIT(2)

static struct bt_conn *current_conn;
static uint8_t pending_report;
static bool is_pairing_mode = false;
static int64_t sw1_press_time;

/* Work items */
struct k_work report_work;
struct k_work pairing_start_work;
struct k_work_delayable pairing_timeout_work;
struct k_work_delayable blink_work;
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

/* LED Blinking & Success indication */
static void blink_work_handler(struct k_work *work)
{
	if (!is_pairing_mode) {
		gpio_pin_set_dt(&led, 0);
		return;
	}

	gpio_pin_toggle_dt(&led);
	k_work_schedule(&blink_work, BLINK_INTERVAL);
}

static void led_success_handler(struct k_work *work)
{
	gpio_pin_set_dt(&led, 1);
	k_sleep(K_MSEC(1000));
	gpio_pin_set_dt(&led, 0);
}

/* Pairing Mode Control */
static void pairing_start_handler(struct k_work *work)
{
	LOG_INF("Entering Pairing Mode (Just Works)...");
	
	/* Stop any existing advertising */
	bt_le_adv_stop();

	/* Enable bonding for new devices */
	bt_set_bondable(true);

	/* Start fast advertising */
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return;
	}

	is_pairing_mode = true;
	k_work_schedule(&blink_work, K_NO_WAIT);
	k_work_schedule(&pairing_timeout_work, PAIRING_MODE_DURATION);
}

static void stop_pairing_mode(struct k_work *work)
{
	if (!is_pairing_mode) {
		return;
	}

	LOG_INF("Pairing Mode Timed Out.");
	is_pairing_mode = false;
	bt_set_bondable(false);
	
	if (!current_conn) {
		bt_le_adv_stop();
	}
	
	gpio_pin_set_dt(&led, 0);
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

/* Button Handler with Long Press Detection */
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
				k_work_submit(&pairing_start_work);
				return;
			}
			
			/* Normal SW1 action (Vol Up) if not long pressed */
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

	/* Stop pairing mode on successful connection */
	if (is_pairing_mode) {
		is_pairing_mode = false;
		k_work_cancel_delayable(&pairing_timeout_work);
		k_work_cancel_delayable(&blink_work);
		k_work_schedule(&led_success_work, K_NO_WAIT);
	}
	
	/* Trigger security after connection */
	bt_conn_set_security(conn, BT_SECURITY_L2);
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
	
	if (!is_pairing_mode) {
		bt_le_adv_stop();
	}
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	if (!err) {
		LOG_INF("Security level changed: level %u", level);
	} else {
		LOG_ERR("Security failed: level %u err %d", level, err);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	LOG_INF("Pairing complete, bonded: %s", bonded ? "yes" : "no");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	LOG_ERR("Pairing failed (reason %d)", reason);
}

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
		settings_load();
	}

	bt_set_bondable(false);

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Initial advertising failed (err %d)", err);
	} else {
		LOG_INF("Advertising for reconnection started");
	}
}

int main(void)
{
	LOG_INF("Starting HID Remote Control with Pairing Mode...");

	k_work_init(&report_work, report_work_handler);
	k_work_init(&pairing_start_work, pairing_start_handler);
	k_work_init_delayable(&pairing_timeout_work, stop_pairing_mode);
	k_work_init_delayable(&blink_work, blink_work_handler);
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

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_subsys_init();
	}

	bt_conn_auth_info_cb_register(&auth_cb_info);
	bt_enable(bt_ready);

	return 0;
}
