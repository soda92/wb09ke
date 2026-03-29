/* src/main.c: HID Consumer Control for WB09 (Just Works + Bond Reset) */
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

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/* GPIO for Buttons */
static const struct gpio_dt_spec sw1 = GPIO_DT_SPEC_GET(DT_NODELABEL(user_button_1), gpios);
static const struct gpio_dt_spec sw2 = GPIO_DT_SPEC_GET(DT_NODELABEL(user_button_2), gpios);
static const struct gpio_dt_spec sw3 = GPIO_DT_SPEC_GET(DT_NODELABEL(user_button_3), gpios);

static struct gpio_callback sw1_cb_data;
static struct gpio_callback sw2_cb_data;
static struct gpio_callback sw3_cb_data;

/* Reports: 1 byte [VOL_UP, VOL_DOWN, PLAY_PAUSE, 5 bits padding] */
#define CONSUMER_VOL_UP     BIT(0)
#define CONSUMER_VOL_DOWN   BIT(1)
#define CONSUMER_PLAY_PAUSE BIT(2)

static struct bt_conn *current_conn;

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

	/* Trigger 'Just Works' Pairing */
	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		LOG_ERR("Failed to set security (err %d)", err);
	}
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
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		LOG_INF("Security level changed: %s level %u", addr, level);
	} else {
		LOG_ERR("Security failed: %s level %u err %d", addr, level, err);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Pairing complete: %s, bonded: %s", addr, bonded ? "yes" : "no");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_ERR("Pairing failed: %s, reason: %d", addr, reason);
}

static struct bt_conn_auth_info_cb auth_cb_info = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

/* HID Adv Data */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void bt_ready(int err)
{
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return;
	}

	LOG_INF("Bluetooth initialized");

	hog_init();

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
		LOG_INF("Settings loaded");
	}

	bt_bas_set_battery_level(100);

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return;
	}

	LOG_INF("Advertising successfully started");
}

/* Button Handler */
void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	if (!current_conn) {
		return;
	}

	uint8_t report = 0;
	if (pins & BIT(sw1.pin)) {
		LOG_INF("Vol Up");
		report = CONSUMER_VOL_UP;
	} else if (pins & BIT(sw2.pin)) {
		LOG_INF("Vol Down");
		report = CONSUMER_VOL_DOWN;
	} else if (pins & BIT(sw3.pin)) {
		LOG_INF("Play/Pause");
		report = CONSUMER_PLAY_PAUSE;
	}

	if (report) {
		hog_send_report(current_conn, report);
	}
}

int main(void)
{
	int err;

	LOG_INF("Starting HID Remote Control...");

	/* Init Buttons */
	gpio_pin_configure_dt(&sw1, GPIO_INPUT | GPIO_PULL_UP);
	gpio_pin_configure_dt(&sw2, GPIO_INPUT | GPIO_PULL_UP);
	gpio_pin_configure_dt(&sw3, GPIO_INPUT | GPIO_PULL_UP);

	/* Check if SW1 is held during boot to clear bonds */
	k_sleep(K_MSEC(100));
	if (gpio_pin_get_dt(&sw1) == 0) { /* Active Low */
		LOG_INF("SW1 held during boot, clearing all bonds...");
		bt_unpair(BT_ID_DEFAULT, NULL);
	}

	gpio_pin_interrupt_configure_dt(&sw1, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_pin_interrupt_configure_dt(&sw2, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_pin_interrupt_configure_dt(&sw3, GPIO_INT_EDGE_TO_ACTIVE);

	gpio_init_callback(&sw1_cb_data, button_pressed, BIT(sw1.pin));
	gpio_init_callback(&sw2_cb_data, button_pressed, BIT(sw2.pin));
	gpio_init_callback(&sw3_cb_data, button_pressed, BIT(sw3.pin));

	gpio_add_callback(sw1.port, &sw1_cb_data);
	gpio_add_callback(sw2.port, &sw2_cb_data);
	gpio_add_callback(sw3.port, &sw3_cb_data);

	/* Register Info Callbacks */
	bt_conn_auth_info_cb_register(&auth_cb_info);

	/* Initialize Bluetooth */
	err = bt_enable(bt_ready);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
	}

	return 0;
}
