/*
 * Bluetooth klavye/fare girişi.
 *
 * Kart pad'e ek olarak en fazla 2 Bluetooth klavye/fareye bağlanır. Cihazlar "boot" biçiminde
 * okunur (her klavye ve farede bulunan standart biçim). İki kullanım:
 *   mod 0: doğrudan geçir — klavye/fare hedef cihaza (USB ya da Bluetooth) aynen gider
 *   mod 1: gamepad'e çevir — tuşlar pad tuşlarına/çubuklara, fare sağ çubuğa; atanmamış tuşlar geçer
 *
 * Komutlar:
 *   hid list                      → {"ev":"hids",...}
 *   hid use <adr>                 yeni cihaz ekle (taramada görülünce bağlanır)
 *   hid forget <adr>
 *   hid mode 0|1, hid sens <1..100>
 *   hid map <hex: kod hedef kod hedef ...>   (boş = varsayılan)
 */
#include <string.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/settings/settings.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/scan.h>
#include <bluetooth/services/hogp.h>
#include "log.h"
#include "hid_in.h"
#include "out.h"

#define NH 2

struct hdev { bt_addr_le_t a; char name[20]; };
static struct hdev saved[NH];
static uint8_t nsaved;
static struct hid_cfg cfg;

static struct slot {
	struct bt_conn *conn;
	struct bt_hogp hogp;
	bt_addr_le_t a;
	bool kb, ms;
	uint8_t kbr[8];
	struct k_work_delayable disc;
} sl[NH];

static struct k_spinlock lk;
static uint8_t mbtn;
static int32_t mdx, mdy, mwh;
static int32_t vx, vy;
static int64_t m_last, m_prev;
static bt_addr_le_t pending;
static bool pending_set;

extern void scan_resume(void);

/* ---------- saklama ---------- */

