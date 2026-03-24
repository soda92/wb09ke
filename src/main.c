/* src/main.c for Zephyr port of WB09_Blinky iBeacon */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* GPIO for LEDs */
static const struct gpio_dt_spec blue_led = GPIO_DT_SPEC_GET(DT_NODELABEL(blue_led_1), gpios);
static const struct gpio_dt_spec green_led = GPIO_DT_SPEC_GET(DT_NODELABEL(green_led_1), gpios);
static const struct gpio_dt_spec red_led = GPIO_DT_SPEC_GET(DT_NODELABEL(red_led_1), gpios);

/* P2P Server UUIDs */
/* 0000FE40-CC7A-482A-984A-7F2ED5B3E58F */
#define BT_UUID_P2P_SERVER_VAL \
	BT_UUID_128_ENCODE(0x0000fe40, 0xcc7a, 0x482a, 0x984a, 0x7f2ed5b3e58f)
/* 0000FE41-8E22-4541-9D4C-21EDAE82ED19 */
#define BT_UUID_P2P_LED_VAL \
	BT_UUID_128_ENCODE(0x0000fe41, 0x8e22, 0x4541, 0x9d4c, 0x21edae82ed19)
/* 0000FE42-8E22-4541-9D4C-21EDAE82ED19 */
#define BT_UUID_P2P_SWITCH_VAL \
	BT_UUID_128_ENCODE(0x0000fe42, 0x8e22, 0x4541, 0x9d4c, 0x21edae82ed19)

static struct bt_uuid_128 p2p_server_uuid = BT_UUID_INIT_128(BT_UUID_P2P_SERVER_VAL);
static struct bt_uuid_128 p2p_led_uuid = BT_UUID_INIT_128(BT_UUID_P2P_LED_VAL);
static struct bt_uuid_128 p2p_switch_uuid = BT_UUID_INIT_128(BT_UUID_P2P_SWITCH_VAL);

static uint8_t led_val[2];

static ssize_t write_led(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *data = buf;

	if (len < 2) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	uint8_t led_id = data[0];
	uint8_t led_state = data[1];

	LOG_INF("LED Write: ID %d, State %d", led_id, led_state);

	switch (led_id) {
	case 0x01: /* Blue LED */
		gpio_pin_set_dt(&blue_led, led_state);
		break;
	case 0x02: /* Green LED */
		gpio_pin_set_dt(&green_led, led_state);
		break;
	case 0x03: /* Red LED */
		gpio_pin_set_dt(&red_led, led_state);
		break;
	default:
		LOG_WRN("Unknown LED ID %d", led_id);
		break;
	}

	return len;
}

BT_GATT_SERVICE_DEFINE(p2p_server_svc,
	BT_GATT_PRIMARY_SERVICE(&p2p_server_uuid),
	BT_GATT_CHARACTERISTIC(&p2p_led_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       NULL, write_led, led_val),
	BT_GATT_CHARACTERISTIC(&p2p_switch_uuid.uuid,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* iBeacon Advertisement Data */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_MANUFACTURER_DATA,
		      0x4c, 0x00, /* Apple Company ID */
		      0x02, 0x15, /* iBeacon prefix */
		      0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		      0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, /* UUID */
		      0x00, 0x01, /* Major */
		      0x00, 0x02, /* Minor */
		      0xc8)       /* Tx Power */
};

/* Scan Response Data (Name) */
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

	/* Start advertising */
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return;
	}

	LOG_INF("iBeacon & P2P advertising started");
}

int main(void)
{
	int err;

	LOG_INF("Starting WB09 Blinky Beacon port...");

	/* Initialize GPIOs */
	if (!gpio_is_ready_dt(&blue_led) || !gpio_is_ready_dt(&green_led) || !gpio_is_ready_dt(&red_led)) {
		LOG_ERR("GPIOs not ready");
		return 0;
	}
	gpio_pin_configure_dt(&blue_led, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&green_led, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&red_led, GPIO_OUTPUT_INACTIVE);

	/* Initialize Bluetooth */
	err = bt_enable(bt_ready);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return 0;
	}

	return 0;
}
