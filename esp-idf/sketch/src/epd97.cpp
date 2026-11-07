// -----------------------------------------------------------------------------
// epd97 - Pervasive Displays iTC 9.7" dual-COG panel on an ESP32-S3.
//
// Ported from the EFR32FG22 OEPL driver (oepl_display_driver_pervasive_itc_
// medium.c), which is where all the measured numbers in the comments come from.
//
// The panel is two COGs behind one FPC, each owning half of a 960 px scan line.
// Every line therefore goes out as 60 bytes to the master, chip select swapped,
// 60 bytes to the slave. Register writes go to both at once.
//
// Bus arrangement: register writes and the OTP readback are bitbanged, because
// they are a few dozen bytes and the OTP wants a ~50 kHz clock with the data
// pin turned around for the read. The two 80640-byte planes go out over
// hardware SPI, which is what takes a frame from about eighteen seconds to
// under one. The two share MOSI and SCK, so the GPIO matrix is re-pointed
// between them rather than the bus being torn down and rebuilt.
//
// ESP-IDF port (badge Sketch firmware): Arduino calls go through compat.h and
// logging through printf. Adds EPD_FAST_KEEP_RED - see epd97.h.
// -----------------------------------------------------------------------------
#include "compat.h"
#include <stdarg.h>
#include <string.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "soc/spi_periph.h"
#include "soc/gpio_sig_map.h"

#include "epd97.h"
#include "epd97_config.h"
#include "epd97_lut.h"

#if EPD_LOG
#define LOGF(...) do { printf(__VA_ARGS__); } while (0)
#else
#define LOGF(...) do {} while (0)
#endif
#if EPD_LOG_VERBOSE
#define VLOGF(...) LOGF(__VA_ARGS__)
#else
#define VLOGF(...) do {} while (0)
#endif

#define EPD_OTP_LEN   88
#define COG_M         0x01
#define COG_S         0x02
#define COG_BOTH      (COG_M | COG_S)
#define HALF_BYTES    (EPD_STRIDE / 2)   // 60

// The vendor's update register sequence for the 9.7", TCON before STV. Each
// step is a register, its argument, and a delay field: bit 0 says the argument
// is an OTP offset rather than a literal, the upper bits are units of 10 ms.
typedef struct { uint8_t cmd; uint8_t arg; uint8_t dly; } upd_step_t;

static const upd_step_t k_upd[] = {
  { 0x05, 0x7D, 20u << 1 },
  { 0x05, 0x00,  1u << 1 },
  { 0xC2, 0x3F,  0 },
  { 0xD8, 0x1D,  1 },
  { 0xD6, 0x1E,  1 },
  { 0xA7, 0x10, 10u << 1 },
  { 0xA7, 0x00, 10u << 1 },
  { 0x44, 0x00,  0 },
  { 0x45, 0x80,  0 },
  { 0xA7, 0x10, 10u << 1 },
  { 0xA7, 0x00, 10u << 1 },
  { 0x44, 0x06,  0 },
  { 0x45, 0x86,  0 },              // patched at runtime with the waveform bank
  { 0xA7, 0x10, 10u << 1 },
  { 0xA7, 0x00, EPD_T_PRE_SS << 1 },
  { 0x60, 0x0B,  1 },
  { 0x61, 0x1C,  1 },
  { 0x01, 0x10,  1 },
  { 0x02, 0x11,  1 },
};
#define UPD_STEPS (sizeof(k_upd) / sizeof(k_upd[0]))

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------
static uint8_t*  s_bw;
static uint8_t*  s_red;
static uint8_t*  s_shown;    // the bw plane as last put on the panel
static bool      s_ready;

static spi_device_handle_t s_spi;
static uint8_t   s_cog = COG_BOTH;

static uint8_t   s_otp[EPD_OTP_LEN];
static bool      s_otp_valid;
static bool      s_driver_b;          // true = E2969JS0B1, false = E2969JS084
static epd_panel_t s_panel = EPD_PANEL_UNKNOWN;

// The OTP survives deep sleep in RTC memory. It is a fuse block on a panel that
// is still plugged into the same board, so re-probing it on every wake costs a
// full power cycle and about three seconds for an answer that cannot have
// changed. Only trusted after a deep-sleep wake, so unplugging the panel and
// power-cycling the board re-probes.
#define OTP_RTC_MAGIC 0x69544301u
RTC_DATA_ATTR static uint32_t s_rtc_magic;
RTC_DATA_ATTR static uint8_t  s_rtc_otp[EPD_OTP_LEN];
RTC_DATA_ATTR static uint8_t  s_rtc_driver_b;

static uint8_t   s_lut8[LUT8_BYTES];
static uint8_t   s_lutb[LUTB_BYTES];
static uint8_t   s_lut8_keep[LUT8_BYTES];   // EPD_FAST_KEEP_RED tables
static uint8_t   s_lutb_keep[LUTB_BYTES];
static uint8_t   s_lut8_ink[LUT8_BYTES];    // EPD_FAST_INK tables
static uint8_t   s_lutb_ink[LUTB_BYTES];
static bool      s_lut8_has_red;

static bool      s_frame_has_red;
// Whether the picture currently ON the panel carries red. It has to survive
// deep sleep: what the film is holding is not reset by the ESP32 losing its
// RAM, and a wake that starts out believing the panel is red-free will run a
// fast refresh straight over red that nothing short can remove. RTC memory is
// cleared on a power-on reset, which is the case a cold boot's clearing pass
// covers anyway.
RTC_DATA_ATTR static bool s_prev_has_red;

