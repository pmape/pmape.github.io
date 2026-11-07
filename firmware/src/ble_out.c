/*
 * Bluetooth çıkış: kart hedef cihaza (PC, tablet, TV) Bluetooth klavye + fare + gamepad olarak bağlanır.
 *
 * Kurallar:
 *  - Tuşların nereye gideceği (kablo / Bluetooth) kartta saklanır ("route").
 *  - Kablo takılı değilse (powerbank) ya da route = Bluetooth ise kart hedef cihaza görünür olur.
 *  - Sadece "yeni cihaz eşle" modunda (60 sn) yeni bir cihaz eşleşebilir; diğer zamanlarda
 *    sadece önceden eşleşmiş hedefler bağlanabilir.
 *  - Birden çok hedef eşleşmişse seçili hedef ("target") öncelikli; başka hedef bağlanırsa
 *    seçili olan değilse bağlantı kesilir.
 *
 * Ayar ekranı komutları (cfg.c → ble_cmd):
 *   route 0|1      kablo / Bluetooth
 *   pair           60 sn yeni cihaz eşleme
 *   targets        hedef listesi → {"ev":"targets",...}
 *   target <adr>   seçili hedef
 *   unpair <adr>   hedefi unut
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>
#include <bluetooth/services/hids.h>
#include "log.h"
#include "ble_out.h"

/* klavye (1), fare (2), gamepad (3) */
static const uint8_t report_map[] = {
	/* klavye */
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
	0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
	0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
	0xc0,
	/* fare */
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
	0xc0, 0xc0,
	/* gamepad (USB'dekiyle aynı 9 bayt) */
	0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x03,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
	0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3b, 0x01,
	0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
	0x65, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x01,
	0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
	0x05, 0x02, 0x09, 0xc5, 0x09, 0xc4, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
	0xc0,
};

BT_HIDS_DEF(hids_obj, 8, 4, 9);

static struct bt_conn *host;          /* bağlı hedef */
static volatile bool host_ready;      /* şifreli ve bildirimler açık */
static uint8_t route;                 /* 0 kablo, 1 Bluetooth (kartta saklanır) */
static bt_addr_le_t target;           /* seçili hedef (kimlik adresi) */
static bool target_set;
static volatile bool pairing;
static bool adv_on;
static bool usb_present;

extern bool pad_addr_known(const bt_addr_le_t *a);

/* ---------- saklama ---------- */

static int h_set(const char *key, size_t len, settings_read_cb rd, void *arg)
{
	if (!strcmp(key, "route")) { rd(arg, &route, 1); return 0; }
	if (!strcmp(key, "target")) { target_set = rd(arg, &target, sizeof(target)) == sizeof(target); return 0; }
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(pmb, "pmb", NULL, h_set, NULL, NULL);

/* ---------- görünürlük ---------- */

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xc4, 0x03), /* gamepad */
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
};
static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static bool want_adv(void)
{
	if (host) return false;
	return pairing || route == 1 || !usb_present;
}

static void adv_update(struct k_work *w)
{
	bool want = want_adv();

	if (want && !adv_on) {
		/* hızlı yayın (20–30 ms): hedef cihaz kartı hemen görsün, çabuk geri bağlansın */
		static const struct bt_le_adv_param fast = BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONNECTABLE,
			BT_GAP_ADV_FAST_INT_MIN_1 * 2 / 3, BT_GAP_ADV_FAST_INT_MIN_1, NULL);
		int err = bt_le_adv_start(&fast, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

		adv_on = (err == 0 || err == -EALREADY);
		if (err && err != -EALREADY) OUT("{\"ev\":\"ble\",\"adv_err\":%d}\n", err);
	} else if (!want && adv_on) {
		bt_le_adv_stop();
		adv_on = false;
	}
}
static K_WORK_DEFINE(adv_work, adv_update);
static void adv_kick(void) { k_work_submit(&adv_work); }

static void pair_end_fn(struct k_work *w)
{
	pairing = false;
	OUT("{\"ev\":\"ble\",\"pairing\":0}\n");
	adv_kick();
}
static K_WORK_DELAYABLE_DEFINE(pair_end, pair_end_fn);

