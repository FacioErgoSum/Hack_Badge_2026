"""Calibrate partial-update windows. Run from your Mac:

    mpremote run tools/window_test.py

1. Clears the panel to white (normal fast update).
2. Draws a black box in the LEFT half and refreshes only that window.
3. Draws a black box in the RIGHT half and refreshes only that window.

Watch steps 2 and 3: each box should appear complete and nothing else
should flash. Note where it flashes if it doesn't.
"""
import asyncio
import time
from lib.epd import display


async def main():
    task = asyncio.create_task(display.run())
    print("1/3 clearing to white")
    display.clear()
    display.update(fast=True, flash=False)
    await display.wait_done()
    time.sleep(1)

    print("2/3 left box: x 200-319, y 200-279")
    display.window_test(200, 200, 120, 80)
    await display.wait_done()
    time.sleep(2)

    print("3/3 right box: x 650-769, y 400-479")
    display.window_test(650, 400, 120, 80)
    await display.wait_done()
    print("done")
    task.cancel()


asyncio.run(main())
