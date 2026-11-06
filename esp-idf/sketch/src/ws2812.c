// -----------------------------------------------------------------------------
// The badge's 8 WS2812 LEDs on GPIO38, driven by the RMT peripheral.
// -----------------------------------------------------------------------------
#include "ws2812.h"

#include <string.h>
#include "driver/rmt_tx.h"
#include "esp_rom_sys.h"

#define RMT_HZ 10000000u   // 0.1 us per tick

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static uint8_t s_grb[WS2812_COUNT * 3];
static uint8_t s_brightness = 24;

void ws2812_init(void)
{
  rmt_tx_channel_config_t tx = {
    .gpio_num = WS2812_GPIO,
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = RMT_HZ,
    .mem_block_symbols = 64,
    .trans_queue_depth = 2,
  };
  ESP_ERROR_CHECK(rmt_new_tx_channel(&tx, &s_chan));

  // WS2812 bit timing: 0 = 0.3 us high, 0.9 us low; 1 = 0.9 us high, 0.3 us low
  rmt_bytes_encoder_config_t enc = {
    .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
    .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
    .flags.msb_first = 1,
  };
  ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc, &s_enc));
  ESP_ERROR_CHECK(rmt_enable(s_chan));
  ws2812_fill(0, 0, 0);
}

void ws2812_set_brightness(uint8_t b) { s_brightness = b; }

static uint8_t scale(uint8_t v)
{
  if (!v) return 0;
  uint8_t s = (uint8_t)((v * s_brightness + 127) / 255);
  return s ? s : 1;
}

void ws2812_set(int i, uint8_t r, uint8_t g, uint8_t b)
{
  if (i < 0 || i >= WS2812_COUNT) return;
  s_grb[i * 3 + 0] = scale(g);
  s_grb[i * 3 + 1] = scale(r);
  s_grb[i * 3 + 2] = scale(b);
}

void ws2812_show(void)
{
  rmt_transmit_config_t cfg = { .loop_count = 0 };
  rmt_transmit(s_chan, s_enc, s_grb, sizeof(s_grb), &cfg);
  rmt_tx_wait_all_done(s_chan, 100);
  esp_rom_delay_us(80);   // >50 us low latches the colors
}

void ws2812_fill(uint8_t r, uint8_t g, uint8_t b)
{
  for (int i = 0; i < WS2812_COUNT; i++) ws2812_set(i, r, g, b);
  ws2812_show();
}
