"""Calibrate partial-update windows.

Run from your Mac with the badge booted normally (NOT after a soft reset):

    mpremote reset
    (wait a few seconds for it to boot)
    mpremote resume run tools/window_test.py

`resume` matters: it reuses the display main.py already set up instead of
soft-resetting, which would leave LVGL's C state stale.

1. Clears the panel to white (normal fast update).
2. Draws a black box in the LEFT half and refreshes only that window.
3. Draws a black box in the RIGHT half and refreshes only that window.

Watch steps 2 and 3: each box should appear complete and nothing else
should flash. Note where it flashes if it doesn't.
"""
import asyncio
from lib.epd import display


async def main():
    print("1/3 clearing to white")
    display.clear()
    await display.refresh_now(fast=True, flash=False)
    await asyncio.sleep(1)

    print("2/3 left box: x 200-319, y 200-279")
    await display.window_test_now(200, 200, 120, 80)
    await asyncio.sleep(2)

    print("3/3 right box: x 650-769, y 400-479")
    await display.window_test_now(650, 400, 120, 80)
    print("done - mpremote reset to go back to the apps")


asyncio.run(main())
