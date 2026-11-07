// -----------------------------------------------------------------------------
// Sketch - standalone ESP-IDF firmware for the 2026 Supercon billboard badge.
//
// A knob-drawing toy: the left knob moves the pen left/right, the right knob
// up/down (clockwise = right / up). Shake the badge hard, or press User B, to
// erase - the drawing is uploaded to the gallery website as it's erased. (With
// only one knob, User B switches which axis it drives instead.)
//
// User A opens the gallery: the label, then the newest drawings from the
// website with their times. Turn either knob to flip through them; they also
// step along on their own. User A (or B) goes back to the drawing as it was.
//
// Wi-Fi and the website are set in src/wifi_secrets.h; all of it lives in
// net.c, in its own task. While you draw, the website's live view follows.
//
// Hardware
//   Knobs   AS5600 magnetic encoder SAOs, analog OUT on the SAO's GPIO1. Every
//           AS5600 is at I2C 0x36, so they're read with the ADC. The SAO GPIO1
//           pins (GPIO2/4/7) are probed with pull-down vs pull-up: a floating
//           pin follows the pull, a driven one doesn't. First two = X, Y.
//   Shake   optional MPU6050 on the SAO I2C bus (SDA 17, SCL 18), 0x68/0x69.
//   LEDs    8x WS2812 on GPIO38: 0-3 follow the X knob (cyan), 4-7 the Y
//           knob (magenta) - a dot spins the way you turn, and that side
//           flashes red when the pen hits a wall. Off while the knobs are still.
//
// Refreshing
//   The Hack-A-Sketch frame (data/gallery/HackASketch.png, baked into
//   etch_frame.h) goes up once with EPD_SLOW, the only mode that makes good red.
//   New lines go out with EPD_FAST_INK: only the pixels drawn since the last
//   refresh are driven, so the rest of the panel doesn't flash. Erases use
//   EPD_FAST_KEEP_RED, which drives black/white and leaves the red frame alone.
//   The COG session is kept open between images.
// -----------------------------------------------------------------------------
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "epd97.h"
#include "etch_frame.h"
#include "net.h"
#include "ws2812.h"

// The gallery's title card: a drawing made on the badge, baked in by
// tools/drawing_to_label.py. Until there is one, the title is text.
#if __has_include("gallery_label.h")
#include "gallery_label.h"
#define HAVE_LABEL 1
#else
#define HAVE_LABEL 0
#endif

// --- Tunables -----------------------------------------------------------------
#define PX_PER_REV        240     // pen travel per full knob turn
#define LINE_W            3       // line thickness, px
#define DEADBAND          12      // ADC noise to ignore, counts of 4096
#define ADC_SAMPLES       8
#define IDLE_REFRESH_MS   100     // redraw this long after the knobs stop...
#define MAX_WAIT_MS       300     // ...or at least this often while drawing
#define LED_TURN_HOLD_MS  400     // a knob's LEDs stay lit this long after a turn
#define LED_WALL_MS       250     // ...and flash red this long when the pen hits a wall
#define LED_DOT_STEPS     16      // dot steps per knob revolution
#define SLEEP_AFTER_MS    30000   // power the panel down after this long idle
#define CLEAN_EVERY_MS    300000  // full refresh to restore red and black, at most
#define CLEAN_IDLE_MS     15000   // this often, once the knobs have rested this long
#define LED_BRIGHTNESS    24      // of 255
#define INVERT_X          0
#define INVERT_Y          0
#define SWAP_KNOBS        0
#define FLIP_180          1       // badge held upside down: screen turned, knobs swapped

#define SHAKE_G           2.2f    // a jolt is this far from 1 g
#define SHAKE_PEAKS       4       // this many jolts...
#define SHAKE_WINDOW_MS   1200    // ...within this long erases
#define SHAKE_COOLDOWN_MS 3000

