"""Sketch - a knob-drawing toy for two AS5600 magnetic encoders on SAOs

Left knob moves the pen left/right, right knob moves it up/down (clockwise =
right / up). The trace is drawn into the framebuffer as you turn, and the
e-paper redraws shortly after the knobs stop.

Buttons:
  User A  - erase (redraw the blank frame with a cleaning flash)
  User B  - two knobs: re-run the cleaning flash on the current drawing
            one knob:  switch which axis the knob drives
  Menu +/- - back to the app menu (handled by the app switcher)

Finding the knobs
-----------------
Every AS5600 answers at I2C address 0x36, so two of them on the same bus
collide. The SAO I2C pins are one shared bus, so the app looks in several
places and uses the first two encoders it finds:
  1. the shared SAO I2C bus (GPIO18/17) at 0x36 (AS5600) and 0x40 (AS5600L)
  2. a software I2C bus on each SAO port's GPIO1/GPIO2 pins, either way round
So: plug one AS5600 addon in normally, and wire the other addon's SDA/SCL to
GPIO1/GPIO2 on its SAO header (or use an AS5600L, which sits at 0x40).
With only one knob found, User B flips it between X and Y.
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

PX_PER_REV = 240        # pen travel per full knob turn
LINE_W = 3              # trace thickness in pixels
DEADBAND = 3            # encoder counts of jitter to ignore (of 4096)
IDLE_REFRESH_MS = 700   # refresh this long after the knobs stop...
MAX_WAIT_MS = 4000      # ...or at most this long while still drawing
INVERT_X = False
INVERT_Y = False
SWAP_KNOBS = False      # if the knobs come out backwards

AS5600_ADDRS = (0x36, 0x40)    # AS5600, AS5600L default
_REG_STATUS = 0x0B
_REG_ANGLE = 0x0E

# SAO GPIO pairs (GPIO1, GPIO2), from lib/sao.py
_SAO_GPIO = (("SAO1", 2, 42), ("SAO2", 4, 5), ("SAO3", 7, 6))


class Knob:
    def __init__(self, bus, addr, where):
        self.bus = bus
        self.addr = addr
        self.where = where
        self.last = None
        self.errors = 0

    def __str__(self):
        return f"{self.where}@0x{self.addr:02X}"

    def angle(self):
        d = self.bus.readfrom_mem(self.addr, _REG_ANGLE, 2)
        return ((d[0] & 0x0F) << 8) | d[1]

    def magnet_ok(self):
        try:
            return bool(self.bus.readfrom_mem(self.addr, _REG_STATUS, 1)[0] & 0x20)
        except OSError:
            return False

    def delta(self):
        """Signed counts turned since the last accepted reading."""
        try:
            a = self.angle()
        except OSError:
            self.errors += 1
            return 0
        if self.last is None:
            self.last = a
            return 0
        d = ((a - self.last + 2048) & 4095) - 2048
        if -DEADBAND <= d <= DEADBAND:
            return 0  # don't advance `last`, so slow turns still accumulate
        self.last = a
        return d


def _looks_real(addrs):
    # Floating / stuck lines make every address "answer"
    return 0 < len(addrs) <= 8


def find_knobs():
    knobs = []
    try:
        from lib.sao import i2c as sao_bus
        addrs = sao_bus.scan()
        print(f"Sketch: SAO I2C bus: {[hex(a) for a in addrs]}")
        if _looks_real(addrs):
            for a in AS5600_ADDRS:
                if a in addrs:
                    knobs.append(Knob(sao_bus, a, "SAO bus"))
    except Exception as ex:
        print(f"Sketch: SAO I2C bus unavailable: {ex}")

    for name, g1, g2 in _SAO_GPIO:
        if len(knobs) >= 2:
            break
        for scl, sda in ((g1, g2), (g2, g1)):
            try:
                for p in (scl, sda):
                    machine.Pin(p, machine.Pin.OPEN_DRAIN, pull=machine.Pin.PULL_UP, value=1)
                bus = machine.SoftI2C(scl=machine.Pin(scl), sda=machine.Pin(sda),
                                      freq=100_000, timeout=2000)
                addrs = bus.scan()
            except Exception as ex:
                print(f"Sketch: {name} GPIO scan failed: {ex}")
                continue
            if not _looks_real(addrs):
                continue
            hits = [a for a in AS5600_ADDRS if a in addrs]
            if hits:
                print(f"Sketch: {name} GPIO (SCL={scl}, SDA={sda}): {[hex(a) for a in addrs]}")
                knobs.append(Knob(bus, hits[0], f"{name} gpio"))
                break
    for k in knobs:
        print(f"Sketch: knob {k} magnet {'OK' if k.magnet_ok() else 'NOT DETECTED'}")
    return knobs[:2]


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
        return "no AS5600 found - check SAO wiring"

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
