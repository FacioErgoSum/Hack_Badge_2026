"""Sketch - a knob-drawing toy with two AS5600 magnetic encoder SAOs

Left knob moves the pen left/right, right knob moves it up/down (clockwise =
right / up). The trace is drawn into the framebuffer as you turn, and the
e-paper redraws shortly after the knobs stop. The pen stops at the edges of
the white screen.

LEDs: 0-3 animate the X knob, 4-7 the Y knob while turning (a dot spins the
way you turn; that side flashes red when the pen hits a wall). When idle they
show status: green = up to date, yellow = waiting to redraw, cyan = panel busy.

Shake to erase: an MPU6050 on the SAO I2C bus (0x68 / 0x69). It takes
SHAKE_PEAKS hard jolts (> SHAKE_G) within SHAKE_WINDOW_MS, so bumps don't
wipe the drawing.

While drawing, the panel uses display.update_partial(): only a window around
the newly drawn pixels is refreshed, with a black/white-only waveform, so the
red frame isn't touched.

Buttons:
  User A  - erase (redraw the blank frame with a cleaning flash)
  User B  - two knobs: re-run the cleaning flash on the current drawing
            one knob:  switch which axis the knob drives
  Menu +/- - back to the app menu (handled by the app switcher)

Hardware
--------
Each knob is an AS5600 SAO whose analog OUT pin is wired to the SAO's GPIO1.
The app probes every SAO GPIO1 pin (pull-up vs pull-down: a floating pin
swings, a driven AS5600 output doesn't) and uses the first two it finds as
X then Y. Set X_KNOB_PIN / Y_KNOB_PIN to force specific pins.

On launch the AS5600 CONF register is written over the shared SAO I2C bus
(both chips sit at 0x36 and take the same write) to set the analog output to
10-90 % of VDD, inside the ESP32-S3 ADC's ~3.1 V range. If that write fails,
full-range output (0-100 %) is assumed.
"""

import asyncio as aio
import machine
import time

from apps.base_app import BaseApp
from lib.button import UserA, UserB
from lib.epd import display
from lib.led_bar import leds

FRAME_PNG = "/data/gallery/EtchASketch.png"   # 960x670, palette PNG

LINE_W = 3              # trace thickness in pixels
# Largest all-white box in EtchASketch.png's screen (clear of the rounded
# corners) is x 132..826, y 121..524. Keep the whole 3 px line inside it.
_HALF = LINE_W // 2
X_MIN, Y_MIN, X_MAX, Y_MAX = 132 + _HALF + 1, 121 + _HALF + 1, 826 - _HALF - 1, 524 - _HALF - 1

# SAO GPIO1 pins (from lib/sao.py), probed in this order
CANDIDATE_PINS = ((2, "SAO1"), (4, "SAO2"), (7, "SAO3"))
X_KNOB_PIN = None       # None = auto-detect; or e.g. 2
Y_KNOB_PIN = None       # None = auto-detect; or e.g. 7
SWAP_KNOBS = False      # if the knobs come out backwards
INVERT_X = False
INVERT_Y = False

PX_PER_REV = 240        # pen travel per full knob turn
DEADBAND = 12           # counts (of 4096) of ADC noise to ignore
ADC_SAMPLES = 8         # readings averaged per poll
IDLE_REFRESH_MS = 700   # refresh this long after the knobs stop...
MAX_WAIT_MS = 4000      # ...or at most this long while still drawing
DEBUG_KNOBS = True      # print knob voltages/angles while turning

LED_TURN_HOLD_MS = 400  # keep the turning animation this long after a turn
LED_DOT_STEPS = 16      # dot steps per knob revolution
X_COLOR = "cyan"
Y_COLOR = "magenta"
WALL_COLOR = "red"

SHAKE_G = 2.2           # a jolt is a reading this far from 1 g (in g)
SHAKE_PEAKS = 4         # this many jolts...
SHAKE_WINDOW_MS = 1200  # ...within this long = erase
SHAKE_COOLDOWN_MS = 3000

