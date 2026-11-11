/*
 * Telefondan kablosuz ayar.
 *
 *  - Kart, kendi servisini (PM_CFG_UUID) yayınlar. Hedef cihazlar için kullanılan kimlik yerine
 *    ikinci bir kimlikle (PM_CFG_ID) yayın yapar; böylece eşleşmiş PC/TV kendiliğinden bağlanmaz.
 *  - Yayın şu durumda açılır: kartta en az bir telefon anahtarı var, doğrulanmış telefon bağlı değil
 *    ve USB'den 5 sn'dir hiçbir uygulama dinlemiyor (USB'den ayar yapılıyorsa USB'den devam).
 *  - Telefon "nonce" yazar → kart rastgele sayı gönderir → telefon HMAC-SHA256(anahtar, sayı) ile
 *    "auth <hex>" yazar. Doğru değilse bağlantı kesilir. Anahtar kartta en fazla 4 tane saklanır ve
 *    sadece USB'den eklenebilir (uygulama USB'ye bağlanınca kendi anahtarını ekler).
 *  - Doğrulandıktan sonra USB seri porttaki komutların aynısı (dfu ve secret hariç) çalışır, OUT
 *    çıktıları telefona bildirim olarak gider.
 */
#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/crypto.h>
#include <zephyr/settings/settings.h>
#include "log.h"
#include "ble_cfg.h"
#include "ble_out.h"
#include "pm_hmac.h"

#define NKEY 4
static uint8_t keys[NKEY][32];
static uint8_t nkey;

static struct bt_conn *cc;      /* ayar istemcisi (doğrulanmamış da olabilir) */
static volatile bool authed;
static char nonce[33];
static int64_t last_usb;        /* USB'den en son dinlenen an */
static bool want;
static int64_t open_until;      /* tanıtma penceresi (yeni telefon anahtarını kablosuz bırakabilir) */

/* kapı: kayıtlı cihaz yoksa hep açık; varsa kart güce takıldıktan sonraki ilk 60 sn
 * (ya da kayıtlı bir cihazdan "Yeni cihaz ekle" denince 60 sn) yeni cihaz kendini tanıtabilir */
static bool open_now(void)
{
	int64_t n = k_uptime_get();

	return nkey == 0 || n < 60000 || n < open_until;
}
void ble_cfg_open_window(void)
{
	open_until = k_uptime_get() + 60000;
	OUT("{\"ev\":\"pairwin\",\"s\":60}\n");
	ble_out_adv_kick();
}

extern bool usb_listening(void);

/* ---------- saklama ---------- */

static int k_set(const char *key, size_t len, settings_read_cb rd, void *arg)
{
	if (!strcmp(key, "keys")) {
		uint8_t b[1 + sizeof(keys)];
		ssize_t n = rd(arg, b, sizeof(b));

		if (n >= 1 && b[0] <= NKEY && n >= 1 + b[0] * 32) {
			nkey = b[0];
			memcpy(keys, b + 1, nkey * 32);
		}
		return 0;
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(pmk, "pmk", NULL, k_set, NULL, NULL);

static void keys_save(void)
{
	uint8_t b[1 + sizeof(keys)];

	b[0] = nkey;
	memcpy(b + 1, keys, nkey * 32);
	settings_save_one("pmk/keys", b, 1 + nkey * 32);
}

/* ---------- gönderim (halka tampon → bildirim) ---------- */

RING_BUF_DECLARE(txr, 4096);
static struct k_spinlock txl;
static K_SEM_DEFINE(tx_sem, 0, 1);
extern const k_tid_t cfg_rx;

static bool tx_put(const char *s, size_t n, bool wait)
{
	for (int tries = 0; ; tries++) {
		k_spinlock_key_t k = k_spin_lock(&txl);
		bool ok = ring_buf_space_get(&txr) >= n;

		if (ok) ring_buf_put(&txr, (const uint8_t *)s, n);
		k_spin_unlock(&txl, k);
		if (ok) { k_sem_give(&tx_sem); return true; }
		if (!wait || !cc || tries > 1500) return false;
		k_sem_give(&tx_sem);
		k_sleep(K_MSEC(2));
	}
}

static void tx_reset(void)
{
	k_spinlock_key_t k = k_spin_lock(&txl);

	ring_buf_reset(&txr);
	k_spin_unlock(&txl, k);
}

void ble_cfg_vout(const char *fmt, va_list ap)
{
	char buf[200];
	va_list a2;

	if (!authed || !cc || k_is_in_isr()) return;
	va_copy(a2, ap);
	int n = vsnprintk(buf, sizeof(buf), fmt, a2);

	va_end(a2);
	if (n <= 0) return;
	/* büyük çıktıları (profil okuma) sadece ayar iş parçacığı bekleyerek yazar */
	bool wait = k_current_get() == cfg_rx;

	if (n < (int)sizeof(buf)) {
		tx_put(buf, n, wait);
		return;
	}
	char *big = k_malloc(n + 1);

	if (!big) return;
	vsnprintk(big, n + 1, fmt, ap);
	tx_put(big, n, wait);
	k_free(big);
}

static void reply(const char *s) { tx_put(s, strlen(s), false); }

/* ---------- GATT ---------- */

static struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(PM_CFG_UUID_BYTES);
static struct bt_uuid_128 rx_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x7a1e0002, 0x3c4b, 0x4a8e, 0x9c1d, 0x50ad2840f00d));
static struct bt_uuid_128 tx_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x7a1e0003, 0x3c4b, 0x4a8e, 0x9c1d, 0x50ad2840f00d));

