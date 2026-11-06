"""LED Bar for Billboard Badge (8x WS2812 on GPIO38)

Quick start (from an app):

    from lib.led_bar import leds

    leds.fill("green")              # all LEDs, writes immediately
    leds.show(["red", "off", (0, 0, 255), 0xFF8800])   # per-LED, writes
    leds[3] = "blue"; leds.write()  # buffered, then write
    leds.off()

    leds.blink("red", times=3)      # built-in effects run in the background
    leds.pulse("cyan")
    leds.chase("orange")
    leds.rainbow()
    leds.progress(0.6, "green")     # bar graph, 0.0 - 1.0
    leds.bits(0b10110010)           # show a byte
    leds.stop()                     # stop any effect

    leds.brightness = 40            # 0-255 global brightness

Colors can be a name (see COLORS), 0xRRGGBB, or an (r, g, b) tuple 0-255.
Every color is scaled by `brightness`; a channel that is non-zero never
scales below 1, so older code using tiny values like (0, 1, 1) still works.

Custom animations: leds.set_sequence(fn) where fn is a generator function
yielding (list_of_8_colors, delay_ms). See set_sequence() below.
"""

import asyncio as aio
import sys
import time

if sys.platform == "esp32":
    import machine
    import neopixel
else:
    machine = None
    neopixel = None

NUM_LEDS = 8
WS2812_PIN = 38

DEFAULT_BRIGHTNESS = 24     # out of 255. The LEDs are very bright.
_REFRESH_MS = 500           # re-send the frame this often, heals any glitched LED
_TICK_MS = 5

COLORS = {
    "off": (0, 0, 0), "black": (0, 0, 0),
    "white": (255, 200, 150),   # warm white looks more neutral on WS2812
    "red": (255, 0, 0), "green": (0, 255, 0), "blue": (0, 0, 255),
    "yellow": (255, 160, 0), "orange": (255, 70, 0), "cyan": (0, 255, 255),
    "magenta": (255, 0, 255), "purple": (140, 0, 255), "pink": (255, 40, 90),
}

## 64 Apps should be enough for anyone (TM)
MENU_BACKGROUND_COLORS = ["orange", "cyan", "magenta", "red", "green", "blue", "purple", "yellow"]
MENU_SELECTED_COLOR = "white"
SUBMENU_SELECTED_COLORS = MENU_BACKGROUND_COLORS


