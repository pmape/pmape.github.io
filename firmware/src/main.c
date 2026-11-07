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
#include <zephyr/drivers/uart.h>

#include <bluetooth/scan.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/services/hogp.h>

#include "log.h"
#include "pad.h"
#include "out.h"

#include "cfg.h"
#include "ble_out.h"

bool host_listening(void)
{
	static const struct device *con = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0;

	if (!device_is_ready(con)) return false;
	if (uart_line_ctrl_get(con, UART_LINE_CTRL_DTR, &dtr)) return false;
	return dtr != 0;
}

static struct bt_conn *cur_conn;
static struct bt_hogp hogp;

/* ---------- yardımcılar ---------- */

static void print_hex(const uint8_t *d, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		OUT("%02x", d[i]);
	}
}

static void print_err(const char *where, int code)
{
	OUT("{\"ev\":\"err\",\"where\":\"%s\",\"code\":%d}\n", where, code);
}

static void start_scan(void)
{
	int err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);

	if (err && err != -EALREADY) {
		print_err("scan_start", err);
		return;
	}
	OUT("{\"ev\":\"scanning\"}\n");
}

/* ---------- kayıtlı kontrolcüler ---------- */

#define MAXC 4
#define LM_MAX 48
#define LM_ENT 14
static bt_addr_le_t saved[MAXC];
static uint8_t nsaved;
static bt_addr_le_t want;
static bool want_set;
static bt_addr_le_t cur_addr;
static char cur_name[32];
static struct lmap {
	bt_addr_le_t a;
	uint8_t n;
	uint8_t e[LM_MAX * LM_ENT];
} lms[MAXC];
static bool listing;

static bool addr_eq(const bt_addr_le_t *a, const bt_addr_le_t *b) { return bt_addr_le_cmp(a, b) == 0; }

/* ble_out.c: bu adres kayıtlı bir pad mi? (hedef listesine karışmasın) */
bool pad_addr_known(const bt_addr_le_t *a)
{
	for (int i = 0; i < nsaved; i++) if (addr_eq(&saved[i], a)) return true;
	return false;
}

