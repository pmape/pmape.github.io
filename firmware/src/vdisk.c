/*
 * Kartın USB belleği ("PADMAPPER"), salt okunur.
 * İçindeki dosyalar kart yazılımına gömülü gelir; hafızaya yazılmaz:
 *   PADMAPPR.APK  tablet uygulaması
 *   AYAR.HTM      bilgisayar için ayar ekranı (Chrome)
 *   KART.UF2      karttaki yazılımın kendisi (çalışan yazılımdan anında üretilir)
 *   BENIOKU.TXT   kısa not
 * Disk anında üretilir (FAT12, 4 MB görünür, 2 KB küme).
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/storage/disk_access.h>
#include "cfg.h"

static const uint8_t apk_raw[] = { 0,
#include "apk.inc"
};
static const uint8_t html_raw[] = { 0,
#include "html.inc"
};

static const char readme[] =
	"PadMapper " FW_VERSION "\r\n"
	"\r\n"
	"PADMAPPR.APK : Tablet uygulamasi. Dosyaya dokun ve kur\r\n"
	"               (bilinmeyen kaynaktan yukleme izni ister).\r\n"
	"               Kart takilinca ayar ekrani kendiliginden acilir.\r\n"
	"AYAR.HTM     : Bilgisayarda Chrome ile ac, \"Kart\" dugmesine bas.\r\n"
	"KART.UF2     : Kartin yazilimi. Yeniden yuklemek icin karttaki\r\n"
	"               RST dugmesine iki kez hizlica bas, acilan surucuye\r\n"
	"               bu dosyayi kopyala.\r\n"
	"\r\n"
	"Bu bellek salt okunurdur; dosyalar kart yazilimiyla birlikte gelir.\r\n";

/* çalışan yazılımın flaştaki yeri (bağlayıcı sembolleri) */
extern char __rom_region_start[];
extern char _flash_used[];

#define SEC 512
#define SPC 4                 /* küme başına kesim */
#define TOTAL 8192            /* 4 MB */
#define FATSZ 6
#define NFAT 2
#define ROOTENT 32
#define FAT1 1
#define ROOT (FAT1 + NFAT * FATSZ)
#define DATA (ROOT + (ROOTENT * 32) / SEC)
#define CLUSTERS ((TOTAL - DATA) / SPC)

enum { F_APK, F_HTML, F_UF2, F_TXT, NF };
static const char *names[NF] = { "PADMAPPRAPK", "AYAR    HTM", "KART    UF2", "BENIOKU TXT" };
static uint32_t fsize[NF], fstart[NF], fclus[NF];
static uint8_t fat[FATSZ * SEC];
static uint8_t root[(ROOTENT * 32)];
static uint32_t uf2_base, uf2_blocks;

