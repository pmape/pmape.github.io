/* Seri porta yalnızca karşıda dinleyen varsa yaz (tablette kimse dinlemez, takılmasın). */
#pragma once
#include <zephyr/sys/printk.h>
bool host_listening(void);
#define OUT(...) do { if (host_listening()) printk(__VA_ARGS__); } while (0)
