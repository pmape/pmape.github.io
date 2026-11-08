/* Eşleme motoru. Biçim için engine.h'ye bak. */
#include <string.h>
#include "engine.h"

#ifdef HOST_TEST
#include <stdio.h>
#define OUT(...) printf(__VA_ARGS__)
#define LOCK()
#define UNLOCK()
#ifndef BIT
#define BIT(n) (1UL << (n))
#endif
#ifndef CLAMP
#define CLAMP(v, l, h) ((v) < (l) ? (l) : (v) > (h) ? (h) : (v))
#endif
#else
#include <zephyr/kernel.h>
#include "log.h"
static K_MUTEX_DEFINE(mtx);
#define LOCK() k_mutex_lock(&mtx, K_FOREVER)
#define UNLOCK() k_mutex_unlock(&mtx)
#endif

#define MAXB 160
#define MAXP 8
#define HDR 16
#define PHDR 64
#define BHDR 11
#define STEP 5

enum { TR_SINGLE, TR_DOUBLE, TR_LONG, TR_START, TR_RELEASE, TR_COMBO, TR_N };
/* ST_MOVE: fareyi kaydır — kod = x (int8), hareket baytı = y (int8) */
enum { ST_KEY, ST_MOUSE, ST_PAD, ST_CC, ST_MOVE };
/* medya tuşları: ayar ekranındaki sırayla */
static const uint16_t cc_usage[] = { 0, 0x0cf, 0x221, 0x0e9, 0x0ea, 0x0e2, 0x0cd, 0x0b5, 0x0b6, 0x06f, 0x070, 0x223, 0x224 };
#define NCC (sizeof(cc_usage) / sizeof(cc_usage[0]))
enum { SM_INHERIT, SM_OFF, SM_MOUSE, SM_KEYS, SM_SCROLL, SM_PAD };

static inline uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

/* ---------- ayar ---------- */

static const uint8_t *cfg;
static size_t cfg_len;
static const uint8_t *prof_at[MAXP];
static int nprof, cur;

struct bind {
	uint8_t layer, btn, trig, other, kind, mode, tlayer, how, nsteps;
	uint16_t loop;
	const uint8_t *steps;
};
struct stick { uint8_t mode, sens, dead, k[4]; };

static struct bind binds[MAXB];
static int nbinds;
static int16_t bidx[4][NBTN][TR_N - 1];
static uint8_t has_combo[4][NBTN];
static struct stick sticks[4][2];
static uint8_t p_out, p_thr;
static uint16_t p_long, p_double;

/* cihaz ayarları */
static uint8_t d_comboA = PB_HOME, d_comboB = PB_SELECT, d_flags = 3;
static uint8_t d_pmod = PB_HOME, d_pprev = PB_LB, d_pnext = PB_RB;
static uint16_t d_hold = 1500;
static bool mapping = true;

static int walk_profile(const uint8_t *p, const uint8_t *end, const uint8_t **next)
{
	if (p + PHDR > end) return -1;
	int nb = rd16(p + 62);
	const uint8_t *q = p + PHDR;

	if (nb > MAXB) return -2;
	for (int i = 0; i < nb; i++) {
		if (q + BHDR > end) return -3;
		if (q[0] > 3 || q[1] >= NBTN || q[2] >= TR_N) return -4;
		q += BHDR + q[10] * STEP;
		if (q > end) return -5;
	}
	*next = q;
	return 0;
}

int engine_check(const uint8_t *b, size_t len)
{
	if (len < HDR || b[0] != 'P' || b[1] != 'M' || b[2] != 2) return -1;
	if (b[9] < 1 || b[9] > MAXP) return -2;
	const uint8_t *p = b + HDR, *end = b + len;

	for (int i = 0; i < b[9]; i++) {
		int e = walk_profile(p, end, &p);
		if (e) return e - 10 * (i + 1);
	}
	return p == end ? 0 : -9;
}

