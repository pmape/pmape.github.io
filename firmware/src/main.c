/*
 * PadMapper – XIAO nRF52840 kart yazılımı
 * Adım 1: Bluetooth gamepad'i bul, bağlan, tuş haritasını (Report Map) oku,
 * gelen tuş raporlarını USB seri porttan satır satır JSON olarak gönder.
 *
 * Çıktı biçimi (her satır bir JSON):
 *   {"ev":"hello","fw":"0.1.0"}
 *   {"ev":"found","addr":"..","name":"..","rssi":-53}
 *   {"ev":"connected","addr":".."}
 *   {"ev":"map","off":0,"d":"05010905a101..."}      (parça parça)
 *   {"ev":"map_end","len":123}
 *   {"ev":"rep","id":1,"d":"00800080..."}           (yalnızca değişince)
 *   {"ev":"disconnected","reason":19}
 *   {"ev":"err","where":"..","code":-5}
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>

#include <bluetooth/scan.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/services/hogp.h>

#define FW_VERSION "0.1.0"

static struct bt_conn *cur_conn;
static struct bt_hogp hogp;

/* ---------- yardımcılar ---------- */

static void print_hex(const uint8_t *d, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		printk("%02x", d[i]);
	}
}

static void print_err(const char *where, int code)
{
	printk("{\"ev\":\"err\",\"where\":\"%s\",\"code\":%d}\n", where, code);
}

static void start_scan(void)
{
	int err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);

	if (err && err != -EALREADY) {
		print_err("scan_start", err);
		return;
	}
	printk("{\"ev\":\"scanning\"}\n");
}

/* ---------- tarama ---------- */

struct name_ctx {
	char name[32];
};

static bool adv_name_cb(struct bt_data *data, void *user_data)
{
	struct name_ctx *ctx = user_data;

	if (data->type == BT_DATA_NAME_COMPLETE || data->type == BT_DATA_NAME_SHORTENED) {
		size_t len = MIN(data->data_len, sizeof(ctx->name) - 1);

		memcpy(ctx->name, data->data, len);
		ctx->name[len] = '\0';
		/* JSON'u bozmasın */
		for (size_t i = 0; i < len; i++) {
			if (ctx->name[i] == '"' || ctx->name[i] == '\\' || ctx->name[i] < 0x20) {
				ctx->name[i] = '_';
			}
		}
		return false;
	}
	return true;
}

static void scan_filter_match(struct bt_scan_device_info *info,
			      struct bt_scan_filter_match *match, bool connectable)
{
	char addr[BT_ADDR_LE_STR_LEN];
	struct name_ctx ctx = { .name = "" };
	struct net_buf_simple_state state;

	bt_addr_le_to_str(info->recv_info->addr, addr, sizeof(addr));

	net_buf_simple_save(info->adv_data, &state);
	bt_data_parse(info->adv_data, adv_name_cb, &ctx);
	net_buf_simple_restore(info->adv_data, &state);

	printk("{\"ev\":\"found\",\"addr\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"connectable\":%d}\n",
	       addr, ctx.name, info->recv_info->rssi, connectable);
}

static void scan_connecting_error(struct bt_scan_device_info *info)
{
	print_err("connect_create", 0);
	start_scan();
}

static void scan_connecting(struct bt_scan_device_info *info, struct bt_conn *conn)
{
	cur_conn = bt_conn_ref(conn);
}

BT_SCAN_CB_INIT(scan_cb, scan_filter_match, NULL, scan_connecting_error, scan_connecting);

static int scan_init(void)
{
	struct bt_scan_init_param param = {
		.scan_param = NULL,
		.conn_param = BT_LE_CONN_PARAM_DEFAULT,
		.connect_if_match = 1,
	};
	int err;

	bt_scan_init(&param);
	bt_scan_cb_register(&scan_cb);

	/* Yalnızca HID (0x1812) yayını yapan cihazlar: gamepad, klavye, fare */
	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_HIDS);
	if (err) {
		print_err("filter_add", err);
		return err;
	}
	err = bt_scan_filter_enable(BT_SCAN_UUID_FILTER, false);
	if (err) {
		print_err("filter_enable", err);
	}
	return err;
}