K_MSGQ_DEFINE(lines, 520, 4, 4);
static char lb[520];
static size_t ln;

static bool add_key(const char *hex);

static void kill_fn(struct k_work *w)
{
	if (cc && !authed) bt_conn_disconnect(cc, BT_HCI_ERR_AUTH_FAIL);
}
static K_WORK_DELAYABLE_DEFINE(kill, kill_fn);

static void on_line(struct bt_conn *conn, char *l)
{
	char m[80];

	if (!strcmp(l, "nonce")) {
		uint8_t r[16];

		if (cc && cc != conn && authed) return; /* başka bir telefon ayarda */
		if (cc != conn) {
			if (cc) bt_conn_unref(cc);
			cc = bt_conn_ref(conn);
			tx_reset();
		}
		authed = false;
		bt_rand(r, sizeof(r));
		for (int i = 0; i < 16; i++) sprintf(nonce + 2 * i, "%02x", r[i]);
		snprintf(m, sizeof(m), "{\"ev\":\"auth\",\"n\":\"%s\",\"keys\":%u,\"open\":%d}\n", nonce, nkey, open_now());
		reply(m);
		return;
	}
	if (conn != cc) return;
	if (!strncmp(l, "pair ", 5)) {
		/* tanıtma penceresi açıkken telefon kendi anahtarını bırakır, sonra normal doğrulama yapar */
		if (!open_now() || !add_key(l + 5)) {
			reply("{\"ev\":\"auth\",\"paired\":0}\n");
			return;
		}
		reply("{\"ev\":\"auth\",\"paired\":1}\n");
		OUT("{\"ev\":\"secret\",\"ok\":1,\"n\":%u,\"new\":1}\n", nkey);
		return;
	}
	if (!strncmp(l, "auth ", 5)) {
		uint8_t mac[32];
		char hx[65];
		bool ok = false;

		for (char *q = l + 5; *q; q++) if (*q >= 'A' && *q <= 'F') *q += 'a' - 'A';

		for (int k = 0; k < nkey && nonce[0] && !ok; k++) {
			pm_hmac_sha256(keys[k], 32, (const uint8_t *)nonce, 32, mac);
			for (int i = 0; i < 32; i++) sprintf(hx + 2 * i, "%02x", mac[i]);
			ok = strlen(l + 5) == 64 && !strncmp(hx, l + 5, 64);
		}
		nonce[0] = 0; /* her sayı tek kullanımlık */
		if (!ok) {
			reply("{\"ev\":\"auth\",\"ok\":0}\n");
			/* yanıt gitsin, sonra kes */
			k_work_reschedule(&kill, K_MSEC(400));
			return;
		}
		authed = true;
		reply("{\"ev\":\"auth\",\"ok\":1}\n");
		/* telefonla düşük gecikmeli bağlantı: aktarım hızlı olsun */
		struct bt_le_conn_param p = BT_LE_CONN_PARAM_INIT(6, 24, 0, 400);

		bt_conn_le_param_update(conn, &p);
		ble_out_adv_kick();
		return;
	}
	if (!authed) return;
	/* karttaki yükleyiciye geçiş ve anahtar işlemleri sadece kablodan */
	if (!strcmp(l, "dfu") || !strncmp(l, "secret", 6)) {
		reply("{\"ev\":\"err\",\"where\":\"ble\",\"msg\":\"usb\"}\n");
		return;
	}
	if (k_msgq_put(&lines, l, K_MSEC(300))) reply("{\"ev\":\"err\",\"where\":\"ble\",\"msg\":\"busy\"}\n");
}

static ssize_t rx_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			uint16_t len, uint16_t offset, uint8_t flags)
{
	const char *p = buf;

	if (cc && conn != cc && authed) return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
	for (uint16_t i = 0; i < len; i++) {
		char ch = p[i];

		if (ch == '\n' || ch == '\r') {
			if (ln) {
				lb[ln] = 0;
				ln = 0;
				on_line(conn, lb);
			}
		} else if (ln < sizeof(lb) - 1) {
			lb[ln++] = ch;
		}
	}
	return len;
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) { k_sem_give(&tx_sem); }

