"""E-Paper Display Driver for Billboard Badge

Panel: Pervasive Displays E2969JS0B1, 9.69" 960x672 black/white/red (Spectra),
two CoGs (master = left half, slave = right half) on one SPI bus.

Ported from Wiring in https://github.com/PervasiveDisplays/EPD_Driver_GU_large/blob/e408154652802a82e520cc25ba04adeb347d0528/src/EPD_Driver.cpp

Changes from the original port (see git history on main):
  * Hardware SPI instead of SoftSPI (same 8 MHz clock as the PDi reference).
  * Framebuffer split + bit reversal done in one viper pass instead of a
    Python generator over 161 kB (that alone took seconds).
  * Image upload yields to asyncio between chunks, so LEDs and buttons keep
    running while the display is busy.
  * Optional FAST update using the short waveform tables in
    data/rendered_luts.py (written to register 0xB0 after the OTP load).
    ~3.5 s of panel time instead of ~33 s. A full (OTP waveform) refresh is
    forced every FULL_REFRESH_EVERY fast updates to clear any ghosting.
  * BUSY polling at 10 ms instead of 100 ms.
  * update() requests made while a refresh is running are no longer lost.
  * LVGL flush converted to viper.

--COPYRIGHT--
Brief The drivers and functions of development board
Copyright (c) 2012-2021 Pervasive Displays Inc. All rights reserved.
 Authors: Pervasive Displays Inc.
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions
 are met:
 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution.
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
"""

import asyncio as aio
import btree
import framebuf
import gc
import micropython
from machine import Pin, SoftSPI
from micropython import const
import time

import lvgl

from lib.large_png import show_png

# ---------------------------------------------------------------------------
# Tunables
# ---------------------------------------------------------------------------
USE_HW_SPI = True          # False = old bit-banged SoftSPI
SPI_BAUD = 8_000_000       # PDi reference driver uses 8 MHz
FAST_BY_DEFAULT = True     # display.update() uses the short waveform
FULL_REFRESH_EVERY = 10    # after this many fast updates, do one full refresh
FAST_PASSES = 3            # run the fast waveform this many times per update
                           # (1 pass leaves ghosting / washed-out red)
_SPI_CHUNK = const(4096)   # bytes per SPI write before yielding to asyncio
_BUSY_POLL_MS = const(10)

EPD_CS1_PIN = Pin(13, mode=Pin.OUT)
EPD_CS2_PIN = Pin(3, mode=Pin.OUT)
EPD_BUSY1_PIN = Pin(12, mode=Pin.IN)
EPD_BUSY2_PIN = Pin(8, mode=Pin.IN)
EPD_DC_PIN = Pin(10, mode=Pin.OUT)  # Data High Command Low
EPD_RST_PIN = Pin(11, mode=Pin.OUT)
EPD_SCL_PIN = Pin(21, mode=Pin.OUT)
EPD_SDA_PIN = Pin(47, mode=Pin.OUT)
EPD_SPI_MISO_PIN = Pin(45, mode=Pin.IN)  # Not connected, signal not needed
EPD_PWR_EN_PIN = 48


