/* Çıktı: USB seri portta dinleyen varsa oraya, doğrulanmış ayar telefonu bağlıysa ona da yazılır
 * (tablette kimse dinlemez, takılmasın). */
#pragma once
#include <stdbool.h>
#include <zephyr/sys/printk.h>
#include <zephyr/toolchain.h>
bool host_listening(void);
bool usb_listening(void);
void pm_out(const char *fmt, ...) __printf_like(1, 2);
#define OUT(...) pm_out(__VA_ARGS__)