/* ---------- bağlantı (sadece hedef rolü) ---------- */

static bool is_host_role(struct bt_conn *c)
{
	struct bt_conn_info i;

	return bt_conn_get_info(c, &i) == 0 && i.role == BT_CONN_ROLE_PERIPHERAL;
}

static void addr_str(const bt_addr_le_t *a, char *s, size_t n) { bt_addr_le_to_str(a, s, n); }

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (!is_host_role(conn)) return;
	adv_on = false; /* bağlanınca yayın kendiliğinden durur */
	if (err) { adv_kick(); return; }
	if (host) { bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN); return; }
	host = bt_conn_ref(conn);
	host_ready = false;
	bt_hids_connected(&hids_obj, conn);
	/* eşleşmiş değilse şifreleme sırasında eşleşme istenir; eşleme modu dışındaysa reddedilir */
	bt_conn_set_security(conn, BT_SECURITY_L2);
	OUT("{\"ev\":\"ble\",\"host\":\"connecting\"}\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (conn != host) return;
	bt_hids_disconnected(&hids_obj, conn);
	bt_conn_unref(host);
	host = NULL;
	host_ready = false;
	OUT("{\"ev\":\"ble\",\"host\":\"\",\"reason\":%u}\n", reason);
	adv_kick();
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	char s[BT_ADDR_LE_STR_LEN];

	if (conn != host) return;
	if (err) {
		bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
		return;
	}
	const bt_addr_le_t *a = bt_conn_get_dst(conn);

	/* seçili hedef varsa ve bu o değilse (ve eşleme modunda değilsek) bırak */
	if (target_set && !pairing && bt_addr_le_cmp(a, &target)) {
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}
	if (pairing || !target_set) {
		target = *a;
		target_set = true;
		settings_save_one("pmb/target", &target, sizeof(target));
	}
	host_ready = true;
	addr_str(a, s, sizeof(s));
	OUT("{\"ev\":\"ble\",\"host\":\"%s\"}\n", s);
	/* en kısa aralık (7,5–11 ms) ve 2M PHY: daha az gecikme */
	struct bt_le_conn_param p = BT_LE_CONN_PARAM_INIT(6, 9, 0, 400);

	bt_conn_le_param_update(conn, &p);
	bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
}

BT_CONN_CB_DEFINE(ble_out_cb) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

/* Eşleme sadece pad tarafında ya da eşleme modunda kabul edilir */
static enum bt_security_err pairing_accept(struct bt_conn *conn, const struct bt_conn_pairing_feat *const feat)
{
	if (!is_host_role(conn) || pairing) return BT_SECURITY_ERR_SUCCESS;
	return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	if (is_host_role(conn)) OUT("{\"ev\":\"ble\",\"paired\":%d}\n", bonded);
}

static struct bt_conn_auth_cb auth_cb = { .pairing_accept = pairing_accept };
static struct bt_conn_auth_info_cb auth_info_cb = { .pairing_complete = pairing_complete };

/* ---------- gönderim ---------- */

bool ble_out_ready(void) { return host && host_ready; }
bool ble_out_route_bt(void) { return route == 1; }
bool ble_out_searching(void) { return adv_on && !host; }

int ble_out_send(int which, const uint8_t *buf, uint8_t len)
{
	/* which: 0 klavye, 1 fare, 2 gamepad = rapor sırası */
	if (!ble_out_ready()) return -ENOTCONN;
	return bt_hids_inp_rep_send(&hids_obj, host, which, buf, len, NULL);
}

void ble_out_usb(bool present)
{
	if (usb_present == present) return;
	usb_present = present;
	adv_kick();
}

void ble_out_set_route(int r)
{
	route = r ? 1 : 0;
	settings_save_one("pmb/route", &route, 1);
	OUT("{\"ev\":\"ble\",\"route\":%u}\n", route);
	if (route == 0 && host && !usb_present) {
		/* kablo yoksa Bluetooth'ta kal */
	}
	adv_kick();
}

