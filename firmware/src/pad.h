/* Kontrolcüden gelen veriyi standart bir gamepad durumuna çevirir. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Mantıksal tuşlar (çıkıştaki sıra = Android/Linux gamepad sırası) */
enum pad_btn {
	PB_A = 0, PB_B, PB_C, PB_X, PB_Y, PB_Z, PB_LB, PB_RB, PB_LT, PB_RT,
	PB_SELECT, PB_START, PB_HOME, PB_L3, PB_R3, PB_EXTRA,
	PB_UP, PB_DOWN, PB_LEFT, PB_RIGHT, /* yön tuşları (hat'tan) */
	PB_COUNT
};

struct pad_state {
	uint32_t btn;           /* bit = enum pad_btn */
	uint8_t lx, ly, rx, ry; /* 0..255, orta 128 */
	uint8_t lt, rt;         /* 0..255 */
	uint8_t hat;            /* 0..7, 8 = basılı değil */
	bool valid;
};

/* Tuş haritasını çöz. 0 = tamam */
int pad_parse_map(const uint8_t *map, size_t len);
/* Gelen bir raporu işle */
void pad_input(uint8_t report_id, const uint8_t *data, size_t len);
/* Son durumun kopyası */
void pad_get(struct pad_state *out);
/* Bağlantı kopunca sıfırla */
void pad_reset(void);
/* Kaç rapor işlendi (teşhis) */
uint32_t pad_rx_count(void);
/* Tuş tanıtmayla öğrenilmiş harita (14 baytlık girişler), n=0 kapatır */
void pad_set_learned(const uint8_t *e, int n);
bool pad_learned(void);