static void load_profile(int n)
{
	const uint8_t *p = prof_at[n];

	memset(bidx, 0xff, sizeof(bidx));
	memset(has_combo, 0, sizeof(has_combo));
	p_out = p[0]; p_thr = p[1]; p_long = rd16(p + 2); p_double = rd16(p + 4);
	for (int l = 0; l < 4; l++) {
		for (int s = 0; s < 2; s++) {
			const uint8_t *q = p + 6 + (l * 2 + s) * 7;

			sticks[l][s].mode = q[0]; sticks[l][s].sens = q[1]; sticks[l][s].dead = q[2];
			memcpy(sticks[l][s].k, q + 3, 4);
		}
	}
	nbinds = rd16(p + 62);
	const uint8_t *q = p + PHDR;

	for (int i = 0; i < nbinds; i++) {
		struct bind *b = &binds[i];

		b->layer = q[0]; b->btn = q[1]; b->trig = q[2]; b->other = q[3];
		b->kind = q[4]; b->mode = q[5]; b->loop = rd16(q + 6);
		b->tlayer = q[8] & 3; b->how = q[9]; b->nsteps = q[10];
		b->steps = q + BHDR;
		if (b->trig == TR_COMBO) has_combo[b->layer][b->btn] = 1;
		else bidx[b->layer][b->btn][b->trig] = i;
		q += BHDR + b->nsteps * STEP;
	}
}

static void reset_state(void);

int engine_set_config(const uint8_t *b, size_t len)
{
	if (engine_check(b, len)) return -1;
	LOCK();
	cfg = b; cfg_len = len;
	nprof = b[9];
	const uint8_t *p = b + HDR;

	for (int i = 0; i < nprof; i++) {
		prof_at[i] = p;
		walk_profile(p, b + len, &p);
	}
	d_comboA = b[4] < NBTN ? b[4] : PB_HOME;
	d_comboB = b[5] < NBTN ? b[5] : PB_SELECT;
	d_hold = rd16(b + 6);
	d_flags = b[8];
	d_pmod = b[11] < NBTN ? b[11] : PB_HOME;
	d_pprev = b[12] < NBTN ? b[12] : PB_LB;
	d_pnext = b[13] < NBTN ? b[13] : PB_RB;
	cur = b[10] < nprof ? b[10] : 0;
	mapping = b[3] == 0;
	load_profile(cur);
	reset_state();
	UNLOCK();
	return 0;
}

bool engine_has_config(void) { return cfg != NULL; }
bool engine_mapping(void) { return mapping; }
void engine_set_mapping(bool on) { LOCK(); mapping = on; reset_state(); UNLOCK(); }
int engine_profile(void) { return cur; }
int engine_profile_count(void) { return nprof; }
static int active_layer(void);
int engine_active_layer(void) { return cfg && mapping ? active_layer() : 0; }

int engine_select_profile(int n)
{
	if (!cfg || n < 0 || n >= nprof) return -1;
	LOCK();
	cur = n;
	load_profile(n);
	reset_state();
	UNLOCK();
	return 0;
}

/* ---------- çıkış sayaçları ---------- */

static uint8_t kref[256], mref[5], pref[20], cref[NCC];
static int wheel;
static int mvx, mvy; /* makrodaki fare kaydırmaları */

static void o_down(uint8_t t, uint8_t c)
{
	if (t == ST_KEY) { if (kref[c] < 255) kref[c]++; }
	else if (t == ST_MOUSE) {
		if (c < 5) { if (mref[c] < 255) mref[c]++; }
		else if (c == 5) wheel++;
		else if (c == 6) wheel--;
	} else if (t == ST_PAD && c < 20) { if (pref[c] < 255) pref[c]++; }
	else if (t == ST_CC && c < NCC) { if (cref[c] < 255) cref[c]++; }
}

static void o_up(uint8_t t, uint8_t c)
{
	if (t == ST_KEY) { if (kref[c]) kref[c]--; }
	else if (t == ST_MOUSE) { if (c < 5 && mref[c]) mref[c]--; }
	else if (t == ST_PAD && c < 20) { if (pref[c]) pref[c]--; }
	else if (t == ST_CC && c < NCC) { if (cref[c]) cref[c]--; }
}

/* bas-bırak'ın bırakma anları */
#define MAXUP 32
static struct { uint8_t t, c, used; int64_t at; } ups[MAXUP];

static void up_later(uint8_t t, uint8_t c, int64_t at)
{
	for (int i = 0; i < MAXUP; i++) {
		if (!ups[i].used) { ups[i].used = 1; ups[i].t = t; ups[i].c = c; ups[i].at = at; return; }
	}
	o_up(t, c); /* yer yoksa hemen bırak */
}

