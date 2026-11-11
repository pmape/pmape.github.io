/*
 * Kartın USB belleği ("PADMAPPER"), salt okunur.
 * İçindeki dosyalar kart yazılımına gömülü gelir; hafızaya yazılmaz:
 *   AYAR.HTM      ayar ekranı (Chrome / Edge; USB ya da Bluetooth ile bağlanır, internet gerekmez)
 *   UYGULAMA.URL  Android uygulamasını indir
 *   BENIOKU.TXT   kısa not
 * Disk anında üretilir (FAT12, 4 MB görünür, 2 KB küme).
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/storage/disk_access.h>
#include "cfg.h"

static const uint8_t html_raw[] = { 0,
#include "html.inc"
};

#define PAGE_URL "https://imlegennd.github.io"
#define APK_URL "https://imlegennd.github.io/padmapper-ayar.apk"

static const char url_apk[] = "[InternetShortcut]\r\nURL=" APK_URL "\r\n";

static const char readme[] =
	"PadMapper " FW_VERSION "\r\n"
	"\r\n"
	"BILGISAYAR : AYAR.HTM dosyasini cift tikla (Chrome ya da Edge).\r\n"
	"             \"Baglan\" de, USB'yi sec, listeden PadMapper'i sec.\r\n"
	"             Bir kez izin verince sonraki seferlerde kendisi baglanir.\r\n"
	"\r\n"
	"TELEFON    : UYGULAMA.URL ile uygulamayi indir ya da Chrome'da\r\n"
	"             " PAGE_URL " adresini ac.\r\n"
	"             Uygulama kuruluysa kart takilinca kendisi acilir.\r\n"
	"\r\n"
	"BLUETOOTH  : Kart sadece guce takiliyken Bluetooth'ta gorunur;\r\n"
	"             adi ayar sayfasinin adresidir (" PAGE_URL ").\r\n"
	"             Yeni bir cihaz: karti guce tak, 1 dakika icinde\r\n"
	"             o cihazdan Baglan de; kart cihazi kaydeder.\r\n"
	"\r\n"
	"Bu bellek salt okunurdur; dosyalar kart yazilimiyla birlikte gelir.\r\n";

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

enum { F_HTML, F_APK, F_TXT, NF };
static const char *names[NF] = { "AYAR    HTM", "UYGULAMAURL", "BENIOKU TXT" };
static const uint8_t *fdata[NF];
static uint32_t fsize[NF], fstart[NF], fclus[NF];
static uint8_t fat[FATSZ * SEC];
static uint8_t root[(ROOTENT * 32)];

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
	fdata[F_HTML] = html_raw + 1; fsize[F_HTML] = sizeof(html_raw) - 1;
	fdata[F_APK] = (const uint8_t *)url_apk; fsize[F_APK] = sizeof(url_apk) - 1;
	fdata[F_TXT] = (const uint8_t *)readme; fsize[F_TXT] = sizeof(readme) - 1;

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

static void data_sector(uint8_t *b, uint32_t s)
{
	uint32_t c = (s - DATA) / SPC + 2;

	for (int i = 0; i < NF; i++) {
		if (!fclus[i] || c < fstart[i] || c >= fstart[i] + fclus[i]) continue;
		uint32_t off = (c - fstart[i]) * SPC * SEC + ((s - DATA) % SPC) * SEC;

		if (off >= fsize[i]) return;
		uint32_t n = MIN(SEC, fsize[i] - off);

		memcpy(b, fdata[i] + off, n);
		return;
	}
}

static int vd_init(struct disk_info *d) { return 0; }
/*
 * Zephyr'in USB bellek sınıfı bilgisayara "yazmaya kapalı" diyemiyor; yazma reddedilince
 * Windows cevap bekleyip takılıyordu (Dosya Gezgini yanıt vermiyor). Bu yüzden yazmalar
 * kabul edilir ve RAM'de küçük bir alanda tutulur: Windows yazdığını geri okuyabilir,
 * karta hiçbir şey kaydedilmez, kart çıkarılınca hepsi silinir.
 */
#define OVN 24
static uint32_t ov_sec[OVN];
static uint8_t ov_used;
static uint8_t ov_buf[OVN][SEC];

static int ov_find(uint32_t sector)
{
	for (int i = 0; i < ov_used; i++) {
		if (ov_sec[i] == sector) return i;
	}
	return -1;
}

static int vd_status(struct disk_info *d) { return DISK_STATUS_OK; }

static int vd_read(struct disk_info *d, uint8_t *buf, uint32_t sector, uint32_t count)
{
	for (uint32_t k = 0; k < count; k++, sector++, buf += SEC) {
		int o = ov_find(sector);

		if (o >= 0) {
			memcpy(buf, ov_buf[o], SEC);
			continue;
		}
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
	for (uint32_t k = 0; k < count; k++, sector++, buf += SEC) {
		int o = ov_find(sector);

		if (o < 0 && ov_used < OVN) {
			o = ov_used++;
			ov_sec[o] = sector;
		}
		if (o >= 0) memcpy(ov_buf[o], buf, SEC);
	}
	return 0;
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
