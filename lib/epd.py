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
FLASH_BEFORE_FAST = True   # flash solid frames before each fast update
FLASH_SEQUENCE = ("black", "white")   # any of "black", "white", "red"
FLASH_PASSES = 1           # waveform passes per flash frame
PARTIAL_PASSES = 2         # waveform passes for update_partial()
# update_partial() refreshes only a window around the new pixels via the CoG's
# DRFW register (0x90). Its y units/offset aren't documented; these come from
# the full-screen value the driver uses (x 0..0x3B bytes, y 0..0xA9 = 680 gate
# lines / 4). Check with display.window_test().
DRFW_Y_UNIT = 4            # gate lines per DRFW y step
DRFW_Y_OFFSET = 0          # gate lines before RAM row 0
DRFW_Y_MAX = 0xA9
DRFW_FLIP_Y_LEFT = False   # left CoG scans gates the other way (STV_DIR)
DRFW_FLIP_Y_RIGHT = False
PARTIAL_PAD_ROWS = 8       # extra rows around the new pixels
PARTIAL_PAD_BYTES = 1      # extra 8 px columns around the new pixels
BW_FAST_LUT = True         # frames with no red (incl. flash frames) use a
                           # shortened waveform: red's extra drive groups idled
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


def _bw_lut_driver_b(base):
    """Driver B table with red (table 2) idled after group 4, like the
    black/white tables. 5 tables x 10 groups x 8 bytes. For frames with no red
    pixels table 2 drives nothing, but its 9 drive groups stretch the waveform."""
    lut = bytearray(base)
    idle = bytes([0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01])
    for g in range(5, 10):
        o = 2 * 80 + g * 8
        lut[o:o + 8] = idle
    return bytes(lut)


def _idle_tables_lut(lut, group_size, groups, tables):
    """Copy of a fast LUT with whole tables replaced by idle rows (the LUT's
    own last VCOM row: ground for one frame). Unlike zeroing one byte, this
    doesn't depend on decoding the row format."""
    b = bytearray(lut)
    idle_at = (4 * groups + groups - 1) * group_size
    idle = bytes(b[idle_at:idle_at + group_size])
    for t in tables:
        for g in range(groups):
            o = (t * groups + g) * group_size
            b[o:o + group_size] = idle
    return bytes(b)


@micropython.viper
def _delta_and_mark(cur, shown, out) -> int:
    """out = black pixels in `cur` that aren't in `shown`; adds them to
    `shown`. Returns 1 if any pixel is new."""
    c = ptr8(cur)
    s = ptr8(shown)
    o = ptr8(out)
    n = int(len(cur))
    found = 0
    i = 0
    while i < n:
        d = c[i] & (s[i] ^ 0xFF)
        o[i] = d
        if d:
            found = 1
            s[i] = s[i] | d
        i += 1
    return found