/* tuş basılı durdukça tutulanlar */
#define MAXH 24
static struct { uint8_t btn, t, c, used; } holds[MAXH];

static void hold_add(int btn, uint8_t t, uint8_t c)
{
	for (int i = 0; i < MAXH; i++) {
		if (!holds[i].used) {
			holds[i].used = 1; holds[i].btn = btn; holds[i].t = t; holds[i].c = c;
			o_down(t, c);
			return;
		}
	}
}

static void hold_release(int btn)
{
	for (int i = 0; i < MAXH; i++) {
		if (holds[i].used && holds[i].btn == btn) { holds[i].used = 0; o_up(holds[i].t, holds[i].c); }
	}
}

/* toggle ile açık kalanlar */
#define MAXT 16
static struct { uint8_t t, c, used; int16_t bi; } togs[MAXT];

static void tog_flip(int bi, uint8_t t, uint8_t c)
{
	for (int i = 0; i < MAXT; i++) {
		if (togs[i].used && togs[i].bi == bi && togs[i].t == t && togs[i].c == c) { togs[i].used = 0; o_up(t, c); return; }
	}
	for (int i = 0; i < MAXT; i++) {
		if (!togs[i].used) { togs[i].used = 1; togs[i].bi = bi; togs[i].t = t; togs[i].c = c; o_down(t, c); return; }
	}
}

/* çalışan makrolar */
#define MAXR 10
static struct { int16_t bi; uint8_t step, used, loop; int8_t btn; int64_t next; } runs[MAXR];

static int run_find(int bi)
{
	for (int i = 0; i < MAXR; i++) if (runs[i].used && runs[i].bi == bi) return i;
	return -1;
}

static void run_start(int bi, bool loop, int btn, int64_t now)
{
	int i = run_find(bi);

	if (i < 0) for (i = 0; i < MAXR && runs[i].used; i++);
	if (i >= MAXR) return;
	runs[i].used = 1; runs[i].bi = bi; runs[i].step = 0; runs[i].loop = loop;
	runs[i].btn = btn; runs[i].next = now;
}

static void run_stop_btn(int btn)
{
	for (int i = 0; i < MAXR; i++) if (runs[i].used && runs[i].btn == btn) runs[i].used = 0;
}

static void exec_step(const uint8_t *s, int64_t now)
{
	uint8_t t = s[0], c = s[1], a = s[2];

	if (t == ST_MOVE) {
		mvx += (int8_t)c;
		mvy += (int8_t)a;
		return;
	}
	if (a == 0) {
		o_down(t, c);
		if (!(t == ST_MOUSE && c >= 5)) up_later(t, c, now + (t == ST_CC ? 60 : 20));
	} else if (a == 1) o_down(t, c);
	else o_up(t, c);
}

static void runs_tick(int64_t now)
{
	for (int i = 0; i < MAXR; i++) {
		int guard = 64;

		while (runs[i].used && now >= runs[i].next && guard--) {
			const struct bind *b = &binds[runs[i].bi];

			if (runs[i].step >= b->nsteps) {
				if (!runs[i].loop) { runs[i].used = 0; break; }
				runs[i].step = 0;
			}
			const uint8_t *s = b->steps + runs[i].step * STEP;

			exec_step(s, now);
			runs[i].step++;
			if (runs[i].step >= b->nsteps) {
				if (!runs[i].loop) { runs[i].used = 0; break; }
				runs[i].next = now + (b->loop < 20 ? 20 : b->loop);
			} else {
				runs[i].next = now + rd16(s + 3);
			}
		}
	}
	for (int i = 0; i < MAXUP; i++) {
		if (ups[i].used && now >= ups[i].at) { ups[i].used = 0; o_up(ups[i].t, ups[i].c); }
	}
}

/* ---------- tuş durumu ---------- */

struct bst {
	uint8_t down, long_fired, consumed, dbl_done, lay;
	int8_t hold_layer;
	int64_t down_at, pend_single;
};
static struct bst bs[NBTN];
static uint8_t layer_toggle;
static uint32_t prev_btn;
static float acc[2][2], wacc;
static float tscale = 1.0f; /* geçen süre / 8 ms */
static int64_t last_tick;

