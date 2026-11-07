/*
 * Tablete giden USB çıkışı: gamepad + klavye + fare.
 * Arayüz sırası: klavye, fare, gamepad. Açılışta eşleme AÇIK.
 * Eşleme KAPALI: D8 Pro tablete normal gamepad olarak gider.
 * Eşleme AÇIK:   şimdilik yerleşik deneme ayarı (ayar ekranından yazma sonraki adım).
 * Aç/kapat:      Home + Select 1,5 sn basılı tut. Işık: mavi = gamepad, yeşil = eşleme.
 */
#include <string.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#include "log.h"
#include "pad.h"
#include "out.h"
#include "engine.h"
#include "ble_out.h"

/* ---------- HID tanımları ---------- */

static const uint8_t gamepad_desc[] = {
	0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,
	/* 16 düğme */
	0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
	/* yön (hat) + 4 bit boşluk */
	0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3b, 0x01,
	0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
	0x65, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x01,
	/* X Y Z Rz */
	0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26, 0xff, 0x00,
	0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
	/* fren (LT) + gaz (RT) */
	0x05, 0x02, 0x09, 0xc5, 0x09, 0xc4, 0x15, 0x00, 0x26, 0xff, 0x00,
	0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
	0xc0,
};

static const uint8_t keyboard_desc[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
	0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
	0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
	0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
	0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
	0xc0,
};

static const uint8_t mouse_desc[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
	0xc0, 0xc0,
};

#define mapping_on engine_mapping()
static bool pad_connected;

enum { D_KB, D_MOUSE, D_PAD, D_N };
static volatile bool usb_cfg, usb_susp;
static int64_t last_ok_ms, last_err_ms, last_any_ms;
static bool keepalive;
static uint32_t tx_ok, tx_err, tx_busy, tx_unstuck, tx_wake;
static int64_t busy_since[3];
static const struct device *hid[D_N];
static struct k_sem ready[D_N];
static uint8_t last[D_N][9];
static uint8_t last_len[D_N];

static void in_ready(const struct device *dev)
{
	for (int i = 0; i < D_N; i++) {
		if (hid[i] == dev) k_sem_give(&ready[i]);
	}
}

/* Tablet klavye ışığı (NumLock vb.) bilgisini yollar; kabul et, yoksa bazı sistemler klavyeyi bırakır */
static int set_report(const struct device *dev, struct usb_setup_packet *setup, int32_t *len, uint8_t **data)
{
	return 0;
}

static int get_report(const struct device *dev, struct usb_setup_packet *setup, int32_t *len, uint8_t **data)
{
	static uint8_t zero[9];

	for (int i = 0; i < D_N; i++) {
		if (hid[i] == dev) {
			*data = zero;
			*len = i == D_KB ? 8 : (i == D_MOUSE ? 4 : 9);
			return 0;
		}
	}
	return -ENOTSUP;
}

static const struct hid_ops ops = {
	.int_in_ready = in_ready,
	.set_report = set_report,
	.get_report = get_report,
};

int out_usb_init(void)
{
	static const char *names[D_N] = { "HID_0", "HID_1", "HID_2" };
	static const uint8_t *descs[D_N] = { keyboard_desc, mouse_desc, gamepad_desc };
	static const size_t sizes[D_N] = { sizeof(keyboard_desc), sizeof(mouse_desc), sizeof(gamepad_desc) };

	for (int i = 0; i < D_N; i++) {
		hid[i] = device_get_binding(names[i]);
		if (!hid[i]) {
			OUT("{\"ev\":\"err\",\"where\":\"hid_dev\",\"code\":%d}\n", i);
			return -ENODEV;
		}
		k_sem_init(&ready[i], 1, 1);
		usb_hid_register_device(hid[i], descs[i], sizes[i], &ops);
		usb_hid_init(hid[i]);
	}
	return 0;
}

