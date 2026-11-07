/* Bluetooth klavye ve fare girişi (pad'in yanında, en fazla 2 cihaz) */
#pragma once
#include <stdbool.h>
#include <zephyr/bluetooth/conn.h>
#include "hid_map.h"

void hid_in_init(void);
/* bu bağlantı klavye/fare bağlantısı mı (pad değil) */
bool hid_in_is(struct bt_conn *c);
/* kayıtlı klavye/fare adresi mi */
bool hid_in_known(const bt_addr_le_t *a);
/* bağlanmayı bekleyen kayıtlı cihaz var mı (tarama sürsün) */
bool hid_in_need_scan(void);
/* taramada görülen cihaza bağlanılacak mı; bağlanma başlatıldıysa true */
bool hid_in_try(const bt_addr_le_t *a, const char *name);
/* adres filtresine eklemek için */
int hid_in_addrs(bt_addr_le_t *out, int max);
/* çıkış iş parçacığı: güncel giriş */
void hid_in_get(struct hid_in *o);
const struct hid_cfg *hid_in_cfg(void);
bool hid_in_active(void);
/* komutlar: hid ... */
bool hid_cmd(char *l);