#define GALLERY_LABEL_MS  1500    // the title card stays up at least this long
#define GALLERY_STEP      512     // knob counts (of 4096 per turn) per drawing
#define GALLERY_SETTLE_MS 250     // show the drawing once the knob rests this long
#define GALLERY_AUTO_MS   10000   // step to the next drawing on its own this often...
#define GALLERY_AUTO_FOR  300000  // ...for this long after the last knob turn
#define GALLERY_FAIL_MS   4000    // "offline" stays up this long, then back to drawing

// --- Pins ---------------------------------------------------------------------
#define BTN_USER_A   39
#define BTN_USER_B   16
#define SAO_SDA      17
#define SAO_SCL      18

// --- Frame geometry (matches data/gallery/HackASketch.png) -------------------
// Largest all-white box inside the image's screen, clear of the rounded corners
#define SCR_X0  132
#define SCR_Y0  121
#define SCR_X1  826
#define SCR_Y1  524
#define INSET   (LINE_W / 2 + 1)
#define X_MIN   (SCR_X0 + INSET)
#define Y_MIN   (SCR_Y0 + INSET)
#define X_MAX   (SCR_X1 - INSET)
#define Y_MAX   (SCR_Y1 - INSET)
#define STATUS_Y 590

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// =============================================================================
// Display task: refreshes run here so the knobs stay live during them.
// =============================================================================
static TaskHandle_t s_epd_task;
static volatile bool s_req_pending;
static volatile bool s_req_sleep;
static volatile epd_refresh_t s_req_mode;
static volatile bool s_epd_busy;

static void epd_task(void* arg)
{
  (void)arg;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (s_req_pending) {
      s_epd_busy = true;
      epd_refresh_t m = s_req_mode;
      s_req_pending = false;
      refresh_display(m);
    }
    if (s_req_sleep && !s_req_pending) {
      s_req_sleep = false;
      epd_sleep();
    }
    s_epd_busy = false;
  }
}

static void request_refresh(epd_refresh_t mode)
{
  // A pending full refresh wins over a fast one: it draws everything anyway.
  if (!(s_req_pending && s_req_mode == EPD_SLOW)) s_req_mode = mode;
  s_req_pending = true;
  s_req_sleep = false;
  xTaskNotifyGive(s_epd_task);
}

static void request_sleep(void)
{
  s_req_sleep = true;
  xTaskNotifyGive(s_epd_task);
}

// =============================================================================
// Drawing
// =============================================================================
static void stamp(int x, int y)
{
  epd_fill_rect(x - LINE_W / 2, y - LINE_W / 2, LINE_W, LINE_W, EPD_BLACK);
}