int ble_out_route(void) { return route; }

/* ---------- komutlar ---------- */

struct list_ctx { int n; };

static void bond_cb(const struct bt_bond_info *info, void *user)
{
	struct list_ctx *c = user;
	char s[BT_ADDR_LE_STR_LEN];

	if (pad_addr_known(&info->addr)) return; /* pad'ler hedef değildir */
	addr_str(&info->addr, s, sizeof(s));
	OUT("%s\"%s\"", c->n++ ? "," : "", s);
}

static void print_targets(void)
{
	struct list_ctx c = { 0 };
	char s[BT_ADDR_LE_STR_LEN] = "", h[BT_ADDR_LE_STR_LEN] = "";

	if (target_set) addr_str(&target, s, sizeof(s));
	if (host && host_ready) addr_str(bt_conn_get_dst(host), h, sizeof(h));
	OUT("{\"ev\":\"targets\",\"list\":[");
	bt_foreach_bond(BT_ID_DEFAULT, bond_cb, &c);
	OUT("],\"target\":\"%s\",\"host\":\"%s\",\"route\":%u,\"pairing\":%d,\"usb\":%d}\n",
	    s, h, route, pairing, usb_present);
}

static int parse(const char *str, bt_addr_le_t *a)
{
	char buf[40];
	const char *type = "random";

	strncpy(buf, str, sizeof(buf) - 1);
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

bool ble_cmd(char *l)
{
	bt_addr_le_t a;

	if (!strncmp(l, "route ", 6)) {
		ble_out_set_route(l[6] == '1');
		print_targets();
		return true;
	}
	if (!strcmp(l, "pair")) {
		pairing = true;
		/* başka bir hedefe bağlıysa yeni cihaz için boşalt */
		if (host) bt_conn_disconnect(host, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		k_work_reschedule(&pair_end, K_SECONDS(60));
		OUT("{\"ev\":\"ble\",\"pairing\":1}\n");
		adv_kick();
		return true;
	}
	if (!strcmp(l, "targets")) {
		print_targets();
		return true;
	}
	if (!strncmp(l, "target ", 7)) {
		if (parse(l + 7, &a)) return true;
		target = a;
		target_set = true;
		settings_save_one("pmb/target", &target, sizeof(target));
		if (host && bt_addr_le_cmp(bt_conn_get_dst(host), &a)) bt_conn_disconnect(host, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		print_targets();
		return true;
	}
	if (!strncmp(l, "unpair ", 7)) {
		if (parse(l + 7, &a)) return true;
		if (host && !bt_addr_le_cmp(bt_conn_get_dst(host), &a)) bt_conn_disconnect(host, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_unpair(BT_ID_DEFAULT, &a);
		if (target_set && !bt_addr_le_cmp(&target, &a)) {
			target_set = false;
			settings_delete("pmb/target");
		}
		print_targets();
		return true;
	}
	return false;
}

/* main.c, bt_enable ve settings_load'dan sonra çağırır */
int ble_out_init(void)
{
	struct bt_hids_init_param p = { 0 };
	struct bt_hids_inp_rep *r;
	static const uint8_t sizes[3] = { 8, 4, 9 };

	p.rep_map.data = report_map;
	p.rep_map.size = sizeof(report_map);
	p.info.bcd_hid = 0x0101;
	p.info.b_country_code = 0;
	p.info.flags = BT_HIDS_REMOTE_WAKE | BT_HIDS_NORMALLY_CONNECTABLE;
	for (int i = 0; i < 3; i++) {
		r = &p.inp_rep_group_init.reports[i];
		r->size = sizes[i];
		r->id = i + 1;
		p.inp_rep_group_init.cnt++;
	}
	int err = bt_hids_init(&hids_obj, &p);

	if (err) {
		OUT("{\"ev\":\"ble\",\"hids_err\":%d}\n", err);
		return err;
	}
	bt_conn_auth_cb_register(&auth_cb);
	bt_conn_auth_info_cb_register(&auth_info_cb);
	adv_kick();
	return 0;
}