/* ---------- HID servisini bulma ---------- */

static void discovery_completed(struct bt_gatt_dm *dm, void *ctx)
{
	int err = bt_hogp_handles_assign(dm, &hogp);

	if (err) {
		print_err("hogp_assign", err);
	}
	bt_gatt_dm_data_release(dm);
}

static void discovery_not_found(struct bt_conn *conn, void *ctx)
{
	print_err("no_hid_service", 0);
}

static void discovery_error(struct bt_conn *conn, int err, void *ctx)
{
	print_err("discovery", err);
}

static const struct bt_gatt_dm_cb discovery_cb = {
	.completed = discovery_completed,
	.service_not_found = discovery_not_found,
	.error_found = discovery_error,
};

static void start_discovery(struct bt_conn *conn)
{
	int err = bt_gatt_dm_start(conn, BT_UUID_HIDS, &discovery_cb, NULL);

	if (err) {
		print_err("discovery_start", err);
	}
}

/* ---------- bağlantı ---------- */

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (conn_err) {
		printk("{\"ev\":\"connect_failed\",\"addr\":\"%s\",\"code\":%u}\n", addr, conn_err);
		if (conn == cur_conn) {
			bt_conn_unref(cur_conn);
			cur_conn = NULL;
		}
		start_scan();
		return;
	}

	printk("{\"ev\":\"connected\",\"addr\":\"%s\"}\n", addr);

	/* Şifreleme iste; desteklemeyen cihazlarda doğrudan devam et */
	if (bt_conn_set_security(conn, BT_SECURITY_L2)) {
		start_discovery(conn);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("{\"ev\":\"disconnected\",\"reason\":%u}\n", reason);

	if (bt_hogp_assign_check(&hogp)) {
		bt_hogp_release(&hogp);
	}
	if (cur_conn == conn) {
		bt_conn_unref(cur_conn);
		cur_conn = NULL;
	}
	start_scan();
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	printk("{\"ev\":\"security\",\"level\":%u,\"err\":%d}\n", level, err);
	start_discovery(conn);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

/* ---------- tuş haritası ve raporlar ---------- */

#define MAX_REPS 8
#define MAX_REP_LEN 64

static struct {
	uint8_t id;
	uint8_t len;
	uint8_t last[MAX_REP_LEN];
	bool used;
} rep_cache[MAX_REPS];

static uint8_t rep_cb(struct bt_hogp *h, struct bt_hogp_rep_info *rep, uint8_t err,
		      const uint8_t *data)
{
	if (!data) {
		return BT_GATT_ITER_STOP;
	}
	if (err) {
		print_err("rep", err);
		return BT_GATT_ITER_CONTINUE;
	}

	uint8_t id = bt_hogp_rep_id(rep);
	size_t len = MIN(bt_hogp_rep_size(rep), MAX_REP_LEN);
	int slot = -1;

	for (int i = 0; i < MAX_REPS; i++) {
		if (rep_cache[i].used && rep_cache[i].id == id) {
			slot = i;
			break;
		}
		if (!rep_cache[i].used && slot < 0) {
			slot = i;
		}
	}
	if (slot >= 0) {
		if (rep_cache[slot].used && rep_cache[slot].len == len &&
		    !memcmp(rep_cache[slot].last, data, len)) {
			return BT_GATT_ITER_CONTINUE; /* değişmedi, yazma */
		}
		rep_cache[slot].used = true;
		rep_cache[slot].id = id;
		rep_cache[slot].len = len;
		memcpy(rep_cache[slot].last, data, len);
	}

	printk("{\"ev\":\"rep\",\"id\":%u,\"d\":\"", id);
	print_hex(data, len);
	printk("\"}\n");
	return BT_GATT_ITER_CONTINUE;
}

static void subscribe_inputs(void)
{
	struct bt_hogp_rep_info *rep = NULL;
	int n = 0;

	while ((rep = bt_hogp_rep_next(&hogp, rep)) != NULL) {
		printk("{\"ev\":\"report\",\"id\":%u,\"type\":%d,\"size\":%u}\n",
		       bt_hogp_rep_id(rep), bt_hogp_rep_type(rep), (unsigned)bt_hogp_rep_size(rep));
		if (bt_hogp_rep_type(rep) == BT_HIDS_REPORT_TYPE_INPUT) {
			int err = bt_hogp_rep_subscribe(&hogp, rep, rep_cb);

			if (err) {
				print_err("subscribe", err);
			} else {
				n++;
			}
		}
	}
	memset(rep_cache, 0, sizeof(rep_cache));
	printk("{\"ev\":\"ready\",\"inputs\":%d}\n", n);
}

static size_t map_total;
static size_t map_next_off;
static void map_read_work_fn(struct k_work *w);
static K_WORK_DEFINE(map_read_work, map_read_work_fn);

static void map_cb(struct bt_hogp *h, uint8_t err, const uint8_t *data, size_t size,
		   size_t offset)
{
	if (err || !data || size == 0) {
		if (err && err != BT_ATT_ERR_INVALID_OFFSET) {
			print_err("map_read", err);
		}
		printk("{\"ev\":\"map_end\",\"len\":%u}\n", (unsigned)map_total);
		subscribe_inputs();
		return;
	}

	printk("{\"ev\":\"map\",\"off\":%u,\"d\":\"", (unsigned)offset);
	print_hex(data, size);
	printk("\"}\n");

	map_total = offset + size;
	map_next_off = offset + size;
	k_work_submit(&map_read_work);
}

static void map_read_work_fn(struct k_work *w)
{
	int err = bt_hogp_map_read(&hogp, map_cb, map_next_off, K_MSEC(500));

	if (err) {
		/* okuma bitti ya da desteklenmiyor: yine de tuşları dinle */
		printk("{\"ev\":\"map_end\",\"len\":%u}\n", (unsigned)map_total);
		subscribe_inputs();
	}
}

static void hogp_ready_work_fn(struct k_work *w)
{
	map_total = 0;
	map_next_off = 0;
	printk("{\"ev\":\"hid_ready\"}\n");
	k_work_submit(&map_read_work);
}
static K_WORK_DEFINE(hogp_ready_work, hogp_ready_work_fn);

static void hogp_ready_cb(struct bt_hogp *h)
{
	k_work_submit(&hogp_ready_work);
}

static void hogp_prep_fail_cb(struct bt_hogp *h, int err)
{
	print_err("hogp_prep", err);
}

static void hogp_pm_update_cb(struct bt_hogp *h)
{
}

static const struct bt_hogp_init_params hogp_params = {
	.ready_cb = hogp_ready_cb,
	.prep_fail_cb = hogp_prep_fail_cb,
	.pm_update_cb = hogp_pm_update_cb,
};

/* ---------- eşleşme: "Just Works", sormadan kabul ---------- */

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	printk("{\"ev\":\"paired\",\"bonded\":%d}\n", bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	printk("{\"ev\":\"pair_failed\",\"reason\":%d}\n", reason);
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

/* ---------- ana ---------- */

int main(void)
{
	int err;

	err = usb_enable(NULL);
	if (err && err != -EALREADY) {
		/* seri port yoksa yapacak pek bir şey yok, yine de devam */
	}
	k_sleep(K_MSEC(1500));

	printk("{\"ev\":\"hello\",\"fw\":\"" FW_VERSION "\"}\n");

	bt_hogp_init(&hogp, &hogp_params);

	err = bt_enable(NULL);
	if (err) {
		print_err("bt_enable", err);
		return 0;
	}
	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}
	bt_conn_auth_info_cb_register(&auth_info_cb);

	if (scan_init() == 0) {
		start_scan();
	}

	/* Ekran kartın orada olduğunu anlasın diye ara ara haber ver */
	while (1) {
		k_sleep(K_SECONDS(3));
		printk("{\"ev\":\"alive\",\"connected\":%d}\n", cur_conn != NULL);
	}
	return 0;
}