static void reset_state(void)
{
	mvx = mvy = 0;
	memset(kref, 0, sizeof(kref)); memset(mref, 0, sizeof(mref)); memset(pref, 0, sizeof(pref)); memset(cref, 0, sizeof(cref));
	memset(ups, 0, sizeof(ups)); memset(holds, 0, sizeof(holds)); memset(togs, 0, sizeof(togs));
	memset(runs, 0, sizeof(runs)); memset(bs, 0, sizeof(bs));
	for (int i = 0; i < NBTN; i++) bs[i].hold_layer = -1;
	wheel = 0; layer_toggle = 0; prev_btn = 0;
	memset(acc, 0, sizeof(acc)); wacc = 0;
}

static int active_layer(void)
{
	int l = -1;

	for (int i = 0; i < NBTN; i++) if (bs[i].down && bs[i].hold_layer > l) l = bs[i].hold_layer;
	return l >= 0 ? l : layer_toggle;
}

static bool has_any(int l, int b)
{
	if (has_combo[l][b]) return true;
	for (int t = 0; t < TR_N - 1; t++) if (bidx[l][b][t] >= 0) return true;
	return false;
}

static int resolve(int b)
{
	int l = active_layer();

	return (l > 0 && !has_any(l, b)) ? 0 : l;
}

static bool single_out(const struct bind *b)
{
	return b->nsteps == 1 && b->steps[0] != ST_MOVE && b->steps[2] == 0 && !(b->steps[0] == ST_MOUSE && b->steps[1] >= 5);
}

/* Birden fazla çıkış "birlikte": hepsi bas-bırak, beklemesiz, kaydırma/tekerlek yok.
 * Normal modda pad tuşu basılıyken hepsi basılı kalır; aç-kapa modunda hepsi birlikte açılıp kapanır. */
static bool all_out(const struct bind *b)
{
	if (b->nsteps < 2 || b->nsteps > 8) return false;
	for (int i = 0; i < b->nsteps; i++) {
		const uint8_t *s = b->steps + 5 * i;

		if (s[0] == ST_MOVE || s[2] != 0 || (s[0] == ST_MOUSE && s[1] >= 5)) return false;
		if (i < b->nsteps - 1 && (s[3] | s[4])) return false;
	}
	return true;
}

/* hb: basılı tutulan tuş (tutma davranışı için), -1: tek seferlik */
static void fire(int bi, int hb, int64_t now)
{
	const struct bind *b = &binds[bi];

	if (b->kind == 1) {
		if (b->how == 0 && hb >= 0) bs[hb].hold_layer = b->tlayer;
		else layer_toggle = layer_toggle == b->tlayer ? 0 : b->tlayer;
		return;
	}
	if (!b->nsteps) return;
	switch (b->mode) {
	case 1: /* turbo */
		run_start(bi, hb >= 0, hb, now);
		break;
	case 3: /* makro: bir basışta bir kez, bıraksan da biter */
		run_start(bi, false, -1, now);
		break;
	case 2: /* toggle */
		if (single_out(b)) tog_flip(bi, b->steps[0], b->steps[1]);
		else if (all_out(b)) { for (int i = 0; i < b->nsteps; i++) tog_flip(bi, b->steps[5 * i], b->steps[5 * i + 1]); }
		else if (run_find(bi) >= 0) runs[run_find(bi)].used = 0;
		else run_start(bi, true, -1, now);
		break;
	default:
		if (hb >= 0 && single_out(b)) hold_add(hb, b->steps[0], b->steps[1]);
		else if (hb >= 0 && all_out(b)) { for (int i = 0; i < b->nsteps; i++) hold_add(hb, b->steps[5 * i], b->steps[5 * i + 1]); }
		else run_start(bi, false, -1, now);
	}
}

static int combo_find(int l, int a, int o)
{
	for (int i = 0; i < nbinds; i++) {
		const struct bind *b = &binds[i];

		if (b->trig == TR_COMBO && b->layer == l &&
		    ((b->btn == a && b->other == o) || (b->btn == o && b->other == a))) return i;
	}
	return -1;
}

