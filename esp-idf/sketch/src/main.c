// -----------------------------------------------------------------------------
// Sketch - standalone ESP-IDF firmware for the 2026 Supercon billboard badge.
//
// A knob-drawing toy: the left knob moves the pen left/right, the right knob
// up/down (clockwise = right / up). Shake the badge hard, or press User A, to
// erase. User B does a full refresh to clean up ghosting (or, with only one
// knob, switches which axis it drives).
//
// Hardware
//   Knobs   AS5600 magnetic encoder SAOs, analog OUT on the SAO's GPIO1. Every
//           AS5600 is at I2C 0x36, so they're read with the ADC. The SAO GPIO1
//           pins (GPIO2/4/7) are probed with pull-down vs pull-up: a floating
//           pin follows the pull, a driven one doesn't. First two = X, Y.
//   Shake   optional MPU6050 on the SAO I2C bus (SDA 17, SCL 18), 0x68/0x69.
//   LEDs    8x WS2812 on GPIO38: red while Sketch is running, yellow while
//           you're drawing (knobs turning, or new lines being put on screen).
//
// Refreshing
//   The frame goes up once with EPD_SLOW (the only mode that makes good red).
//   Lines and erases then use EPD_FAST_KEEP_RED, which drives black/white and
//   leaves the red frame alone, with the COG session kept open between images.
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
#include "esp_timer.h"

#include "epd97.h"
#include "ws2812.h"

// --- Tunables -----------------------------------------------------------------
#define PX_PER_REV        240     // pen travel per full knob turn
#define LINE_W            3       // line thickness, px
#define DEADBAND          12      // ADC noise to ignore, counts of 4096
#define ADC_SAMPLES       8
#define IDLE_REFRESH_MS   700     // redraw this long after the knobs stop...
#define MAX_WAIT_MS       4000    // ...or at least this often while drawing
#define DRAWING_HOLD_MS   600     // LEDs stay yellow this long after a turn
#define SLEEP_AFTER_MS    30000   // power the panel down after this long idle
#define LED_BRIGHTNESS    24      // of 255
#define INVERT_X          0
#define INVERT_Y          0
#define SWAP_KNOBS        0

#define SHAKE_G           2.2f    // a jolt is this far from 1 g
#define SHAKE_PEAKS       4       // this many jolts...
#define SHAKE_WINDOW_MS   1200    // ...within this long erases
#define SHAKE_COOLDOWN_MS 3000

// --- Pins ---------------------------------------------------------------------
#define BTN_USER_A   39
#define BTN_USER_B   16
#define SAO_SDA      17
#define SAO_SCL      18

// --- Frame geometry (matches data/graphics/sketch_frame.png) ------------------
#define SCR_X0  70
#define SCR_Y0  58
#define SCR_X1  890
#define SCR_Y1  532
#define SCR_R   18
#define INSET   (12 + LINE_W)
#define X_MIN   (SCR_X0 + INSET)
#define Y_MIN   (SCR_Y0 + INSET)
#define X_MAX   (SCR_X1 - INSET)
#define Y_MAX   (SCR_Y1 - INSET)
#define KNOB_Y  604
#define KNOB_LX 98
#define KNOB_RX (EPD_W - 98)

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// =============================================================================
// Display task: refreshes run here so the knobs stay live during them.
// =============================================================================
static TaskHandle_t s_epd_task;
static volatile bool s_req_pending;
static volatile bool s_req_sleep;
static volatile epd_refresh_t s_req_mode;
static volatile bool s_epd_busy;
static volatile bool s_lines_in_flight;   // the refresh running is drawing lines

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
    s_lines_in_flight = false;
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
static void fill_round_rect(int x0, int y0, int x1, int y1, int r, epd_color_t c)
{
  for (int y = y0; y <= y1; y++) {
    int dy = 0;
    if (y < y0 + r)      dy = y0 + r - y;
    else if (y > y1 - r) dy = y - (y1 - r);
    int dx = dy ? r - (int)sqrtf((float)(r * r - dy * dy)) : 0;
    epd_fill_rect(x0 + dx, y, (x1 - x0 + 1) - 2 * dx, 1, c);
  }
}

