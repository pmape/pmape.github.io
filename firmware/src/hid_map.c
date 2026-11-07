#include <string.h>
#include <math.h>
#include "hid_map.h"

void hid_cfg_default(struct hid_cfg *c)
{
	/* nişancı oyunları için alışılmış düzen */
	static const uint8_t def[][2] = {
		{ 0x1a, HT_LS + 0 }, { 0x16, HT_LS + 1 }, { 0x04, HT_LS + 2 }, { 0x07, HT_LS + 3 }, /* W S A D */
		{ 0x2c, PB_A }, { 0xe0, PB_B }, { 0x15, PB_X }, { 0x08, PB_Y },                       /* Boşluk, Ctrl, R, E */
		{ 0x14, PB_LB }, { 0x09, PB_RB }, { 0xe1, PB_L3 }, { 0x19, PB_R3 },                   /* Q F Shift V */
		{ 0x2b, PB_SELECT }, { 0x29, PB_START }, { 0x0b, PB_HOME },                            /* Tab Esc H */
		{ 0x1e, PB_UP }, { 0x1f, PB_RIGHT }, { 0x20, PB_DOWN }, { 0x21, PB_LEFT },            /* 1 2 3 4 */
		{ HK_MOUSE + 0, PB_RT }, { HK_MOUSE + 1, PB_LT }, { HK_MOUSE + 2, PB_R3 },
		{ HK_MOUSE + 3, PB_LB }, { HK_MOUSE + 4, PB_RB },
	};
	memset(c, 0, sizeof(*c));
	c->mode = 0;
	c->sens = 40;
	c->n = sizeof(def) / 2;
	memcpy(c->map, def, sizeof(def));
}

static int target_of(const struct hid_cfg *c, uint8_t code)
{
	for (int i = 0; i < c->n; i++) if (c->map[i][0] == code) return c->map[i][1];
	return -1;
}

static uint8_t hat_of(uint32_t b)
{
	bool u = b & (1u << PB_UP), d = b & (1u << PB_DOWN), l = b & (1u << PB_LEFT), r = b & (1u << PB_RIGHT);

	if (u && r) return 1;
	if (d && r) return 3;
	if (d && l) return 5;
	if (u && l) return 7;
	if (u) return 0;
	if (r) return 2;
	if (d) return 4;
	if (l) return 6;
	return 8;
}

static uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

void hid_to_pad(const struct hid_cfg *c, const struct hid_in *in, struct pad_state *p, uint8_t rest_kb[8], uint8_t *rest_mbtn)
{
	uint32_t btn = 0;
	int ls[4] = { 0 }, rs[4] = { 0 };
	int nrest = 2;

	memset(rest_kb, 0, 8);
	*rest_mbtn = 0;

	/* basılı kaynak kodları: değiştiriciler (E0..E7), tuşlar, fare tuşları */
	uint8_t codes[8 + 6 + 5];
	int nc = 0;

	for (int i = 0; i < 8; i++) if (in->kb[0] & (1 << i)) codes[nc++] = 0xe0 + i;
	for (int i = 2; i < 8; i++) if (in->kb[i] > 3) codes[nc++] = in->kb[i];
	for (int i = 0; i < 5; i++) if (in->mbtn & (1 << i)) codes[nc++] = HK_MOUSE + i;

	for (int i = 0; i < nc; i++) {
		uint8_t k = codes[i];
		int t = target_of(c, k);

		if (t < 0 || t >= HT_COUNT) {
			/* atanmamış: olduğu gibi klavye/fare olarak geçsin */
			if (k >= HK_MOUSE) *rest_mbtn |= 1 << (k - HK_MOUSE);
			else if (k >= 0xe0) rest_kb[0] |= 1 << (k - 0xe0);
			else if (nrest < 8) rest_kb[nrest++] = k;
			continue;
		}
		if (t < HT_LS) btn |= 1u << t;
		else if (t < HT_RS) ls[t - HT_LS] = 1;
		else rs[t - HT_RS] = 1;
	}

	if (!p->valid) {
		p->btn = 0;
		p->lx = p->ly = p->rx = p->ry = 128;
		p->lt = p->rt = 0;
		p->hat = 8;
		p->valid = true;
	}
	p->btn |= btn;
	if (btn & (1u << PB_LT)) p->lt = 255;
	if (btn & (1u << PB_RT)) p->rt = 255;
	if (btn & ((1u << PB_UP) | (1u << PB_DOWN) | (1u << PB_LEFT) | (1u << PB_RIGHT))) p->hat = hat_of(p->btn);

	/* tuşlarla sol/sağ çubuk (karşılıklı basılıysa ortada kalır) */
	if (ls[0] || ls[1]) p->ly = ls[0] && !ls[1] ? 0 : ls[1] && !ls[0] ? 255 : 128;
	if (ls[2] || ls[3]) p->lx = ls[2] && !ls[3] ? 0 : ls[3] && !ls[2] ? 255 : 128;
	if (rs[0] || rs[1]) p->ry = rs[0] && !rs[1] ? 0 : rs[1] && !rs[0] ? 255 : 128;
	if (rs[2] || rs[3]) p->rx = rs[2] && !rs[3] ? 0 : rs[3] && !rs[2] ? 255 : 128;

	/* fare hareketi → sağ çubuk. Oyunların ölü bölgesini aşmak için küçük hareket de en az %20 iter. */
	if (in->vx || in->vy) {
		float fx = in->vx / 16.0f * c->sens * 1.6f, fy = in->vy / 16.0f * c->sens * 1.6f;
		float m = sqrtf(fx * fx + fy * fy);

		if (m > 0.5f) {
			float m2 = 26 + m * (127 - 26) / 127;

			if (m2 > 127) m2 = 127;
			fx = fx / m * m2;
			fy = fy / m * m2;
			p->rx = clamp8(128 + (int)lrintf(fx));
			p->ry = clamp8(128 + (int)lrintf(fy));
		}
	}
}

static int8_t add8(int a, int b)
{
	int v = a + b;

	return v > 127 ? 127 : v < -127 ? -127 : v;
}

void hid_merge_out(const uint8_t kb[8], uint8_t mbtn, int dx, int dy, int wh, struct out_rep *r)
{
	r->kb[0] |= kb[0];
	for (int i = 2; i < 8; i++) {
		uint8_t k = kb[i];
		bool have = false;
		int fr = -1;

		if (k <= 3) continue;
		for (int j = 2; j < 8; j++) {
			if (r->kb[j] == k) have = true;
			if (!r->kb[j] && fr < 0) fr = j;
		}
		if (!have && fr >= 0) r->kb[fr] = k;
	}
	r->ms[0] |= mbtn & 0x1f;
	r->ms[1] = add8((int8_t)r->ms[1], dx);
	r->ms[2] = add8((int8_t)r->ms[2], dy);
	r->ms[3] = add8((int8_t)r->ms[3], wh);
}