static int h_set(const char *key, size_t len, settings_read_cb rd, void *arg)
{
	if (!strcmp(key, "list")) {
		ssize_t n = rd(arg, saved, sizeof(saved));

		nsaved = n > 0 ? n / sizeof(struct hdev) : 0;
		return 0;
	}
	if (!strcmp(key, "cfg")) {
		struct hid_cfg c;
		ssize_t n = rd(arg, &c, sizeof(c));

		if (n >= 3 && c.n <= HMAP_MAX && c.sens >= 1 && c.sens <= 100) cfg = c;
		return 0;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(pmh, "pmh", NULL, h_set, NULL, NULL);

static void save_list(void) { settings_save_one("pmh/list", saved, nsaved * sizeof(struct hdev)); }
static void save_cfg(void) { settings_save_one("pmh/cfg", &cfg, 3 + cfg.n * 2); }

/* ---------- bilgi ---------- */

bool hid_in_is(struct bt_conn *c)
{
	for (int i = 0; i < NH; i++) if (c && sl[i].conn == c) return true;
	return false;
}

bool hid_in_known(const bt_addr_le_t *a)
{
	for (int i = 0; i < nsaved; i++) if (!bt_addr_le_cmp(&saved[i].a, a)) return true;
	return false;
}

static struct slot *slot_of_addr(const bt_addr_le_t *a)
{
	for (int i = 0; i < NH; i++) if (sl[i].conn && !bt_addr_le_cmp(&sl[i].a, a)) return &sl[i];
	return NULL;
}

static struct slot *slot_of(struct bt_conn *c)
{
	for (int i = 0; i < NH; i++) if (c && sl[i].conn == c) return &sl[i];
	return NULL;
}

bool hid_in_need_scan(void)
{
	if (pending_set) return true;
	for (int i = 0; i < nsaved; i++) if (!slot_of_addr(&saved[i].a)) return true;
	return false;
}

int hid_in_addrs(bt_addr_le_t *out, int max)
{
	int n = 0;

	for (int i = 0; i < nsaved && n < max; i++) out[n++] = saved[i].a;
	if (pending_set && n < max) out[n++] = pending;
	return n;
}

bool hid_in_active(void)
{
	for (int i = 0; i < NH; i++) if (sl[i].conn && (sl[i].kb || sl[i].ms)) return true;
	return false;
}

const struct hid_cfg *hid_in_cfg(void) { return &cfg; }

void hid_in_get(struct hid_in *o)
{
	int64_t now = k_uptime_get();

	memset(o, 0, sizeof(*o));
	k_spinlock_key_t k = k_spin_lock(&lk);
	int nk = 2;

	for (int i = 0; i < NH; i++) {
		if (!sl[i].conn) continue;
		if (sl[i].kb) {
			o->kb_on = true;
			o->kb[0] |= sl[i].kbr[0];
			for (int j = 2; j < 8 && nk < 8; j++) if (sl[i].kbr[j] > 3) o->kb[nk++] = sl[i].kbr[j];
		}
		if (sl[i].ms) o->ms_on = true;
	}
	o->mbtn = mbtn;
	o->dx = CLAMP(mdx, -32767, 32767);
	o->dy = CLAMP(mdy, -32767, 32767);
	o->wh = CLAMP(mwh, -127, 127);
	mdx = mdy = mwh = 0;
	/* fare durduysa (40 ms rapor yok) hız sıfır */
	if (now - m_last <= 40) { o->vx = CLAMP(vx, -32767, 32767); o->vy = CLAMP(vy, -32767, 32767); }
	k_spin_unlock(&lk, k);
}

/* ---------- raporlar ---------- */

static uint8_t kb_cb(struct bt_hogp *h, struct bt_hogp_rep_info *rep, uint8_t err, const uint8_t *data)
{
	struct slot *s = CONTAINER_OF(h, struct slot, hogp);
	size_t n = bt_hogp_rep_size(rep);

	if (err || !data) return BT_GATT_ITER_CONTINUE;
	k_spinlock_key_t k = k_spin_lock(&lk);

	memset(s->kbr, 0, 8);
	memcpy(s->kbr, data, MIN(n, 8));
	k_spin_unlock(&lk, k);
	out_kick();
	return BT_GATT_ITER_CONTINUE;
}

static uint8_t ms_cb(struct bt_hogp *h, struct bt_hogp_rep_info *rep, uint8_t err, const uint8_t *data)
{
	size_t n = bt_hogp_rep_size(rep);
	int64_t now = k_uptime_get();

	if (err || !data || n < 3) return BT_GATT_ITER_CONTINUE;
	int dx = (int8_t)data[1], dy = (int8_t)data[2], w = n > 3 ? (int8_t)data[3] : 0;
	k_spinlock_key_t k = k_spin_lock(&lk);

	mbtn = data[0] & 0x1f;
	mdx += dx;
	mdy += dy;
	mwh += w;
	/* hız: bu rapordaki hareket / önceki rapordan beri geçen süre (sayım/ms x 16) */
	int64_t dt = now - m_prev;

	if (dt < 4) dt = 4;
	if (dt > 40) dt = 40;
	if (dx || dy) {
		int32_t ix = dx * 16 / (int32_t)dt, iy = dy * 16 / (int32_t)dt;

		/* yumuşatma: önceki hızla ortalama (fare yeni hareket ettiyse doğrudan) */
		if (now - m_last > 40) { vx = ix; vy = iy; }
		else { vx = (vx + ix) / 2; vy = (vy + iy) / 2; }
		m_last = now;
	}
	m_prev = now;
	k_spin_unlock(&lk, k);
	out_kick();
	return BT_GATT_ITER_CONTINUE;
}

static void ready_cb(struct bt_hogp *h)
{
	struct slot *s = CONTAINER_OF(h, struct slot, hogp);
	char a[BT_ADDR_LE_STR_LEN];

	if (h->rep_boot.kbd_inp && !bt_hogp_rep_subscribe(h, h->rep_boot.kbd_inp, kb_cb)) s->kb = true;
	if (h->rep_boot.mouse_inp && !bt_hogp_rep_subscribe(h, h->rep_boot.mouse_inp, ms_cb)) s->ms = true;
	bt_hogp_pm_write(h, BT_HIDS_PM_BOOT);
	bt_addr_le_to_str(&s->a, a, sizeof(a));
	OUT("{\"ev\":\"hid\",\"addr\":\"%s\",\"ready\":1,\"kb\":%d,\"ms\":%d}\n", a, s->kb, s->ms);
}

static void prep_fail_cb(struct bt_hogp *h, int err) { OUT("{\"ev\":\"hid\",\"err\":%d}\n", err); }
static void pm_cb(struct bt_hogp *h) { }

static const struct bt_hogp_init_params hp = {
	.ready_cb = ready_cb,
	.prep_error_cb = prep_fail_cb,
	.pm_update_cb = pm_cb,
};

/* ---------- servis bulma (tek seferde bir cihaz; pad ile çakışırsa tekrar dener) ---------- */

static void dm_done(struct bt_gatt_dm *dm, void *ctx)
{
	struct slot *s = ctx;
	int err = bt_hogp_handles_assign(dm, &s->hogp);

	if (err) OUT("{\"ev\":\"hid\",\"err\":%d,\"where\":\"assign\"}\n", err);
	bt_gatt_dm_data_release(dm);
}
static void dm_nf(struct bt_conn *c, void *ctx) { OUT("{\"ev\":\"hid\",\"err\":0,\"where\":\"nohid\"}\n"); }
static void dm_err(struct bt_conn *c, int err, void *ctx) { OUT("{\"ev\":\"hid\",\"err\":%d,\"where\":\"disc\"}\n", err); }
static const struct bt_gatt_dm_cb dm_cb = { .completed = dm_done, .service_not_found = dm_nf, .error_found = dm_err };

static void disc_fn(struct k_work *w)
{
	struct slot *s = CONTAINER_OF(k_work_delayable_from_work(w), struct slot, disc);

	if (!s->conn) return;
	int err = bt_gatt_dm_start(s->conn, BT_UUID_HIDS, &dm_cb, s);

	if (err == -EALREADY || err == -EBUSY) k_work_reschedule(&s->disc, K_MSEC(400));
	else if (err) OUT("{\"ev\":\"hid\",\"err\":%d,\"where\":\"disc_start\"}\n", err);
}

/* ---------- bağlantı ---------- */

bool hid_in_try(const bt_addr_le_t *a, const char *name)
{
	bool known = hid_in_known(a);
	bool want = pending_set && !bt_addr_le_cmp(a, &pending);

	if ((!known && !want) || slot_of_addr(a)) return false;
	struct slot *s = NULL;

	for (int i = 0; i < NH; i++) if (!sl[i].conn) { s = &sl[i]; break; }
	if (!s) return false;
	if (want) {
		/* yeni cihaz: listeye ekle */
		pending_set = false;
		if (!known) {
			if (nsaved == NH) { memmove(&saved[0], &saved[1], sizeof(saved[0]) * (NH - 1)); nsaved--; }
			saved[nsaved].a = *a;
			strncpy(saved[nsaved].name, name ? name : "", sizeof(saved[0].name) - 1);
			saved[nsaved].name[sizeof(saved[0].name) - 1] = 0;
			nsaved++;
			save_list();
		}
	}
	bt_scan_stop();
	struct bt_le_conn_param *p = BT_LE_CONN_PARAM(6, 12, 0, 400);
	int err = bt_conn_le_create(a, BT_CONN_LE_CREATE_CONN, p, &s->conn);

	if (err) {
		s->conn = NULL;
		OUT("{\"ev\":\"hid\",\"err\":%d,\"where\":\"create\"}\n", err);
		scan_resume();
		return true;
	}
	s->a = *a;
	s->kb = s->ms = false;
	memset(s->kbr, 0, 8);
	return true;
}

static void free_slot(struct slot *s)
{
	k_spinlock_key_t k = k_spin_lock(&lk);

	s->kb = s->ms = false;
	memset(s->kbr, 0, 8);
	mbtn = 0;
	k_spin_unlock(&lk, k);
	k_work_cancel_delayable(&s->disc);
	if (bt_hogp_assign_check(&s->hogp)) bt_hogp_release(&s->hogp);
	if (s->conn) { bt_conn_unref(s->conn); s->conn = NULL; }
	out_kick();
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	struct slot *s = slot_of(conn);
	char a[BT_ADDR_LE_STR_LEN];

	if (!s) return;
	bt_addr_le_to_str(&s->a, a, sizeof(a));
	if (err) {
		OUT("{\"ev\":\"hid\",\"addr\":\"%s\",\"fail\":%u}\n", a, err);
		free_slot(s);
		scan_resume();
		return;
	}
	OUT("{\"ev\":\"hid\",\"addr\":\"%s\",\"connected\":1}\n", a);
	bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
	if (bt_conn_set_security(conn, BT_SECURITY_L2)) k_work_reschedule(&s->disc, K_NO_WAIT);
	scan_resume();
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct slot *s = slot_of(conn);
	char a[BT_ADDR_LE_STR_LEN];

	if (!s) return;
	bt_addr_le_to_str(&s->a, a, sizeof(a));
	OUT("{\"ev\":\"hid\",\"addr\":\"%s\",\"connected\":0,\"reason\":%u}\n", a, reason);
	free_slot(s);
	scan_resume();
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	struct slot *s = slot_of(conn);

	if (!s) return;
	k_work_reschedule(&s->disc, K_NO_WAIT);
}

static bool le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
	if (!slot_of(conn)) return true;
	/* fare/klavye daha yavaş aralık isteyebilir; 15 ms'ye kadar kabul, gecikmesiz */
	if (param->interval_min > 12) return false;
	if (param->interval_max > 12) param->interval_max = 12;
	param->latency = 0;
	return true;
}

BT_CONN_CB_DEFINE(hid_in_cb) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
	.le_param_req = le_param_req,
};