static bool      s_session_open;
static bool      s_lut_latched;
static epd_refresh_t s_latched_mode = EPD_SLOW;
static uint32_t  s_session_images;
static bool      s_keep_awake;
static bool      s_rot180;

static uint32_t  s_power_off_ms;
static bool      s_power_off_valid;
static uint32_t  s_last_panel_ms;
static uint32_t  s_last_frame_ms;

// A 60-byte staging buffer in internal RAM. The planes live in PSRAM and the
// SPI FIFO path wants a source it can read directly.
static uint8_t   s_txbuf[HALF_BYTES];

// -----------------------------------------------------------------------------
// Pins
// -----------------------------------------------------------------------------
static inline void dc_set(int v)  { gpio_set_level((gpio_num_t)EPD_PIN_DC, v); }
static inline void rst_set(int v) { gpio_set_level((gpio_num_t)EPD_PIN_RST, v); }
static inline void clk_set(int v) { gpio_set_level((gpio_num_t)EPD_PIN_SCK, v); }
static inline void sdi_set(int v) { gpio_set_level((gpio_num_t)EPD_PIN_MOSI, v); }

static inline void cog_select(uint8_t which) { s_cog = which; }

static void cs_hi(void)
{
  gpio_set_level((gpio_num_t)EPD_PIN_CS_M, 1);
  gpio_set_level((gpio_num_t)EPD_PIN_CS_S, 1);
}

// Raise the deselected side before lowering the selected one, so the two COGs
// are never both listening.
static void cs_lo(void)
{
  if (!(s_cog & COG_M)) gpio_set_level((gpio_num_t)EPD_PIN_CS_M, 1);
  if (!(s_cog & COG_S)) gpio_set_level((gpio_num_t)EPD_PIN_CS_S, 1);
  if (s_cog & COG_M)    gpio_set_level((gpio_num_t)EPD_PIN_CS_M, 0);
  if (s_cog & COG_S)    gpio_set_level((gpio_num_t)EPD_PIN_CS_S, 0);
}

// Both COGs have to report ready. A panel with only the master BUSY wired can
// set EPD_PIN_BUSY_S to -1.
static bool busy_idle(void)
{
  if (!gpio_get_level((gpio_num_t)EPD_PIN_BUSY_M)) return false;
#if EPD_PIN_BUSY_S >= 0
  if (!gpio_get_level((gpio_num_t)EPD_PIN_BUSY_S)) return false;
#endif
  return true;
}

// -----------------------------------------------------------------------------
// Bus: bitbang and hardware SPI over the same two pins
// -----------------------------------------------------------------------------
// Only the two bus pins change hands. DC, RST and the two chip selects are
// configured as outputs once in epd_begin() and stay that way.
//
// An earlier version let go of all six while the panel supply was off, on the
// theory that driving an unpowered COG backfeeds it through the ESD diodes.
// That is sound in general and it broke this panel: the refresh got as far as
// the DC/DC soft start and BUSY never came ready. Reverted rather than
// explained - the bench is the authority here, not the theory.
static void bus_bitbang(void)
{
  esp_rom_gpio_connect_out_signal(EPD_PIN_MOSI, SIG_GPIO_OUT_IDX, false, false);
  esp_rom_gpio_connect_out_signal(EPD_PIN_SCK,  SIG_GPIO_OUT_IDX, false, false);
  gpio_set_direction((gpio_num_t)EPD_PIN_MOSI, GPIO_MODE_OUTPUT);
  gpio_set_direction((gpio_num_t)EPD_PIN_SCK,  GPIO_MODE_OUTPUT);
  clk_set(0);
  sdi_set(0);
}

static void bus_spi(void)
{
  esp_rom_gpio_connect_out_signal(
      EPD_PIN_MOSI, spi_periph_signal[EPD_SPI_HOST_NUM].spid_out, false, false);
  esp_rom_gpio_connect_out_signal(
      EPD_PIN_SCK, spi_periph_signal[EPD_SPI_HOST_NUM].spiclk_out, false, false);
}

// Mode 0 by hand: set the data, raise the clock, lower it. The panel samples on
// the rising edge and the clock idles low.
static void bb_byte(uint8_t v)
{
  for (uint8_t i = 0; i < 8; i++) {
    sdi_set(v & 0x80u);
    clk_set(1);
    clk_set(0);
    v <<= 1;
  }
}

// ~50 kHz. The panel does not follow a full-speed bitbang for the OTP.
static void bb_out_slow(uint8_t v)
{
  for (uint8_t i = 0; i < 8; i++) {
    sdi_set(v & 0x80u);
    esp_rom_delay_us(10);
    clk_set(1);
    esp_rom_delay_us(10);
    clk_set(0);
    esp_rom_delay_us(10);
    v <<= 1;
  }
}

static uint8_t bb_in_slow(void)
{
  uint8_t v = 0;
  for (uint8_t i = 0; i < 8; i++) {
    clk_set(1);
    esp_rom_delay_us(10);
    v = (uint8_t)((v << 1) | (gpio_get_level((gpio_num_t)EPD_PIN_MOSI) ? 1u : 0u));
    clk_set(0);
    esp_rom_delay_us(10);
  }
  return v;
}

static void send_index(uint8_t index, const uint8_t* data, uint16_t len)
{
  dc_set(0);
  cs_lo();
  bb_byte(index);
  cs_hi();
  if (len == 0) return;
  dc_set(1);
  cs_lo();
  while (len--) bb_byte(*data++);
  cs_hi();
}

