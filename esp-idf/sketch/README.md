# Sketch — ESP-IDF firmware for the billboard badge

A standalone C firmware that boots straight into the knob-drawing toy. It
replaces MicroPython on the badge (see "Going back to MicroPython" below).

- Left knob moves the pen left/right, right knob up/down (clockwise = right/up)
- **User A** or a hard **shake** erases
- **User B** does a full refresh to clean up ghosting (with one knob: switches its axis)
- **LEDs**: 0-3 follow the X knob (cyan), 4-7 the Y knob (magenta) as a dot
  spinning the way you turn; red when the pen hits a wall; off when idle

## Hardware

| What | Where |
| --- | --- |
| Knobs | Two AS5600 SAOs, sensor analog OUT wired to the SAO's GPIO1. Found automatically on SAO1/2/3 (GPIO2/4/7); the first two are X and Y. |
| Shake (optional) | MPU6050 on the SAO I2C bus (SDA 17, SCL 18) at 0x68 or 0x69 |
| Panel | E2969JS0B1 / E2969JS084, detected from the panel's OTP |
| LEDs | 8x WS2812 on GPIO38 |

## Upload

1. Install [VS Code](https://code.visualstudio.com/) and the **PlatformIO IDE**
   extension. (Or the command line: `pip install platformio`.)
2. Open this folder (`esp-idf/sketch`) in VS Code. The first build downloads
   the ESP32 toolchain and ESP-IDF 5.3, which takes a few minutes.
3. Plug the badge in over USB-C.
4. Build and flash: PlatformIO's **Upload** button (→ in the status bar), or

   ```
   cd esp-idf/sketch
   pio run -t upload
   ```

   If it can't connect, put the badge in download mode by hand: hold **BOOT**,
   tap **RESET**, release **BOOT**, then upload again. Press RESET afterwards
   to start the new firmware.
5. Watch the log: PlatformIO's **Serial Monitor** button, or `pio device monitor`.

On boot you should see the knobs being probed, then a full refresh (~35 s)
drawing the red frame. After that, lines show up a few seconds after you stop
turning.

## Going back to MicroPython

This firmware overwrites MicroPython and its partition table, so the files on
the badge go too (everything is in git). To restore, from the Hackaday repo's
`firmware/` folder:

```
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 bin/bootloader.bin 0x8000 bin/partition-table.bin 0x10000 bin/micropython.bin
```

then copy the badge files back with `mpremote fs cp -r lib apps data main.py boot.py :`.

## Files

| File | |
| --- | --- |
| `src/main.c` | The app: knobs, shake, buttons, LEDs, frame drawing, display task |
| `src/ws2812.c` | LED driver on the RMT peripheral |
| `src/epd97*.cpp`, `src/epd97_lut.h`, `include/epd97*.h` | Panel driver from the Hackaday repo's `firmware/esp32s3-itc97`, ported from Arduino to ESP-IDF (`src/compat.h`) with one addition, `EPD_FAST_KEEP_RED` |
| `sdkconfig.defaults` | Flash/PSRAM, 1 ms FreeRTOS tick, USB console |

`EPD_FAST_KEEP_RED` is the fast waveform with the red tables idle: black and
white are driven, red pixels aren't touched. The frame goes up once with
`EPD_SLOW` (the only mode that makes good red), then every line and erase uses
`EPD_FAST_KEEP_RED` with the panel session kept open, so the frame doesn't
fade.

Tunables (pen speed, line width, refresh timing, shake sensitivity, LED
brightness) are `#define`s at the top of `src/main.c`.
