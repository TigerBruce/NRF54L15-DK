/*
 * 极致低功耗 BLE 从机 — 手机写 GATT 触发（IO 由你处理）
 *
 * 功能：广播 + 可连接，一个自定义 Service + 一个 Write 特征。
 *       手机写入该特征时触发（回调里不操作 IO，你在别处处理）。
 * 功耗：长连接间隔 800ms + Slave Latency 4，无串口/控制台。
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

/* 自定义 Service / Characteristic UUID（16-bit 便于 nRF Connect 识别） */
#define BT_UUID_IO_TRIGGER_SVC_VAL  0x1234
#define BT_UUID_IO_TRIGGER_CHRC_VAL 0x1235

#define BT_UUID_IO_TRIGGER_SVC  BT_UUID_DECLARE_16(BT_UUID_IO_TRIGGER_SVC_VAL)
#define BT_UUID_IO_TRIGGER_CHRC BT_UUID_DECLARE_16(BT_UUID_IO_TRIGGER_CHRC_VAL)

/* 连接参数：800ms 间隔，Slave Latency 4，满足 1 秒内响应且低功耗 */
#define CONN_INTERVAL_800MS  640   /* 800 / 1.25 */
#define CONN_LATENCY         4
#define CONN_TIMEOUT_MS      4000  /* 单位 10ms → 400 */

static const char device_name[] = "IOTrigger";
static struct bt_conn *current_conn;
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, device_name, sizeof(device_name) - 1),
};

static ssize_t io_trigger_write(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr,
			       const void *buf, uint16_t len, uint16_t offset,
			       uint8_t flags)
{
	(void)conn;
	(void)attr;
	(void)offset;
	(void)flags;
	/* 手机每次写入会进这里：buf/len 为写入数据。在此处或你扩展的逻辑里做 IO 翻转即可 */
	(void)buf;
	(void)len;
	return len;
}

BT_GATT_SERVICE_DEFINE(io_trigger_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_IO_TRIGGER_SVC),
	BT_GATT_CHARACTERISTIC(BT_UUID_IO_TRIGGER_CHRC,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE,
			       NULL, io_trigger_write, NULL),
);

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		return;
	}
	current_conn = bt_conn_ref(conn);

	struct bt_le_conn_param param = {
		.interval_min = CONN_INTERVAL_800MS,
		.interval_max = CONN_INTERVAL_800MS,
		.latency = CONN_LATENCY,
		.timeout = CONN_TIMEOUT_MS / 10,
	};
	(void)bt_conn_le_param_update(conn, &param);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
};

int main(void)
{
	int err;

	err = bt_enable(NULL);
	if (err) {
		return 0;
	}

	static const struct bt_le_adv_param adv_param = {
		.options = BT_LE_ADV_OPT_CONN,
	};
	err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		return 0;
	}

	for (;;) {
		k_sleep(K_FOREVER);
	}
	return 0;
}