static void send_u8(uint8_t index, uint8_t v)          { send_index(index, &v, 1); }
static void send_u8x2(uint8_t index, uint8_t a, uint8_t b)
{
  uint8_t d[2] = { a, b };
  send_index(index, d, 2);
}
static void send_otp(uint8_t index, uint8_t off, uint8_t n)
{
  send_index(index, &s_otp[off], n);
}

// -----------------------------------------------------------------------------
// Power and reset
// -----------------------------------------------------------------------------
static void power_on(void)
{
  // The booster rails have to bleed down before the next cycle powers up, or
  // the COG sits on the update waiting for its own voltages. This shows up
  // mostly on the second refresh, which is what points at the rails.
  if (s_power_off_valid) {
    uint32_t off = millis() - s_power_off_ms;
    if (off < EPD_T_OFF_MIN) {
      VLOGF("epd: panel only off %ums, waiting out %ums\n",
            (unsigned)off, (unsigned)(EPD_T_OFF_MIN - off));
      delay(EPD_T_OFF_MIN - off);
    }
    s_power_off_valid = false;
  }
  gpio_set_level((gpio_num_t)EPD_PIN_PWR, EPD_PWR_ACTIVE_HIGH ? 1 : 0);
}

static void power_off(void)
{
  gpio_set_level((gpio_num_t)EPD_PIN_PWR, EPD_PWR_ACTIVE_HIGH ? 0 : 1);
  s_power_off_ms = millis();
  s_power_off_valid = true;
}

static void cog_reset(void)
{
  power_on();
  delay(EPD_T_RST_PWR);
  rst_set(1);
  delay(EPD_T_RST_HI);
  rst_set(0);
  delay(EPD_T_RST_LO);
  rst_set(1);
  delay(EPD_T_RST_SET);
}

// -----------------------------------------------------------------------------
// OTP: which panel is this?
// -----------------------------------------------------------------------------
static void otp_read_raw(bool use_b9)
{
  cog_select(COG_M);
  cog_reset();

  if (use_b9) {
    // Driver B opens the session with 0x70 before the OTP read. A panel that
    // never gets that register draws about 2 A during refresh, so it is
    // configuring the booster. It goes to both COGs; only the read itself is
    // master-only, because both COGs would otherwise drive the bus.
    cog_select(COG_BOTH);
    send_u8x2(0x70, 0x71, 0x36);
    cog_select(COG_M);
    dc_set(0);
    cs_lo();
    bb_out_slow(0xB9);
  } else {
    send_u8(0xA1, 0x00);
    send_u8(0xA7, 0x80);
    send_u8x2(0xA3, 0x00, 0x40);
    dc_set(0);
    cs_lo();
    bb_out_slow(0xA8);
  }
  delay(1);

  dc_set(1);
  gpio_set_direction((gpio_num_t)EPD_PIN_MOSI, GPIO_MODE_INPUT);
  gpio_set_pull_mode((gpio_num_t)EPD_PIN_MOSI, GPIO_PULLDOWN_ONLY);
  (void)bb_in_slow();
  for (uint16_t i = 0; i < EPD_OTP_LEN; i++) s_otp[i] = bb_in_slow();
  cs_hi();
  gpio_set_direction((gpio_num_t)EPD_PIN_MOSI, GPIO_MODE_OUTPUT);
  gpio_set_pull_mode((gpio_num_t)EPD_PIN_MOSI, GPIO_FLOATING);
  sdi_set(0);
  cog_select(COG_BOTH);
}

// Stage 0 of the DC/DC ramp must not be 0xFF - that ramps the booster 127x and
// hangs BUSY. All four stages are checked, not just the first: the booster loop
// runs the OTP repeat counts verbatim, so a garbled block has to be rejected
// here rather than clamped later.
static bool otp_sane(void)
{
  for (uint8_t stage = 0; stage < 4; stage++) {
    uint8_t sb = s_otp[0x20 + 8 * stage];
    uint8_t rp = (uint8_t)(sb & 0x7F);
    if (sb == 0xFF || rp == 0 || rp > 40) {
      LOGF("epd: OTP ramp stage %u implausible: 0x%02X\n", stage, sb);
      return false;
    }
  }
  // Both variants are checked in the driver-8 layout (driver B is remapped into
  // it first). DUW sits at 0x16..0x1B and DRFW at 0x0C..0x0F, and for a 960 px
  // dual COG both carry x_end = 0x3B - 60 bytes per half.
  if (s_otp[0x17] != 0x3B || s_otp[0x0D] != 0x3B) {
    LOGF("epd: OTP window check failed: DUW xe=0x%02X DRFW xe=0x%02X\n",
         s_otp[0x17], s_otp[0x0D]);
    return false;
  }
  return true;
}

static bool otp_looks_like_driver_b(void)
{
  return (s_otp[0x16] == 0x3B && s_otp[0x0D] == 0x3B && (s_otp[0x28] & 0x80) != 0);
}

static void otp_remap_b_to_8(void)
{
  for (uint8_t i = 0x1E; i > 0x12; i--) s_otp[i] = s_otp[i - 1];
  for (uint8_t i = 0; i < 32; i++)      s_otp[0x20 + i] = s_otp[0x28 + i];
  s_otp[0] = 0xA5;
}

static void otp_stash(void)
{
  memcpy(s_rtc_otp, s_otp, EPD_OTP_LEN);
  s_rtc_driver_b = s_driver_b ? 1 : 0;
  s_rtc_magic = OTP_RTC_MAGIC;
}

