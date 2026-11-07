#pragma once
#include <stdbool.h>
#include <stdint.h>

int ble_out_init(void);
/* hedef bağlı ve şifreli mi */
bool ble_out_ready(void);
/* hedef aranıyor (yayın açık, bağlı değil) */
bool ble_out_searching(void);
/* seçilen yol Bluetooth mu (kartta saklı) */
bool ble_out_route_bt(void);
int ble_out_route(void);
void ble_out_set_route(int r);
/* 0 klavye, 1 fare, 2 gamepad */
int ble_out_send(int which, const uint8_t *buf, uint8_t len);
/* USB'de veri bağlantısı var mı (yoksa güç kaynağına takılı demektir) */
void ble_out_usb(bool present);
bool ble_cmd(char *l);
/* yayını yeniden değerlendir (ayar telefonu bağlanınca/ayrılınca) */
void ble_out_adv_kick(void);
