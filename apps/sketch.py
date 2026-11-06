"""Sketch - a knob-drawing toy with two AS5600 magnetic encoder SAOs

Left knob moves the pen left/right, right knob moves it up/down (clockwise =
right / up). The trace is drawn into the framebuffer as you turn, and the
e-paper redraws shortly after the knobs stop.

Buttons:
  User A  - erase (redraw the blank frame with a cleaning flash)
  User B  - two knobs: re-run the cleaning flash on the current drawing
            one knob:  switch which axis the knob drives
  Menu +/- - back to the app menu (handled by the app switcher)

Hardware
--------
Each knob is an AS5600 SAO whose analog OUT pin is wired to the SAO's GPIO1:
  SAO1 GPIO1 = GPIO2  -> X knob
  SAO3 GPIO1 = GPIO7  -> Y knob
Both are ADC1 pins. On launch the app also writes the AS5600 CONF register over
the shared SAO I2C bus (both chips sit at 0x36 and take the same write) to set
the analog output to 10-90 % of VDD, which keeps it inside the ESP32-S3 ADC's
~3.1 V range. If that write fails, full-range output (0-100 %) is assumed.
"""

import asyncio as aio
import machine
import time

from apps.base_app import BaseApp
from lib.button import UserA, UserB
from lib.epd import display
from lib.led_bar import leds

FRAME_PNG = "/data/graphics/sketch_frame.png"

# Drawable area inside the frame's white screen (inclusive)
X_MIN, Y_MIN, X_MAX, Y_MAX = 80, 68, 880, 522

X_KNOB_PIN = 2          # SAO1 GPIO1
Y_KNOB_PIN = 7          # SAO3 GPIO1
SWAP_KNOBS = False      # if the knobs come out backwards
INVERT_X = False
INVERT_Y = False

PX_PER_REV = 240        # pen travel per full knob turn
LINE_W = 3              # trace thickness in pixels
DEADBAND = 12           # counts (of 4096) of ADC noise to ignore
ADC_SAMPLES = 8         # readings averaged per poll
IDLE_REFRESH_MS = 700   # refresh this long after the knobs stop...
MAX_WAIT_MS = 4000      # ...or at most this long while still drawing

AS5600_ADDR = 0x36
_VDD_UV = 3_300_000


def set_reduced_analog_range():
    """CONF.OUTS = 01 (analog 10-90 % of VDD) on every AS5600 on the SAO bus.
    Volatile (not burned), so it's redone each launch. Returns True if ACKed."""
    try:
        from lib.sao import i2c
        # CONF is 0x07 (high byte) / 0x08 (low byte). Low byte: OUTS bits 5:4.
        # Everything else at defaults (no hysteresis, slow filter, normal power).
        i2c.writeto_mem(AS5600_ADDR, 0x07, bytes([0x00, 0x10]))
        print("Sketch: AS5600 analog output set to 10-90 % of VDD")
        return True
    except Exception as ex:
        print(f"Sketch: couldn't configure AS5600 over I2C ({ex}); assuming 0-100 % output")
        return False


class AnalogKnob:
    """AS5600 OUT pin read with the ESP32-S3 ADC."""

    def __init__(self, pin, label, reduced_range):
        self.label = label
        self.adc = machine.ADC(machine.Pin(pin), atten=machine.ADC.ATTN_11DB)
        if reduced_range:
            self.lo, self.hi = _VDD_UV * 10 // 100, _VDD_UV * 90 // 100
        else:
            self.lo, self.hi = 0, _VDD_UV
        self.last = None

    def __str__(self):
        return self.label

    def angle(self):
        uv = 0
        for _ in range(ADC_SAMPLES):
            uv += self.adc.read_uv()
        uv //= ADC_SAMPLES
        a = (uv - self.lo) * 4096 // (self.hi - self.lo)
        return min(max(a, 0), 4095)

    def volts(self):
        return self.adc.read_uv() / 1_000_000

    def delta(self):
        """Signed counts turned since the last accepted reading."""
        a = self.angle()
        if self.last is None:
            self.last = a
            return 0
        d = ((a - self.last + 2048) & 4095) - 2048
        if -DEADBAND <= d <= DEADBAND:
            return 0  # don't advance `last`, so slow turns still accumulate
        self.last = a
        return d


def find_knobs():
    reduced = set_reduced_analog_range()
    knobs = []
    for pin, label in ((X_KNOB_PIN, "SAO1"), (Y_KNOB_PIN, "SAO3")):
        try:
            k = AnalogKnob(pin, label, reduced)
            print(f"Sketch: knob {label} on GPIO{pin}: {k.volts():.2f} V")
            knobs.append(k)
        except Exception as ex:
            print(f"Sketch: knob {label} on GPIO{pin} failed: {ex}")
    return knobs