pdi_cp = 0x9608
pdi_size = 0x96  # 9.69"
display_width = const(960)
half_width = const(display_width // 2)
display_height = const(672)
# Frame-buffer size = width * height / 8 * depth, uint32_t
# image_data_size is really only half the panel pixels, of one color
half_frame_one_color_size = const(display_height * display_width // (8 * 2))
# The width of rows in the full-frame and half-frame framebuffers
# Counting bytes, not pixels, since each byte is 8 packed pixels
display_width_bytes = const(display_width // 8)
display_half_width_bytes = const(display_width_bytes // 2)


# Two displays use different bit orders
def reverse_byte(byte: int) -> int:
    byte = (byte & 0xF0) >> 4 | (byte & 0x0F) << 4
    byte = (byte & 0xCC) >> 2 | (byte & 0x33) << 2
    byte = (byte & 0xAA) >> 1 | (byte & 0x55) << 1
    return byte


_REVERSE_LUT = bytes(reverse_byte(b) for b in range(256))
_IDENTITY_LUT = bytes(range(256))


@micropython.viper
def _split_halves(src, left, right, lut):
    """Split a 120-byte-wide frame into two 60-byte-wide halves, mapping
    every byte through `lut` (bit reversal or identity)."""
    s = ptr8(src)
    l = ptr8(left)
    r = ptr8(right)
    t = ptr8(lut)
    row = 0
    while row < 672:
        sb = row * 120
        db = row * 60
        c = 0
        while c < 60:
            l[db + c] = t[s[sb + c]]
            r[db + c] = t[s[sb + 60 + c]]
            c += 1
        row += 1


@micropython.viper
def _any_set(buf) -> bool:
    b = ptr8(buf)
    n = int(len(buf))
    i = 0
    while i < n:
        if b[i]:
            return True
        i += 1
    return False


@micropython.viper
def _rgb565_to_planes(src, stride: int, w: int, h: int, x1: int, y1: int, black, red):
    """LVGL RGB565 area -> black/red 1bpp planes (MSB = leftmost pixel)."""
    s = ptr8(src)
    bk = ptr8(black)
    rd = ptr8(red)
    row = 0
    while row < h:
        base = (y1 + row) * 960 + x1
        ro = row * stride
        col = 0
        while col < w:
            i = ro + col * 2
            c = s[i] | (s[i + 1] << 8)
            idx = base + col
            byte = idx >> 3
            mask = 128 >> (idx & 7)
            clear = mask ^ 255
            r_hi = c & 0x8000   # top bit of R5
            g_hi = c & 0x0400   # top bit of G6
            b_hi = c & 0x0010   # top bit of B5
            if r_hi and g_hi == 0 and b_hi == 0:
                rd[byte] = rd[byte] | mask
                bk[byte] = bk[byte] & clear
            elif r_hi == 0 and g_hi == 0 and b_hi == 0:
                bk[byte] = bk[byte] | mask
                rd[byte] = rd[byte] & clear
            else:
                bk[byte] = bk[byte] & clear
                rd[byte] = rd[byte] & clear
            col += 1
        row += 1


def _busy() -> bool:
    """True while either CoG is busy (BUSY is active low)."""
    return not EPD_BUSY1_PIN.value() or not EPD_BUSY2_PIN.value()


async def _wait_not_busy(poll_ms: int = _BUSY_POLL_MS, timeout_ms: int = 120_000):
    t0 = time.ticks_ms()
    while _busy():
        if time.ticks_diff(time.ticks_ms(), t0) > timeout_ms:
            print("EPD: BUSY timeout")
            return
        await aio.sleep_ms(poll_ms)


def _make_spi():
    if USE_HW_SPI:
        try:
            from machine import SPI
            spi = SPI(
                1, baudrate=SPI_BAUD, polarity=0, phase=0,
                sck=EPD_SCL_PIN, mosi=EPD_SDA_PIN, miso=EPD_SPI_MISO_PIN,
            )
            print(f"EPD: hardware SPI @ {SPI_BAUD} Hz")
            return spi
        except Exception as ex:
            print(f"EPD: hardware SPI failed ({ex}), falling back to SoftSPI")
    spi = SoftSPI(
        baudrate=SPI_BAUD, sck=EPD_SCL_PIN, mosi=EPD_SDA_PIN, miso=EPD_SPI_MISO_PIN
    )
    spi.init()
    print("EPD: SoftSPI")
    return spi


class EPaperDisplay:
    width = display_width
    height = display_height

    def __init__(self) -> None:
        print("Initializing E-Paper Display")
        EPD_DC_PIN.value(True)
        EPD_RST_PIN.value(True)
        EPD_CS1_PIN.value(True)
        EPD_CS2_PIN.value(True)
        self.spi = _make_spi()

        display_pn = b"B1"
        try:
            with open("/data/hwconf", "rb") as f:
                hwconf = btree.open(f)
                display_pn = hwconf.get(b"display_pn", b"B1")
        except OSError:
            pass
        self.display_pn = display_pn
        self._reverse_fb_bits = display_pn != b"84"
        # CoG revision for the fast waveform tables: "84" panels -> driver 8,
        # everything else (B1, the default) -> driver B.
        self._cog_driver = "8" if display_pn == b"84" else "B"
        print(f"EPD: panel {display_pn}, CoG driver {self._cog_driver}")

        # Update settings
        self.fast_by_default = FAST_BY_DEFAULT
        self.full_refresh_every = FULL_REFRESH_EVERY
        self.fast_passes = FAST_PASSES
        # None = unknown, True/False = whether the CoG accepts a second 0x15
        # refresh without a full re-init. Learned on the first multi-pass update.
        self._repeat_refresh_ok = None
        self._fast_count = 0
        self._request_fast = FAST_BY_DEFAULT
        self._luts = None  # loaded lazily
        self.last_update_ms = 0

        # The framebuffers are set and display should be updated.
        self._framebuffer_ready = aio.Event()

        """Each byte is a mini-row of 8 bits for pixels of one color (1 sets the color, 0 is white)
        Bytes are in rows across the screen, as seen on the badge
        0 ------------------------> 119 [120 bytes per row]
        ...                        ...
        [672 rows]
        """
        self._framebuffer_black = bytearray(half_frame_one_color_size * 2)
        self._framebuffer_red = bytearray(half_frame_one_color_size * 2)
        # Framebuffers that can be drawn to directly
        self.black = framebuf.FrameBuffer(
            self._framebuffer_black, display_width, display_height, framebuf.MONO_HLSB
        )
        self.red = framebuf.FrameBuffer(
            self._framebuffer_red, display_width, display_height, framebuf.MONO_HLSB
        )

        # Is the display done updating? Start set since the display is not updating before boot.
        self._display_updated = aio.Event()
        self._display_updated.set()
        gc.collect()

        # Set up lvgl
        lvgl.init()
        # Give LVGL a buffer 2-bytes-per-pixel for RGB565 format
        self._lv_draw_buf = bytearray(display_width * display_height * 2)

        self.lv_display = lvgl.display_create(display_width, display_height)
        self.lv_display.set_color_format(lvgl.COLOR_FORMAT.RGB565)
        self.lv_display.set_buffers(
            self._lv_draw_buf, None, len(self._lv_draw_buf),
            lvgl.DISPLAY_RENDER_MODE.PARTIAL,
        )
        self.lv_display.set_flush_cb(self._lv_flush)
        self.lv_screen = lvgl.screen_active()
        self.lv_screen.set_size(display_width, display_height)

    # ------------------------------------------------------------------
    # Drawing API
    # ------------------------------------------------------------------
    def _lv_flush(self, disp, area, color_p):
        x1, y1 = area.x1, area.y1
        w = area.x2 - x1 + 1
        h = area.y2 - y1 + 1
        try:
            stride = lvgl.draw_buf_width_to_stride(w, lvgl.COLOR_FORMAT.RGB565)
        except AttributeError:
            stride = w * 2
        src = color_p.__dereference__(stride * h)
        try:
            _rgb565_to_planes(src, stride, w, h, x1, y1,
                              self._framebuffer_black, self._framebuffer_red)
        except TypeError:
            # Some LVGL builds hand back an object viper can't take a pointer to
            _rgb565_to_planes(bytes(src), stride, w, h, x1, y1,
                              self._framebuffer_black, self._framebuffer_red)
        disp.flush_ready()

    def update(self, fast=None):
        """Done drawing: push the framebuffers to the panel.

        fast=None  -> use display.fast_by_default
        fast=True  -> short waveform (~3.5 s), may ghost slightly over time
        fast=False -> panel's own full waveform (~33 s), best quality
        Returns immediately; the refresh runs in the background.
        """
        self._request_fast = self.fast_by_default if fast is None else bool(fast)
        self._display_updated.clear()
        self._framebuffer_ready.set()

    def full_update(self):
        """Shortcut for update(fast=False)."""
        self.update(fast=False)

    def clear(self):
        """Clear framebuffers to full white"""
        self.black.fill(0)
        self.red.fill(0)

    def is_done_updating(self):
        """Is the display done updating?"""
        return self._display_updated.is_set()

    async def wait_done(self):
        """await display.wait_done() to block an app until the refresh finishes."""
        await self._display_updated.wait()

    def draw_image(self, image, height: int, width: int, row: int = 0, col: int = 0):
        for img_r in range(height):
            for img_c in range(width):
                color = image[img_r][img_c]
                red = color[0] > 127
                black = color[0] < 128 and color[1] < 128 and color[2] < 128
                self.black.pixel((col + img_c), (row + img_r), int(black))
                self.red.pixel((col + img_c), (row + img_r), int(red))

    def draw_png(self, filename: str, row: int = 0, col: int = 0):
        gc.collect()
        t0 = time.ticks_ms()
        show_png(filename, self._framebuffer_black, self._framebuffer_red, row, col)
        print(f"Loaded {filename} in {time.ticks_diff(time.ticks_ms(), t0)} ms")
        gc.collect()

    # ------------------------------------------------------------------
    # Background task
    # ------------------------------------------------------------------
    async def run(self):
        black_left = bytearray(half_frame_one_color_size)
        black_right = bytearray(half_frame_one_color_size)
        red_left = bytearray(half_frame_one_color_size)
        red_right = bytearray(half_frame_one_color_size)
        lut = _REVERSE_LUT if self._reverse_fb_bits else _IDENTITY_LUT
        while True:
            await self._framebuffer_ready.wait()
            # Clear *before* copying, so an update() requested during the
            # refresh below triggers another refresh instead of being dropped.
            self._framebuffer_ready.clear()
            fast = self._request_fast
            if fast and self._fast_count >= self.full_refresh_every:
                print("EPD: periodic full refresh to clear ghosting")
                fast = False

            _split_halves(self._framebuffer_black, black_left, black_right, lut)
            _split_halves(self._framebuffer_red, red_left, red_right, lut)
            has_red = _any_set(self._framebuffer_red)

            try:
                await self.global_update(black_left, red_left, black_right, red_right,
                                         fast=fast, has_red=has_red)
            except Exception as ex:
                import sys
                print("EPD: update failed")
                sys.print_exception(ex)

            self._fast_count = self._fast_count + 1 if fast else 0
            gc.collect()
            if not self._framebuffer_ready.is_set():
                self._display_updated.set()

    async def global_update(self, black_left, red_left, black_right, red_right,
                            fast=False, has_red=True):
        t_start = time.ticks_ms()
        lut = self._fast_lut(has_red) if fast else None
        if fast and lut is None:
            fast = False
        print(f"EPD: {'fast' if fast else 'full'} update")

        await self._reset()
        t = time.ticks_ms()
        await self._send_images(black_left, red_left, black_right, red_right)
        t_upload = time.ticks_diff(time.ticks_ms(), t)

        t = time.ticks_ms()
        await self.cog_initial()
        if lut is not None:
            self._send_index_data(0xB0, lut)
        await self._dcdc_soft_start()
        t_init = time.ticks_diff(time.ticks_ms(), t)

        t = time.ticks_ms()
        await self._display_refresh()
        await self._dcdc_soft_shutdown()
        t_panel = time.ticks_diff(time.ticks_ms(), t)

        self.last_update_ms = time.ticks_diff(time.ticks_ms(), t_start)
        print(f"EPD: upload {t_upload} ms, init {t_init} ms, "
              f"panel {t_panel} ms, total {self.last_update_ms} ms")

    def _fast_lut(self, has_red):
        if self._luts is None:
            try:
                import data.rendered_luts as L
                if self._cog_driver == "B":
                    self._luts = {"base": bytes(L.lutb_base)}
                else:
                    self._luts = {
                        "red": bytes(L.lut8_red_clear),
                        "nored": bytes(L.lut8_nored_clear),
                    }
                del L
                gc.collect()
            except Exception as ex:
                print(f"EPD: fast LUTs unavailable ({ex}), using full refresh")
                self._luts = {}
        if not self._luts:
            return None
        if self._cog_driver == "B":
            return self._luts["base"]
        return self._luts["red" if has_red else "nored"]

    # ------------------------------------------------------------------
    # CoG sequences (timings from the PDi reference driver)
    # ------------------------------------------------------------------
    async def cog_initial(self):
        self._send_index_data(0x05, [0x7D])
        await aio.sleep_ms(200)
        self._send_index_data(0x05, [0x00])
        await aio.sleep_ms(10)
        self._send_index_data(0xD8, [0x80])  # MS_SYNC)
        self._send_index_data(0xD6, [0x00])  # BVSS
        self._send_index_data(0xA7, [0x10])
        await aio.sleep_ms(100)
        self._send_index_data(0xA7, [0x00])
        await aio.sleep_ms(100)
        self._send_index_data(0x03, [0x00, 0x11])  # OSC

        for send in (self._send_index_data_left, self._send_index_data_right):
            send(0x44, [0x00])
            send(0x45, [0x80])
            send(0xA7, [0x10])
            await aio.sleep_ms(100)
            send(0xA7, [0x00])
            await aio.sleep_ms(100)
            send(0x44, [0x06])
            send(0x45, [0x82])  # Temperature 0x82@25C
            send(0xA7, [0x10])
            await aio.sleep_ms(100)
            send(0xA7, [0x00])
            await aio.sleep_ms(100)

        self._send_index_data(0x60, [0x25])  # TCON
        self._send_index_data_left(0x61, [0x01])  # STV_DIR for Display 1
        self._send_index_data(0x02, [0x00])  # VCOM

    async def cog_poweroff(self):
        """CoG shutdown function
        Shuts down the CoG and DC/DC circuit after all update functions
        """
        self._send_index_data(0x09, [0x7F, 0x7D, 0x00])
        await aio.sleep_ms(200)
        await _wait_not_busy(1)
        EPD_DC_PIN.value(False)
        EPD_CS1_PIN.value(False)
        await aio.sleep_ms(150)
        EPD_RST_PIN.value(False)

    def _send_index_data(self, index: int, data):
        EPD_DC_PIN.value(False)
        EPD_CS1_PIN.value(False)
        EPD_CS2_PIN.value(False)
        self.spi.write(bytes([index]))
        EPD_CS1_PIN.value(True)
        EPD_CS2_PIN.value(True)
        EPD_DC_PIN.value(True)
        EPD_CS1_PIN.value(False)
        EPD_CS2_PIN.value(False)
        self.spi.write(bytes(data))
        EPD_CS1_PIN.value(True)
        EPD_CS2_PIN.value(True)

    def _send_one(self, cs, other_cs, index: int, data):
        EPD_DC_PIN.value(False)
        cs.value(False)
        other_cs.value(True)
        self.spi.write(bytes([index]))
        cs.value(True)
        EPD_DC_PIN.value(True)
        cs.value(False)
        self.spi.write(bytes(data))
        cs.value(True)

    def _send_index_data_left(self, index: int, data):
        self._send_one(EPD_CS1_PIN, EPD_CS2_PIN, index, data)

    def _send_index_data_right(self, index: int, data):
        self._send_one(EPD_CS2_PIN, EPD_CS1_PIN, index, data)

    async def _send_big(self, cs, other_cs, index: int, buf):
        """Like _send_one, but streams a large buffer in chunks and lets the
        other asyncio tasks (LEDs, buttons) run between chunks. CS stays low."""
        EPD_DC_PIN.value(False)
        cs.value(False)
        other_cs.value(True)
        self.spi.write(bytes([index]))
        cs.value(True)
        EPD_DC_PIN.value(True)
        cs.value(False)
        mv = memoryview(buf)
        n = len(mv)
        i = 0
        while i < n:
            self.spi.write(mv[i:i + _SPI_CHUNK])
            i += _SPI_CHUNK
            await aio.sleep_ms(0)
        cs.value(True)

    async def _display_refresh(self):
        await _wait_not_busy()
        self._send_index_data(0x15, [0x3C])
        await aio.sleep_ms(5)

    async def _reset(self):
        """CoG driver power-on hard reset"""
        await aio.sleep_ms(200)
        EPD_RST_PIN.value(True)
        await aio.sleep_ms(20)
        EPD_RST_PIN.value(False)
        await aio.sleep_ms(200)
        EPD_RST_PIN.value(True)
        await aio.sleep_ms(200)
        EPD_CS1_PIN.value(True)
        EPD_CS2_PIN.value(True)
        await aio.sleep_ms(5)

    async def _send_images(self, black_left, red_left, black_right, red_right):
        assert len(black_left) == half_frame_one_color_size
        assert len(red_left) == half_frame_one_color_size
        assert len(black_right) == half_frame_one_color_size
        assert len(red_right) == half_frame_one_color_size

        L, R = EPD_CS1_PIN, EPD_CS2_PIN
        # DUW for both left and right halves
        self._send_index_data(0x13, [0x00, 0x3B, 0x00, 0x00, 0x9F, 0x02])
        # DRFW for Both left and right halves
        self._send_index_data(0x90, [0x00, 0x3B, 0x00, 0xA9])
        ram_rw_data = [0x3B, 0x00, 0x14]
        self._send_one(L, R, 0x12, ram_rw_data)    # RAM_RW for left half
        await self._send_big(L, R, 0x10, black_left)
        self._send_one(L, R, 0x12, ram_rw_data)
        await self._send_big(L, R, 0x11, red_left)
        self._send_one(R, L, 0x12, ram_rw_data)    # RAM_RW for right half
        await self._send_big(R, L, 0x10, black_right)
        self._send_one(R, L, 0x12, ram_rw_data)
        await self._send_big(R, L, 0x11, red_right)

    async def _dcdc_soft_start(self):
        """DC-DC soft-start command
        Implemented after image data are uploaded to CoG
        Specific to mid-sized EPDs only
        """
        self._send_index_data(0x51, [0x50, 0x01])

        for val in range(1, 5):
            self._send_index_data(0x09, [0x1F])
            self._send_index_data(0x51, [0x50, val])
            self._send_index_data(0x09, [0x9F])
            await aio.sleep_ms(2)

        for val in range(1, 11):
            self._send_index_data(0x09, [0x1F])
            self._send_index_data(0x51, [0x0A, val])
            self._send_index_data(0x09, [0x9F])
            await aio.sleep_ms(2)

        for val in range(3, 11):
            self._send_index_data(0x09, [0x7F])
            self._send_index_data(0x51, [0x0A, val])
            self._send_index_data(0x09, [0xFF])
            await aio.sleep_ms(2)

        for val in range(9, 1, -1):
            self._send_index_data(0x09, [0x7F])
            self._send_index_data(0x51, [val, 0x0A])
            self._send_index_data(0x09, [0xFF])
            await aio.sleep_ms(2)

        self._send_index_data(0x09, [0xFF])
        await aio.sleep_ms(10)

    async def _dcdc_soft_shutdown(self):
        """DC-DC soft-shutdown command
        Implemented after image data are uploaded to CoG
        Specific to mid-sized EPDs only
        """
        await _wait_not_busy()
        self._send_index_data(0x09, [0x7F])
        self._send_index_data(0x05, [0x7D])
        self._send_index_data(0x09, [0x00])
        await aio.sleep_ms(200)
        await _wait_not_busy()
        EPD_DC_PIN.value(False)
        EPD_CS1_PIN.value(False)
        EPD_RST_PIN.value(False)
        EPD_CS1_PIN.value(True)


display = EPaperDisplay()
