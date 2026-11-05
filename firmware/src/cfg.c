/*
 * Ayar ekranıyla konuşma (USB seri) ve ayarı kartın hafızasında saklama.
 *
 * Ekrandan karta (satır satır):
 *   hello            → {"ev":"hello","fw":"..","cfg":uzunluk,"crc":"..","prof":n,"mapping":0/1}
 *   cb <uzunluk>     ayar gönderimi başlıyor
 *   cd <hex>         ayar parçası
 *   ce <crc32 hex>   bitti: denetle, kaydet, uygula → {"ev":"cfg","ok":1} ya da {"ev":"cfg","ok":0,"err":..}
 *   cs <n>           profil seç → {"ev":"prof","cur":n}
 *   map <0|1>        eşlemeyi kapat / aç
 */
#include <string.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include "log.h"
#include "engine.h"
#include "cfg.h"

#define CHUNK 1024

static uint8_t blob[CFG_MAX];
static size_t blob_len;
static uint8_t rx[CFG_MAX];
static size_t rx_len, rx_expect;
static int saved_prof = -1;

/* ---------- hafıza ---------- */

static int h_set(const char *key, size_t len, settings_read_cb read_cb, void *arg)
{
	if (!strcmp(key, "len")) {
		uint16_t v = 0;

		if (read_cb(arg, &v, sizeof(v)) == sizeof(v) && v <= CFG_MAX) blob_len = v;
		return 0;
	}
	if (!strcmp(key, "prof")) {
		uint8_t v;

		if (read_cb(arg, &v, 1) == 1) saved_prof = v;
		return 0;
	}
	if (key[0] == 'c') {
		int n = atoi(key + 1);

		if (n >= 0 && (n + 1) * CHUNK <= CFG_MAX && len <= CHUNK) read_cb(arg, blob + n * CHUNK, len);
		return 0;
	}
	return -ENOENT;
}

static int h_commit(void)
{
	if (blob_len && engine_set_config(blob, blob_len) == 0) {
		if (saved_prof >= 0) engine_select_profile(saved_prof);
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(pm, "pm", NULL, h_set, h_commit, NULL);

static int store(const uint8_t *b, size_t len)
{
	char key[12];
	uint16_t l = len;
	int err;

	for (size_t off = 0, n = 0; off < len; off += CHUNK, n++) {
		snprintk(key, sizeof(key), "pm/c%u", (unsigned)n);
		err = settings_save_one(key, b + off, MIN(CHUNK, len - off));
		if (err) return err;
	}
	return settings_save_one("pm/len", &l, sizeof(l));
}

static void prof_save(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(prof_work, prof_save);
static void prof_save(struct k_work *w)
{
	uint8_t v = engine_profile();

	settings_save_one("pm/prof", &v, 1);
}

void engine_profile_changed(int n)
{
	k_work_reschedule(&prof_work, K_MSEC(1500));
}

/* ---------- komutlar ---------- */

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static void hello(void)
{
	OUT("{\"ev\":\"hello\",\"fw\":\"%s\",\"cfg\":%u,\"crc\":\"%08x\",\"prof\":%d,\"nprof\":%d,\"mapping\":%d}\n",
	    FW_VERSION, (unsigned)blob_len, blob_len ? crc32_ieee(blob, blob_len) : 0,
	    engine_profile(), engine_profile_count(), engine_mapping());
}

extern bool bt_cmd(char *l);

static void cmd(char *l)
{
	if (bt_cmd(l)) return;
	if (!strcmp(l, "hello")) {
		hello();
	} else if (!strncmp(l, "cb ", 3)) {
		rx_expect = strtoul(l + 3, NULL, 10);
		rx_len = 0;
		if (rx_expect > CFG_MAX) {
			OUT("{\"ev\":\"cfg\",\"ok\":0,\"err\":\"büyük\"}\n");
			rx_expect = 0;
		}
	} else if (!strncmp(l, "cd ", 3)) {
		for (char *p = l + 3; p[0] && p[1] && rx_len < rx_expect; p += 2) {
			int h = hexval(p[0]), lo = hexval(p[1]);

			if (h < 0 || lo < 0) break;
			rx[rx_len++] = (h << 4) | lo;
		}
	} else if (!strncmp(l, "ce ", 3)) {
		uint32_t want = strtoul(l + 3, NULL, 16);
		uint32_t got = crc32_ieee(rx, rx_len);
		int e;

		if (!rx_expect || rx_len != rx_expect || want != got) {
			OUT("{\"ev\":\"cfg\",\"ok\":0,\"err\":\"eksik\",\"len\":%u,\"want\":%u}\n",
			    (unsigned)rx_len, (unsigned)rx_expect);
			return;
		}
		e = engine_check(rx, rx_len);
		if (e) {
			OUT("{\"ev\":\"cfg\",\"ok\":0,\"err\":\"bicim\",\"code\":%d}\n", e);
			return;
		}
		memcpy(blob, rx, rx_len);
		blob_len = rx_len;
		engine_set_config(blob, blob_len);
		e = store(blob, blob_len);
		rx_expect = 0;
		OUT("{\"ev\":\"cfg\",\"ok\":1,\"saved\":%d}\n", e == 0);
		hello();
	} else if (!strncmp(l, "cs ", 3)) {
		if (engine_select_profile(atoi(l + 3)) == 0) {
			engine_profile_changed(engine_profile());
			OUT("{\"ev\":\"prof\",\"cur\":%d}\n", engine_profile());
		}
	} else if (!strncmp(l, "map ", 4)) {
		engine_set_mapping(l[4] == '1');
		OUT("{\"ev\":\"mode\",\"mapping\":%d}\n", engine_mapping());
	}
}

static void rx_thread(void *a, void *b, void *c)
{
	const struct device *con = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	static char line[600];
	size_t n = 0;
	unsigned char ch;

	while (1) {
		if (!device_is_ready(con) || uart_poll_in(con, &ch) != 0) {
			k_sleep(K_MSEC(3));
			continue;
		}
		if (ch == '\n' || ch == '\r') {
			if (n) {
				line[n] = 0;
				cmd(line);
				n = 0;
			}
		} else if (n < sizeof(line) - 1) {
			line[n++] = ch;
		}
	}
}

K_THREAD_DEFINE(cfg_rx, 2048, rx_thread, NULL, NULL, NULL, 7, 0, 0);