void hid_in_init(void)
{
	if (!cfg.sens) hid_cfg_default(&cfg);
	for (int i = 0; i < NH; i++) {
		bt_hogp_init(&sl[i].hogp, &hp);
		k_work_init_delayable(&sl[i].disc, disc_fn);
	}
}

/* ---------- komutlar ---------- */

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

static int hv(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static void print_list(void)
{
	char a[BT_ADDR_LE_STR_LEN];

	OUT("{\"ev\":\"hids\",\"mode\":%u,\"sens\":%u,\"list\":[", cfg.mode, cfg.sens);
	for (int i = 0; i < nsaved; i++) {
		struct slot *s = slot_of_addr(&saved[i].a);

		bt_addr_le_to_str(&saved[i].a, a, sizeof(a));
		OUT("%s{\"addr\":\"%s\",\"name\":\"%s\",\"on\":%d,\"kb\":%d,\"ms\":%d}", i ? "," : "", a, saved[i].name,
		    s != NULL, s && s->kb, s && s->ms);
	}
	OUT("],\"pending\":%d,\"map\":\"", pending_set);
	for (int i = 0; i < cfg.n; i++) OUT("%02x%02x", cfg.map[i][0], cfg.map[i][1]);
	OUT("\"}\n");
}

extern void scan_filters_changed(void);

bool hid_cmd(char *l)
{
	bt_addr_le_t a;

	if (strncmp(l, "hid ", 4)) return false;
	l += 4;
	if (!strcmp(l, "list")) {
	} else if (!strncmp(l, "use ", 4)) {
		if (parse(l + 4, &a)) return true;
		pending = a;
		pending_set = true;
		scan_filters_changed();
	} else if (!strncmp(l, "forget ", 7)) {
		if (parse(l + 7, &a)) return true;
		struct slot *s = slot_of_addr(&a);

		if (s) bt_conn_disconnect(s->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		for (int i = 0; i < nsaved; i++) {
			if (bt_addr_le_cmp(&saved[i].a, &a)) continue;
			memmove(&saved[i], &saved[i + 1], sizeof(saved[0]) * (nsaved - i - 1));
			nsaved--;
			break;
		}
		if (pending_set && !bt_addr_le_cmp(&pending, &a)) pending_set = false;
		save_list();
		bt_unpair(BT_ID_DEFAULT, &a);
		scan_filters_changed();
	} else if (!strncmp(l, "mode ", 5)) {
		cfg.mode = l[5] == '1';
		save_cfg();
	} else if (!strncmp(l, "sens ", 5)) {
		cfg.sens = CLAMP(atoi(l + 5), 1, 100);
		save_cfg();
	} else if (!strncmp(l, "map", 3)) {
		const char *p = l[3] == ' ' ? l + 4 : "";
		size_t n = strlen(p) / 4;

		if (!n) {
			uint8_t m = cfg.mode, s = cfg.sens;

			hid_cfg_default(&cfg);
			cfg.mode = m;
			cfg.sens = s;
		} else {
			cfg.n = 0;
			for (size_t i = 0; i < n && cfg.n < HMAP_MAX; i++) {
				int b[4];

				for (int j = 0; j < 4; j++) b[j] = hv(p[4 * i + j]);
				if (b[0] < 0 || b[1] < 0 || b[2] < 0 || b[3] < 0) break;
				cfg.map[cfg.n][0] = b[0] << 4 | b[1];
				cfg.map[cfg.n][1] = b[2] << 4 | b[3];
				cfg.n++;
			}
		}
		save_cfg();
	} else {
		return false;
	}
	print_list();
	return true;
}