def to_rgb(c):
    """Name / 0xRRGGBB / (r, g, b) -> (r, g, b) tuple."""
    if c is None:
        return (0, 0, 0)
    if isinstance(c, str):
        return COLORS[c.lower()]
    if isinstance(c, int):
        return ((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF)
    return (int(c[0]), int(c[1]), int(c[2]))


def wheel(pos):
    """0-255 -> rainbow color."""
    pos &= 0xFF
    if pos < 85:
        return (255 - pos * 3, pos * 3, 0)
    if pos < 170:
        pos -= 85
        return (0, 255 - pos * 3, pos * 3)
    pos -= 170
    return (pos * 3, 0, 255 - pos * 3)


def _ticks():
    return time.ticks_ms() if hasattr(time, "ticks_ms") else int(time.time() * 1000)


def _diff(a, b):
    return time.ticks_diff(a, b) if hasattr(time, "ticks_diff") else a - b


def _add(a, b):
    return time.ticks_add(a, b) if hasattr(time, "ticks_add") else a + b


class _FakePixels:
    """Stand-in for neopixel.NeoPixel when running under CPython."""

    def __init__(self, n):
        self.buf = [(0, 0, 0)] * n

    def __setitem__(self, i, v):
        self.buf[i] = v

    def __getitem__(self, i):
        return self.buf[i]

    def write(self):
        pass


class LedBar:
    def __init__(self, pin=WS2812_PIN, n=NUM_LEDS) -> None:
        print("Starting LED manager")
        if neopixel:
            self.np = neopixel.NeoPixel(machine.Pin(pin, machine.Pin.OUT), n, bpp=3)
        else:
            self.np = _FakePixels(n)
        self.n = n
        self.brightness = DEFAULT_BRIGHTNESS
        self._buf = [(0, 0, 0)] * n     # what the user asked for, unscaled
        self._last_write_ms = 0

        # LED pattern sequencer
        self.sequence_func = None
        self.sequence_paused = False
        self._sequence_updated = aio.Event()

    # ---------------- basic access ----------------
    @property
    def leds(self):
        # Backwards compatibility: old code reads leds.leds[i]
        return self._buf

    def __len__(self):
        return self.n

    def __setitem__(self, idx, value) -> None:
        self._buf[int(idx)] = to_rgb(value)

    def __getitem__(self, idx):
        return self._buf[int(idx)]

    def __str__(self) -> str:
        return ":".join(f"{l[0]}.{l[1]}.{l[2]}" for l in self._buf)

    def _scale(self, c):
        b = self.brightness
        out = []
        for v in c:
            if v <= 0:
                out.append(0)
            else:
                s = (min(v, 255) * b + 127) // 255
                out.append(s if s > 0 else 1)
        return (out[0], out[1], out[2])

    def write(self):
        """Send the buffered colors to the LEDs."""
        for i in range(self.n):
            self.np[i] = self._scale(self._buf[i])
        self.np.write()
        self._last_write_ms = _ticks()
        return list(self._buf)

    # ---------------- simple setters (write immediately) ----------------
    def set(self, values):
        """Buffer a list of colors (does not write; call write())."""
        for i, c in enumerate(values):
            if i >= self.n:
                break
            self._buf[i] = to_rgb(c)

    def show(self, values):
        """Set a list of colors and write them."""
        self.set(values)
        self.write()

    def fill(self, color):
        """Set every LED to one color and write."""
        c = to_rgb(color)
        self._buf = [c] * self.n
        self.write()

    def clear(self):
        self.fill((0, 0, 0))

    def off(self):
        self.fill((0, 0, 0))

    def set_pixel(self, i, color, write=True):
        self._buf[i % self.n] = to_rgb(color)
        if write:
            self.write()

    # ---------------- sequencer ----------------
    def set_sequence(self, fn):
        """Run an animation in the background.

        `fn` is a generator function (or a generator) that yields
        (list of colors, delay in ms) forever or until it returns.

        def example_sequence():
            i = 0
            while True:
                yield [wheel(i + n * 32) for n in range(8)], 50
                i += 4

        leds.set_sequence(example_sequence)
        """
        self.sequence_func = fn
        self.sequence_paused = False
        self._sequence_updated.set()

    def clear_sequence(self):
        self.sequence_func = None
        self.sequence_paused = False
        self._sequence_updated.set()

    def stop(self):
        self.clear_sequence()

    def pause(self):
        self.sequence_paused = True

    def unpause(self):
        self.sequence_paused = False

    def toggle_pause(self):
        self.sequence_paused = not self.sequence_paused

    async def update(self):
        """Background task: runs sequences and periodically re-sends the frame."""
        gen = None
        next_ms = _ticks()
        while True:
            if self._sequence_updated.is_set():
                self._sequence_updated.clear()
                fn = self.sequence_func
                if fn is None:
                    gen = None
                else:
                    gen = fn() if callable(fn) else fn
                next_ms = _ticks()

            now = _ticks()
            if gen is not None and not self.sequence_paused and _diff(now, next_ms) >= 0:
                try:
                    vals, delay = next(gen)
                    self.set(vals)
                    self.write()
                    next_ms = _add(now, max(1, int(delay)))
                except StopIteration:
                    gen = None
                    self.sequence_func = None
                except Exception as ex:
                    print("LED sequence crashed:")
                    if hasattr(sys, "print_exception"):
                        sys.print_exception(ex)
                    gen = None
                    self.sequence_func = None
            elif _diff(now, self._last_write_ms) > _REFRESH_MS:
                self.write()
            await aio.sleep_ms(_TICK_MS)

    # ---------------- built-in effects ----------------
    def blink(self, color="white", times=3, on_ms=150, off_ms=150):
        c = to_rgb(color)
        n = self.n

        def seq():
            for _ in range(times):
                yield [c] * n, on_ms
                yield [(0, 0, 0)] * n, off_ms
        self.set_sequence(seq)

    def pulse(self, color="cyan", period_ms=1500, steps=30):
        r, g, b = to_rgb(color)
        n = self.n
        dt = max(10, period_ms // (2 * steps))

        def seq():
            while True:
                for k in list(range(steps)) + list(range(steps, 0, -1)):
                    f = k / steps
                    f = f * f  # perceptual-ish
                    yield [(int(r * f), int(g * f), int(b * f))] * n, dt
        self.set_sequence(seq)

    def chase(self, color="orange", ms=80, tail=3):
        r, g, b = to_rgb(color)
        n = self.n

        def seq():
            i = 0
            while True:
                vals = [(0, 0, 0)] * n
                for t in range(tail):
                    f = (tail - t) / tail
                    vals[(i - t) % n] = (int(r * f), int(g * f), int(b * f))
                yield vals, ms
                i += 1
        self.set_sequence(seq)

    def rainbow(self, ms=40):
        n = self.n

        def seq():
            i = 0
            while True:
                yield [wheel(i + k * 256 // n) for k in range(n)], ms
                i = (i + 3) & 0xFF
        self.set_sequence(seq)

    def progress(self, fraction, color="green", background="off"):
        """Bar graph, 0.0-1.0. Stops any running effect."""
        self.clear_sequence()
        lit = int(max(0.0, min(1.0, fraction)) * self.n + 0.5)
        self.show([color if i < lit else background for i in range(self.n)])

    def bits(self, x, color="white"):
        """Show an 8 bit number, MSB on LED 0. Stops any running effect."""
        self.clear_sequence()
        self.show(bitmask_to_leds(x, color=color))

    # ---------------- menu helpers (used by app_switcher) ----------------
    def menu_display(self, app_index, total_number_apps):
        """Light one LED per app on the current page, selected app in white."""
        self.pause()
        page = app_index // self.n
        sel = app_index % self.n
        bg = MENU_BACKGROUND_COLORS[page % len(MENU_BACKGROUND_COLORS)]
        on_page = min(self.n, total_number_apps - page * self.n)
        vals = [bg if i < on_page else "off" for i in range(self.n)]
        vals[sel] = MENU_SELECTED_COLOR
        self.show(vals)

    def submenu_display(self, item_index):
        """Single LED in submenus, color by page."""
        self.pause()
        vals = ["off"] * self.n
        vals[item_index % self.n] = SUBMENU_SELECTED_COLORS[
            (item_index // self.n) % len(SUBMENU_SELECTED_COLORS)]
        self.show(vals)

    # ---------------- color math (kept from the original API) ----------------
    def _combine(self, other, op):
        vals = []
        for i, o in enumerate(other):
            a = self._buf[i]
            o = to_rgb(o)
            vals.append([op(a[k], o[k]) for k in range(3)])
        return vals

    def __or__(self, other):
        return self._combine(other, lambda a, b: min(max(a, b), 255))

    def __ror__(self, other):
        return self.__or__(other)

    def __and__(self, other):
        return self._combine(other, lambda a, b: max(min(a, b), 0))

    def __rand__(self, other):
        return self.__and__(other)

    def __add__(self, other):
        return self._combine(other, lambda a, b: min(a + b, 255))

    def __radd__(self, other):
        return self.__add__(other)

    def __iadd__(self, other):
        for i, v in enumerate(self + other):
            self._buf[i] = tuple(v)
        return self


leds = LedBar()


def bitmask_to_leds(x: int, r=3, g=3, b=3, color=None):
    """Convert an 8 bit int into a list of LED colors, MSB first.
       Pass color= (name / tuple), or r/g/b for the old behavior.
    """
    on = to_rgb(color) if color is not None else (r, g, b)
    vals = []
    for i in range(NUM_LEDS):
        v = (x >> (NUM_LEDS - 1 - i)) & 0x1
        vals.append(on if v else (0, 0, 0))
    return vals
