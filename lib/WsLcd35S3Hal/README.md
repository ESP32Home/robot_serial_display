# WsLcd35S3Hal (local library)

Board/HAL bring-up for the ESP32‑S3 3.5" 320×480 touch LCD used by this repo’s examples:

- Arduino_GFX display driver (ST7796)
- FT6X36 touch (I2C)
- LVGL display + input registration
- Internal FFat mount + LVGL FS driver registration (flash only, no SD)

## Quick start

```cpp
#include <WsLcd35S3Hal.h>

ws_lcd_35_s3_hal::WsLcd35S3Hal hal;

void setup() {
  Serial.begin(115200);
  if (!hal.begin()) {
    Serial.println("HAL init failed");
    while (true) delay(1000);
  }
}

void loop() {
  hal.loop(); // calls lv_timer_handler() + small delay
}
```

## API

- `bool begin()`
  - Initializes I2C, touch, display, LVGL draw buffers, LVGL display/input drivers.
  - Mounts FFat and registers it as an LVGL filesystem drive (default letter `F`).
- `void loop()`
  - Calls `lv_timer_handler()` and `delay(1)`.
- `uint16_t width() / height()`
  - Current display size.
- `fs::FS& flashFs()`
  - The mounted FFat filesystem instance (internal flash).
- `bool flashFsMounted()`
  - Whether FFat mounted successfully.
- `char lvglFlashDriveLetter()`
  - The LVGL drive letter used for FFat (default: `'F'`).

## LVGL filesystem note

After `begin()`, LVGL can load assets from internal FFat using paths like:

- `F:/rovi.bmp`

This is how the dashboard splash image is loaded (PNG decoder must be enabled in your `lv_conf.h`).

## PlatformIO notes

This sample uses a FATFS partition (`FFat`) configured by:

- `board_build.filesystem = fatfs`
- `board_build.partitions = ../partitions/partitions_16MB_3MBapp_9_9MB_fatfs.csv`

Upload filesystem content with:

- `pio run -e esp32-s3-touch-lcd-35 -t uploadfs`