void out_usb_status(int status)
{
	if (status == USB_DC_CONFIGURED) {
		for (int i = 0; i < D_N; i++) {
			k_sem_reset(&ready[i]);
			k_sem_give(&ready[i]);
			last_len[i] = 0; /* her şeyi yeniden gönder */
		}
		usb_cfg = true;
		usb_susp = false;
		ble_out_usb(true);
	} else if (status == USB_DC_RESET || status == USB_DC_DISCONNECTED) {
		usb_cfg = false;
		usb_susp = false;
		if (status == USB_DC_DISCONNECTED) ble_out_usb(false);
	} else if (status == USB_DC_SUSPEND) {
		usb_susp = true;
	} else if (status == USB_DC_RESUME) {
		usb_susp = false;
	}
	OUT("{\"ev\":\"usb\",\"status\":%d,\"configured\":%d}\n", status, usb_cfg);
}

/* force: değişmese de gönder (fare hareketi gibi) */
static void send(int i, const uint8_t *buf, uint8_t len, bool force)
{
	uint32_t wrote;
	int err;

	if (!force && last_len[i] == len && !memcmp(last[i], buf, len)) return;
	if (usb_susp) {
		/* tablet USB'yi uyuttu: yeni veri var, uyandır; uyanınca gönderilir */
		static int64_t last_wake;
		int64_t now = k_uptime_get();

		if (now - last_wake > 30) {
			last_wake = now;
			if (usb_wakeup_request() == 0) tx_wake++;
		}
		return;
	}
	if (k_sem_take(&ready[i], K_NO_WAIT)) {
		/* önceki gönderim bitmedi; çok uzun sürdüyse kilidi aç */
		tx_busy++;
		if (!busy_since[i]) busy_since[i] = k_uptime_get();
		if (k_uptime_get() - busy_since[i] > 100) {
			busy_since[i] = 0;
			tx_unstuck++;
			k_sem_give(&ready[i]);
		}
		return;
	}
	busy_since[i] = 0;
	err = hid_int_ep_write(hid[i], buf, len, &wrote);
	if (err) {
		tx_err++;
		last_err_ms = k_uptime_get();
		k_sem_give(&ready[i]);
		return;
	}
	tx_ok++;
	last_any_ms = k_uptime_get();
	if (keepalive) { memcpy(last[i], buf, len); last_len[i] = len; return; }
	if (i != D_PAD || mapping_on == false) last_ok_ms = k_uptime_get();
	if (i == D_KB) {
		OUT("{\"ev\":\"kb\",\"mod\":%u,\"k\":[%u,%u,%u,%u,%u,%u]}\n",
		    buf[0], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
	}
	memcpy(last[i], buf, len);
	last_len[i] = len;
}

void out_print_stats(void)
{
	OUT("{\"ev\":\"stats\",\"usb\":%d,\"pad\":%d,\"mapping\":%d,\"rx\":%u,\"tx_ok\":%u,\"tx_err\":%u,\"tx_busy\":%u,\"unstuck\":%u,\"wake\":%u}\n",
	    usb_cfg, pad_connected, mapping_on, pad_rx_count(), tx_ok, tx_err, tx_busy, tx_unstuck, tx_wake);
}

/* ---------- ışık ---------- */

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay) && DT_NODE_HAS_STATUS(DT_ALIAS(led1), okay) && DT_NODE_HAS_STATUS(DT_ALIAS(led2), okay)
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#define HAVE_LED 1
#else
#define HAVE_LED 0
#endif


/* Tuşlar nereye: bt_side = Bluetooth tarafı seçili (ya da kablo yok); use_bt = şu an Bluetooth'tan gidiyor */
static volatile bool bt_side, use_bt;
static int64_t bt_wait_since;

static void route_tick(int64_t now)
{
	bool side = ble_out_route_bt() || !usb_cfg;

	if (side && !bt_side) bt_wait_since = now;
	bt_side = side;
	if (ble_out_ready()) bt_wait_since = now;
	use_bt = side && ble_out_ready();
	/* Bluetooth hedefi 10 sn bulunamazsa ve kablo varsa kabloya dön */
	if (side && !use_bt && usb_cfg && now - bt_wait_since > 10000) bt_side = false;
}

static uint8_t bt_last[3][9], bt_len[3];

static void bt_send(int which, const uint8_t *buf, uint8_t len, bool force)
{
	if (!force && bt_len[which] == len && !memcmp(bt_last[which], buf, len)) return;
	if (ble_out_send(which, buf, len) == 0) {
		memcpy(bt_last[which], buf, len);
		bt_len[which] = len;
		last_ok_ms = k_uptime_get();
	}
}