static void draw_line(int x0, int y0, int x1, int y1)
{
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    stamp(x0, y0);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// Unpack the RLE'd frame: (count, byte) pairs over the bw plane, then red.
static void draw_frame(const char* status)
{
  uint8_t* bw = epd_plane_bw();
  uint8_t* red = epd_plane_red();
  size_t o = 0;
  for (size_t i = 0; i < sizeof(k_etch_frame_rle); i += 2) {
    for (int n = k_etch_frame_rle[i]; n > 0 && o < 2 * EPD_PLANE_BYTES; n--, o++) {
      if (o < EPD_PLANE_BYTES) bw[o] = k_etch_frame_rle[i + 1];
      else                     red[o - EPD_PLANE_BYTES] = k_etch_frame_rle[i + 1];
    }
  }
  if (status) {
    epd_text((EPD_W - epd_text_width(status, 2)) / 2, STATUS_Y, status, EPD_WHITE, 2);
  }
}

// =============================================================================
// Knobs: AS5600 analog output on the SAO GPIO1 pins
// =============================================================================
typedef struct {
  int gpio;
  const char* name;
  adc_channel_t ch;
  adc_cali_handle_t cali;
  int last;
  bool have_last;
} knob_t;

static adc_oneshot_unit_handle_t s_adc;
static knob_t s_cand[3] = { { .gpio = 2, .name = "SAO1" }, { .gpio = 4, .name = "SAO2" },
                             { .gpio = 7, .name = "SAO3" } };
static knob_t* s_knob_x;
static knob_t* s_knob_y;
static int s_lo_mv = 0, s_hi_mv = 3300;

static int knob_mv(knob_t* k, int n)
{
  int sum = 0;
  for (int i = 0; i < n; i++) {
    int raw = 0, mv = 0;
    adc_oneshot_read(s_adc, k->ch, &raw);
    if (k->cali) adc_cali_raw_to_voltage(k->cali, raw, &mv);
    else         mv = raw * 3100 / 4095;
    sum += mv;
  }
  return sum / n;
}

static bool knob_init(knob_t* k)
{
  adc_unit_t unit;
  if (adc_oneshot_io_to_channel(k->gpio, &unit, &k->ch) != ESP_OK || unit != ADC_UNIT_1) return false;
  adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
  if (adc_oneshot_config_channel(s_adc, k->ch, &cc) != ESP_OK) return false;
  adc_cali_curve_fitting_config_t cf = {
    .unit_id = ADC_UNIT_1, .chan = k->ch, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  if (adc_cali_create_scheme_curve_fitting(&cf, &k->cali) != ESP_OK) k->cali = NULL;
  return true;
}

// A floating pin follows the internal pull; an AS5600 output holds its level.
// With the output set to 10-90 % of VDD a knob never reads below ~330 mV, so
// `min_mv` also rejects a pin that something on the board holds near ground.
static bool knob_probe(knob_t* k, int min_mv)
{
  gpio_set_pull_mode(k->gpio, GPIO_PULLDOWN_ONLY);
  vTaskDelay(pdMS_TO_TICKS(3));
  int lo = knob_mv(k, 8);
  gpio_set_pull_mode(k->gpio, GPIO_PULLUP_ONLY);
  vTaskDelay(pdMS_TO_TICKS(3));
  int hi = knob_mv(k, 8);
  gpio_set_pull_mode(k->gpio, GPIO_FLOATING);
  bool driven = abs(hi - lo) < 300 && lo >= min_mv && hi >= min_mv;
  printf("sketch: %s GPIO%d pulled down %d mV, up %d mV -> %s\n",
         k->name, k->gpio, lo, hi, driven ? "knob" : "nothing");
  return driven;
}

static int knob_delta(knob_t* k)
{
  int a = (knob_mv(k, ADC_SAMPLES) - s_lo_mv) * 4096 / (s_hi_mv - s_lo_mv);
  if (a < 0) a = 0;
  if (a > 4095) a = 4095;
  if (!k->have_last) { k->last = a; k->have_last = true; return 0; }
  int d = ((a - k->last + 2048) & 4095) - 2048;
  if (d >= -DEADBAND && d <= DEADBAND) return 0;   // keep `last`: slow turns add up
  k->last = a;
  return d;
}

// =============================================================================
// SAO I2C bus: AS5600 output range, MPU6050 shake
// =============================================================================
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_mpu;

static i2c_master_dev_handle_t i2c_dev(uint16_t addr)
{
  i2c_device_config_t dc = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000,
  };
  i2c_master_dev_handle_t d = NULL;
  i2c_master_bus_add_device(s_i2c, &dc, &d);
  return d;
}

static void i2c_init(void)
{
  i2c_master_bus_config_t bc = {
    .i2c_port = I2C_NUM_0, .sda_io_num = SAO_SDA, .scl_io_num = SAO_SCL,
    .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = true,
  };
  if (i2c_new_master_bus(&bc, &s_i2c) != ESP_OK) s_i2c = NULL;
}

// CONF.OUTS = 01: analog output 10-90 % of VDD, inside the ADC's ~3.1 V range.
// Every AS5600 sits at 0x36 and takes the same write. Volatile, so redone at boot.
static bool as5600_reduced_range(void)
{
  if (!s_i2c || i2c_master_probe(s_i2c, 0x36, 50) != ESP_OK) return false;
  i2c_master_dev_handle_t d = i2c_dev(0x36);
  const uint8_t conf[3] = { 0x07, 0x00, 0x10 };
  bool ok = d && i2c_master_transmit(d, conf, sizeof(conf), 50) == ESP_OK;
  if (d) i2c_master_bus_rm_device(d);
  return ok;
}

static void mpu_init(void)
{
  if (!s_i2c) return;
  for (uint16_t a = 0x68; a <= 0x69; a++) {
    if (i2c_master_probe(s_i2c, a, 50) != ESP_OK) continue;
    i2c_master_dev_handle_t d = i2c_dev(a);
    uint8_t reg = 0x75, who = 0;
    if (d && i2c_master_transmit_receive(d, &reg, 1, &who, 1, 50) == ESP_OK &&
        (who == 0x68 || who == 0x70 || who == 0x71 || who == 0x72 || who == 0x73 || who == 0x98)) {
      const uint8_t init[3][2] = { { 0x6B, 0x00 },     // wake up
                                   { 0x1C, 0x10 },     // accel +-8 g
                                   { 0x1A, 0x03 } };   // DLPF ~44 Hz
      for (int i = 0; i < 3; i++) i2c_master_transmit(d, init[i], 2, 50);
      s_mpu = d;
      printf("sketch: MPU6050 at 0x%02X, shake to erase\n", a);
      return;
    }
    if (d) i2c_master_bus_rm_device(d);
  }
}

static bool shake_detected(void)
{
  static uint32_t peaks[SHAKE_PEAKS];
  static int npeaks;
  static bool in_peak;
  static uint32_t cooldown_until;
  if (!s_mpu) return false;
  uint32_t t = now_ms();
  if ((int32_t)(cooldown_until - t) > 0) return false;

  uint8_t reg = 0x3B, d[6];
  if (i2c_master_transmit_receive(s_mpu, &reg, 1, d, 6, 20) != ESP_OK) return false;
  float g2 = 0;
  for (int i = 0; i < 6; i += 2) {
    float g = (int16_t)((d[i] << 8) | d[i + 1]) / 4096.0f;   // 4096 LSB/g at +-8 g
    g2 += g * g;
  }
  float jolt = fabsf(sqrtf(g2) - 1.0f);

  // Drop jolts older than the window
  int k = 0;
  for (int i = 0; i < npeaks; i++) if (t - peaks[i] < SHAKE_WINDOW_MS) peaks[k++] = peaks[i];
  npeaks = k;

  if (jolt > SHAKE_G && !in_peak) {
    in_peak = true;
    if (npeaks < SHAKE_PEAKS) peaks[npeaks++] = t;
  } else if (jolt < SHAKE_G * 0.5f) {
    in_peak = false;
  }
  if (npeaks >= SHAKE_PEAKS) {
    npeaks = 0;
    cooldown_until = t + SHAKE_COOLDOWN_MS;
    return true;
  }
  return false;
}

// =============================================================================
// Buttons (active low, debounced over 3 polls)
// =============================================================================
typedef struct { int gpio; bool stable; int count; } button_t;

static bool button_pressed(button_t* b)
{
  bool down = gpio_get_level(b->gpio) == 0;
  if (down == b->stable) { b->count = 0; return false; }
  if (++b->count < 3) return false;
  b->stable = down;
  b->count = 0;
  return down;   // true once, on the press
}

// =============================================================================
// LEDs: a spinning dot per knob while it turns, red at a wall, off otherwise
// =============================================================================
// Indexed by axis (0 = X, 1 = Y). Each axis lights the four LEDs on its own
// knob's side: 0-3 for the first knob found, 4-7 for the second.
static float s_phase[2];            // dot position, in LED steps
static uint32_t s_turn_ms[2], s_wall_ms[2];
static int s_led_group[2] = { 0, 1 };

static void leds_turned(int axis, int counts, uint32_t t)
{
  s_phase[axis] += counts * (float)LED_DOT_STEPS / 4096.0f;
  s_turn_ms[axis] = t;
}

// Network feedback: a sweep across all eight that wins over the knob dots
static uint32_t s_flash_ms, s_flash_len;
static uint8_t s_flash_rgb[3];

static void leds_flash(uint8_t r, uint8_t g, uint8_t b, uint32_t len, uint32_t t)
{
  s_flash_rgb[0] = r; s_flash_rgb[1] = g; s_flash_rgb[2] = b;
  s_flash_ms = t;
  s_flash_len = len;
}

static void leds_update(uint32_t t)
{
  static uint8_t last[WS2812_COUNT][3];
  static bool have_last;
  uint8_t f[WS2812_COUNT][3] = { { 0 } };

  if (net_take_uploaded())  leds_flash(0, 255, 40, 700, t);    // green: on the website
  if (net_take_connected()) leds_flash(0, 60, 255, 700, t);    // blue: joined Wi-Fi

  if (s_flash_ms && t - s_flash_ms < s_flash_len) {
    // Light fills in from LED 0 to 7 over the first half, then holds
    int lit = (int)((t - s_flash_ms) * 2 * WS2812_COUNT / s_flash_len) + 1;
    for (int i = 0; i < WS2812_COUNT && i < lit; i++) memcpy(f[i], s_flash_rgb, 3);
    goto out;
  }
  for (int a = 0; a < 2; a++) {
    uint8_t* side = f[s_led_group[a] * 4];
    if (s_wall_ms[a] && t - s_wall_ms[a] < LED_WALL_MS) {
      for (int i = 0; i < 4; i++) { side[i * 3] = 255; side[i * 3 + 1] = 0; side[i * 3 + 2] = 0; }
    } else if (s_turn_ms[a] && t - s_turn_ms[a] < LED_TURN_HOLD_MS) {
      int pos = (((int)floorf(s_phase[a])) % 4 + 4) % 4;
      if (FLIP_180) pos = 3 - pos;   // the strip runs the other way too
      uint8_t* q = f[s_led_group[a] * 4 + pos];
      if (a == 0) { q[0] = 0;   q[1] = 255; q[2] = 255; }   // cyan
      else        { q[0] = 255; q[1] = 0;   q[2] = 255; }   // magenta
    }
  }
out:
  if (have_last && memcmp(f, last, sizeof(f)) == 0) return;
  memcpy(last, f, sizeof(f));
  have_last = true;
  for (int i = 0; i < WS2812_COUNT; i++) ws2812_set(i, f[i][0], f[i][1], f[i][2]);
  ws2812_show();
}

// =============================================================================
// Pages: the gallery's title card and its drawings
// =============================================================================
static void draw_path(const int16_t* xy, int n)
{
  int px = 0, py = 0;
  for (int i = 0; i < n; i++) {
    int x = xy[2 * i], y = xy[2 * i + 1];
    x = x < X_MIN ? X_MIN : x > X_MAX ? X_MAX : x;
    y = y < Y_MIN ? Y_MIN : y > Y_MAX ? Y_MAX : y;
    if (i == 0) stamp(x, y);
    else        draw_line(px, py, x, y);
    px = x;
    py = y;
  }
}

// Black text on a white patch, so it reads over a drawing
#define CAPTION_SCALE 2
#define CAPTION_Y     (Y_MAX - 8 * CAPTION_SCALE - 6)

static void caption(int x, const char* s)
{
  int w = epd_text_width(s, CAPTION_SCALE);
  epd_fill_rect(x - 4, CAPTION_Y - 4, w + 8, 8 * CAPTION_SCALE + 6, EPD_WHITE);
  epd_text(x, CAPTION_Y, s, EPD_BLACK, CAPTION_SCALE);
}

static void caption_right(const char* s)
{
  caption(X_MAX - 6 - epd_text_width(s, CAPTION_SCALE), s);
}

static void page_label(const char* note)
{
  draw_frame(NULL);
#if HAVE_LABEL
  draw_path(k_gallery_label, k_gallery_label_n);
#else
  const char* title = "GALLERY";
  int w = epd_text_width(title, 10);
  epd_text((SCR_X0 + SCR_X1 - w) / 2, (SCR_Y0 + SCR_Y1) / 2 - 40, title, EPD_BLACK, 10);
#endif
  if (note) caption_right(note);
  request_refresh(EPD_FAST_KEEP_RED);
}

static void page_drawing(int i, int n)
{
  const net_gallery_item_t* it = net_gallery_item(i);
  draw_frame(NULL);
  if (it) draw_path(it->xy, it->n);
  char num[16];
  snprintf(num, sizeof(num), "%d/%d", i + 1, n);
  caption(X_MIN + 6, num);
  if (it && it->t[0]) caption_right(it->t);
  request_refresh(EPD_FAST_KEEP_RED);
}

// =============================================================================
// App
// =============================================================================
static int s_px = (X_MIN + X_MAX) / 2, s_py = (Y_MIN + Y_MAX) / 2;
static float s_fx, s_fy;
static bool s_single_x = true;   // one-knob mode: which axis it drives
static uint8_t* s_saved[2];      // the drawing's planes while the gallery is up

static const char* status_text(void)
{
  if (s_knob_x && s_knob_y) return NULL;
  if (s_knob_x) return "one knob found - User B switches X/Y";
  return "no knobs found - AS5600 OUT to SAO GPIO1";
}

static void erase(epd_refresh_t mode)
{
  draw_frame(status_text());
  stamp(s_px, s_py);
  request_refresh(mode);
}

// Erase, sending the drawing to the website on the way out
static void erase_and_upload(uint32_t t)
{
  if (net_path_finish() && !net_online()) leds_flash(255, 120, 0, 700, t);   // amber: saved for later
  erase(EPD_FAST_KEEP_RED);
  net_path_begin(s_px, s_py);
}

static void wait_panel_idle(void)
{
  for (int i = 0; i < 6000 && (s_epd_busy || s_req_pending); i++) vTaskDelay(pdMS_TO_TICKS(10));
}

void app_main(void)
{
  printf("\n=== Sketch (ESP-IDF) ===\n");
  ws2812_init();
  ws2812_set_brightness(LED_BRIGHTNESS);
  ws2812_fill(0, 0, 0);
  net_start();

  gpio_config_t bc = {
    .pin_bit_mask = (1ULL << BTN_USER_A) | (1ULL << BTN_USER_B),
    .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&bc);
  button_t btn_a = { .gpio = BTN_USER_A }, btn_b = { .gpio = BTN_USER_B };

  // Knobs
  adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc));
  i2c_init();
  int min_mv = 0;
  if (as5600_reduced_range()) {
    s_lo_mv = 330;
    s_hi_mv = 2970;
    min_mv = 250;
    printf("sketch: AS5600 output set to 10-90 %% of VDD\n");
    vTaskDelay(pdMS_TO_TICKS(5));   // let the outputs settle to the new range
  }
  knob_t* found[2] = { NULL, NULL };
  int nfound = 0;
  for (int i = 0; i < 3 && nfound < 2; i++) {
    if (knob_init(&s_cand[i]) && knob_probe(&s_cand[i], min_mv)) found[nfound++] = &s_cand[i];
  }
  // Upside down, the first knob found ends up on the right, so it drives Y.
  const bool swap = SWAP_KNOBS ^ FLIP_180;
  s_knob_x = found[swap ? 1 : 0];
  s_knob_y = found[swap ? 0 : 1];
  if (!s_knob_x) { s_knob_x = s_knob_y; s_knob_y = NULL; }
  s_led_group[0] = (s_knob_x == found[0]) ? 0 : 1;
  s_led_group[1] = 1 - s_led_group[0];
  printf("sketch: X knob %s, Y knob %s\n",
         s_knob_x ? s_knob_x->name : "-", s_knob_y ? s_knob_y->name : "-");
  mpu_init();

  // Panel
  if (!epd_begin()) {
    printf("sketch: no panel, stopping\n");
    ws2812_fill(255, 0, 255);
    return;
  }
  epd_set_rotate_180(FLIP_180);
  epd_set_keep_awake(true);   // lines then cost planes + trigger, not a power-up
  xTaskCreatePinnedToCore(epd_task, "epd", 8192, NULL, 5, &s_epd_task, 1);
  erase(EPD_SLOW);            // full refresh: clears the panel and makes good red
  net_path_begin(s_px, s_py);

  bool dirty = false, drawn_since_clean = false;
  uint32_t first_change = 0, last_move = 0, last_activity = now_ms(), last_clean = now_ms();

  // Gallery state
  bool gallery = false, g_note = false;
  int g_shown = -1, g_want = 0, g_acc = 0;   // g_shown -1: the title card
  uint32_t g_entered = 0, g_turned = 0, g_stepped = 0, g_note_at = 0;

  for (;;) {
    uint32_t t = now_ms();

    // -------------------------------------------------------------- gallery --
    if (gallery) {
      bool a = button_pressed(&btn_a), b = button_pressed(&btn_b);
      bool leave = a || b;
      (void)shake_detected();   // keep its window current; shaking doesn't erase in here

      int d = 0;
      if (s_knob_x) d += knob_delta(s_knob_x);
      if (s_knob_y) d += knob_delta(s_knob_y);
      if (d) {
        g_acc += d;
        g_turned = t;
        last_activity = t;
        leds_turned(0, d, t);
      }

      net_gallery_state_t gs = net_gallery_state();
      int n = net_gallery_count();
      if (gs == NET_GALLERY_READY && n > 0) {
        while (g_acc >= GALLERY_STEP)  { g_acc -= GALLERY_STEP; g_want = (g_want + 1) % n; }
        while (g_acc <= -GALLERY_STEP) { g_acc += GALLERY_STEP; g_want = (g_want + n - 1) % n; }
        if (g_shown >= 0 && g_want == g_shown && t - g_turned < GALLERY_AUTO_FOR &&
            t - g_stepped >= GALLERY_AUTO_MS) {
          g_want = (g_shown + 1) % n;
        }
        if (g_want != g_shown && t - g_entered >= GALLERY_LABEL_MS && t - g_turned >= GALLERY_SETTLE_MS &&
            !s_epd_busy && !s_req_pending) {
          page_drawing(g_want, n);
          g_shown = g_want;
          g_stepped = t;
          last_activity = t;
        }
      } else if (!g_note && (gs == NET_GALLERY_FAILED || gs == NET_GALLERY_READY) &&
                 !s_epd_busy && !s_req_pending) {
        page_label(gs == NET_GALLERY_READY ? "nothing here yet" : "offline");
        g_note = true;
        g_note_at = t;
      }
      if (g_note && t - g_note_at >= GALLERY_FAIL_MS) leave = true;

      if (leave) {
        wait_panel_idle();
        memcpy(epd_plane_bw(), s_saved[0], EPD_PLANE_BYTES);
        memcpy(epd_plane_red(), s_saved[1], EPD_PLANE_BYTES);
        request_refresh(EPD_FAST_KEEP_RED);
        net_gallery_free();
        net_set_mode(NET_MODE_DRAWING);
        gallery = false;
        drawn_since_clean = true;   // all those fast pages: clean up when it's quiet
        last_activity = t;
        printf("sketch: back to drawing\n");
      }

      if (gallery && epd_keep_awake() && !s_epd_busy && !s_req_pending &&
          t - last_activity > SLEEP_AFTER_MS && t - g_turned >= GALLERY_AUTO_FOR) {
        request_sleep();
        last_activity = t;
      }
      leds_update(t);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // -------------------------------------------------------------- drawing --
    if (shake_detected()) {
      printf("sketch: shake, erasing\n");
      for (int i = 0; i < 3; i++) {
        ws2812_fill(255, 255, 255); vTaskDelay(pdMS_TO_TICKS(80));
        ws2812_fill(0, 0, 0);       vTaskDelay(pdMS_TO_TICKS(80));
      }
      erase_and_upload(t);
      dirty = false;
      last_activity = t;
    }
    if (button_pressed(&btn_a)) {
      // Into the gallery: keep the drawing to put back afterwards
      if (!s_saved[0]) s_saved[0] = heap_caps_malloc(EPD_PLANE_BYTES, MALLOC_CAP_SPIRAM);
      if (!s_saved[1]) s_saved[1] = heap_caps_malloc(EPD_PLANE_BYTES, MALLOC_CAP_SPIRAM);
      if (s_saved[0] && s_saved[1]) {
        printf("sketch: gallery\n");
        wait_panel_idle();
        memcpy(s_saved[0], epd_plane_bw(), EPD_PLANE_BYTES);
        memcpy(s_saved[1], epd_plane_red(), EPD_PLANE_BYTES);
        gallery = true;
        dirty = false;
        net_set_mode(NET_MODE_GALLERY);
        g_shown = -1;
        g_want = 0;
        g_acc = 0;
        g_entered = g_turned = g_stepped = now_ms();
        last_activity = g_entered;
        if (!net_configured()) {
          page_label("no website set up");
          g_note = true;
        } else if (!net_online()) {
          page_label("offline");
          g_note = true;
        } else {
          page_label("loading...");
          g_note = false;
          net_gallery_request();
        }
        g_note_at = g_entered;
        continue;
      }
    }
    if (button_pressed(&btn_b)) {
      if (s_knob_x && !s_knob_y) {
        s_single_x = !s_single_x;
        printf("sketch: knob now drives %s\n", s_single_x ? "X" : "Y");
      } else {
        erase_and_upload(t);
        dirty = false;
      }
      last_activity = t;
    }

    // Knobs -> pen
    int cx = 0, cy = 0;
    if (s_knob_x && s_knob_y) {
      cx = knob_delta(s_knob_x);
      cy = knob_delta(s_knob_y);
    } else if (s_knob_x) {
      int d = knob_delta(s_knob_x);
      if (s_single_x) cx = d; else cy = d;
    }
    if (INVERT_X) cx = -cx;
    if (INVERT_Y) cy = -cy;
    if (cx) leds_turned(0, cx, t);
    if (cy) leds_turned(1, cy, t);
    if (cx || cy) {
      last_move = t;
      last_activity = t;
      s_fx += cx * (float)PX_PER_REV / 4096.0f;
      s_fy -= cy * (float)PX_PER_REV / 4096.0f;   // clockwise = up
      int mx = (int)s_fx, my = (int)s_fy;
      s_fx -= mx;
      s_fy -= my;
      int nx = s_px + mx, ny = s_py + my;
      if (nx < X_MIN || nx > X_MAX) { s_fx = 0; nx = nx < X_MIN ? X_MIN : X_MAX; s_wall_ms[0] = t; }
      if (ny < Y_MIN || ny > Y_MAX) { s_fy = 0; ny = ny < Y_MIN ? Y_MIN : Y_MAX; s_wall_ms[1] = t; }
      if (nx != s_px || ny != s_py) {
        draw_line(s_px, s_py, nx, ny);
        net_path_add(nx, ny);
        s_px = nx;
        s_py = ny;
        if (!dirty) first_change = t;
        dirty = true;
      }
    }

    // Put new lines on the panel once the knobs pause
    if (dirty && !s_epd_busy && !s_req_pending &&
        (t - last_move >= IDLE_REFRESH_MS || t - first_change >= MAX_WAIT_MS)) {
      request_refresh(EPD_FAST_INK);
      dirty = false;
      drawn_since_clean = true;
    }

    // Every few minutes of drawing, once the knobs rest: one full refresh.
    // Fast line updates leave red and black a little weaker each time.
    if (drawn_since_clean && !dirty && !s_epd_busy && !s_req_pending &&
        t - last_clean >= CLEAN_EVERY_MS && t - last_move >= CLEAN_IDLE_MS) {
      printf("sketch: periodic full refresh\n");
      request_refresh(EPD_SLOW);
      drawn_since_clean = false;
      last_clean = t;
      last_activity = t;
    }

    // Long idle: let the panel's rails come down
    if (epd_keep_awake() && !s_epd_busy && !s_req_pending && !dirty &&
        t - last_activity > SLEEP_AFTER_MS) {
      request_sleep();
      last_activity = t;
    }

    leds_update(t);

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