AS5600_ADDR = 0x36
_VDD_UV = 3_300_000


def set_reduced_analog_range():
    """CONF.OUTS = 01 (analog 10-90 % of VDD) on every AS5600 on the SAO bus.
    Volatile (not burned), so it's redone each launch. Returns True if ACKed."""
    try:
        from lib.sao import i2c
        # CONF is 0x07 (high byte) / 0x08 (low byte). Low byte: OUTS bits 5:4.
        i2c.writeto_mem(AS5600_ADDR, 0x07, bytes([0x00, 0x10]))
        print("Sketch: AS5600 analog output set to 10-90 % of VDD")
        return True
    except Exception as ex:
        print(f"Sketch: couldn't configure AS5600 over I2C ({ex}); assuming 0-100 % output")
        return False


def _avg_uv(adc, n=8):
    t = 0
    for _ in range(n):
        t += adc.read_uv()
    return t // n


def probe_pin(pin):
    """Return (driven, volts). A floating pin follows the internal pull,
    a pin driven by the AS5600 output barely moves."""
    P = machine.Pin
    adc = machine.ADC(P(pin), atten=machine.ADC.ATTN_11DB)
    P(pin, P.IN, P.PULL_DOWN)
    time.sleep_ms(3)
    lo = _avg_uv(adc)
    P(pin, P.IN, P.PULL_UP)
    time.sleep_ms(3)
    hi = _avg_uv(adc)
    P(pin, P.IN, None)
    time.sleep_ms(3)
    mid = _avg_uv(machine.ADC(P(pin), atten=machine.ADC.ATTN_11DB))
    driven = abs(hi - lo) < 300_000
    print(f"Sketch: GPIO{pin}: pulled down {lo / 1e6:.2f} V, up {hi / 1e6:.2f} V, "
          f"free {mid / 1e6:.2f} V -> {'AS5600' if driven else 'floating'}")
    return driven


class AnalogKnob:
    """AS5600 OUT pin read with the ESP32-S3 ADC."""

    def __init__(self, pin, label, reduced_range):
        self.pin = pin
        self.label = label
        machine.Pin(pin, machine.Pin.IN, None)
        self.adc = machine.ADC(machine.Pin(pin), atten=machine.ADC.ATTN_11DB)
        if reduced_range:
            self.lo, self.hi = _VDD_UV * 10 // 100, _VDD_UV * 90 // 100
        else:
            self.lo, self.hi = 0, _VDD_UV
        self.last = None
        self.uv = 0
        self.raw_angle = 0

    def __str__(self):
        return f"{self.label}/GPIO{self.pin}"

    def angle(self):
        uv = _avg_uv(self.adc, ADC_SAMPLES)
        self.uv = uv
        a = (uv - self.lo) * 4096 // (self.hi - self.lo)
        self.raw_angle = min(max(a, 0), 4095)
        return self.raw_angle

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
    labels = {p: l for p, l in CANDIDATE_PINS}
    if X_KNOB_PIN is not None or Y_KNOB_PIN is not None:
        pins = [p for p in (X_KNOB_PIN, Y_KNOB_PIN) if p is not None]
    else:
        pins = [p for p, _ in CANDIDATE_PINS if probe_pin(p)]
        if not pins:
            print("Sketch: no driven SAO GPIO1 pins found, trying SAO1 + SAO3 anyway")
            pins = [2, 7]
    knobs = []
    for pin in pins[:2]:
        try:
            k = AnalogKnob(pin, labels.get(pin, "SAO?"), reduced)
            print(f"Sketch: knob {k}: {k.volts():.2f} V")
            knobs.append(k)
        except Exception as ex:
            print(f"Sketch: knob on GPIO{pin} failed: {ex}")
    return knobs