@micropython.viper
def _bbox(buf, out) -> int:
    """Bounding box of set bits in a 120-byte-wide frame.
    out (bytearray(8)) <- row0, row1, byte_col0, byte_col1 as uint16.
    Returns 1 if anything is set."""
    b = ptr8(buf)
    o = ptr16(out)
    r0 = 9999
    r1 = -1
    c0 = 9999
    c1 = -1
    row = 0
    while row < 672:
        base = row * 120
        c = 0
        while c < 120:
            if b[base + c]:
                if row < r0:
                    r0 = row
                r1 = row
                if c < c0:
                    c0 = c
                if c > c1:
                    c1 = c
            c += 1
        row += 1
    if r1 < 0:
        return 0
    o[0] = r0
    o[1] = r1
    o[2] = c0
    o[3] = c1
    return 1


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
        self.flash_before_fast = FLASH_BEFORE_FAST
        self.flash_sequence = FLASH_SEQUENCE
        self.flash_passes = FLASH_PASSES
        self._request_flash = None
        self._request_passes = None
        self._request_bw = None
        self._request_auto_full = True
        self._solid_ff = None  # lazily allocated all-ones half plane
        self._solid_00 = None  # lazily allocated all-zeros half plane
        self.partial_passes = PARTIAL_PASSES
        self._request_partial = False
        self._partial_buf = None
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
        # Black pixels currently on the panel, for update_partial()
        self._shown_black = bytearray(half_frame_one_color_size * 2)
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

    def update(self, fast=None, flash=None, passes=None, bw=None, auto_full=True):
        """Done drawing: push the framebuffers to the panel.

        fast=None  -> use display.fast_by_default
        fast=True  -> short waveform (~3.5 s), may ghost slightly over time
        fast=False -> panel's own full waveform (~33 s), best quality
        Returns immediately; the refresh runs in the background.
        Fast updates run display.fast_passes times (default 3) to drive the
        pigment fully and clear ghosting. Set display.fast_passes = 1 for speed.

        flash=None  -> use display.flash_before_fast (fast updates only)
        flash=True  -> flash display.flash_sequence (black, white) first
        flash=False -> no flash

        passes=N    -> override display.fast_passes for this update
        bw=True     -> use the short black/white waveform even if the frame has
                       red (red pixels get less drive; good for quick redraws)
        auto_full=False -> don't count toward / trigger the periodic full refresh
        """
        self._request_fast = self.fast_by_default if fast is None else bool(fast)
        self._request_partial = False
        self._request_flash = flash
        self._request_passes = passes
        self._request_bw = bw
        self._request_auto_full = auto_full
        self._display_updated.clear()
        self._framebuffer_ready.set()

    def update_partial(self, passes=None):
        """Draw only black pixels added since the last update.

        White and red pixels (and black ones already showing) get no drive,
        so nothing else on the panel changes or flashes. It can only ADD
        black: erasing needs a normal update(). Meant for drawing apps.
        """
        if self._framebuffer_ready.is_set() and not self._request_partial:
            return  # a full update is already queued and will cover this
        self._request_partial = True
        self._request_passes = passes
        self._display_updated.clear()
        self._framebuffer_ready.set()

    def full_update(self):
        """Shortcut for update(fast=False)."""
        self.update(fast=False)

    def clean(self):
        """Redraw what's in the framebuffer with a flash, to clear ghosting."""
        self.update(fast=True, flash=True)

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

            if self._request_partial:
                self._request_partial = False
                passes = self._request_passes
                try:
                    box = getattr(self, "_test_box", None)
                    self._test_box = None
                    await self._partial(lut, black_left, red_left,
                                        black_right, red_right, passes, box)
                except Exception as ex:
                    import sys
                    print("EPD: partial update failed")
                    sys.print_exception(ex)
                gc.collect()
                if not self._framebuffer_ready.is_set():
                    self._display_updated.set()
                continue

            fast = self._request_fast
            flash = self._request_flash
            if flash is None:
                flash = self.flash_before_fast
            passes = self._request_passes
            bw = self._request_bw
            auto_full = self._request_auto_full
            if fast and auto_full and self._fast_count >= self.full_refresh_every:
                print("EPD: periodic full refresh to clear ghosting")
                fast = False

            _split_halves(self._framebuffer_black, black_left, black_right, lut)
            _split_halves(self._framebuffer_red, red_left, red_right, lut)
            has_red = _any_set(self._framebuffer_red)
            self._shown_black[:] = self._framebuffer_black

            try:
                await self.global_update(black_left, red_left, black_right, red_right,
                                         fast=fast, has_red=has_red, flash=flash,
                                         passes=passes, bw=bw)
            except Exception as ex:
                import sys
                print("EPD: update failed")
                sys.print_exception(ex)

            if not fast:
                self._fast_count = 0
            elif auto_full:
                self._fast_count += 1
            gc.collect()
            if not self._framebuffer_ready.is_set():
                self._display_updated.set()

    async def _partial(self, bitlut, black_left, red_left, black_right,
                       red_right, passes, box=None):
        """Refresh only a window around black pixels added since the last update."""
        if box is None:
            if self._partial_buf is None:
                self._partial_buf = bytearray(len(self._framebuffer_black))
            if not _delta_and_mark(self._framebuffer_black, self._shown_black,
                                   self._partial_buf):
                return  # nothing new to draw
            bb = bytearray(8)
            _bbox(self._partial_buf, bb)
            box = (bb[0] | bb[1] << 8, bb[2] | bb[3] << 8,
                   bb[4] | bb[5] << 8, bb[6] | bb[7] << 8)
        lut = self._bw_short_lut()
        if lut is None:
            print("EPD: no fast LUT, can't do a partial update")
            return
        if passes is None:
            passes = self.partial_passes
        # Real image data everywhere; only the window gets driven
        _split_halves(self._framebuffer_black, black_left, black_right, bitlut)
        _split_halves(self._framebuffer_red, red_left, red_right, bitlut)
        window = self._drfw_for_box(*box)
        t = time.ticks_ms()
        print(f"EPD: partial rows {box[0]}-{box[1]} px {box[2] * 8}-{box[3] * 8 + 7}"
              f" x{passes}, DRFW L={window[0]} R={window[1]}")
        await self._run_frame((black_left, red_left, black_right, red_right), lut,
                              max(1, int(passes)), window)
        self.last_update_ms = time.ticks_diff(time.ticks_ms(), t)
        print(f"EPD: partial total {self.last_update_ms} ms")

    def _drfw_for_box(self, r0, r1, c0, c1):
        """(left, right) DRFW byte lists [x0, x1, y0, y1] for a frame box
        (rows r0..r1, full-frame byte columns c0..c1). A half with nothing in
        the box gets a 1-unit window in the white middle of the screen."""
        r0 = max(0, r0 - PARTIAL_PAD_ROWS)
        r1 = min(display_height - 1, r1 + PARTIAL_PAD_ROWS)
        c0 = max(0, c0 - PARTIAL_PAD_BYTES)
        c1 = min(display_width_bytes - 1, c1 + PARTIAL_PAD_BYTES)

        def ywin(a, b, flip):
            y0 = (a + DRFW_Y_OFFSET) // DRFW_Y_UNIT
            y1 = (b + DRFW_Y_OFFSET) // DRFW_Y_UNIT
            if flip:
                y0, y1 = DRFW_Y_MAX - y1, DRFW_Y_MAX - y0
            return max(0, min(DRFW_Y_MAX, y0)), max(0, min(DRFW_Y_MAX, y1))

        hw = display_half_width_bytes
        out = []
        for half, flip in ((0, DRFW_FLIP_Y_LEFT), (1, DRFW_FLIP_Y_RIGHT)):
            lo, hi = half * hw, half * hw + hw - 1
            if c1 < lo or c0 > hi:
                ya, yb = ywin(display_height // 2, display_height // 2, flip)
                out.append([hw // 2, hw // 2, ya, yb])
            else:
                ya, yb = ywin(r0, r1, flip)
                out.append([max(c0, lo) - lo, min(c1, hi) - lo, ya, yb])
        return out

    def _bw_short_lut(self):
        """Fast LUT with the red tables (2, 3) idle. For windows with no red
        pixels: black/white drive unchanged, none of red's long groups."""
        self._fast_lut(False)  # make sure the tables are loaded
        if not self._luts:
            return None
        if "bw_short" not in self._luts:
            if self._cog_driver == "B":
                self._luts["bw_short"] = _idle_tables_lut(self._luts["base"], 8, 10, (2, 3))
            else:
                self._luts["bw_short"] = _idle_tables_lut(self._luts["nored"], 4, 9, (2, 3))
        return self._luts["bw_short"]

    def window_test(self, x=200, y=200, w=120, h=80, passes=None):
        """Calibrate partial windows: draws a black box and refreshes ONLY
        that box. Watch the panel: the box should appear, nothing else
        should flash. Try one in each half, e.g. window_test(200, 200) and
        window_test(650, 400)."""
        self.black.fill_rect(x, y, w, h, 1)
        self.red.fill_rect(x, y, w, h, 0)
        self._shown_black[:] = self._framebuffer_black
        self._test_box = (y, y + h - 1, x // 8, (x + w - 1) // 8)
        self._request_partial = True
        self._request_passes = passes
        self._display_updated.clear()
        self._framebuffer_ready.set()

    async def global_update(self, black_left, red_left, black_right, red_right,
                            fast=False, has_red=True, flash=False,
                            passes=None, bw=False):
        t_start = time.ticks_ms()
        lut = self._fast_lut(has_red and not bw) if fast else None
        if fast and lut is None:
            fast = False
        if passes is None:
            passes = self.fast_passes
        passes = max(1, int(passes)) if fast else 1
        print(f"EPD: {'fast x%d' % passes if fast else 'full'} update")

        if fast and flash and self.flash_sequence:
            # Flash solid frames to wipe the old image. Black/white frames use
            # the short black/white waveform.
            for color in self.flash_sequence:
                flash_lut = self._fast_lut(color == "red")
                bk, rd = self._solid(color)
                t = time.ticks_ms()
                await self._run_frame((bk, rd, bk, rd), flash_lut,
                                      max(1, int(self.flash_passes)))
                print(f"EPD: flash {color} {time.ticks_diff(time.ticks_ms(), t)} ms")

        await self._run_frame((black_left, red_left, black_right, red_right), lut, passes)

        self.last_update_ms = time.ticks_diff(time.ticks_ms(), t_start)
        print(f"EPD: total {self.last_update_ms} ms")

    def _solid(self, color):
        """(black_plane, red_plane) half-frame buffers for a solid color."""
        if self._solid_00 is None:
            self._solid_00 = bytes(half_frame_one_color_size)
            self._solid_ff = b"\xff" * half_frame_one_color_size
        if color == "black":
            return self._solid_ff, self._solid_00
        if color == "red":
            return self._solid_00, self._solid_ff
        return self._solid_00, self._solid_00  # white

    async def _run_frame(self, images, lut, passes, window=None):
        """Upload one frame and run the waveform `passes` times."""
        if passes > 1 and self._repeat_refresh_ok is False:
            # This CoG ignores back-to-back refreshes: redo everything per pass
            for p in range(passes):
                await self._prepare(images, lut, window)
                dt = await self._refresh_once()
                await self._dcdc_soft_shutdown()
                print(f"EPD: pass {p + 1}/{passes} (full cycle) {dt} ms")
        else:
            await self._prepare(images, lut, window)
            p = 0
            while p < passes:
                dt = await self._refresh_once()
                print(f"EPD: pass {p + 1}/{passes} {dt} ms")
                if p == 1 and self._repeat_refresh_ok is None:
                    # A real pass takes seconds; a few ms means it was ignored
                    self._repeat_refresh_ok = dt > 500
                    print(f"EPD: repeat refresh {'works' if self._repeat_refresh_ok else 'ignored, switching to full cycles'}")
                    if not self._repeat_refresh_ok:
                        await self._dcdc_soft_shutdown()
                        for q in range(p, passes):
                            await self._prepare(images, lut, window)
                            dt = await self._refresh_once()
                            await self._dcdc_soft_shutdown()
                            print(f"EPD: pass {q + 1}/{passes} (full cycle) {dt} ms")
                        break
                p += 1
            else:
                await self._dcdc_soft_shutdown()

    async def _prepare(self, images, lut, window=None):
        """Reset, upload the image, init the CoG, load LUT, power up DC/DC."""
        await self._reset()
        t = time.ticks_ms()
        await self._send_images(*images)
        if window is not None:
            # Refresh only this window on each CoG (default is full screen)
            self._send_index_data_left(0x90, window[0])
            self._send_index_data_right(0x90, window[1])
        t_upload = time.ticks_diff(time.ticks_ms(), t)
        t = time.ticks_ms()
        await self.cog_initial()
        if lut is not None:
            self._send_index_data(0xB0, lut)
        await self._dcdc_soft_start()
        print(f"EPD: upload {t_upload} ms, init {time.ticks_diff(time.ticks_ms(), t)} ms")

    async def _refresh_once(self):
        """Trigger one waveform run and wait for it. Returns ms the panel was busy."""
        await self._display_refresh()
        t = time.ticks_ms()
        await _wait_not_busy()
        return time.ticks_diff(time.ticks_ms(), t) + 5

    def _fast_lut(self, has_red):
        if self._luts is None:
            try:
                import data.rendered_luts as L
                if self._cog_driver == "B":
                    self._luts = {"base": bytes(L.lutb_base),
                                  "bw": _bw_lut_driver_b(L.lutb_base)}
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
            if not has_red and BW_FAST_LUT:
                return self._luts["bw"]
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