static void on_press(int b, int64_t now)
{
	struct bst *s = &bs[b];
	int l = resolve(b);

	s->down = 1; s->down_at = now; s->long_fired = 0; s->consumed = 0; s->lay = l; s->hold_layer = -1;

	/* birlikte basış */
	for (int o = 0; o < NBTN; o++) {
		if (o == b || !bs[o].down || bs[o].consumed) continue;
		int ci = combo_find(l, b, o);

		if (ci < 0 && l) ci = combo_find(0, b, o);
		if (ci < 0) continue;
		hold_release(o);
		run_stop_btn(o);
		bs[o].pend_single = 0;
		bs[o].consumed = s->consumed = 1;
		fire(ci, -1, now);
		return;
	}

	int16_t *bx = bidx[l][b];

	if (bx[TR_START] >= 0) fire(bx[TR_START], -1, now);
	if (bx[TR_SINGLE] >= 0 && bx[TR_DOUBLE] < 0 && bx[TR_LONG] < 0) {
		fire(bx[TR_SINGLE], b, now);
		return;
	}
	if (bx[TR_DOUBLE] >= 0 && s->pend_single && now - s->pend_single <= p_double) {
		s->pend_single = 0;
		s->dbl_done = 1;
		fire(bx[TR_DOUBLE], b, now);
		return;
	}
	s->dbl_done = 0;
}

static void on_release(int b, int64_t now)
{
	struct bst *s = &bs[b];
	int16_t *bx = bidx[s->lay][b];

	s->down = 0;
	hold_release(b);
	run_stop_btn(b);
	s->hold_layer = -1;
	if (s->consumed) return;
	if (bx[TR_RELEASE] >= 0) fire(bx[TR_RELEASE], -1, now);
	if (bx[TR_SINGLE] < 0 || (bx[TR_DOUBLE] < 0 && bx[TR_LONG] < 0)) return;
	if (s->long_fired || s->dbl_done) return;
	if (bx[TR_DOUBLE] >= 0) s->pend_single = now;
	else fire(bx[TR_SINGLE], -1, now);
}

static void buttons_tick(int64_t now)
{
	for (int b = 0; b < NBTN; b++) {
		struct bst *s = &bs[b];
		int16_t *bx = bidx[s->lay][b];

		if (s->down && !s->consumed && !s->long_fired && bx[TR_LONG] >= 0 &&
		    now - s->down_at >= p_long) {
			s->long_fired = 1;
			s->pend_single = 0;
			fire(bx[TR_LONG], b, now);
		}
		if (!s->down && s->pend_single && now - s->pend_single > p_double) {
			int16_t *bl = bidx[s->lay][b];

			s->pend_single = 0;
			if (bl[TR_SINGLE] >= 0) fire(bl[TR_SINGLE], -1, now);
		}
	}
}

/* ---------- çubuklar ---------- */

static const struct stick *stick_of(int l, int s)
{
	if (l > 0 && sticks[l][s].mode == SM_INHERIT) return &sticks[0][s];
	return &sticks[l][s];
}

static uint8_t tick_keys[8];
static int ntick;

static void stick_tick(int s, uint8_t x, uint8_t y, int8_t *mx, int8_t *my, uint8_t *gx, uint8_t *gy)
{
	const struct stick *st = stick_of(active_layer(), s);
	float nx = ((float)x - 128.0f) / 127.0f, ny = ((float)y - 128.0f) / 127.0f;
	float dz = st->dead / 100.0f;
	float ax = nx < 0 ? -nx : nx, ay = ny < 0 ? -ny : ny;

	switch (st->mode) {
	case SM_MOUSE: {
		float mag = ax > ay ? ax : ay;

		if (mag <= dz) { acc[s][0] = acc[s][1] = 0; break; }
		float k = (mag - dz) / (1.0f - dz) / mag;
		float v[2] = { nx * k, ny * k };
		float mx_ = 3.5f * (st->sens ? st->sens : 8) * tscale;

		for (int i = 0; i < 2; i++) {
			float a = v[i] < 0 ? -v[i] : v[i];

			if (a < 0.001f) continue;
			acc[s][i] += v[i] * (0.35f * a + 0.65f * a * a) * mx_ + (v[i] > 0 ? 0.1f : -0.1f) * tscale;
			int o = (int)acc[s][i];

			acc[s][i] -= o;
			int t = (i ? *my : *mx) + o;

			t = t > 127 ? 127 : t < -127 ? -127 : t;
			if (i) *my = t; else *mx = t;
		}
		break;
	}
	case SM_KEYS: {
		float thr = dz > 0.4f ? dz : 0.4f;

		if (ny < -thr && st->k[0] && ntick < 8) tick_keys[ntick++] = st->k[0];
		if (ny > thr && st->k[1] && ntick < 8) tick_keys[ntick++] = st->k[1];
		if (nx < -thr && st->k[2] && ntick < 8) tick_keys[ntick++] = st->k[2];
		if (nx > thr && st->k[3] && ntick < 8) tick_keys[ntick++] = st->k[3];
		break;
	}
	case SM_SCROLL:
		if (ay <= dz) { wacc = 0; break; }
		wacc += -ny * (st->sens ? st->sens : 8) * 0.02f * tscale;
		while (wacc >= 1) { wheel++; wacc -= 1; }
		while (wacc <= -1) { wheel--; wacc += 1; }
		break;
	case SM_PAD:
		*gx = ax <= dz ? 128 : x;
		*gy = ay <= dz ? 128 : y;
		break;
	default:
		break;
	}
}

