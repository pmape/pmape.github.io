#pragma once
#include <stdbool.h>

/* USB HID cihazlarını kaydet (usb_enable'dan ÖNCE çağrılmalı) */
int out_usb_init(void);
/* Işık ve çıkış iş parçacığını başlat */
void out_start(void);
/* Bağlantı durumu (ışık için) */
void out_set_connected(bool c);
