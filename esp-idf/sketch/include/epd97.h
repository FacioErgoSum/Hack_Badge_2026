// -----------------------------------------------------------------------------
// epd97 - an easy driver for the 9.7" Pervasive Displays iTC dual-COG panel.
//
// Two panel variants live behind the same FPC and the same 960x672 geometry:
//
//   E2969JS084  "driver 8"  - opens its OTP with 0xA8, takes a 180-byte
//                             waveform table, refresh follows the table
//   E2969JS0B1  "driver B"  - opens its OTP with 0xB9, takes a 400-byte
//                             waveform table, refresh is a fixed ~1 s
//
// Which one is attached is decided at runtime from the OTP, so nothing has to
// be configured for it. Usage is:
//
//     epd_begin();
//     epd_clear();
//     epd_text(40, 40, "hello", EPD_BLACK, 6);
//     refresh_display(EPD_SLOW);   // the panel's own waveform
//     refresh_display(EPD_FAST);   // the short waveform
//
// Both framebuffer planes live in PSRAM and are yours to write directly if the
// drawing helpers are not enough - see epd_plane_bw() / epd_plane_red().
// -----------------------------------------------------------------------------
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_W            960
#define EPD_H            672
#define EPD_STRIDE       (EPD_W / 8)              // 120 bytes per line
#define EPD_PLANE_BYTES  (EPD_STRIDE * EPD_H)     // 80640 bytes per plane

// How the image is driven onto the film.
//
//   EPD_SLOW  the panel's own waveform out of its OTP. ~51 s on the 084,
//             ~34 s on the B1. This is the one that restores the film
//             completely - worth running now and then, and worth using for
//             the first image after power-up.
//   EPD_FAST  the short waveform written to register 0xB0. ~3 s on the 084,
//             ~1 s per pull (three pulls) on the B1. Black and white only:
//             the short waveform cannot make a good red, so red content is
//             drawn as ink instead. See EPD_FAST_ALLOW_RED.
//
// One fast refresh silently runs slow: the first one after a frame that put red
// on the panel. Nothing short gets red back off the film, so it has to be
// driven off once with the panel's own waveform before fast refreshes work
// again. It happens once, not every frame - that pass clears the red.
//   EPD_FAST_KEEP_RED  (added for the badge Sketch firmware) the short waveform
//             with the red tables idle. Black and white are driven as in
//             EPD_FAST; red pixels get no drive at all, so red put up by an
//             EPD_SLOW refresh stays as it is instead of fading or being
//             drawn as ink. For drawing black over a red/white layout.
//   EPD_FAST_INK  (also badge Sketch) for adding lines. Ink is pushed towards
//             black with no shake, so lines already there stay put; the paper
//             and red tables are idle, so white and red get no drive. Skipped
//             when nothing went black since the last refresh. Can't erase:
//             black to white needs one of the modes above.
typedef enum {
  EPD_FAST = 0,
  EPD_SLOW = 1,
  EPD_FAST_KEEP_RED = 2,
  EPD_FAST_INK = 3
} epd_refresh_t;

typedef enum {
  EPD_PANEL_UNKNOWN = 0,
  EPD_PANEL_084,          // E2969JS084, driver 8
  EPD_PANEL_B1            // E2969JS0B1, driver B
} epd_panel_t;

// A pixel is (red bit, bw bit) and the pair picks one of four waveform
// tables, so red is not "black plus red" - it is its own combination.
typedef enum {
  EPD_WHITE = 0,
  EPD_BLACK = 1,
  EPD_RED   = 2
} epd_color_t;

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

// Claim the pins, allocate both planes, power the panel up once and read its
// OTP to find out which variant is attached. Leaves the panel powered down.
// Returns false if the planes could not be allocated or no panel answered.
bool epd_begin(void);

// Draw whatever is in the framebuffer. Returns false if the COG did not come
// up or the refresh timed out.
bool refresh_display(epd_refresh_t mode);

// Drive the whole panel to blank paper on its own waveform.
//
// What a boot wants before its first picture: e-paper holds whatever the last
// firmware left on it, and a short waveform cannot cross the full distance in
// one pass, so drawing straight over old content leaves it showing through.
// This clears the framebuffer as a side effect, so call it before you draw.
bool epd_clear_screen(void);

// Close an open session and cut the panel supply. Called for you unless
// keep-awake is on; call it by hand before a long idle.
void epd_sleep(void);

// Everything epd_sleep() does, plus latching the supply switch off across an
// ESP32 deep sleep. Deep sleep lets every pad go high-Z, and the MOSFET gate
// has no pull-up, so without the latch the panel supply is undefined while the
// chip is asleep. Call this immediately before esp_deep_sleep_start().
//
// The detected panel and its OTP are kept in RTC memory, so the wake after this
// skips the probe - about three seconds and a whole power cycle per wake.
void epd_deep_sleep_prepare(void);

// Hold the COG powered and initialised between images. An image then costs
// only its plane data and the refresh (~2.7 s instead of ~8 s on the 084) at
// the price of the panel keeping its rails charged in between. Off by default,
// which is the vendor flow. The session is torn down every
// EPD_SESSION_IMAGES images either way.
void epd_set_keep_awake(bool on);
bool epd_keep_awake(void);

// Turn the picture 180 degrees on its way to the panel. The framebuffer and
// the drawing calls stay in the viewer's coordinates. Set it before drawing
// the first image.
void epd_set_rotate_180(bool on);

// -----------------------------------------------------------------------------
// What is attached, and what the last refresh cost
// -----------------------------------------------------------------------------
epd_panel_t epd_panel(void);
const char* epd_panel_name(void);
uint32_t    epd_last_panel_ms(void);   // BUSY low to BUSY high
uint32_t    epd_last_frame_ms(void);   // the whole call

// -----------------------------------------------------------------------------
// Framebuffer
// -----------------------------------------------------------------------------
// Two 1-bpp planes, MSB first, x = 0 in bit 7 of byte 0 of the line.
// A set bit in the bw plane is ink; a set bit in the red plane is red. A red
// pixel has the red bit set and the bw bit clear.
uint8_t* epd_plane_bw(void);
uint8_t* epd_plane_red(void);

// -----------------------------------------------------------------------------
// Drawing
// -----------------------------------------------------------------------------
void epd_clear(void);
void epd_fill(epd_color_t c);
void epd_pixel(int x, int y, epd_color_t c);
void epd_fill_rect(int x, int y, int w, int h, epd_color_t c);
void epd_rect(int x, int y, int w, int h, int thickness, epd_color_t c);
void epd_hline(int x, int y, int w, int thickness, epd_color_t c);
void epd_vline(int x, int y, int h, int thickness, epd_color_t c);

// A 5x7 font, scaled by whole pixels. Returns the x the text ended at.
// Character cell is 6*scale wide and 8*scale tall.
int  epd_text(int x, int y, const char* s, epd_color_t c, int scale);
int  epd_printf(int x, int y, epd_color_t c, int scale, const char* fmt, ...);
int  epd_text_width(const char* s, int scale);

#ifdef __cplusplus
}
#endif
