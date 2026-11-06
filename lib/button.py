"""Button interface for the Billboard Badge

    from lib.button import UserA
    if UserA.pressed():      # True once per press, even if the press happened
        ...                  # while something slow (PNG decode) was running
    UserA.is_down()          # live state
    UserA.held_ms()          # how long it has been held (0 if up)

Presses are latched by a pin interrupt, so a quick tap can't be missed by
the polling loop. Both edges are debounced.
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

DEBOUNCE_MS = 30


class Button:
    def __init__(self, pin: int, name: str):
        self._raw = machine.Pin(pin, machine.Pin.IN, machine.Pin.PULL_UP)
        self.pin = machine.Signal(self._raw, invert=True)
        self.name = name
        self.value = False
        self._pressed_state = UNPRESSED
        self._press_onset_ms = None
        self._last_edge_ms = time.ticks_ms()
        try:
            self._raw.irq(trigger=machine.Pin.IRQ_FALLING, handler=self._irq)
        except Exception as ex:
            print(f"Button {name}: no IRQ ({ex}), polling only")

    def _irq(self, _pin):
        now = time.ticks_ms()
        if (self._pressed_state == UNPRESSED
                and time.ticks_diff(now, self._last_edge_ms) > DEBOUNCE_MS):
            self._pressed_state = PRESSED_UNREAD
            self._press_onset_ms = now
            self._last_edge_ms = now

    def scan(self):
        self.value = self.pin.value()
        now = time.ticks_ms()
        if self.value:
            if (self._pressed_state == UNPRESSED
                    and time.ticks_diff(now, self._last_edge_ms) > DEBOUNCE_MS):
                self._pressed_state = PRESSED_UNREAD
                self._press_onset_ms = now
                self._last_edge_ms = now
        elif self._pressed_state == PRESSED_READ:
            # Released after the app saw it. An unread press stays latched.
            self._pressed_state = UNPRESSED
            self._press_onset_ms = None
            self._last_edge_ms = now

    def pressed(self) -> bool:
        """Was a button pressed? Only returns True once for each press."""
        if self._pressed_state == PRESSED_UNREAD:
            self._pressed_state = PRESSED_READ
            return True
        return False

    def is_down(self) -> bool:
        return bool(self.pin.value())

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


async def scan():
    """Scan buttons and update persisted state"""
    while True:
        for b in ALL_BUTTONS:
            b.scan()
        await aio.sleep_ms(5)
