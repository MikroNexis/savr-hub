/*
 * SAVR Hub - M0 Milestone
 *
 * Fake TPMS sensor data advertised over BLE (manufacturer-specific)
 * plus a GATT sync service for the app developer to integrate against.
 *
 * Target: adafruit_feather_nrf52840
 * Toolchain: nRF Connect SDK v3.4.1 / Zephyr
 */

#include <zephyr/types.h>
#include <string.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/settings/settings.h>

/* ── Fake sensor constants ────────────────────────────────────── */
#define FAKE_HUB_ID         0x5A5B5C5DUL
#define FAKE_SENSOR_TYPE    0x01U          /* SAVR sensor type */
#define FAKE_PRESSURE_KPA   241U           /* ~35 PSI */
#define FAKE_TEMP_C         25

#define FAKE_RSSI_VAL       ((uint8_t)(-70))
#define RECORD_FORMAT_VER   0x01U
#define MFR_COMPANY_ID      0x0059U        /* Nordic placeholder; agree final ID with client */
uint8_t battery_level = 100;
static const uint8_t fake_sensor_id[8] = {
	0xAA, 0xBB, 0xCC, 0xDD, 0x01, 0x02, 0x03, 0x04
};

/* ── BLE advertisement manufacturer-specific payload (25 bytes) ─
 *
 * [0..1]   company_id (LE)
 * [2]      format_ver
 * [3..6]   hub_id (LE)
 * [7..8]   record_counter (LE)
 * [9..10]  age_seconds (LE)
 * [11]     sensor_type
 * [12..19] sensor_id
 * [20..21] pressure_kpa (LE)
 * [22]     temperature_c
 * [23]     battery_percentage
 * [24]     rssi
 *
 * AD overhead: 3 bytes flags + 27 bytes mfr data = 30 bytes (fits in 31)
 */
#define MFR_DATA_LEN 25
static uint8_t mfr_data[MFR_DATA_LEN];

static struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, mfr_data, sizeof(mfr_data)),
};

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

/* ── Hub record (GATT payload) ───────────────────────────────── */
struct savr_record {
	uint8_t  format_ver;
	uint8_t  sensor_type;
	uint8_t  sensor_id[8];
	uint32_t timestamp;        /* Unix seconds; 0 until phone syncs time */
	uint16_t pressure_kpa;
	int8_t   temperature_c;
	uint8_t  battery_percentage;
	int8_t   rssi;
	uint8_t  raw_payload[16]; /* reserved for raw sensor bytes */
	uint16_t age_seconds;
	uint16_t record_counter;
	uint32_t hub_id;
} __packed;

/* ── State ───────────────────────────────────────────────────── */
static uint16_t        record_counter;
static uint32_t        time_base_s;     /* set by phone via time-sync write */
static bool            notify_enabled;
static struct bt_conn *active_conn;
static struct k_work   adv_work;
static struct k_work   notify_work;

static struct bt_gatt_exchange_params mtu_exchange_params;

static bool            mtu_negotiated = false;
#ifdef ADAFRUIT_FEATHER52840
/* ── LEDs (Feather: led0 = red P0.06, led1 = blue P0.30) ──────
 * Brief: blue = phone connected, orange/red = listening
 */
static const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_red  = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#endif
/* ── GATT service UUIDs (placeholder — finalise with app dev wk1)
 *
 * Service:      12345678-1234-1234-1234-123456789ABC
 * Record:       12345678-1234-1234-1234-123456789ABD  (read + notify)
 * Last Rec ID:  12345678-1234-1234-1234-123456789ABE  (write)
 * Time Sync:    12345678-1234-1234-1234-123456789ABF  (write)
 */
static struct bt_uuid_128 savr_svc_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x1234, 0x1234, 0x123456789ABCULL));
static struct bt_uuid_128 savr_record_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x1234, 0x1234, 0x123456789ABDULL));
static struct bt_uuid_128 savr_last_id_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x1234, 0x1234, 0x123456789ABEULL));
static struct bt_uuid_128 savr_time_sync_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x1234, 0x1234, 0x123456789ABFULL));

/* Forward declaration needed by GATT callbacks (defined after service) */
extern const struct bt_gatt_service_static savr_hub_svc;

