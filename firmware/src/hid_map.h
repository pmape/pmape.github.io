/* Bluetooth klavye/fare girişini çıkışa ya da sanal gamepad'e çevirme (Zephyr'siz, bilgisayarda da test edilir) */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "pad.h"
#include "engine.h"

/* hedefler: 0..19 pad tuşları (pad.h sırası), 20..23 sol çubuk yukarı/aşağı/sol/sağ, 24..27 sağ çubuk */
#define HT_LS 20
#define HT_RS 24
#define HT_COUNT 28
/* fare tuşları kaynak kodu: 0xF0 sol, 0xF1 sağ, 0xF2 orta, 0xF3 geri, 0xF4 ileri */
#define HK_MOUSE 0xF0
#define HMAP_MAX 48

struct hid_in {
	uint8_t kb[8];          /* birleşik klavye (boot biçimi): değiştiriciler, 0, 6 tuş */
	uint8_t mbtn;           /* fare tuşları */
	int16_t dx, dy, wh;     /* son okumadan beri fare hareketi */
	int16_t vx, vy;         /* fare hızı (sayım/ms x 16), duruyorsa 0 */
	bool kb_on, ms_on;      /* bağlı mı */
};

struct hid_cfg {
	uint8_t mode;           /* 0 doğrudan geçir, 1 gamepad'e çevir */
	uint8_t sens;           /* fare → sağ çubuk hassasiyeti 1..100 */
	uint8_t n;
	uint8_t map[HMAP_MAX][2]; /* kaynak kod, hedef */
};

void hid_cfg_default(struct hid_cfg *c);
/* gamepad modunda: tuşları/fareyi pad durumuna ekler; kalan (atanmamış) tuşları geri verir */
void hid_to_pad(const struct hid_cfg *c, const struct hid_in *in, struct pad_state *p, uint8_t rest_kb[8], uint8_t *rest_mbtn);
/* klavye/fare raporunu çıkışa ekle */
void hid_merge_out(const uint8_t kb[8], uint8_t mbtn, int dx, int dy, int wh, struct out_rep *r);