// Recover the probe result from a previous wake, if this boot is one.
static bool otp_restore(void)
{
  if (esp_reset_reason() != ESP_RST_DEEPSLEEP) return false;
  if (s_rtc_magic != OTP_RTC_MAGIC) return false;
  memcpy(s_otp, s_rtc_otp, EPD_OTP_LEN);
  s_driver_b = (s_rtc_driver_b != 0);
  s_panel = s_driver_b ? EPD_PANEL_B1 : EPD_PANEL_084;
  s_otp_valid = true;
  return true;
}

// The OTP is a fuse block: it cannot change between refreshes, and reading it
// costs about 2.8 s because it is clocked out at 50 kHz and tried twice, once
// per variant. So it is read once and kept.
static int otp_read(void)
{
  if (s_otp_valid) {
    VLOGF("epd: OTP from cache (%s)\n", s_driver_b ? "driver B" : "driver 8");
    return 0;
  }

  // Driver B first, then driver 8 - the order the stock firmware uses.
  otp_read_raw(true);
  if (otp_looks_like_driver_b()) {
    otp_remap_b_to_8();
    s_driver_b = true;
    if (!otp_sane()) {
      LOGF("epd: driver-B remap failed the soft-start sanity check\n");
      return -1;
    }
    s_panel = EPD_PANEL_B1;
    s_otp_valid = true;
    LOGF("epd: E2969JS0B1 (driver B) detected\n");
    otp_stash();
    return 0;
  }

  otp_read_raw(false);
  s_driver_b = false;
  if (s_otp[0] != 0xA5 || !otp_sane()) {
    LOGF("epd: driver-8 OTP read unusable (hdr=0x%02X)\n", s_otp[0]);
    return -2;
  }
  s_panel = EPD_PANEL_084;
  s_otp_valid = true;
  LOGF("epd: E2969JS084 (driver 8) detected\n");
  otp_stash();
  return 0;
}

static int cog_initial(void)
{
  // With the OTP cached, otp_read() returns without ever reaching cog_reset(),
  // so the power-up and the rail settle have to happen here instead.
  const bool cached = s_otp_valid;
  if (cached) {
    power_on();
    delay(EPD_T_RST_PWR);
  }
  int rc = otp_read();
  if (rc != 0) {
    LOGF("epd: OTP failed (%d), aborting - the booster must not be left on\n", rc);
    return rc;
  }
  // Second reset after the OTP read, as the stock firmware does.
  delay(EPD_T_OTP_HI);
  rst_set(0);
  delay(EPD_T_OTP_LO);
  rst_set(1);
  delay(EPD_T_OTP_SET);
  return 0;
}

// -----------------------------------------------------------------------------
// Plane transfer
// -----------------------------------------------------------------------------
static inline uint8_t rev8(uint8_t b)
{
  b = (uint8_t)((b & 0xF0u) >> 4 | (b & 0x0Fu) << 4);
  b = (uint8_t)((b & 0xCCu) >> 2 | (b & 0x33u) << 2);
  b = (uint8_t)((b & 0xAAu) >> 1 | (b & 0x55u) << 1);
  return b;
}

// `orsrc`, when given, is merged in bit by bit. That is how a fast refresh
// draws red as ink: the red plane is OR'ed into the black one on the way out
// and the red plane itself goes out empty.
static void spi_half(uint8_t which, const uint8_t* src, const uint8_t* orsrc)
{
  memcpy(s_txbuf, src, HALF_BYTES);
  if (orsrc) {
    for (unsigned i = 0; i < HALF_BYTES; i++) s_txbuf[i] |= orsrc[i];
  }
  cog_select(which);
  cs_lo();
  spi_transaction_t t = {};
  t.length    = HALF_BYTES * 8;
  t.tx_buffer = s_txbuf;
  spi_device_polling_transmit(s_spi, &t);
}

// 0x10 is the black/white plane, 0x13 the red one. The window registers come
// out of the OTP and have to precede the first plane. A NULL framebuffer sends
// a plane of zeros, which is what the red plane is on a frame with no red - the
// COG wants both planes either way. `orfb` is merged in per line if given.
static void write_plane(uint8_t index, const uint8_t* fb, const uint8_t* orfb)
{
  static const uint8_t zeros[HALF_BYTES] = { 0 };

  static uint8_t line_buf[EPD_STRIDE];

  if (index == 0x10) {
    // Driver B sets DCTL before the window registers, exactly as its stock
    // trace does; driver 8 sends 0x01 at the end of the update sequence.
    if (s_driver_b) send_u8(0x01, s_otp[0x10]);
    send_otp(0x13, 0x16, 6);   // DUW
    send_otp(0x90, 0x0C, 4);   // DRFW
    send_otp(0x12, 0x13, 3);   // RAM_RW
  } else {
    send_otp(0x12, 0x13, 3);
    index = 0x11;
  }

  dc_set(0);
  cs_lo();
  bb_byte(index);
  cs_hi();
  dc_set(1);

  bus_spi();
  for (int y = 0; y < EPD_H; y++) {
    if (!fb) {
      spi_half(COG_M, zeros, NULL);
      spi_half(COG_S, zeros, NULL);
      continue;
    }
    if (s_rot180) {
      // Upside down: the last framebuffer line first, each one back to front.
      const size_t off = (size_t)(EPD_H - 1 - y) * EPD_STRIDE;
      for (int j = 0; j < EPD_STRIDE; j++) {
        uint8_t v = fb[off + EPD_STRIDE - 1 - j];
        if (orfb) v |= orfb[off + EPD_STRIDE - 1 - j];
        line_buf[j] = rev8(v);
      }
      spi_half(COG_M, line_buf, NULL);
      spi_half(COG_S, line_buf + HALF_BYTES, NULL);
      continue;
    }
    const size_t off = (size_t)y * EPD_STRIDE;
    const uint8_t* line = fb + off;
    const uint8_t* orl  = orfb ? (orfb + off) : NULL;
    spi_half(COG_M, line, orl);
    spi_half(COG_S, line + HALF_BYTES, orl ? (orl + HALF_BYTES) : NULL);
  }
  bus_bitbang();
  cs_hi();
  cog_select(COG_BOTH);
}

