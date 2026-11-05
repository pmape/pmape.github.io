#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "log.h"
#include "pad.h"

#define MAX_FIELDS 64

enum role {
	R_NONE, R_BTN, R_LX, R_LY, R_RX, R_RY, R_LT, R_RT, R_HAT, R_CONSUMER,
};

struct field {
	uint8_t rid;
	uint16_t bit;
	uint8_t size;
	uint16_t page;
	uint16_t usage;
	int32_t lmin, lmax;
	uint8_t role;
	uint8_t arg; /* R_BTN: mantıksal tuş */
};

static struct field fields[MAX_FIELDS];
static int nfields;
static struct pad_state st;
static struct k_spinlock lock;
static uint32_t rx_count;

uint32_t pad_rx_count(void) { return rx_count; }

/* ---------- öğrenilmiş harita (tuş tanıtma ile) ----------
 * Her giriş 14 bayt: rapor_no tür hedef bit_sayısı bit(u16) bayrak boş arg(u16) min(i16) max(i16)
 * tür: 0 düğme (değer≠0), 1 çubuk ekseni (hedef 0 lx,1 ly,2 rx,3 ry), 2 tetik (0 lt,1 rt),
 *      3 yön (hat, arg = en küçük değer), 4 eksen eşiği → düğme (arg = eşik 0..127, bayrak b2 = eksi yön),
 *      5 değer eşitse düğme (arg = değer)
 * bayrak: b0 ters çevir */
#define LMAX 48
#define LENT 14
static uint8_t lent[LMAX][LENT];
static int ln;
static struct { uint8_t id, used; uint8_t len; uint8_t d[64]; } lcache[4];

void pad_set_learned(const uint8_t *e, int n)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	ln = MIN(n, LMAX);
	if (ln > 0) memcpy(lent, e, ln * LENT);
	memset(lcache, 0, sizeof(lcache));
	k_spin_unlock(&lock, key);
}

bool pad_learned(void) { return ln > 0; }

static uint32_t getbits(const uint8_t *d, size_t len, int bit, int size)
{
	uint32_t v = 0;

	for (int k = 0; k < size && k < 32; k++) {
		int b = bit + k;

		if ((size_t)(b >> 3) < len && (d[b >> 3] & (1 << (b & 7)))) v |= 1u << k;
	}
	return v;
}

static void learned_input(uint8_t id, const uint8_t *data, size_t len)
{
	int slot = -1;

	for (int i = 0; i < 4; i++) {
		if (lcache[i].used && lcache[i].id == id) { slot = i; break; }
		if (!lcache[i].used && slot < 0) slot = i;
	}
	if (slot < 0) return;
	lcache[slot].used = 1;
	lcache[slot].id = id;
	lcache[slot].len = MIN(len, sizeof(lcache[slot].d));
	memcpy(lcache[slot].d, data, lcache[slot].len);

	uint32_t btn = 0;

	st.lx = st.ly = st.rx = st.ry = 128;
	st.lt = st.rt = 0;
	st.hat = 8;
	for (int i = 0; i < ln; i++) {
		const uint8_t *e = lent[i];
		int c = -1;

		for (int j = 0; j < 4; j++) if (lcache[j].used && lcache[j].id == e[0]) c = j;
		if (c < 0) continue;
		int size = e[3], bit = e[4] | (e[5] << 8);
		uint16_t arg = e[8] | (e[9] << 8);
		int16_t mn = (int16_t)(e[10] | (e[11] << 8)), mx = (int16_t)(e[12] | (e[13] << 8));
		uint32_t raw = getbits(lcache[c].d, lcache[c].len, bit, size);
		int32_t v = (int32_t)raw;

		if (mn < 0 && size < 32 && (raw & (1u << (size - 1)))) v = (int32_t)raw - (1 << size);
		int32_t sc = mx > mn ? (v - mn) * 255 / (mx - mn) : (v ? 255 : 0);

		sc = CLAMP(sc, 0, 255);
		if (e[6] & 1) sc = 255 - sc;
		switch (e[1]) {
		case 0: if (raw) btn |= BIT(e[2]); break;
		case 1:
			if (e[2] == 0) st.lx = sc; else if (e[2] == 1) st.ly = sc;
			else if (e[2] == 2) st.rx = sc; else st.ry = sc;
			break;
		case 2: if (e[2] == 0) st.lt = sc; else st.rt = sc; break;
		case 3: { int32_t hh = v - (int16_t)arg; st.hat = (hh >= 0 && hh <= 7) ? hh : 8; break; }
		case 4:
			if ((e[6] & 4) ? sc < 128 - arg : sc > 128 + arg) btn |= BIT(e[2]);
			break;
		case 5: if (raw == arg) btn |= BIT(e[2]); break;
		}
	}
	static const uint8_t hat_bits2[9] = {
		BIT(0), BIT(0) | BIT(3), BIT(3), BIT(1) | BIT(3), BIT(1),
		BIT(1) | BIT(2), BIT(2), BIT(0) | BIT(2), 0,
	};
	uint8_t hb = hat_bits2[MIN(st.hat, 8)];

	if (hb & BIT(0)) btn |= BIT(PB_UP);
	if (hb & BIT(1)) btn |= BIT(PB_DOWN);
	if (hb & BIT(2)) btn |= BIT(PB_LEFT);
	if (hb & BIT(3)) btn |= BIT(PB_RIGHT);
	/* yön tuşları düğme olarak geldiyse gamepad yönü de onlardan */
	if (st.hat > 7) {
		bool u = btn & BIT(PB_UP), d = btn & BIT(PB_DOWN), l = btn & BIT(PB_LEFT), r = btn & BIT(PB_RIGHT);

		st.hat = u && r ? 1 : d && r ? 3 : d && l ? 5 : u && l ? 7 : u ? 0 : r ? 2 : d ? 4 : l ? 6 : 8;
	}
	st.btn = btn;
	st.valid = true;
}

