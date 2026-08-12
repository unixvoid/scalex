#ifndef LED_H
#define LED_H

#include <stdint.h>
#include <stdbool.h>

void init_led(void);
void set_color_scale(const char *color);
void refresh_led_colors(void);
bool led_is_refreshing(void);
void deinit_led(void);  // Deinitialize LED strips for hibernation

#endif // LED_H