// -----------------------------------------------------------------------------
// Refresh
// -----------------------------------------------------------------------------
static void stage_delay(uint8_t d)
{
  if (d & 0x80) delay((d & 0x7F) ? (d & 0x7F) : 1);
  else          esp_rom_delay_us(10u * (uint32_t)(d & 0x7F));
}

// Literally the vendor loop: the repeat count is whatever the OTP says, no
// skipping and no cap. Ramping the booster differently is what stresses a
// panel, so the OTP is validated up front instead and a garbled block never
// gets this far.
static void dcdc_soft_start(void)
{
  unsigned rounds = 0;
  for (uint8_t stage = 0; stage < 4; stage++) {
    uint8_t off = (uint8_t)(0x20 + 8 * stage);
    uint8_t rep = (uint8_t)(s_otp[off] & 0x7F);
    rounds += rep;
    if (s_otp[off] & 0x80) {
      uint8_t ph0 = s_otp[off + 1];
      uint8_t ph1 = s_otp[off + 2];
      for (uint8_t i = 0; i < rep; i++) {
        send_u8(0x09, s_otp[off + 5]);
        ph0 = (uint8_t)(ph0 + s_otp[off + 3]);
        ph1 = (uint8_t)(ph1 + s_otp[off + 4]);
        send_u8x2(0x51, ph0, ph1);
        send_u8(0x09, s_otp[off + 6]);
        stage_delay(s_otp[off + 7]);
      }
    } else {
      for (uint8_t i = 0; i < rep; i++) {
        send_u8(0x09, s_otp[off + 1]);
        stage_delay(s_otp[off + 3]);
        send_u8(0x09, s_otp[off + 2]);
        stage_delay(s_otp[off + 4]);
      }
    }
  }
  VLOGF("epd: soft start done, %u booster rounds\n", rounds);
}

static int wait_busy(unsigned want_idle, uint32_t timeout_ms, const char* tag)
{
  uint32_t waited = 0, next_log = 5000;
  while (waited < timeout_ms) {
    if (want_idle ? busy_idle() : !busy_idle()) return (int)waited;
    delay(10);
    waited += 10;
    if (timeout_ms >= 5000 && waited >= next_log) {
      VLOGF("epd: ... BUSY '%s' still waiting %us\n", tag, (unsigned)(waited / 1000u));
      next_log += 5000;
    }
  }
  LOGF("epd: BUSY wait '%s' timed out after %ums (want_idle=%u, pin=%u)\n",
       tag, (unsigned)timeout_ms, want_idle, busy_idle());
  return -1;
}

// Trigger the update again without touching any register the waveform lives in.
// Everything the panel needs is already loaded: the planes, the table and the
// booster rails.
static int retrigger(void)
{
  send_u8(0x15, 0x3C);
  if (wait_busy(0, 2000, "post-0x15 busy") < 0) return -2;
  if (wait_busy(1, EPD_IDLE_WAIT_MS, "refresh done") < 0) return -3;
  return 0;
}