static void fat_set(uint32_t k, uint16_t v)
{
	uint32_t o = k * 3 / 2;

	if (o + 1 >= sizeof(fat)) return;
	if (k & 1) {
		fat[o] = (fat[o] & 0x0f) | ((v & 0x0f) << 4);
		fat[o + 1] = v >> 4;
	} else {
		fat[o] = v & 0xff;
		fat[o + 1] = (fat[o + 1] & 0xf0) | ((v >> 8) & 0x0f);
	}
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void build(void)
{
	uf2_base = (uint32_t)__rom_region_start;
	uf2_blocks = ((uint32_t)_flash_used + 255) / 256;
	fsize[F_APK] = sizeof(apk_raw) - 1;
	fsize[F_HTML] = sizeof(html_raw) - 1;
	fsize[F_UF2] = uf2_blocks * 512;
	fsize[F_TXT] = sizeof(readme) - 1;

	memset(fat, 0, sizeof(fat));
	fat_set(0, 0xff8);
	fat_set(1, 0xfff);
	uint32_t c = 2;

	for (int i = 0; i < NF; i++) {
		uint32_t n = (fsize[i] + SPC * SEC - 1) / (SPC * SEC);

		if (c + n > CLUSTERS + 2) n = 0; /* sığmazsa boş göster */
		fstart[i] = n ? c : 0;
		fclus[i] = n;
		for (uint32_t k = 0; k < n; k++) fat_set(c + k, k + 1 < n ? c + k + 1 : 0xfff);
		c += n;
	}

	memset(root, 0, sizeof(root));
	uint8_t *e = root;

	memcpy(e, "PADMAPPER  ", 11);
	e[11] = 0x08; /* birim adı */
	e += 32;
	for (int i = 0; i < NF; i++, e += 32) {
		memcpy(e, names[i], 11);
		e[11] = 0x01 | 0x20;        /* salt okunur, arşiv */
		put16(e + 14, 12 << 11);    /* 12:00 */
		put16(e + 16, ((2026 - 1980) << 9) | (11 << 5) | 7);
		put16(e + 18, ((2026 - 1980) << 9) | (11 << 5) | 7);
		put16(e + 22, 12 << 11);
		put16(e + 24, ((2026 - 1980) << 9) | (11 << 5) | 7);
		put16(e + 26, fstart[i]);
		put32(e + 28, fclus[i] ? fsize[i] : 0);
	}
}

static void boot_sector(uint8_t *b)
{
	static const uint8_t head[] = { 0xeb, 0x3c, 0x90, 'M', 'S', 'D', 'O', 'S', '5', '.', '0' };

	memcpy(b, head, sizeof(head));
	put16(b + 11, SEC);
	b[13] = SPC;
	put16(b + 14, 1);
	b[16] = NFAT;
	put16(b + 17, ROOTENT);
	put16(b + 19, TOTAL);
	b[21] = 0xf8;
	put16(b + 22, FATSZ);
	put16(b + 24, 32);
	put16(b + 26, 2);
	b[36] = 0x80;
	b[38] = 0x29;
	put32(b + 39, 0x50AD2840);
	memcpy(b + 43, "PADMAPPER  ", 11);
	memcpy(b + 54, "FAT12   ", 8);
	b[510] = 0x55;
	b[511] = 0xaa;
}

static void uf2_block(uint8_t *b, uint32_t i)
{
	put32(b + 0, 0x0A324655);
	put32(b + 4, 0x9E5D5157);
	put32(b + 8, 0x00002000);          /* aile kimliği var */
	put32(b + 12, uf2_base + i * 256);
	put32(b + 16, 256);
	put32(b + 20, i);
	put32(b + 24, uf2_blocks);
	put32(b + 28, 0xADA52840);         /* nRF52840 */
	uint32_t len = MIN(256, (uint32_t)_flash_used - i * 256);

	memcpy(b + 32, (const void *)(uf2_base + i * 256), len);
	put32(b + 508, 0x0AB16F30);
}

static void data_sector(uint8_t *b, uint32_t s)
{
	uint32_t c = (s - DATA) / SPC + 2;

	for (int i = 0; i < NF; i++) {
		if (!fclus[i] || c < fstart[i] || c >= fstart[i] + fclus[i]) continue;
		uint32_t off = (c - fstart[i]) * SPC * SEC + ((s - DATA) % SPC) * SEC;

		if (off >= fsize[i]) return;
		uint32_t n = MIN(SEC, fsize[i] - off);

		switch (i) {
		case F_APK: memcpy(b, apk_raw + 1 + off, n); break;
		case F_HTML: memcpy(b, html_raw + 1 + off, n); break;
		case F_TXT: memcpy(b, readme + off, n); break;
		case F_UF2: uf2_block(b, off / 512); break;
		}
		return;
	}
}

static int vd_init(struct disk_info *d) { return 0; }
static int vd_status(struct disk_info *d) { return DISK_STATUS_WR_PROTECT; }

static int vd_read(struct disk_info *d, uint8_t *buf, uint32_t sector, uint32_t count)
{
	for (uint32_t k = 0; k < count; k++, sector++, buf += SEC) {
		memset(buf, 0, SEC);
		if (sector == 0) boot_sector(buf);
		else if (sector < ROOT) {
			uint32_t f = (sector - FAT1) % FATSZ;

			memcpy(buf, fat + f * SEC, SEC);
		} else if (sector < DATA) memcpy(buf, root + (sector - ROOT) * SEC, SEC);
		else if (sector < TOTAL) data_sector(buf, sector);
	}
	return 0;
}

static int vd_write(struct disk_info *d, const uint8_t *buf, uint32_t sector, uint32_t count)
{
	return -EROFS;
}

static int vd_ioctl(struct disk_info *d, uint8_t cmd, void *buf)
{
	switch (cmd) {
	case DISK_IOCTL_GET_SECTOR_COUNT: *(uint32_t *)buf = TOTAL; return 0;
	case DISK_IOCTL_GET_SECTOR_SIZE: *(uint32_t *)buf = SEC; return 0;
	case DISK_IOCTL_GET_ERASE_BLOCK_SZ: *(uint32_t *)buf = 1; return 0;
	default: return 0;
	}
}

static const struct disk_operations vd_ops = {
	.init = vd_init,
	.status = vd_status,
	.read = vd_read,
	.write = vd_write,
	.ioctl = vd_ioctl,
};

static struct disk_info vd = { .name = "PM", .ops = &vd_ops };

static int vd_setup(void)
{
	build();
	return disk_access_register(&vd);
}

SYS_INIT(vd_setup, POST_KERNEL, 90);