/* ── Helpers ─────────────────────────────────────────────────── */
static void fill_fake_record(struct savr_record *rec)
{
	uint32_t now = (uint32_t)(k_uptime_get() / 1000U);

	rec->format_ver     = RECORD_FORMAT_VER;
	rec->sensor_type    = FAKE_SENSOR_TYPE;
	memcpy(rec->sensor_id, fake_sensor_id, sizeof(rec->sensor_id));
	rec->timestamp      = time_base_s + now;
	rec->pressure_kpa   = FAKE_PRESSURE_KPA;
	rec->temperature_c  = FAKE_TEMP_C;
	rec->battery_percentage   = battery_level;
	rec->rssi           = (int8_t)FAKE_RSSI_VAL;
	memset(rec->raw_payload, 0xAB, sizeof(rec->raw_payload));
	rec->age_seconds    = 0U;
	rec->record_counter = record_counter;
	rec->hub_id         = FAKE_HUB_ID;
}

static void update_mfr_data(void)
{
	uint32_t age = (uint32_t)(k_uptime_get() / 1000U);

	sys_put_le16(MFR_COMPANY_ID,    &mfr_data[0]);
	mfr_data[2] = RECORD_FORMAT_VER;
	sys_put_le32(FAKE_HUB_ID,       &mfr_data[3]);
	sys_put_le16(record_counter,    &mfr_data[7]);
	sys_put_le16((uint16_t)age,     &mfr_data[9]);
	mfr_data[11] = FAKE_SENSOR_TYPE;
	memcpy(&mfr_data[12], fake_sensor_id, 8);
	sys_put_le16(FAKE_PRESSURE_KPA, &mfr_data[20]);
	mfr_data[22] = (uint8_t)FAKE_TEMP_C;
	mfr_data[23] = battery_level;
	mfr_data[24] = FAKE_RSSI_VAL;
}

static int send_record_notify(void)
{
	int ret = 0;
	static struct savr_record rec;

	if (!notify_enabled || !active_conn) {
		return -ENOTCONN;
	}
	// C-Code Safety Gate: Reject if the MTU size cannot hold your 43-byte payload yet
	uint16_t current_mtu = bt_gatt_get_mtu(active_conn);
	if (current_mtu < (sizeof(struct savr_record) + 3)) { 
		printk("MTU too small (%d bytes), deferring 43-byte notification...\n", current_mtu);
		return -EMSGSIZE; 
	}
	fill_fake_record(&rec);
	
	/* 
	 * Dynamic Safe Alternative: Find the value attribute cleanly.
	 * This loops through your service to locate the specific UUID 
	 * match instead of relying on fragile hardcoded array indices.
	 */
	const struct bt_gatt_attr *attr = bt_gatt_find_by_uuid(savr_hub_svc.attrs, 
	                                                       savr_hub_svc.attr_count, 
	                                                       &savr_record_uuid.uuid);
	if (!attr) {
		printk("Error: Could not locate characteristic attribute structure!\n");
		return -ENOENT;
	}

	// In a standard declaration, the value attribute immediately follows the declaration.
	// We increment by +1 to move from the base declaration attribute to the actual value handle.
	ret = bt_gatt_notify(active_conn, attr, &rec, sizeof(rec));
	
	if (ret) {
		printk("bt_gatt_notify failed (err %d)\n", ret);
	} else {
		printk("Notification pushed successfully for record %u\n", rec.record_counter);
	}

	return ret;
}

/* ── GATT callbacks ──────────────────────────────────────────── */
static ssize_t record_read(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	static struct savr_record rec;

	fill_fake_record(&rec);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &rec, sizeof(rec));
}

static void record_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	printk("Record notify %s\n", notify_enabled ? "enabled" : "disabled");
	if (notify_enabled) {
		/* Defer to work queue — calling bt_gatt_notify directly from the
		 * BT RX thread (CCC write context) exhausts ACL TX buffers (-ENOMEM).
		 * Work queue runs on a separate thread with fresh buffer availability. */
		//k_work_submit(&notify_work);
	}
}

/* Phone writes its last seen record ID; hub replies with newer records.
 * For M0: immediately notify one fake record regardless of ID. */