static int do_refresh(epd_refresh_t mode, bool need_red_lut)
{
  const uint8_t reg45 = s_driver_b ? (uint8_t)EPD_REG45_B1 : (uint8_t)EPD_REG45_084;

  delay(EPD_T_PRE_UPD);

  for (size_t i = 0; i < UPD_STEPS; i++) {
    uint8_t d = k_upd[i].dly;
    if (s_driver_b && (k_upd[i].cmd == 0xC2 || k_upd[i].cmd == 0x01)) continue;

    uint8_t arg = (d & 1) ? s_otp[k_upd[i].arg] : k_upd[i].arg;
    // The second 0x45 of the pair carries the waveform bank; the first stays
    // at the driver's own 0x80.
    if (k_upd[i].cmd == 0x45 && k_upd[i].arg != 0x80) arg = reg45;

    send_u8(k_upd[i].cmd, arg);
    if (d >> 1) delay((uint32_t)(d >> 1) * 10u);

    // 0x03 is the oscillator, and the stock sequence only sends it on driver 8.
    if (i == 6 && !s_driver_b) send_u8x2(0x03, 0x00, s_otp[0x12]);
  }

  // After the register table and before the soft start: everything above would
  // have overwritten 0xB0, and nothing below touches it. Both chip selects are
  // low here, so master and slave get the same table in one go.
  if (mode == EPD_FAST_KEEP_RED) {
    if (s_driver_b) send_index(0xB0, s_lutb_keep, (uint16_t)LUTB_BYTES);
    else            send_index(0xB0, s_lut8_keep, (uint16_t)LUT8_BYTES);
  } else if (mode == EPD_FAST_INK) {
    if (s_driver_b) send_index(0xB0, s_lutb_ink, (uint16_t)LUTB_BYTES);
    else            send_index(0xB0, s_lut8_ink, (uint16_t)LUT8_BYTES);
  } else if (mode == EPD_FAST) {
    if (s_driver_b) {
      send_index(0xB0, s_lutb, (uint16_t)LUTB_BYTES);
    } else {
      // Built here, not once at startup: which table this frame needs depends
      // on whether it carries red, and on whether the one before it did.
      epd_build_lut8(s_lut8, need_red_lut, need_red_lut && !s_frame_has_red);
      s_lut8_has_red = need_red_lut;
      send_index(0xB0, s_lut8, (uint16_t)LUT8_BYTES);
    }
  }

  dcdc_soft_start();

  // Stock fires 0x15 about 100 ms after the last soft-start write. The 084 is
  // ready by then; the B1 still holds BUSY low because its DC/DC ramp is not
  // finished, and starting the update into a booster that is still ramping is
  // the intermittent "sits still for a minute, then draws" symptom.
  delay(100);
  if (!busy_idle()) {
    int ready = wait_busy(1, 10000, "pre-0x15 ready");
    if (ready < 0) LOGF("epd: booster never reported ready in 10 s, going anyway\n");
    else           VLOGF("epd: booster ready %d ms after the soft start\n", ready);
  }

  send_u8(0x15, 0x3C);
  if (wait_busy(0, 2000, "post-0x15 busy") < 0) {
    LOGF("epd: BUSY never went low after 0x15\n");
    return -2;
  }
  int waited = wait_busy(1, EPD_IDLE_WAIT_MS, "refresh done");
  if (waited < 0) return -3;

  // Watch a little longer: some panels drop BUSY again for a second waveform
  // pass, which is what a refresh that seems to sit still and then suddenly
  // draws actually is.
  for (uint32_t i = 0; i < EPD_SECOND_PASS_MS / 10u; i++) {
    delay(10);
    if (!busy_idle()) {
      int again = wait_busy(1, EPD_IDLE_WAIT_MS, "second pass");
      if (again > 0) waited += again;
      break;
    }
  }
  s_last_panel_ms = (uint32_t)waited;
  return 0;
}

static void cog_stop(void)
{
  send_u8(0x09, 0x7F);
  delay(20);
  send_u8(0x05, 0x7D);
  send_u8(0x09, 0x00);
  delay(200);
  dc_set(0);
  cs_lo();
  delay(150);
  rst_set(0);
  power_off();
  // Held in reset while the rails come down.
  delay(EPD_T_DISCHARGE);
  cs_hi();
  rst_set(1);
}

// -----------------------------------------------------------------------------
// Public
// -----------------------------------------------------------------------------
static bool frame_has_red(void)
{
  const uint32_t* p = (const uint32_t*)s_red;
  for (size_t i = 0; i < EPD_PLANE_BYTES / 4; i++) if (p[i]) return true;
  return false;
}

