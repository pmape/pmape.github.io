/*
 * Eşleme motoru: ayar ekranından gelen profile göre pad durumunu
 * klavye / fare / gamepad raporlarına çevirir.
 *
 * Ayar biçimi (küçük uçlu, ayar ekranı bunu üretir):
 *  Başlık 12 bayt:
 *   'P' 'M' sürüm(1) açılış(0=eşleme açık,1=gamepad) kombA kombB tutma_ms(u16)
 *   bayraklar(b0=LB/RB ile profil, b1=atanmamış gamepad'e geçsin) profil_sayısı seçili boş
 *  Her profil:
 *   çıkış(0 kb+fare,1 kb,2 pad,3 hepsi) tetik_eşik(%) uzun_ms(u16) çift_ms(u16)
 *   4 katman x 2 çubuk (sol, sağ) x 7 bayt: mod hız ölü% yukarı aşağı sol sağ (HID kodu)
 *     mod: 0 ana gibi, 1 kapalı, 2 fare, 3 tuşlar, 4 tekerlek, 5 gamepad
 *   atama_sayısı(u16), sonra her atama:
 *     katman tuş tetik diğer tür tekrar döngü_ms(u16) hedef_katman nasıl adım_sayısı
 *       tetik: 0 tek, 1 çift, 2 uzun, 3 basma anı, 4 bırakma, 5 birlikte (diğer = öbür tuş)
 *       tür: 0 tuş dizisi, 1 katman değiştir   tekrar: 0 normal, 1 turbo, 2 toggle
 *       nasıl: 0 basılıyken, 1 bas aç/bas kapat
 *     her adım 5 bayt: tip(0 klavye,1 fare,2 pad) kod hareket(0 bas-bırak,1 basılı tut,2 bırak) sonra_ms(u16)
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "pad.h"

#define CFG_MAX 8192
#define NBTN 22 /* pad.h düğmeleri + M1, M2 */

struct out_rep {
	uint8_t kb[8];
	uint8_t ms[4];
	uint8_t g[9];
};

enum eng_mode { E_PAD, E_MAP, E_LEGACY };

/* Ayarı denetle (0 = geçerli) */
int engine_check(const uint8_t *b, size_t len);
/* Ayarı kullanmaya başla; b bellekte kalmalı */
int engine_set_config(const uint8_t *b, size_t len);
bool engine_has_config(void);
/* Her 8 ms'de bir */
enum eng_mode engine_tick(const struct pad_state *p, int64_t now, struct out_rep *r);
bool engine_mapping(void);
void engine_set_mapping(bool on);
int engine_select_profile(int n);
int engine_profile(void);
int engine_profile_count(void);
/* Profil kartın üzerinden değişince (kaydetmek için) */
extern void engine_profile_changed(int n);