/* ---------- ana adım ---------- */

static const uint8_t g_idle[9] = { 0, 0, 0x0f, 128, 128, 128, 128, 0, 0 };

static uint8_t hat_of(bool u, bool d, bool l, bool r)
{
	if (u && r) return 1;
	if (d && r) return 3;
	if (d && l) return 5;
	if (u && l) return 7;
	if (u) return 0;
	if (r) return 2;
	if (d) return 4;
	if (l) return 6;
	return 0x0f;
}

static void passthrough(const struct pad_state *p, uint8_t g[9])
{
	g[0] = p->btn & 0xff;
	g[1] = (p->btn >> 8) & 0xff;
	g[2] = p->hat > 7 ? 0x0f : p->hat;
	g[3] = p->lx; g[4] = p->ly; g[5] = p->rx; g[6] = p->ry;
	g[7] = p->lt; g[8] = p->rt;
}

static int64_t combo_since;
static bool combo_done;

enum eng_mode engine_tick(const struct pad_state *p, int64_t now, struct out_rep *r)
{
	enum eng_mode m;

	LOCK();
	/* eşleme aç/kapat: kombA + kombB basılı tut */
	uint32_t cm = BIT(d_comboA) | BIT(d_comboB);

	if ((p->btn & cm) == cm) {
		if (!combo_since) combo_since = now;
		if (!combo_done && now - combo_since >= d_hold) {
			combo_done = true;
			mapping = !mapping;
			reset_state();
			prev_btn = p->btn; /* bırakılınca tuş tetiklenmesin */
			bs[d_comboA].consumed = bs[d_comboB].consumed = 1;
			bs[d_comboA].down = bs[d_comboB].down = 1;
			OUT("{\"ev\":\"mode\",\"mapping\":%d}\n", mapping);
		}
	} else {
		combo_since = 0;
		combo_done = false;
	}

	memset(r, 0, sizeof(*r));
	if (!mapping) {
		passthrough(p, r->g);
		/* aç/kapat kombinasyonu tutulurken tablete Home/Select gitmesin */
		if ((p->btn & cm) == cm) { r->g[0] &= ~(cm & 0xff); r->g[1] &= ~((cm >> 8) & 0xff); }
		UNLOCK();
		return E_PAD;
	}
	if (!cfg) {
		UNLOCK();
		return E_LEGACY;
	}

	/* basılı tuşlar (tetikler eşikle) */
	uint32_t btn = p->btn;
	uint8_t thr = (uint8_t)(p_thr * 255 / 100);

	if (p->lt > thr && p->lt) btn |= BIT(PB_LT);
	if (p->rt > thr && p->rt) btn |= BIT(PB_RT);

	uint32_t pressed = btn & ~prev_btn, released = prev_btn & ~btn;

	/* profil değiştir: değiştirici tuş basılıyken önceki / sonraki tuşu */
	if ((d_flags & 1) && (btn & BIT(d_pmod)) && (pressed & (BIT(d_pprev) | BIT(d_pnext))) && nprof > 1) {
		int n = (cur + ((pressed & BIT(d_pnext)) ? 1 : nprof - 1)) % nprof;

		cur = n;
		load_profile(n);
		reset_state();
		prev_btn = btn;
		for (int b = 0; b < NBTN; b++) if (btn & BIT(b)) { bs[b].down = 1; bs[b].consumed = 1; }
		OUT("{\"ev\":\"prof\",\"cur\":%d}\n", n);
		engine_profile_changed(n);
		pressed = released = 0;
	}