static ssize_t last_id_write(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len,
			     uint16_t offset, uint8_t flags)
{
	uint16_t last_id;

	if (len != sizeof(last_id)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	last_id = sys_get_le16(buf);
	printk("Phone last record ID: %u\n", last_id);
	(void)send_record_notify();
	return (ssize_t)len;
}

/* Phone writes current Unix timestamp to sync hub time. */
static ssize_t time_sync_write(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr,
			       const void *buf, uint16_t len,
			       uint16_t offset, uint8_t flags)
{
	if (len != sizeof(uint32_t)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	time_base_s = sys_get_le32(buf);
	printk("Time sync: Unix=%u\n", time_base_s);
	return (ssize_t)len;
}

/* ── GATT service ────────────────────────────────────────────── */
/*
 * Attribute index map:
 *   [0] Primary service
 *   [1] Record char declaration
 *   [2] Record char value       ← notify target
 *   [3] Record CCC
 *   [4] Last Record ID char declaration
 *   [5] Last Record ID char value
 *   [6] Time Sync char declaration
 *   [7] Time Sync char value
 */
BT_GATT_SERVICE_DEFINE(savr_hub_svc,
	BT_GATT_PRIMARY_SERVICE(&savr_svc_uuid),
	BT_GATT_CHARACTERISTIC(&savr_record_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ,
			       record_read, NULL, NULL),
	BT_GATT_CCC(record_ccc_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&savr_last_id_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE,
			       NULL, last_id_write, NULL),
	BT_GATT_CHARACTERISTIC(&savr_time_sync_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE,
			       NULL, time_sync_write, NULL),
);

/* ── Connection callbacks ─────────────────────────────────────── */
static void notify_work_handler(struct k_work *work)
{
	send_record_notify();
}

static void adv_work_handler(struct k_work *work)
{
	int err;

	update_mfr_data();
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		printk("Adv start failed (err %d)\n", err);
		return;
	}
	printk("Advertising as '%s'\n", CONFIG_BT_DEVICE_NAME);
#ifdef ADAFRUIT_FEATHER52840
	gpio_pin_set_dt(&led_red, 1);
	gpio_pin_set_dt(&led_blue, 0);
#endif
}

static void mtu_exchange_cb(struct bt_conn *conn, uint8_t att_err,
			    struct bt_gatt_exchange_params *params)
{
	uint16_t mtu = bt_gatt_get_mtu(conn);
	printk("MTU exchange %s, MTU=%u\n",
	       att_err ? "failed" : "done", mtu);
	if (!att_err && mtu >= (sizeof(struct savr_record) + 3)) {
		mtu_negotiated = true;
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("Connect failed (err 0x%02x)\n", err);
		return;
	}
	printk("Phone connected\n");
	active_conn = bt_conn_ref(conn);
#ifdef ADAFRUIT_FEATHER52840
	gpio_pin_set_dt(&led_blue, 1);
	gpio_pin_set_dt(&led_red, 0);
#endif
	/* Request MTU exchange immediately — savr_record is 43 bytes and
	 * default ATT MTU is 23 (20 bytes data). Without this, bt_gatt_notify
	 * returns -ENOMEM because 43 > 20. */
	mtu_exchange_params.func = mtu_exchange_cb;
	bt_gatt_exchange_mtu(conn, &mtu_exchange_params);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("Phone disconnected (reason 0x%02x)\n", reason);
	if (active_conn) {
		bt_gatt_cancel(active_conn, NULL); 
		bt_conn_unref(active_conn);
		active_conn = NULL;
	}
	notify_enabled = false;
	mtu_negotiated = false;
}

static void recycled_cb(void)
{
	k_work_submit(&adv_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected    = connected,
	.disconnected = disconnected,
	.recycled     = recycled_cb,
};

void update_battery_level(void)
{
    static uint16_t loop_counter = 0;

    if (battery_level == 0)
    {
        return;
    }

    loop_counter++;

    /* Decrement battery every 3 minutes (36 x 5 seconds) */
    if (loop_counter >= 36)
    {
        loop_counter = 0;
        battery_level--;
    }
}
/* ── Main ─────────────────────────────────────────────────────── */
int main(void)
{
	int err;

	printk("SAVR Hub M0 starting\n");
#ifdef ADAFRUIT_FEATHER52840
	if (gpio_is_ready_dt(&led_blue)) {
		gpio_pin_configure_dt(&led_blue, GPIO_OUTPUT_INACTIVE);
	}
	if (gpio_is_ready_dt(&led_red)) {
		gpio_pin_configure_dt(&led_red, GPIO_OUTPUT_INACTIVE);
	}
#endif

	err = bt_enable(NULL);
	if (err) {
		printk("bt_enable failed (err %d)\n", err);
		return 0;
	}
	printk("Bluetooth ready\n");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}
	k_sleep(K_SECONDS(1));
	k_work_init(&adv_work, adv_work_handler);
	k_work_init(&notify_work, notify_work_handler);
	k_work_submit(&adv_work);

	/* Every 5 s: bump record counter, refresh advertisement data,
	 * send notification if phone is subscribed. */
	for (;;) {
		update_battery_level();
		k_sleep(K_SECONDS(5));
		record_counter++;
		if(active_conn == NULL)
		{
			update_mfr_data();
			(void)bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
		}
		else
		{
			if (notify_enabled && mtu_negotiated)
			{
				k_work_submit(&notify_work);
			}
			else
			{
				printk("Waiting on phone handshake (Notify=%d, MTU_Ready=%d)\n", 
					notify_enabled, mtu_negotiated);
			}
		}
	}
	return 0;
}
