"""Button interface for the Billboard Badge

    from lib.button import UserA
    if UserA.pressed():      # True once per press
        ...
    UserA.is_down()          # live state
    UserA.held_ms()          # how long it has been held (0 if up)

Buttons are polled every 5 ms and a level has to be stable for
STABLE_SAMPLES polls before it counts, so glitches (e.g. coupled from the
LED data line on GPIO38 next to User A / Menu -) are ignored. A press is
kept for UNREAD_TTL_MS after release (so quick taps aren't lost), then
forgotten, so a press an app never read can't leak into the menu later.
"""

import machine
import asyncio as aio
import time

## Note: plus/minus mislabeled on the silkscreen!
MENU_MINUS_PIN = 40  # SW5  BUTTON_2  IO40
MENU_PLUS_PIN = 15  # SW6  BUTTON_3  IO15
USER_A_PIN = 39  # SW4  BUTTON_1  IO39
USER_B_PIN = 16  # SW7  BUTTON_4  IO16
BOOT_PIN = 0

UNPRESSED = 0
PRESSED_UNREAD = 1
PRESSED_READ = 2

SCAN_MS = 5
STABLE_SAMPLES = 3   # 3 x 5 ms = level must hold 10-15 ms to count
UNREAD_TTL_MS = 300  # a released press nobody read is dropped after this


class Button:
    def __init__(self, pin: int, name: str):
        self.pin = machine.Signal(machine.Pin(pin, machine.Pin.IN, machine.Pin.PULL_UP), invert=True)
        self.name = name
        self.value = False
        self._pressed_state = UNPRESSED
        self._press_onset_ms = None
        self._raw = False
        self._stable = 0
        self._release_ms = None

    def scan(self):
        raw = bool(self.pin.value())
        if raw == self._raw:
            if self._stable < STABLE_SAMPLES:
                self._stable += 1
        else:
            self._raw = raw
            self._stable = 1
        if self._stable < STABLE_SAMPLES:
            return  # not settled yet, keep the last debounced state

        self.value = raw
        now = time.ticks_ms()
        if raw:
            self._release_ms = None
            if self._pressed_state == UNPRESSED:
                self._pressed_state = PRESSED_UNREAD
                self._press_onset_ms = now
        elif self._pressed_state == PRESSED_UNREAD:
            # Quick tap: keep it briefly so a slow app loop still sees it
            if self._release_ms is None:
                self._release_ms = now
            elif time.ticks_diff(now, self._release_ms) > UNREAD_TTL_MS:
                self._pressed_state = UNPRESSED
                self._press_onset_ms = None
        else:
            self._pressed_state = UNPRESSED
            self._press_onset_ms = None

    def pressed(self) -> bool:
        """Was a button pressed? Only returns True once for each press."""
        if self._pressed_state == PRESSED_UNREAD:
            self._pressed_state = PRESSED_READ
            return True
        return False

    def flush(self):
        """Forget any unread press (a held button stays 'read' until released)."""
        if self._pressed_state == PRESSED_UNREAD:
            self._pressed_state = PRESSED_READ

    def is_down(self) -> bool:
        return self.value

    def pressed_ms(self) -> int:
        if self.value and self._press_onset_ms is not None:
            return time.ticks_diff(time.ticks_ms(), self._press_onset_ms)
        return 0

    held_ms = pressed_ms

    async def wait_press(self):
        """await UserA.wait_press()"""
        while not self.pressed():
            await aio.sleep_ms(10)

    def __str__(self):
        names = {UNPRESSED: "unpressed", PRESSED_UNREAD: "pressed-unread",
                 PRESSED_READ: "pressed-read"}
        return f"{self.name}:{self.value}:{names.get(self._pressed_state, '')}"


MenuMinus = Button(MENU_MINUS_PIN, "Menu -")
MenuPlus = Button(MENU_PLUS_PIN, "Menu +")
UserA = Button(USER_A_PIN, "User A")
UserB = Button(USER_B_PIN, "User B")
Boot = Button(BOOT_PIN, "Boot")

ALL_BUTTONS = (MenuMinus, MenuPlus, UserA, UserB, Boot)


def flush_all():
    for b in ALL_BUTTONS:
        b.flush()


async def scan():
    """Scan buttons and update persisted state"""
    while True:
        for b in ALL_BUTTONS:
            b.scan()
        await aio.sleep_ms(SCAN_MS)