	for (int b = 0; b < NBTN; b++) {
		if (released & BIT(b)) on_release(b, now);
	}
	for (int b = 0; b < NBTN; b++) {
		if (pressed & BIT(b)) on_press(b, now);
	}
	prev_btn = btn;
	buttons_tick(now);
	runs_tick(now);

	/* çubuklar */
	int8_t mx = 0, my = 0;

	{
		int64_t dt = last_tick ? now - last_tick : 8;

		last_tick = now;
		tscale = (dt < 1 ? 1 : dt > 30 ? 30 : (float)dt) / 8.0f;
	}
	memcpy(r->g, g_idle, 9);
	ntick = 0;
	stick_tick(0, p->lx, p->ly, &mx, &my, &r->g[3], &r->g[4]);
	stick_tick(1, p->rx, p->ry, &mx, &my, &r->g[5], &r->g[6]);

	/* klavye */
	int n = 0;

	for (int c = 0xe0; c <= 0xe7; c++) if (kref[c]) r->kb[0] |= BIT(c - 0xe0);
	for (int c = 1; c < 0xe0 && n < 6; c++) if (kref[c]) r->kb[2 + n++] = c;
	for (int i = 0; i < ntick && n < 6; i++) {
		uint8_t c = tick_keys[i];

		if (c >= 0xe0 && c <= 0xe7) { r->kb[0] |= BIT(c - 0xe0); continue; }
		bool dup = false;

		for (int j = 0; j < n; j++) if (r->kb[2 + j] == c) dup = true;
		if (!dup) r->kb[2 + n++] = c;
	}

	/* medya */
	for (unsigned i = 1; i < NCC; i++) if (cref[i]) { r->cc[0] = cc_usage[i] & 0xff; r->cc[1] = cc_usage[i] >> 8; break; }

	/* fare */
	for (int i = 0; i < 5; i++) if (mref[i]) r->ms[0] |= BIT(i);
	{
		/* makro fare kaydırması: bu karede en fazla 127, kalanı sonraki karelere */
		int x = mx + CLAMP(mvx, -127, 127), y = my + CLAMP(mvy, -127, 127);

		mvx -= CLAMP(mvx, -127, 127);
		mvy -= CLAMP(mvy, -127, 127);
		mx = CLAMP(x, -127, 127);
		my = CLAMP(y, -127, 127);
	}
	r->ms[1] = (uint8_t)mx;
	r->ms[2] = (uint8_t)my;
	r->ms[3] = (uint8_t)(int8_t)(wheel > 127 ? 127 : wheel < -127 ? -127 : wheel);
	wheel = 0;

	/* gamepad: atanmış pad çıkışları + (istenirse) atanmamış tuşlar */
	uint32_t gb = 0;

	for (int i = 0; i < 20; i++) if (pref[i]) gb |= BIT(i);
	if (d_flags & 2) {
		int l = active_layer();

		for (int b = 0; b < 20; b++) {
			if (!(btn & BIT(b)) || bs[b].consumed || b == d_comboA || b == d_pmod || (b == d_comboB && (btn & BIT(d_comboA)))) continue;
			if (!has_any(l, b) && !has_any(0, b)) gb |= BIT(b);
		}
		if (!has_any(l, PB_LT) && !has_any(0, PB_LT)) r->g[7] = p->lt;
		if (!has_any(l, PB_RT) && !has_any(0, PB_RT)) r->g[8] = p->rt;
	}
	r->g[0] = gb & 0xff;
	r->g[1] = (gb >> 8) & 0xff;
	r->g[2] = hat_of(gb & BIT(PB_UP), gb & BIT(PB_DOWN), gb & BIT(PB_LEFT), gb & BIT(PB_RIGHT));
	if (gb & BIT(PB_LT) && !r->g[7]) r->g[7] = 255;
	if (gb & BIT(PB_RT) && !r->g[8]) r->g[8] = 255;

	m = E_MAP;
	UNLOCK();
	return m;
}