bool epd_begin(void)
{
  printf("epd: 9.7\" iTC, %ux%u, planes %u bytes each\n",
                EPD_W, EPD_H, (unsigned)EPD_PLANE_BYTES);

  s_bw    = (uint8_t*)heap_caps_malloc(EPD_PLANE_BYTES, MALLOC_CAP_SPIRAM);
  s_red   = (uint8_t*)heap_caps_malloc(EPD_PLANE_BYTES, MALLOC_CAP_SPIRAM);
  // Only EPD_FAST_INK needs this; without it that mode falls back to KEEP_RED.
  s_shown = (uint8_t*)heap_caps_calloc(1, EPD_PLANE_BYTES, MALLOC_CAP_SPIRAM);
  if (!s_bw || !s_red) {
    // No PSRAM: 161 kB of internal RAM is a squeeze but not impossible.
    LOGF("epd: no PSRAM, falling back to internal RAM\n");
    free(s_bw); free(s_red);
    s_bw  = (uint8_t*)malloc(EPD_PLANE_BYTES);
    s_red = (uint8_t*)malloc(EPD_PLANE_BYTES);
  }
  if (!s_bw || !s_red) {
    LOGF("epd: could not allocate the framebuffer\n");
    return false;
  }
  epd_clear();

  // Park the supply switch BEFORE the pin becomes an output: the OUT register
  // is written while it is still high-Z, so the first level the gate ever sees
  // driven is the off one. Without this the pin leaves gpio_config at 0, and
  // on a P-channel high-side switch 0 is ON.
  gpio_set_level((gpio_num_t)EPD_PIN_PWR, EPD_PWR_ACTIVE_HIGH ? 0 : 1);

  const uint64_t out_mask =
      (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_RST) | (1ULL << EPD_PIN_PWR) |
      (1ULL << EPD_PIN_CS_M) | (1ULL << EPD_PIN_CS_S);
  gpio_config_t oc = {};
  oc.pin_bit_mask = out_mask;
  oc.mode = GPIO_MODE_OUTPUT;
  gpio_config(&oc);

  // Release the deep-sleep latch only now, with the pad already configured and
  // the OUT register already holding the off level, so the level never lapses
  // between the latch letting go and this driving it.
  gpio_hold_dis((gpio_num_t)EPD_PIN_PWR);
  gpio_deep_sleep_hold_dis();

  uint64_t in_mask = (1ULL << EPD_PIN_BUSY_M);
#if EPD_PIN_BUSY_S >= 0
  in_mask |= (1ULL << EPD_PIN_BUSY_S);
#endif
  gpio_config_t ic = {};
  ic.pin_bit_mask = in_mask;
  ic.mode = GPIO_MODE_INPUT;
  ic.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&ic);

  power_off();
  s_power_off_valid = false;   // nothing to bleed down on a cold start
  rst_set(1);
  cs_hi();
  dc_set(1);

  spi_bus_config_t bus = {};
  bus.mosi_io_num = EPD_PIN_MOSI;
  bus.miso_io_num = -1;
  bus.sclk_io_num = EPD_PIN_SCK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = HALF_BYTES;
  if (spi_bus_initialize(EPD_SPI_HOST_NUM, &bus, SPI_DMA_DISABLED) != ESP_OK) {
    LOGF("epd: spi_bus_initialize failed\n");
    return false;
  }
  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = EPD_SPI_HZ;
  dev.mode = 0;                 // the panel samples on the rising edge
  dev.spics_io_num = -1;        // both chip selects are driven by hand
  dev.queue_size = 1;
  if (spi_bus_add_device(EPD_SPI_HOST_NUM, &dev, &s_spi) != ESP_OK) {
    LOGF("epd: spi_bus_add_device failed\n");
    return false;
  }
  bus_bitbang();

  epd_build_lut8(s_lut8, false, false);
  epd_build_lutb(s_lutb);
  epd_build_lut8(s_lut8_keep, false, false);
  epd_idle_tables(s_lut8_keep, 9u, 4u, 0x0Cu);
  epd_build_lutb(s_lutb_keep);
  epd_idle_tables(s_lutb_keep, 10u, 8u, 0x0Cu);
  epd_build_lut8(s_lut8_ink, false, false);
  epd_idle_tables(s_lut8_ink, 9u, 4u, 0x0Du);
  epd_build_lutb(s_lutb_ink);
  epd_idle_tables(s_lutb_ink, 10u, 8u, 0x0Du);
  epd_idle_shake(s_lut8_ink, 9u, 4u, 1u, 1u);
  epd_idle_shake(s_lutb_ink, 10u, 8u, (unsigned)EPD_B_INK_TABLE, (unsigned)EPD_B_SHAKE_GROUPS);

  // One power-up to find out what is attached, unless a previous wake already
  // answered that. The OTP is cached from here on either way.
  if (otp_restore()) {
    LOGF("epd: panel from RTC memory, probe skipped\n");
  } else if (cog_initial() != 0) {
    cog_stop();
    return false;
  } else {
    cog_stop();
  }
  s_ready = true;
  LOGF("epd: ready, panel %s\n", epd_panel_name());
  LOGF("epd: OTP VCOM 0x%02X, DUW %02X %02X %02X %02X %02X %02X, DRFW %02X %02X %02X %02X, RAM_RW %02X %02X %02X\n",
       s_otp[0x11], s_otp[0x16], s_otp[0x17], s_otp[0x18], s_otp[0x19], s_otp[0x1A], s_otp[0x1B],
       s_otp[0x0C], s_otp[0x0D], s_otp[0x0E], s_otp[0x0F], s_otp[0x13], s_otp[0x14], s_otp[0x15]);
  return true;
}