static void led_update(uint32_t tick)
{
#if HAVE_LED
	int64_t now = k_uptime_get();
	bool on = pad_connected || (tick / 50) % 2; /* pad bağlı değilken yanıp söner */
	/* tablete gönderirken kısa söner: veri gidiyor demek */
	if (last_ok_ms && now - last_ok_ms < 50) on = false;
	/* kırmızı: tablet kartı hazırlamadı (sabit) veya uyuttu (yanıp söner) ya da gönderim hatası */
	bool err = last_err_ms > last_ok_ms && now - last_err_ms < 1000;
	bool bt = use_bt;
	bool red = !bt && (((!usb_cfg && !usb_susp) || err || (usb_susp && (tick / 25) % 2)));

	if (bt_side) {
		/* turkuaz: tuşlar Bluetooth'tan hedefe gidiyor; yanıp sönüyorsa hedef aranıyor */
		bool blink = !ble_out_ready() && (tick / 30) % 2;

		gpio_pin_set_dt(&led_g, on && !blink);
		gpio_pin_set_dt(&led_b, on && !blink);
		gpio_pin_set_dt(&led_r, false);
		return;
	}
	gpio_pin_set_dt(&led_g, on && mapping_on && !red);
	gpio_pin_set_dt(&led_b, on && !mapping_on && !red);
	gpio_pin_set_dt(&led_r, red);
#endif
}

void out_set_connected(bool c)
{
	pad_connected = c;
}

/* ---------- yerleşik deneme ayarı (eşleme açıkken) ---------- */

#define K_A 4
#define K_W 26
#define K_S 22
#define K_D 7
#define K_E 8
#define K_R 21
#define K_Q 20
#define K_F 9
#define K_V 25
#define K_SPACE 44
#define K_ESC 41
#define K_ENTER 40
#define K_TAB 43
#define K_RIGHT 79
#define K_LEFT 80
#define K_DOWN 81
#define K_UP 82
#define MOD_LCTRL 0x01
#define MOD_LSHIFT 0x02

static const struct { uint8_t btn; uint8_t key; uint8_t mod; } demo_keys[] = {
	{ PB_A, K_SPACE, 0 }, { PB_B, K_ESC, 0 }, { PB_X, K_R, 0 }, { PB_Y, K_Q, 0 },
	{ PB_RB, K_F, 0 }, { PB_LB, 0, MOD_LSHIFT }, { PB_L3, 0, MOD_LCTRL }, { PB_R3, K_V, 0 },
	{ PB_START, K_ENTER, 0 }, { PB_SELECT, K_TAB, 0 },
	{ PB_UP, K_UP, 0 }, { PB_DOWN, K_DOWN, 0 }, { PB_LEFT, K_LEFT, 0 }, { PB_RIGHT, K_RIGHT, 0 },
};

static void kb_add(uint8_t *rep, int *n, uint8_t key)
{
	if (key && *n < 6) rep[2 + (*n)++] = key;
}

#define MOUSE_MAX 28.0f   /* tam itişte 8 ms'de piksel (≈3500 px/sn) */
static float acc_x, acc_y;

static int8_t stick_mouse(uint8_t v, float *acc)
{
	float d = (float)v - 128.0f;
	const float dead = 12.0f;

	if (d > -dead && d < dead) { *acc = 0; return 0; }
	d = d > 0 ? d - dead : d + dead;
	float n = d / 116.0f;                       /* -1..1 */
	float a = n < 0 ? -n : n;
	/* eğri: küçük itişte hassas, tam itişte hızlı */
	*acc += n * (0.35f * a + 0.65f * a * a) * MOUSE_MAX + (n > 0 ? 0.15f : -0.15f);
	int out = (int)*acc;
	*acc -= out;
	return CLAMP(out, -127, 127);
}