def big_text(fb, text, x, y, color, scale=2):
    """framebuf only has an 8x8 font; draw it scaled up."""
    import framebuf
    w = 8 * len(text)
    buf = bytearray((w * 8 + 7) // 8)
    tmp = framebuf.FrameBuffer(buf, w, 8, framebuf.MONO_HLSB)
    tmp.text(text, 0, 0, 1)
    for ty in range(8):
        for tx in range(w):
            if tmp.pixel(tx, ty):
                fb.fill_rect(x + tx * scale, y + ty * scale, scale, scale, color)


class SketchApp(BaseApp):
    def __init__(self, name=None) -> None:
        self.name = "Sketch"
        self.sleep_ms = 15
        self.launched = False
        self.knobs = None
        self.x_knob = None
        self.y_knob = None
        self.single_axis = "x"
        self.px = (X_MIN + X_MAX) // 2
        self.py = (Y_MIN + Y_MAX) // 2
        self._fx = 0.0
        self._fy = 0.0
        self._dirty = False
        self._last_move = 0
        self._first_change = 0

    # ---------------- setup ----------------
    def _setup_knobs(self):
        if self.knobs:  # keep what we found, unless nothing was found
            return
        self.knobs = find_knobs()
        ks = list(reversed(self.knobs)) if SWAP_KNOBS else self.knobs
        self.x_knob = ks[0] if len(ks) > 0 else None
        self.y_knob = ks[1] if len(ks) > 1 else None

    def _status_text(self):
        if self.x_knob and self.y_knob:
            return None
        if self.x_knob:
            return f"1 knob ({self.x_knob}) - B switches X/Y"
        return "no knobs - check SAO1/SAO3"

    def _blank(self, flash=True):
        """Draw the empty frame and push it with a cleaning flash."""
        leds.fill("cyan")
        display.clear()
        display.draw_png(FRAME_PNG)
        msg = self._status_text()
        if msg:
            # White text on the red frame, under the screen
            big_text(display.red, msg, 200, 552, 0)
            big_text(display.black, msg, 200, 552, 0)
        self._pen_dot()
        display.update(fast=True, flash=flash)
        self._dirty = False

    def _pen_dot(self):
        r = LINE_W // 2
        display.black.fill_rect(self.px - r, self.py - r, LINE_W, LINE_W, 1)
        display.red.fill_rect(self.px - r, self.py - r, LINE_W, LINE_W, 0)

    # ---------------- drawing ----------------
    def _line(self, x0, y0, x1, y1):
        lo = -(LINE_W // 2)
        hi = lo + LINE_W
        for ox in range(lo, hi):
            for oy in range(lo, hi):
                display.black.line(x0 + ox, y0 + oy, x1 + ox, y1 + oy, 1)
                display.red.line(x0 + ox, y0 + oy, x1 + ox, y1 + oy, 0)

    def _poll_knobs(self):
        scale = PX_PER_REV / 4096
        dx = dy = 0.0
        if self.x_knob and self.y_knob:
            dx = self.x_knob.delta() * scale
            dy = -self.y_knob.delta() * scale     # clockwise = up
        elif self.x_knob:
            d = self.x_knob.delta() * scale
            if self.single_axis == "x":
                dx = d
            else:
                dy = -d
        if INVERT_X:
            dx = -dx
        if INVERT_Y:
            dy = -dy
        if not dx and not dy:
            return

        self._fx += dx
        self._fy += dy
        mx = int(self._fx)
        my = int(self._fy)
        if not mx and not my:
            return
        self._fx -= mx
        self._fy -= my
        nx = min(max(self.px + mx, X_MIN), X_MAX)
        ny = min(max(self.py + my, Y_MIN), Y_MAX)
        if nx == self.px and ny == self.py:
            return
        self._line(self.px, self.py, nx, ny)
        self.px, self.py = nx, ny
        now = time.ticks_ms()
        if not self._dirty:
            self._first_change = now
        self._dirty = True
        self._last_move = now

    def _maybe_refresh(self):
        if not self._dirty or not display.is_done_updating():
            return
        now = time.ticks_ms()
        idle = time.ticks_diff(now, self._last_move) >= IDLE_REFRESH_MS
        overdue = time.ticks_diff(now, self._first_change) >= MAX_WAIT_MS
        if idle or overdue:
            # Quick redraw: one pass of the short black/white waveform, no
            # flash, and don't trigger the 35 s periodic full refresh mid-drawing.
            display.update(fast=True, flash=False, passes=1, bw=True, auto_full=False)
            self._dirty = False

    def _leds(self):
        if not display.is_done_updating():
            leds.fill("cyan")       # panel busy
        elif self._dirty:
            leds.fill("yellow")     # drawn, waiting to refresh
        else:
            leds.fill("green")      # up to date

    # ---------------- app loop ----------------
    async def arun(self):
        try:
            if self.launched:
                self._setup_knobs()
                self._blank(flash=True)
                self._led_state = None

            if UserA.pressed():
                print("Sketch: erase")
                self._blank(flash=True)
            if UserB.pressed():
                if self.x_knob and not self.y_knob:
                    self.single_axis = "y" if self.single_axis == "x" else "x"
                    print(f"Sketch: knob now drives {self.single_axis.upper()}")
                    leds.blink("white", times=1 if self.single_axis == "x" else 2)
                else:
                    print("Sketch: clean redraw")
                    display.clean()
                    self._dirty = False

            self._poll_knobs()
            self._maybe_refresh()

            state = (display.is_done_updating(), self._dirty)
            if state != getattr(self, "_led_state", None) and leds.sequence_func is None:
                self._led_state = state
                self._leds()
        except Exception as ex:
            import sys
            print("Sketch hit an exception")
            sys.print_exception(ex)
        await aio.sleep_ms(self.sleep_ms)
