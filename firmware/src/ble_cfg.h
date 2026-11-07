/* Telefondan kablosuz ayar: sadece bu karta USB ile bir kez tanıtılmış uygulama bağlanabilir. */
#pragma once
#include <stdbool.h>
#include <stdarg.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>

#define PM_CFG_UUID_BYTES BT_UUID_128_ENCODE(0x7a1e0001, 0x3c4b, 0x4a8e, 0x9c1d, 0x50ad2840f00d)
/* ayar için yayın yapılan ikinci kimlik (eşleşmiş hedefler bunu tanımaz, kendiliğinden bağlanmaz) */
#define PM_CFG_ID 1

void ble_cfg_init(void);
/* doğrulanmış ayar telefonu bu bağlantı mı */
bool ble_cfg_is(struct bt_conn *c);
/* doğrulanmış bir ayar telefonu bağlı mı */
bool ble_cfg_authed(void);
/* ayar telefonu için yayın gerekli mi */
bool ble_cfg_want_adv(void);
void ble_cfg_disconnected(struct bt_conn *c);
/* OUT çıktısını telefona da gönder */
void ble_cfg_vout(const char *fmt, va_list ap);
/* telefondan gelen bir satırı al (cfg iş parçacığı) */
bool ble_cfg_line(char *buf, size_t n);
/* USB'den: secret add <64 hex> | secret clear | secret count */
bool ble_cfg_cmd(char *l);