static void fill_circle(int cx, int cy, int r, epd_color_t c)
{
  for (int dy = -r; dy <= r; dy++) {
    int dx = (int)sqrtf((float)(r * r - dy * dy));
    epd_fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, c);
  }
}

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

static void draw_knob(int cx, int cy)
{
  fill_circle(cx, cy, 50, EPD_BLACK);
  fill_circle(cx, cy, 46, EPD_WHITE);
  fill_circle(cx, cy, 35, EPD_BLACK);
  fill_circle(cx, cy, 33, EPD_WHITE);
  for (int k = 0; k < 24; k++) {          // grip ridges
    float a = 2.0f * (float)M_PI * k / 24.0f;
    for (int r = 38; r <= 44; r++) {
      int x = cx + (int)lroundf(r * cosf(a)), y = cy + (int)lroundf(r * sinf(a));
      epd_fill_rect(x, y, 2, 2, EPD_BLACK);
    }
  }
}

static void draw_frame(const char* status)
{
  epd_clear();
  fill_round_rect(4, 4, EPD_W - 5, EPD_H - 5, 48, EPD_BLACK);
  fill_round_rect(7, 7, EPD_W - 8, EPD_H - 8, 45, EPD_RED);
  fill_round_rect(SCR_X0 - 6, SCR_Y0 - 6, SCR_X1 + 6, SCR_Y1 + 6, SCR_R + 4, EPD_BLACK);
  fill_round_rect(SCR_X0, SCR_Y0, SCR_X1, SCR_Y1, SCR_R, EPD_WHITE);
  draw_knob(KNOB_LX, KNOB_Y);
  draw_knob(KNOB_RX, KNOB_Y);
  const char* title = "SKETCH";
  epd_text((EPD_W - epd_text_width(title, 4)) / 2, 16, title, EPD_WHITE, 4);
  if (status) {
    epd_text((EPD_W - epd_text_width(status, 2)) / 2, 580, status, EPD_WHITE, 2);
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
static bool knob_probe(knob_t* k)
{
  gpio_set_pull_mode(k->gpio, GPIO_PULLDOWN_ONLY);
  vTaskDelay(pdMS_TO_TICKS(3));
  int lo = knob_mv(k, 8);
  gpio_set_pull_mode(k->gpio, GPIO_PULLUP_ONLY);
  vTaskDelay(pdMS_TO_TICKS(3));
  int hi = knob_mv(k, 8);
  gpio_set_pull_mode(k->gpio, GPIO_FLOATING);
  bool driven = abs(hi - lo) < 300;
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
// App
// =============================================================================
static int s_px = (X_MIN + X_MAX) / 2, s_py = (Y_MIN + Y_MAX) / 2;
static float s_fx, s_fy;
static bool s_single_x = true;   // one-knob mode: which axis it drives

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

void app_main(void)
{
  printf("\n=== Sketch (ESP-IDF) ===\n");
  ws2812_init();
  ws2812_set_brightness(LED_BRIGHTNESS);
  ws2812_fill(255, 0, 0);   // red: Sketch is running

  gpio_config_t bc = {
    .pin_bit_mask = (1ULL << BTN_USER_A) | (1ULL << BTN_USER_B),
    .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&bc);
  button_t btn_a = { .gpio = BTN_USER_A }, btn_b = { .gpio = BTN_USER_B };

  // Knobs
  adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc));
  knob_t* found[2] = { NULL, NULL };
  int nfound = 0;
  for (int i = 0; i < 3 && nfound < 2; i++) {
    if (knob_init(&s_cand[i]) && knob_probe(&s_cand[i])) found[nfound++] = &s_cand[i];
  }
  i2c_init();
  if (nfound && as5600_reduced_range()) {
    s_lo_mv = 330;
    s_hi_mv = 2970;
    printf("sketch: AS5600 output set to 10-90 %% of VDD\n");
  }
  s_knob_x = found[SWAP_KNOBS ? 1 : 0];
  s_knob_y = found[SWAP_KNOBS ? 0 : 1];
  if (!s_knob_x) { s_knob_x = s_knob_y; s_knob_y = NULL; }
  printf("sketch: X knob %s, Y knob %s\n",
         s_knob_x ? s_knob_x->name : "-", s_knob_y ? s_knob_y->name : "-");
  mpu_init();

  // Panel
  if (!epd_begin()) {
    printf("sketch: no panel, stopping\n");
    ws2812_fill(255, 0, 255);
    return;
  }
  epd_set_keep_awake(true);   // lines then cost planes + trigger, not a power-up
  xTaskCreatePinnedToCore(epd_task, "epd", 8192, NULL, 5, &s_epd_task, 1);
  erase(EPD_SLOW);            // full refresh: clears the panel and makes good red

  bool dirty = false;
  uint32_t first_change = 0, last_move = 0, last_activity = now_ms();
  int led_state = -1;

  for (;;) {
    uint32_t t = now_ms();

    if (shake_detected()) {
      printf("sketch: shake, erasing\n");
      for (int i = 0; i < 3; i++) {
        ws2812_fill(255, 255, 255); vTaskDelay(pdMS_TO_TICKS(80));
        ws2812_fill(0, 0, 0);       vTaskDelay(pdMS_TO_TICKS(80));
      }
      led_state = -1;
      erase(EPD_FAST_KEEP_RED);
      dirty = false;
      last_activity = t;
    }
    if (button_pressed(&btn_a)) {
      erase(EPD_FAST_KEEP_RED);
      dirty = false;
      last_activity = t;
    }
    if (button_pressed(&btn_b)) {
      if (s_knob_x && !s_knob_y) {
        s_single_x = !s_single_x;
        printf("sketch: knob now drives %s\n", s_single_x ? "X" : "Y");
      } else {
        request_refresh(EPD_SLOW);   // full clean-up of the current drawing
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
    if (cx || cy) {
      last_move = t;
      last_activity = t;
      s_fx += cx * (float)PX_PER_REV / 4096.0f;
      s_fy -= cy * (float)PX_PER_REV / 4096.0f;   // clockwise = up
      int mx = (int)s_fx, my = (int)s_fy;
      s_fx -= mx;
      s_fy -= my;
      int nx = s_px + mx, ny = s_py + my;
      if (nx < X_MIN || nx > X_MAX) { s_fx = 0; nx = nx < X_MIN ? X_MIN : X_MAX; }
      if (ny < Y_MIN || ny > Y_MAX) { s_fy = 0; ny = ny < Y_MIN ? Y_MIN : Y_MAX; }
      if (nx != s_px || ny != s_py) {
        draw_line(s_px, s_py, nx, ny);
        s_px = nx;
        s_py = ny;
        if (!dirty) first_change = t;
        dirty = true;
      }
    }

    // Put new lines on the panel once the knobs pause
    if (dirty && !s_epd_busy && !s_req_pending &&
        (t - last_move >= IDLE_REFRESH_MS || t - first_change >= MAX_WAIT_MS)) {
      s_lines_in_flight = true;
      request_refresh(EPD_FAST_KEEP_RED);
      dirty = false;
    }

    // Long idle: let the panel's rails come down
    if (epd_keep_awake() && !s_epd_busy && !s_req_pending && !dirty &&
        t - last_activity > SLEEP_AFTER_MS) {
      request_sleep();
      last_activity = t;
    }

    // LEDs: yellow while drawing, red otherwise
    bool drawing = (t - last_move < DRAWING_HOLD_MS) || dirty || s_lines_in_flight;
    if ((int)drawing != led_state) {
      led_state = drawing;
      if (drawing) ws2812_fill(255, 160, 0);
      else         ws2812_fill(255, 0, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