static int parse_addr(const char *s, bt_addr_le_t *a)
{
	char buf[40];
	const char *type = "random";

	strncpy(buf, s, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	char *t = strchr(buf, ' ');

	if (t) {
		*t++ = 0;
		if (*t == '(') t++;
		char *e = strchr(t, ')');

		if (e) *e = 0;
		type = t;
	}
	return bt_addr_le_from_str(buf, type, a);
}

static void save_list(void)
{
	settings_save_one("pmc/list", saved, nsaved * sizeof(bt_addr_le_t));
}

static void save_lm(int i)
{
	char key[12];

	snprintk(key, sizeof(key), "pmc/lm%d", i);
	if (!lms[i].n) settings_delete(key);
	else settings_save_one(key, &lms[i], offsetof(struct lmap, e) + lms[i].n * LM_ENT);
}

static int lm_find(const bt_addr_le_t *a)
{
	for (int i = 0; i < MAXC; i++) if (lms[i].n && addr_eq(&lms[i].a, a)) return i;
	return -1;
}

static void saved_front(const bt_addr_le_t *a)
{
	int i;

	for (i = 0; i < nsaved; i++) if (addr_eq(&saved[i], a)) break;
	if (i == nsaved) {
		if (nsaved < MAXC) nsaved++;
		i = nsaved - 1;
	}
	for (; i > 0; i--) saved[i] = saved[i - 1];
	saved[0] = *a;
	save_list();
}

static void print_ctrls(void)
{
	char s[BT_ADDR_LE_STR_LEN];

	OUT("{\"ev\":\"ctrls\",\"list\":[");
	for (int i = 0; i < nsaved; i++) {
		bt_addr_le_to_str(&saved[i], s, sizeof(s));
		OUT("%s{\"addr\":\"%s\",\"learned\":%d}", i ? "," : "", s, lm_find(&saved[i]) >= 0);
	}
	bt_addr_le_to_str(&cur_addr, s, sizeof(s));
	OUT("],\"cur\":\"%s\",\"name\":\"%s\"}\n", cur_conn ? s : "", cur_conn ? cur_name : "");
}

static int h_ctrl_set(const char *key, size_t len, settings_read_cb read_cb, void *arg)
{
	if (!strcmp(key, "list")) {
		ssize_t r = read_cb(arg, saved, MIN(len, sizeof(saved)));

		nsaved = r > 0 ? r / sizeof(bt_addr_le_t) : 0;
		return 0;
	}
	if (!strncmp(key, "lm", 2)) {
		int i = key[2] - '0';

		if (i >= 0 && i < MAXC) {
			memset(&lms[i], 0, sizeof(lms[i]));
			read_cb(arg, &lms[i], MIN(len, sizeof(lms[i])));
		}
		return 0;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(pmc, "pmc", NULL, h_ctrl_set, NULL, NULL);

static bool may_connect(const bt_addr_le_t *a)
{
	if (cur_conn) return false;
	if (want_set) return addr_eq(a, &want);
	if (!nsaved) return true;
	for (int i = 0; i < nsaved; i++) if (addr_eq(&saved[i], a)) return true;
	return false;
}

/* arama listesinde aynı cihazı sık sık yazmamak için */
static struct { bt_addr_le_t a; int64_t t; } seen[24];

static bool seen_recent(const bt_addr_le_t *a)
{
	int64_t now = k_uptime_get();
	int old = 0;

	for (int i = 0; i < ARRAY_SIZE(seen); i++) {
		if (seen[i].t && addr_eq(&seen[i].a, a)) {
			if (now - seen[i].t < 1500) return true;
			seen[i].t = now;
			return false;
		}
		if (seen[i].t < seen[old].t) old = i;
	}
	seen[old].a = *a;
	seen[old].t = now;
	return false;
}

static void list_end_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(list_end, list_end_fn);
static void list_end_fn(struct k_work *w)
{
	listing = false;
	OUT("{\"ev\":\"scan_end\"}\n");
	if (cur_conn) bt_scan_stop();
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

	if (listing && !seen_recent(info->recv_info->addr)) {
		OUT("{\"ev\":\"dev\",\"addr\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"hid\":1}\n",
		    addr, ctx.name, info->recv_info->rssi);
	}
	if (!connectable || !may_connect(info->recv_info->addr)) return;

	OUT("{\"ev\":\"found\",\"addr\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"connectable\":%d}\n",
	       addr, ctx.name, info->recv_info->rssi, connectable);
	strcpy(cur_name, ctx.name);
	bt_scan_stop();
	int err = bt_conn_le_create(info->recv_info->addr, BT_CONN_LE_CREATE_CONN,
				    BT_LE_CONN_PARAM_DEFAULT, &cur_conn);

	if (err) {
		print_err("conn_create", err);
		cur_conn = NULL;
		start_scan();
	}
}

static void scan_filter_no_match(struct bt_scan_device_info *info, bool connectable)
{
	char addr[BT_ADDR_LE_STR_LEN];
	struct name_ctx ctx = { .name = "" };
	struct net_buf_simple_state state;

	if (!listing || seen_recent(info->recv_info->addr)) return;
	bt_addr_le_to_str(info->recv_info->addr, addr, sizeof(addr));
	net_buf_simple_save(info->adv_data, &state);
	bt_data_parse(info->adv_data, adv_name_cb, &ctx);
	net_buf_simple_restore(info->adv_data, &state);
	if (!ctx.name[0]) return; /* adsızları gösterme */
	OUT("{\"ev\":\"dev\",\"addr\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"hid\":0}\n",
	    addr, ctx.name, info->recv_info->rssi);
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

BT_SCAN_CB_INIT(scan_cb, scan_filter_match, scan_filter_no_match, scan_connecting_error, scan_connecting);

static int scan_init(void)
{
	struct bt_scan_init_param param = {
		.scan_param = NULL,
		.conn_param = BT_LE_CONN_PARAM_DEFAULT,
		.connect_if_match = 0,
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

/* Kart iki rolde bağlanır: pad'e "merkez", hedef cihaza "çevre" (ble_out.c). Burası sadece pad içindir. */
static bool is_pad_conn(struct bt_conn *c)
{
	struct bt_conn_info i;

	return bt_conn_get_info(c, &i) == 0 && i.role == BT_CONN_ROLE_CENTRAL;
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (!is_pad_conn(conn)) return;
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (conn_err) {
		OUT("{\"ev\":\"connect_failed\",\"addr\":\"%s\",\"code\":%u}\n", addr, conn_err);
		if (conn == cur_conn) {
			bt_conn_unref(cur_conn);
			cur_conn = NULL;
		}
		start_scan();
		return;
	}

	OUT("{\"ev\":\"connected\",\"addr\":\"%s\",\"name\":\"%s\"}\n", addr, cur_name);
	cur_addr = *bt_conn_get_dst(conn);
	if (listing) bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	want_set = false;
	saved_front(&cur_addr);

	/* Şifreleme iste; desteklemeyen cihazlarda doğrudan devam et */
	if (bt_conn_set_security(conn, BT_SECURITY_L2)) {
		start_discovery(conn);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (!is_pad_conn(conn)) return;
	OUT("{\"ev\":\"disconnected\",\"reason\":%u}\n", reason);
	out_set_connected(false);
	pad_reset();

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
	if (!is_pad_conn(conn)) return;
	OUT("{\"ev\":\"security\",\"level\":%u,\"err\":%d}\n", level, err);
	start_discovery(conn);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

/* ---------- tuş haritası ve raporlar ---------- */

static uint8_t map_buf[512];
static size_t map_total;

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

	pad_input(id, data, bt_hogp_rep_size(rep));
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
	if (!host_listening()) {
		return BT_GATT_ITER_CONTINUE;
	}

	OUT("{\"ev\":\"rep\",\"id\":%u,\"d\":\"", id);
	print_hex(data, len);
	OUT("\"}\n");
	return BT_GATT_ITER_CONTINUE;
}

static void subscribe_inputs(void)
{
	struct bt_hogp_rep_info *rep = NULL;
	int n = 0;

	pad_reset();
	pad_parse_map(map_buf, MIN(map_total, sizeof(map_buf)));
	{
		int li = lm_find(&cur_addr);

		pad_set_learned(li >= 0 ? lms[li].e : NULL, li >= 0 ? lms[li].n : 0);
		OUT("{\"ev\":\"learned\",\"on\":%d}\n", li >= 0);
	}

	while ((rep = bt_hogp_rep_next(&hogp, rep)) != NULL) {
		OUT("{\"ev\":\"report\",\"id\":%u,\"type\":%d,\"size\":%u}\n",
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
	OUT("{\"ev\":\"ready\",\"inputs\":%d}\n", n);
	out_set_connected(n > 0);
}

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
		OUT("{\"ev\":\"map_end\",\"len\":%u}\n", (unsigned)map_total);
		subscribe_inputs();
		return;
	}

	if (offset + size <= sizeof(map_buf)) {
		memcpy(&map_buf[offset], data, size);
	}
	OUT("{\"ev\":\"map\",\"off\":%u,\"d\":\"", (unsigned)offset);
	print_hex(data, size);
	OUT("\"}\n");

	map_total = offset + size;
	map_next_off = offset + size;
	k_work_submit(&map_read_work);
}

static void map_read_work_fn(struct k_work *w)
{
	int err = bt_hogp_map_read(&hogp, map_cb, map_next_off, K_MSEC(500));

	if (err) {
		/* okuma bitti ya da desteklenmiyor: yine de tuşları dinle */
		OUT("{\"ev\":\"map_end\",\"len\":%u}\n", (unsigned)map_total);
		subscribe_inputs();
	}
}

static void hogp_ready_work_fn(struct k_work *w)
{
	map_total = 0;
	map_next_off = 0;
	OUT("{\"ev\":\"hid_ready\"}\n");
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
	.prep_error_cb = hogp_prep_fail_cb,
	.pm_update_cb = hogp_pm_update_cb,
};

/* ---------- eşleşme: "Just Works", sormadan kabul ---------- */

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	OUT("{\"ev\":\"paired\",\"bonded\":%d}\n", bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	OUT("{\"ev\":\"pair_failed\",\"reason\":%d}\n", reason);
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

/* ---------- ayar ekranından gelen kontrolcü komutları ---------- */

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool bt_cmd(char *l)
{
	bt_addr_le_t a;

	if (!strcmp(l, "scan")) {
		memset(seen, 0, sizeof(seen));
		listing = true;
		OUT("{\"ev\":\"scan_start\"}\n");
		bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
		k_work_reschedule(&list_end, K_SECONDS(20));
		return true;
	}
	if (!strcmp(l, "scan_stop")) {
		k_work_reschedule(&list_end, K_NO_WAIT);
		return true;
	}
	if (!strcmp(l, "ctrls")) {
		print_ctrls();
		return true;
	}
	if (!strncmp(l, "use ", 4)) {
		if (parse_addr(l + 4, &a)) return true;
		want = a;
		want_set = true;
		saved_front(&a);
		if (cur_conn && !addr_eq(&cur_addr, &a)) bt_conn_disconnect(cur_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		else if (!cur_conn) start_scan();
		print_ctrls();
		return true;
	}
	if (!strncmp(l, "forget ", 7)) {
		if (parse_addr(l + 7, &a)) return true;
		for (int i = 0; i < nsaved; i++) {
			if (addr_eq(&saved[i], &a)) {
				for (int j = i; j < nsaved - 1; j++) saved[j] = saved[j + 1];
				nsaved--;
				break;
			}
		}
		save_list();
		int li = lm_find(&a);

		if (li >= 0) { lms[li].n = 0; save_lm(li); }
		bt_unpair(BT_ID_DEFAULT, &a);
		if (cur_conn && addr_eq(&cur_addr, &a)) bt_conn_disconnect(cur_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		print_ctrls();
		return true;
	}
	if (!strcmp(l, "map")) {
		char s[BT_ADDR_LE_STR_LEN];

		bt_addr_le_to_str(&cur_addr, s, sizeof(s));
		OUT("{\"ev\":\"ctrl\",\"addr\":\"%s\",\"name\":\"%s\",\"connected\":%d,\"learned\":%d}\n",
		    cur_conn ? s : "", cur_name, cur_conn != NULL, pad_learned());
		if (cur_conn && map_total) {
			OUT("{\"ev\":\"map\",\"off\":0,\"d\":\"");
			print_hex(map_buf, MIN(map_total, sizeof(map_buf)));
			OUT("\"}\n{\"ev\":\"map_end\",\"len\":%u}\n", (unsigned)map_total);
		}
		/* son raporlar: ayar ekranı tuşlar bırakılmışken nasıl göründüğünü bilsin */
		for (int i = 0; i < MAX_REPS; i++) {
			if (!rep_cache[i].used) continue;
			OUT("{\"ev\":\"rep\",\"id\":%u,\"d\":\"", rep_cache[i].id);
			print_hex(rep_cache[i].last, rep_cache[i].len);
			OUT("\"}\n");
		}
		return true;
	}
	if (!strncmp(l, "lm ", 3)) {
		/* lm <adres (tür)> <hex>  — hex boşsa öğrenilmiş harita silinir */
		char *h = strrchr(l, ' ');
		bool empty = false;

		if (!h || h == l + 2) return true;
		*h++ = 0;
		if (parse_addr(l + 3, &a)) {
			/* hex yoksa son kelime adresin parçasıdır */
			h[-1] = ' ';
			if (parse_addr(l + 3, &a)) return true;
			empty = true;
		}
		int li = lm_find(&a);

		if (li < 0) for (li = 0; li < MAXC && lms[li].n; li++);
		if (li >= MAXC) li = MAXC - 1;
		lms[li].a = a;
		lms[li].n = 0;
		if (!empty) {
			size_t n = 0;

			for (; h[0] && h[1] && n < sizeof(lms[li].e); h += 2) {
				int x = hexval(h[0]), y = hexval(h[1]);

				if (x < 0 || y < 0) break;
				lms[li].e[n++] = (x << 4) | y;
			}
			lms[li].n = n / LM_ENT;
		}
		save_lm(li);
		if (cur_conn && addr_eq(&cur_addr, &a)) pad_set_learned(lms[li].e, lms[li].n);
		OUT("{\"ev\":\"lm\",\"ok\":1,\"n\":%u}\n", lms[li].n);
		return true;
	}
	return false;
}

/* ---------- ana ---------- */

static void usb_status_cb(enum usb_dc_status_code status, const uint8_t *param)
{
	out_usb_status(status);
}

int main(void)
{
	int err;

	out_usb_init();
	err = usb_enable(usb_status_cb);
	if (err && err != -EALREADY) {
		/* seri port yoksa yapacak pek bir şey yok, yine de devam */
	}
	k_sleep(K_MSEC(1500));
	out_start();

	OUT("{\"ev\":\"hello\",\"fw\":\"" FW_VERSION "\"}\n");

	bt_hogp_init(&hogp, &hogp_params);

	err = bt_enable(NULL);
	if (err) {
		print_err("bt_enable", err);
		return 0;
	}
	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
		ble_out_init();
	}
	bt_conn_auth_info_cb_register(&auth_info_cb);

	if (scan_init() == 0) {
		start_scan();
	}

	/* Ekran kartın orada olduğunu anlasın diye ara ara haber ver */
	while (1) {
		k_sleep(K_SECONDS(3));
		OUT("{\"ev\":\"alive\",\"connected\":%d}\n", cur_conn != NULL);
		out_print_stats();
	}
	return 0;
}