class Shake:
    """MPU6050 shake detector on the SAO I2C bus."""

    def __init__(self):
        self.i2c = None
        self.addr = None
        self._peaks = []
        self._in_peak = False
        self._cooldown_until = 0
        try:
            from lib.sao import i2c
            found = i2c.scan()
            for a in (0x68, 0x69):
                if a in found and i2c.readfrom_mem(a, 0x75, 1)[0] in (0x68, 0x70, 0x71, 0x72, 0x73, 0x98):
                    self.i2c, self.addr = i2c, a
                    break
            if self.addr is None:
                print("Sketch: no MPU6050 found, shake-to-erase off (I2C: "
                      f"{[hex(a) for a in found]})")
                return
            i2c.writeto_mem(self.addr, 0x6B, b"\x00")   # wake up
            i2c.writeto_mem(self.addr, 0x1C, b"\x10")   # accel +-8 g
            i2c.writeto_mem(self.addr, 0x1A, b"\x03")   # DLPF ~44 Hz
            print(f"Sketch: MPU6050 at 0x{self.addr:02X}, shake to erase")
        except Exception as ex:
            print(f"Sketch: MPU6050 setup failed: {ex}")
            self.addr = None

    def accel_g(self):
        d = self.i2c.readfrom_mem(self.addr, 0x3B, 6)
        vals = []
        for i in (0, 2, 4):
            v = (d[i] << 8) | d[i + 1]
            if v & 0x8000:
                v -= 0x10000
            vals.append(v / 4096)   # 4096 LSB/g at +-8 g
        return vals

    def poll(self):
        """Returns True once when a hard shake is detected."""
        if self.addr is None:
            return False
        now = time.ticks_ms()
        if time.ticks_diff(self._cooldown_until, now) > 0:
            return False
        try:
            x, y, z = self.accel_g()
        except OSError:
            return False
        jolt = abs((x * x + y * y + z * z) ** 0.5 - 1.0)
        if jolt > SHAKE_G and not self._in_peak:
            self._in_peak = True
            self._peaks.append(now)
        elif jolt < SHAKE_G * 0.5:
            self._in_peak = False
        self._peaks = [t for t in self._peaks if time.ticks_diff(now, t) < SHAKE_WINDOW_MS]
        if len(self._peaks) >= SHAKE_PEAKS:
            self._peaks = []
            self._cooldown_until = time.ticks_add(now, SHAKE_COOLDOWN_MS)
            return True
        return False


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
        # LED animation state
        self._phase = [0.0, 0.0]          # X, Y dot phase (in LED steps)
        self._turn_ms = [None, None]      # last time each knob turned
        self._wall_ms = [None, None]      # last time each axis hit a wall
        self._led_frame = None
        self._dbg_ms = 0
        self.shake = None

    # ---------------- setup ----------------
    def _setup_knobs(self):
        if self.knobs:  # keep what we found, unless nothing was found
            return
        self.knobs = find_knobs()
        ks = list(reversed(self.knobs)) if SWAP_KNOBS else self.knobs
        self.x_knob = ks[0] if len(ks) > 0 else None
        self.y_knob = ks[1] if len(ks) > 1 else None
        print(f"Sketch: X = {self.x_knob}, Y = {self.y_knob}")

    def _status_text(self):
        if self.x_knob and self.y_knob:
            return None
        if self.x_knob:
            return f"1 knob ({self.x_knob}) - B switches X/Y"
        return "no knobs - check SAO addons"

    def _blank(self, flash=True):
        """Draw the empty frame and push it with a cleaning flash."""
        leds.fill("cyan")
        display.clear()
        display.red.fill_rect(0, 668, 960, 4, 1)   # image is 2 rows short
        display.draw_png(FRAME_PNG)
        msg = self._status_text()
        if msg:
            # White text on the red frame, under the screen
            big_text(display.red, msg, 200, 560, 0)
            big_text(display.black, msg, 200, 560, 0)
        self._pen_dot()
        display.update(fast=True, flash=flash)
        self._dirty = False
        self._led_frame = None

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

    def _read_deltas(self):
        """Knob turns in counts as (x_counts, y_counts), axis-mapped."""
        cx = cy = 0
        if self.x_knob and self.y_knob:
            cx = self.x_knob.delta()
            cy = self.y_knob.delta()
        elif self.x_knob:
            d = self.x_knob.delta()
            if self.single_axis == "x":
                cx = d
            else:
                cy = d
        if INVERT_X:
            cx = -cx
        if INVERT_Y:
            cy = -cy
        return cx, cy

    def _poll_knobs(self):
        cx, cy = self._read_deltas()
        if not cx and not cy:
            return
        now = time.ticks_ms()
        steps_per_count = LED_DOT_STEPS / 4096
        if cx:
            self._turn_ms[0] = now
            self._phase[0] += cx * steps_per_count
        if cy:
            self._turn_ms[1] = now
            self._phase[1] += cy * steps_per_count

        if DEBUG_KNOBS and time.ticks_diff(now, self._dbg_ms) > 250:
            self._dbg_ms = now
            parts = []
            for name, k in (("X", self.x_knob), ("Y", self.y_knob)):
                if k:
                    parts.append(f"{name} {k.uv / 1e6:.2f}V a={k.raw_angle}")
            print("Sketch: " + " | ".join(parts) + f" pen=({self.px},{self.py})")

        scale = PX_PER_REV / 4096
        self._fx += cx * scale
        self._fy -= cy * scale          # clockwise = up
        mx = int(self._fx)
        my = int(self._fy)
        if not mx and not my:
            return
        self._fx -= mx
        self._fy -= my

        tx, ty = self.px + mx, self.py + my
        nx = min(max(tx, X_MIN), X_MAX)
        ny = min(max(ty, Y_MIN), Y_MAX)
        if nx != tx:
            self._wall_ms[0] = now
            self._fx = 0.0          # don't bank turns against the wall
        if ny != ty:
            self._wall_ms[1] = now
            self._fy = 0.0
        if nx == self.px and ny == self.py:
            return
        self._line(self.px, self.py, nx, ny)
        self.px, self.py = nx, ny
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
            # Partial: only the newly drawn black pixels get driven
            display.update_partial()
            self._dirty = False

    # ---------------- LEDs ----------------
    def _recent(self, t, now, ms=LED_TURN_HOLD_MS):
        return t is not None and time.ticks_diff(now, t) < ms

    def _knob_leds(self, axis, now):
        """4 LEDs for one knob: a spinning dot, or red at a wall, or None if idle."""
        if self._recent(self._wall_ms[axis], now, 250):
            return [WALL_COLOR] * 4
        if not self._recent(self._turn_ms[axis], now):
            return None
        color = X_COLOR if axis == 0 else Y_COLOR
        pos = int(self._phase[axis]) % 4
        vals = ["off"] * 4
        vals[pos] = color
        return vals

    def _status_color(self):
        if not display.is_done_updating():
            return "cyan"       # panel busy
        if self._dirty:
            return "yellow"     # drawn, waiting to refresh
        return "green"          # up to date

    def _update_leds(self):
        if leds.sequence_func is not None:
            self._led_frame = None  # redraw once the blink etc. finishes
            return
        now = time.ticks_ms()
        left = self._knob_leds(0, now)
        right = self._knob_leds(1, now)
        if left is None and right is None:
            frame = [self._status_color()] * 8
        else:
            dim = "off"
            frame = (left or [dim] * 4) + (right or [dim] * 4)
        frame = tuple(frame)
        if frame != self._led_frame:
            self._led_frame = frame
            leds.show(frame)

    # ---------------- app loop ----------------
    async def arun(self):
        try:
            if self.launched:
                self._setup_knobs()
                if self.shake is None:
                    self.shake = Shake()
                self._blank(flash=True)

            if self.shake is not None and self.shake.poll():
                print("Sketch: shake! erasing")
                leds.blink("white", times=3, on_ms=80, off_ms=80)
                self._blank(flash=True)

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
            self._update_leds()
        except Exception as ex:
            import sys
            print("Sketch hit an exception")
            sys.print_exception(ex)
        await aio.sleep_ms(self.sleep_ms)
