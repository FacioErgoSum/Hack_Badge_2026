// -----------------------------------------------------------------------------
// Drawing into the two planes.
//
// A pixel is a pair of bits, one in each plane, and the pair picks one of the
// COG's four waveform tables: (red << 1) | bw. So red is not "black with red on
// top" - a red pixel has the red bit set and the bw bit clear, and writing both
// lands on table 3, which no waveform drives.
// -----------------------------------------------------------------------------
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "epd97.h"
#include "font5x7.h"

static inline void plane_set(uint8_t* p, int x, int y, bool on)
{
  uint8_t* b = &p[(size_t)y * EPD_STRIDE + (x >> 3)];
  const uint8_t m = (uint8_t)(0x80u >> (x & 7));
  if (on) *b |= m; else *b &= (uint8_t)~m;
}

void epd_clear(void)
{
  memset(epd_plane_bw(), 0, EPD_PLANE_BYTES);
  memset(epd_plane_red(), 0, EPD_PLANE_BYTES);
}

void epd_fill(epd_color_t c)
{
  memset(epd_plane_bw(),  (c == EPD_BLACK) ? 0xFF : 0x00, EPD_PLANE_BYTES);
  memset(epd_plane_red(), (c == EPD_RED)   ? 0xFF : 0x00, EPD_PLANE_BYTES);
}

void epd_pixel(int x, int y, epd_color_t c)
{
  if ((unsigned)x >= EPD_W || (unsigned)y >= EPD_H) return;
  plane_set(epd_plane_bw(),  x, y, c == EPD_BLACK);
  plane_set(epd_plane_red(), x, y, c == EPD_RED);
}

void epd_fill_rect(int x, int y, int w, int h, epd_color_t c)
{
  if (w <= 0 || h <= 0) return;
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = x + w, y1 = y + h;
  if (x1 > EPD_W) x1 = EPD_W;
  if (y1 > EPD_H) y1 = EPD_H;
  if (x0 >= x1 || y0 >= y1) return;

  uint8_t* bw  = epd_plane_bw();
  uint8_t* red = epd_plane_red();
  const uint8_t vbw  = (c == EPD_BLACK) ? 0xFF : 0x00;
  const uint8_t vred = (c == EPD_RED)   ? 0xFF : 0x00;

  for (int yy = y0; yy < y1; yy++) {
    size_t row = (size_t)yy * EPD_STRIDE;
    int xx = x0;
    // Edges bit by bit, the span between them a byte at a time.
    while (xx < x1 && (xx & 7)) { plane_set(bw, xx, yy, vbw); plane_set(red, xx, yy, vred); xx++; }
    int full = (x1 - xx) >> 3;
    if (full > 0) {
      memset(&bw[row + (xx >> 3)],  vbw,  full);
      memset(&red[row + (xx >> 3)], vred, full);
      xx += full * 8;
    }
    while (xx < x1) { plane_set(bw, xx, yy, vbw); plane_set(red, xx, yy, vred); xx++; }
  }
}

void epd_hline(int x, int y, int w, int thickness, epd_color_t c)
{
  epd_fill_rect(x, y, w, thickness < 1 ? 1 : thickness, c);
}

void epd_vline(int x, int y, int h, int thickness, epd_color_t c)
{
  epd_fill_rect(x, y, thickness < 1 ? 1 : thickness, h, c);
}

void epd_rect(int x, int y, int w, int h, int thickness, epd_color_t c)
{
  if (thickness < 1) thickness = 1;
  epd_fill_rect(x, y, w, thickness, c);
  epd_fill_rect(x, y + h - thickness, w, thickness, c);
  epd_fill_rect(x, y, thickness, h, c);
  epd_fill_rect(x + w - thickness, y, thickness, h, c);
}

int epd_text_width(const char* s, int scale)
{
  if (scale < 1) scale = 1;
  return (int)strlen(s) * 6 * scale;
}

int epd_text(int x, int y, const char* s, epd_color_t c, int scale)
{
  if (scale < 1) scale = 1;
  for (const char* p = s; *p; p++) {
    unsigned ch = (unsigned char)*p;
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8_t* g = k_font5x7[ch - 0x20];
    for (int col = 0; col < 5; col++) {
      uint8_t bits = g[col];
      for (int row = 0; row < 7; row++) {
        if (!(bits & (1u << row))) continue;
        epd_fill_rect(x + col * scale, y + row * scale, scale, scale, c);
      }
    }
    x += 6 * scale;
  }
  return x;
}

int epd_printf(int x, int y, epd_color_t c, int scale, const char* fmt, ...)
{
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return epd_text(x, y, buf, c, scale);
}
