#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WS2812_GPIO  38
#define WS2812_COUNT 8

void ws2812_init(void);
// 0-255, applied to every color; a non-zero channel never scales below 1.
void ws2812_set_brightness(uint8_t b);
void ws2812_set(int i, uint8_t r, uint8_t g, uint8_t b);  // buffered
void ws2812_show(void);
void ws2812_fill(uint8_t r, uint8_t g, uint8_t b);          // set all + show

#ifdef __cplusplus
}
#endif