BT_GATT_SERVICE_DEFINE(pm_cfg_svc,
	BT_GATT_PRIMARY_SERVICE(&svc_uuid),
	BT_GATT_CHARACTERISTIC(&rx_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, rx_write, NULL),
	BT_GATT_CHARACTERISTIC(&tx_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

static void tx_thread(void *a, void *b, void *c)
{
	static uint8_t chunk[244];

	while (1) {
		k_sem_take(&tx_sem, K_MSEC(100));
		while (1) {
			struct bt_conn *conn = cc;

			if (!conn || !bt_gatt_is_subscribed(conn, &pm_cfg_svc.attrs[4], BT_GATT_CCC_NOTIFY)) break;
			uint16_t mtu = bt_gatt_get_mtu(conn);
			uint32_t max = MIN(sizeof(chunk), mtu > 3 ? mtu - 3u : 20u);
			k_spinlock_key_t k = k_spin_lock(&txl);
			uint32_t n = ring_buf_peek(&txr, chunk, max);

			k_spin_unlock(&txl, k);
			if (!n) break;
			int err = bt_gatt_notify(conn, &pm_cfg_svc.attrs[4], chunk, n);

			if (err == -ENOMEM || err == -ENOBUFS || err == -EAGAIN) {
				k_sleep(K_MSEC(3));
				continue;
			}
			k = k_spin_lock(&txl);
			ring_buf_get(&txr, NULL, n);
			k_spin_unlock(&txl, k);
			if (err) break;
		}
	}
}
K_THREAD_DEFINE(cfg_tx, 1536, tx_thread, NULL, NULL, NULL, 8, 0, 0);

/* ---------- durum ---------- */

bool ble_cfg_is(struct bt_conn *c) { return c && c == cc && authed; }
bool ble_cfg_authed(void) { return cc && authed; }
bool ble_cfg_want_adv(void) { return want; }

void ble_cfg_disconnected(struct bt_conn *c)
{
	if (c != cc) return;
	authed = false;
	bt_conn_unref(cc);
	cc = NULL;
	ln = 0;
	nonce[0] = 0;
	tx_reset();
	ble_out_adv_kick();
}

bool ble_cfg_line(char *buf, size_t n)
{
	return n >= 520 && k_msgq_get(&lines, buf, K_NO_WAIT) == 0;
}

static void tick_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(tick, tick_fn);
static void tick_fn(struct k_work *w)
{
	int64_t now = k_uptime_get();

	if (usb_listening()) last_usb = now;
	/* USB'de dinleyen uygulama yoksa kart ayar için Bluetooth'ta görünür */
	bool w2 = !ble_cfg_authed() && now - last_usb >= 5000;

	if (w2 != want) {
		want = w2;
		ble_out_adv_kick();
	}
	k_work_reschedule(&tick, K_SECONDS(1));
}

void ble_cfg_init(void)
{
	bt_addr_le_t ids[2];
	size_t cnt = 2;

	bt_id_get(ids, &cnt);
	if (cnt < 2) bt_id_create(NULL, NULL);
	last_usb = k_uptime_get();
	k_work_reschedule(&tick, K_SECONDS(1));
}

/* ---------- USB komutları ---------- */

static int hv(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static bool add_key(const char *p)
{
	uint8_t k[32];

	if (strlen(p) != 64) return false;
	for (int i = 0; i < 32; i++) {
		int h = hv(p[2 * i]), lo = hv(p[2 * i + 1]);

		if (h < 0 || lo < 0) return false;
		k[i] = h << 4 | lo;
	}
	for (int i = 0; i < nkey; i++) {
		if (!memcmp(keys[i], k, 32)) { OUT("{\"ev\":\"secret\",\"ok\":1,\"n\":%u}\n", nkey); return true; }
	}
	if (nkey == NKEY) { /* en eskiyi at */
		memmove(keys[0], keys[1], 32 * (NKEY - 1));
		nkey--;
	}
	memcpy(keys[nkey++], k, 32);
	keys_save();
	OUT("{\"ev\":\"secret\",\"ok\":1,\"n\":%u,\"new\":1}\n", nkey);
	return true;
}

bool ble_cfg_cmd(char *l)
{
	if (strncmp(l, "secret", 6)) return false;
	if (!strncmp(l, "secret add ", 11)) {
		if (!add_key(l + 11)) goto bad;
		return true;
	}
	if (!strcmp(l, "secret clear")) {
		nkey = 0;
		keys_save();
		if (cc) bt_conn_disconnect(cc, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		OUT("{\"ev\":\"secret\",\"ok\":1,\"n\":0}\n");
		return true;
	}
	if (!strcmp(l, "secret open")) {
		ble_cfg_open_window();
		return true;
	}
	if (!strcmp(l, "secret count")) {
		OUT("{\"ev\":\"secret\",\"n\":%u}\n", nkey);
		return true;
	}
bad:
	OUT("{\"ev\":\"secret\",\"ok\":0}\n");
	return true;
}
