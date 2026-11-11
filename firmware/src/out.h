#pragma once
#include <stdbool.h>

/* USB HID cihazlarını kaydet (usb_enable'dan ÖNCE çağrılmalı) */
int out_usb_init(void);
/* Işık ve çıkış iş parçacığını başlat */
void out_start(void);
/* Bağlantı durumu (ışık için) */
void out_set_connected(bool c);
/* USB durum değişikliği (usb_enable geri çağrısından) */
void out_usb_status(int status);
/* Teşhis sayaçlarını seri porta yaz */
void out_print_stats(void);

/* pad raporu geldi: çıkışı hemen hesapla */
void out_kick(void);
/* durum mesajını hemen gönder */
void out_state_request(void);
void out_live(int on);