static void build_mapped(const struct pad_state *p, uint8_t kb[8], uint8_t ms[4])
{
	int n = 0;

	memset(kb, 0, 8);
	memset(ms, 0, 4);
	for (size_t i = 0; i < ARRAY_SIZE(demo_keys); i++) {
		if (p->btn & BIT(demo_keys[i].btn)) {
			kb[0] |= demo_keys[i].mod;
			kb_add(kb, &n, demo_keys[i].key);
		}
	}
	/* sol çubuk → W A S D */
	if (p->ly < 64) kb_add(kb, &n, K_W);
	if (p->ly > 192) kb_add(kb, &n, K_S);
	if (p->lx < 64) kb_add(kb, &n, K_A);
	if (p->lx > 192) kb_add(kb, &n, K_D);
	/* sağ çubuk → fare, tetikler → tık */
	ms[1] = (uint8_t)stick_mouse(p->rx, &acc_x);
	ms[2] = (uint8_t)stick_mouse(p->ry, &acc_y);
	if (p->rt > 100 || (p->btn & BIT(PB_RT))) ms[0] |= 1;
	if (p->lt > 100 || (p->btn & BIT(PB_LT))) ms[0] |= 2;
}

/* ---------- ana döngü (yaklaşık 125 Hz) ---------- */

static K_SEM_DEFINE(kick, 0, 1);

/* pad'den rapor gelince çağrılır: 8 ms beklemeden hesapla */
void out_kick(void) { k_sem_give(&kick); }

static void out_thread(void *a, void *b, void *c)
{
	struct pad_state p;
	uint32_t tick = 0;
	static const uint8_t g_idle[9] = { 0, 0, 0x0f, 128, 128, 128, 128, 0, 0 };
	static const uint8_t z8[8] = { 0 };

#if HAVE_LED
	gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_b, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
#endif
	pad_reset();

	while (1) {
		/* pad'den yeni rapor gelirse hemen, yoksa en geç 8 ms'de bir */
		k_sem_take(&kick, K_MSEC(8));
		{
			static int64_t last_led;
			int64_t n = k_uptime_get();

			if (n - last_led >= 8) { last_led = n; tick++; }
		}
		led_update(tick);
		pad_get(&p);

		route_tick(k_uptime_get());
		if (!p.valid || !pad_connected) {
			send(D_PAD, g_idle, 9, false);
			send(D_KB, z8, 8, false);
			send(D_MOUSE, z8, 4, false);
			continue;
		}

		/* Home + Start 2 sn: tuşlar kablo <-> Bluetooth */
		{
			static int64_t since;
			static bool done;
			uint32_t hs = BIT(PB_HOME) | BIT(PB_START);

			if ((p.btn & hs) == hs) {
				if (!since) since = k_uptime_get();
				if (!done && k_uptime_get() - since > 2000) {
					done = true;
					ble_out_set_route(!ble_out_route_bt());
				}
			} else {
				since = 0;
				done = false;
			}
		}

		struct out_rep r;
		enum eng_mode m = engine_tick(&p, k_uptime_get(), &r);

		if (m == E_LEGACY) {
			build_mapped(&p, r.kb, r.ms);
			memcpy(r.g, g_idle, 9);
		}

		route_tick(k_uptime_get());
		if (bt_side) {
			/* Bluetooth tarafı: takılı cihaza (telefon/kumanda) hiçbir tuş gitmez */
			if (use_bt) {
				bt_send(2, r.g, 9, false);
				bt_send(0, r.kb, 8, false);
				bt_send(1, r.ms, 4, r.ms[1] || r.ms[2] || r.ms[3]);
			}
			send(D_PAD, g_idle, 9, false);
			send(D_KB, z8, 8, false);
			send(D_MOUSE, z8, 4, false);
			continue;
		}
		memset(bt_len, 0, sizeof(bt_len));

		/* 150 ms'dir hiçbir şey gitmediyse aynı gamepad durumunu tekrar gönder:
		 * tablet bağlantıyı boşta sanıp uyutmasın (uyanma gecikmesi olmasın) */
		if (k_uptime_get() - last_any_ms > 150) {
			keepalive = true;
			send(D_PAD, r.g, 9, true);
			keepalive = false;
		}
		send(D_PAD, r.g, 9, false);
		send(D_KB, r.kb, 8, false);
		send(D_MOUSE, r.ms, 4, r.ms[1] || r.ms[2] || r.ms[3]);
	}
}

K_THREAD_STACK_DEFINE(out_stack, 2048);
static struct k_thread out_th;

void out_start(void)
{
	k_thread_create(&out_th, out_stack, K_THREAD_STACK_SIZEOF(out_stack), out_thread,
			NULL, NULL, NULL, 5, 0, K_NO_WAIT);
}