/* HID düğme numarası (1..16) → mantıksal tuş. Linux/Android'in gamepad sırası. */
static const uint8_t btn_map[16] = {
	PB_A, PB_B, PB_C, PB_X, PB_Y, PB_Z, PB_LB, PB_RB,
	PB_LT, PB_RT, PB_SELECT, PB_START, PB_HOME, PB_L3, PB_R3, PB_EXTRA,
};

static int32_t sext(uint32_t v, int sz)
{
	if (sz == 1 && v > 127) return (int32_t)v - 256;
	if (sz == 2 && v > 32767) return (int32_t)v - 65536;
	return (int32_t)v;
}

int pad_parse_map(const uint8_t *b, size_t len)
{
	uint16_t page = 0, rsize = 0, rcount = 0;
	uint8_t rid = 0;
	int32_t lmin = 0, lmax = 0;
	uint32_t usages[16];
	int nus = 0;
	int32_t umin = -1, umax = -1;
	uint16_t off[256] = {0};
	bool has_z = false, has_rz = false, has_rx = false, has_ry = false;
	size_t i = 0;

	nfields = 0;
	while (i < len) {
		uint8_t p = b[i];
		int sz = (p & 3) == 3 ? 4 : (p & 3);
		uint8_t tag = p & 0xfc;
		uint32_t d = 0;

		if (i + 1 + sz > len) break;
		for (int k = 0; k < sz; k++) d |= (uint32_t)b[i + 1 + k] << (8 * k);
		i += 1 + sz;

		switch (tag) {
		case 0x04: page = d; break;
		case 0x14: lmin = sext(d, sz); break;
		case 0x24: lmax = sext(d, sz); break;
		case 0x74: rsize = d; break;
		case 0x94: rcount = d; break;
		case 0x84: rid = d; break;
		case 0x08:
			if (nus < 16) usages[nus++] = sz == 4 ? d : ((uint32_t)page << 16) | d;
			break;
		case 0x18: umin = d; break;
		case 0x28: umax = d; break;
		case 0xa0: /* collection */
		case 0x90: case 0xb0: /* output, feature */
			nus = 0; umin = umax = -1;
			break;
		case 0x80: { /* input */
			bool cnst = d & 1, var = d & 2;
			uint16_t o = off[rid];

			if (!cnst) {
				for (int k = 0; k < rcount && nfields < MAX_FIELDS; k++) {
					struct field *f = &fields[nfields];
					uint32_t u;

					if (nus) u = usages[MIN(k, nus - 1)];
					else if (umin >= 0) u = ((uint32_t)page << 16) | (uint32_t)(var ? MIN(umin + k, umax) : umin);
					else continue;
					f->rid = rid; f->bit = o + k * rsize; f->size = rsize;
					f->page = u >> 16; f->usage = u & 0xffff;
					f->lmin = lmin; f->lmax = lmax; f->role = R_NONE;
					if (!var) { f->role = (f->page == 0x0c) ? R_CONSUMER : R_NONE; }
					nfields++;
					if (!var) break; /* dizi alanlarından birini izlemek yeterli */
				}
			}
			off[rid] = o + rcount * rsize;
			nus = 0; umin = umax = -1;
			break;
		}
		default: break;
		}
	}

	for (int k = 0; k < nfields; k++) {
		struct field *f = &fields[k];
		if (f->page == 1 && f->usage == 0x32) has_z = true;
		if (f->page == 1 && f->usage == 0x35) has_rz = true;
		if (f->page == 1 && f->usage == 0x33) has_rx = true;
		if (f->page == 1 && f->usage == 0x34) has_ry = true;
	}

	for (int k = 0; k < nfields; k++) {
		struct field *f = &fields[k];

		if (f->role == R_CONSUMER) continue;
		if (f->page == 9 && f->usage >= 1 && f->usage <= 16) {
			f->role = R_BTN; f->arg = btn_map[f->usage - 1];
		} else if (f->page == 1) {
			switch (f->usage) {
			case 0x30: f->role = R_LX; break;
			case 0x31: f->role = R_LY; break;
			case 0x32: f->role = has_rz ? R_RX : R_LT; break;
			case 0x35: f->role = R_RY; break;
			case 0x33: f->role = (has_z && has_rz) ? R_LT : R_RX; break;
			case 0x34: f->role = (has_z && has_rz) ? R_RT : R_RY; break;
			case 0x39: f->role = R_HAT; break;
			}
		} else if (f->page == 2) {
			if (f->usage == 0xc5) f->role = R_LT; /* fren */
			if (f->usage == 0xc4) f->role = R_RT; /* gaz */
		}
	}
	ARG_UNUSED(has_rx); ARG_UNUSED(has_ry);

	OUT("{\"ev\":\"parsed\",\"fields\":%d}\n", nfields);
	return nfields ? 0 : -1;
}