bool refresh_display(epd_refresh_t mode)
{
  if (!s_ready) {
    LOGF("epd: refresh_display() before a successful epd_begin()\n");
    return false;
  }
  const uint32_t t_frame = millis();

  if (mode == EPD_FAST_INK && !s_shown) mode = EPD_FAST_KEEP_RED;
  const bool ink_only = (mode == EPD_FAST_INK);
  if (ink_only) {
    // Skip the refresh when no pixel has gone black since the last one.
    const uint32_t* bw = (const uint32_t*)s_bw;
    uint32_t* shown = (uint32_t*)s_shown;
    uint32_t any = 0;
    for (size_t i = 0; i < EPD_PLANE_BYTES / 4; i++) {
      any |= bw[i] & ~shown[i];
      shown[i] |= bw[i];
    }
    if (!any) {
      VLOGF("epd: no new ink, nothing to refresh\n");
      s_last_frame_ms = 0;
      return true;
    }
  }

  const bool have_red = frame_has_red();
  const bool keep_red = (mode == EPD_FAST_KEEP_RED);
  // Red is a slow-refresh colour - see EPD_FAST_ALLOW_RED. Whether it is drawn
  // follows what the CALLER asked for, not the waveform actually used below.
  // EPD_FAST_KEEP_RED sends the real red plane, but its red tables are idle, so
  // red pixels are left exactly as a slow refresh put them.
  const bool draw_red = !ink_only && have_red &&
                        (mode == EPD_SLOW || keep_red || EPD_FAST_ALLOW_RED);
  // EPD_FAST_INK leaves red exactly where it was, so what the panel holds
  // doesn't change.
  s_frame_has_red = ink_only ? s_prev_has_red : draw_red;

  // Red already on the panel has to be driven off by a full refresh. Giving the
  // short waveform a few clearing groups is not enough - measured on the panel,
  // the red survives them, and once it has sat through one fast frame it does
  // not come out of the next one either. So the first fast refresh after a red
  // one runs on the panel's own waveform instead.
  //
  // The promotion is to the WAVEFORM only: red stays suppressed, because the
  // caller asked for fast and is expecting monochrome. That is also what stops
  // it repeating - this pass takes the red off the panel, so the frame after it
  // is fast again. Promoting with red drawn would put the red straight back and
  // every following frame would be promoted too.
  epd_refresh_t drive = mode;
  if (mode == EPD_FAST && s_prev_has_red) {
    LOGF("epd: red on the panel - this fast refresh runs slow to clear it\n");
    drive = EPD_SLOW;
  }

  // Which 084 table this frame needs. With the promotion above, a fast refresh
  // can no longer follow a red one, so this only ever asks for the red stages
  // when red is actually being drawn.
  const bool need_red_lut = draw_red || s_prev_has_red;

  // The panel latches one waveform per session and will not take a second one,
  // so anything that changes the waveform has to close the session first.
  if (s_session_open &&
      (drive != s_latched_mode ||
       (drive == EPD_FAST && !s_driver_b && need_red_lut != s_lut8_has_red))) {
    VLOGF("epd: the waveform changed, reopening the session\n");
    cog_stop();
    s_session_open = false;
    s_lut_latched = false;
  }

  if (!s_session_open) {
    bus_bitbang();
    if (cog_initial() != 0) {
      LOGF("epd: cog_initial failed\n");
      cog_stop();
      return false;
    }
    s_session_open = true;
    s_lut_latched = false;
    s_session_images = 0;
  } else {
    VLOGF("epd: reusing the open session, image %u\n",
          (unsigned)s_session_images + 1u);
  }

  if (ink_only) {
    // The real picture: red pixels land on the idle red table, ink - old and
    // new - on the ink table, which only ever pushes towards black.
    write_plane(0x10, s_bw, NULL);
    write_plane(0x13, s_red, NULL);
  } else {
    write_plane(0x10, s_bw, (have_red && !draw_red) ? s_red : NULL);
    write_plane(0x13, draw_red ? s_red : NULL, NULL);
    if (s_shown) {
      memcpy(s_shown, s_bw, EPD_PLANE_BYTES);
      if (have_red && !draw_red) {
        for (size_t i = 0; i < EPD_PLANE_BYTES; i++) s_shown[i] |= s_red[i];
      }
    }
  }

  // The first image of a session goes through the full sequence, which is what
  // latches the waveform. Later images only need the planes and a trigger:
  // rewriting 0xB0 inside a session does not re-latch, it drops the panel back
  // onto its own waveform.
  int rc = s_lut_latched ? retrigger() : do_refresh(drive, need_red_lut);

  // Only driver B needs more than one pull. Its refresh runs for a fixed time
  // whatever the table asks for, so a single pass cannot carry enough dose to
  // clear the previous frame; on the 084 the duration follows the table and
  // repeating it only multiplies the time.
  const bool fast_drive = (drive == EPD_FAST || drive == EPD_FAST_KEEP_RED || ink_only);
  const unsigned pulls = ink_only ? (unsigned)EPD_B_INK_PULLS : (unsigned)EPD_B_REFRESH_PULLS;
  if (rc == 0 && fast_drive && s_driver_b) {
    for (unsigned pull = 1; rc == 0 && pull < pulls; pull++) {
      rc = retrigger();
    }
  }

  s_latched_mode = drive;
  s_lut_latched = (rc == 0) && fast_drive;
  s_prev_has_red = s_frame_has_red;
  s_session_images++;

  const bool keep = s_keep_awake && s_lut_latched && (rc == 0) &&
                    (s_session_images < (uint32_t)EPD_SESSION_IMAGES);
  if (!keep) {
    s_session_open = false;
    s_lut_latched = false;
    cog_stop();
  }

  s_last_frame_ms = millis() - t_frame;
  LOGF("epd: %s refresh %s - panel %u ms, frame %u ms, red %s%s\n",
       (drive == mode) ? (drive == EPD_SLOW ? "slow"
                          : drive == EPD_FAST_KEEP_RED ? "fast (keep red)"
                          : drive == EPD_FAST_INK ? "fast (new ink only)" : "fast")
                       : "slow (promoted)",
       rc == 0 ? "done" : "FAILED",
       (unsigned)s_last_panel_ms, (unsigned)s_last_frame_ms,
       ink_only ? "idle" : draw_red ? "drawn" : (have_red ? "as ink" : "none"),
       keep ? ", COG kept up" : "");
  return rc == 0;
}

void epd_sleep(void)
{
  if (s_session_open) {
    s_session_open = false;
    s_lut_latched = false;
    cog_stop();
  }
}

bool epd_clear_screen(void)
{
  LOGF("epd: clearing the panel\n");
  epd_clear();
  return refresh_display(EPD_SLOW);
}

void epd_deep_sleep_prepare(void)
{
  epd_sleep();
  // Deep sleep lets every pad go high-Z. The MOSFET gate has no pull-up, so a
  // floating gate leaves the panel supply undefined for the whole sleep - and
  // the one thing a sleeping tag must not do is power its panel.
  gpio_hold_en((gpio_num_t)EPD_PIN_PWR);
  gpio_deep_sleep_hold_en();
  LOGF("epd: supply latched off, panel ready for deep sleep\n");
}

void epd_set_rotate_180(bool on) { s_rot180 = on; }
void epd_set_keep_awake(bool on) { s_keep_awake = on; }
bool epd_keep_awake(void)        { return s_keep_awake; }

epd_panel_t epd_panel(void)      { return s_panel; }
const char* epd_panel_name(void)
{
  switch (s_panel) {
    case EPD_PANEL_084: return "E2969JS084 (driver 8)";
    case EPD_PANEL_B1:  return "E2969JS0B1 (driver B)";
    default:            return "unknown";
  }
}
uint32_t epd_last_panel_ms(void) { return s_last_panel_ms; }
uint32_t epd_last_frame_ms(void) { return s_last_frame_ms; }

uint8_t* epd_plane_bw(void)  { return s_bw; }
uint8_t* epd_plane_red(void) { return s_red; }