static uint32_t get_bits(const uint8_t *d, size_t len, uint16_t bit, uint8_t size)
{
	uint32_t v = 0;

	for (int k = 0; k < size && k < 32; k++) {
		uint16_t B = bit + k;
		if ((B >> 3) < len && (d[B >> 3] & (1 << (B & 7)))) v |= 1u << k;
	}
	return v;
}

static uint8_t scale8(const struct field *f, uint32_t raw)
{
	int32_t v = f->lmin < 0 ? sext(raw, f->size <= 8 ? 1 : 2) : (int32_t)raw;
	int32_t span = f->lmax - f->lmin;

	if (span <= 0) return 0;
	v = (v - f->lmin) * 255 / span;
	return CLAMP(v, 0, 255);
}

void pad_input(uint8_t rid, const uint8_t *data, size_t len)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	rx_count++;
	if (ln) {
		learned_input(rid, data, len);
		k_spin_unlock(&lock, key);
		return;
	}
	for (int k = 0; k < nfields; k++) {
		struct field *f = &fields[k];
		uint32_t raw;

		if (f->rid != rid || f->role == R_NONE) continue;
		raw = get_bits(data, len, f->bit, f->size);
		switch (f->role) {
		case R_BTN:
			if (raw) st.btn |= BIT(f->arg); else st.btn &= ~BIT(f->arg);
			break;
		case R_LX: st.lx = scale8(f, raw); break;
		case R_LY: st.ly = scale8(f, raw); break;
		case R_RX: st.rx = scale8(f, raw); break;
		case R_RY: st.ry = scale8(f, raw); break;
		case R_LT: st.lt = scale8(f, raw); break;
		case R_RT: st.rt = scale8(f, raw); break;
		case R_HAT: {
			int32_t h = (int32_t)raw - f->lmin;
			st.hat = (h >= 0 && h <= 7) ? h : 8;
			break;
		}
		case R_CONSUMER:
			/* Bazı kontrolcüler Home'u medya tuşu olarak yollar (AC Home 0x223) */
			if (raw == 0x223) st.btn |= BIT(PB_HOME);
			else if (raw == 0) st.btn &= ~BIT(PB_HOME);
			break;
		}
	}

	/* yön tuşları */
	static const uint8_t hat_bits[9] = {
		BIT(0), BIT(0) | BIT(3), BIT(3), BIT(1) | BIT(3), BIT(1),
		BIT(1) | BIT(2), BIT(2), BIT(0) | BIT(2), 0,
	};
	uint8_t hb = hat_bits[MIN(st.hat, 8)];

	st.btn &= ~(BIT(PB_UP) | BIT(PB_DOWN) | BIT(PB_LEFT) | BIT(PB_RIGHT));
	if (hb & BIT(0)) st.btn |= BIT(PB_UP);
	if (hb & BIT(1)) st.btn |= BIT(PB_DOWN);
	if (hb & BIT(2)) st.btn |= BIT(PB_LEFT);
	if (hb & BIT(3)) st.btn |= BIT(PB_RIGHT);
	st.valid = true;

	k_spin_unlock(&lock, key);
}

void pad_get(struct pad_state *out)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*out = st;
	k_spin_unlock(&lock, key);
}

void pad_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	memset(&st, 0, sizeof(st));
	st.lx = st.ly = st.rx = st.ry = 128;
	st.hat = 8;
	k_spin_unlock(&lock, key);
}
