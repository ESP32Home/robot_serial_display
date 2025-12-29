#include "ScreenshotController.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>

#ifndef ROVI_ENABLE_SCREENSHOTS
#define ROVI_ENABLE_SCREENSHOTS 0
#endif

namespace screenshot {

ScreenshotController::ScreenshotController(ws_lcd_35_s3_hal::WsLcd35S3Hal &hal,
                                           live_dashboard::LiveDashboard &dash)
    : hal_(hal), dash_(dash) {}

bool ScreenshotController::choose_next_capture_dir_() {
#if ROVI_ENABLE_SCREENSHOTS
  if (!hal_.sdFs().exists("/screenshots")) {
    hal_.sdFs().mkdir("/screenshots");
  }

  uint32_t max_run = 0;
  File dir = hal_.sdFs().open("/screenshots");
  if (dir && dir.isDirectory()) {
    File f = dir.openNextFile();
    while (f) {
      if (f.isDirectory()) {
        const char *name = f.name();
        if (name != nullptr && strncmp(name, "/screenshots/run_", 17) == 0) {
          const char *num = name + 17;
          char *end = nullptr;
          unsigned long val = strtoul(num, &end, 10);
          if (end != num && val > max_run) {
            max_run = static_cast<uint32_t>(val);
          }
        }
      }
      f = dir.openNextFile();
    }
  }

  const uint32_t next_run = max_run + 1;
  snprintf(dir_, sizeof(dir_), "/screenshots/run_%u", static_cast<unsigned>(next_run));
  if (!hal_.sdFs().mkdir(dir_)) {
    Serial.printf("WARN: mkdir %s failed (screenshots disabled)\n", dir_);
    dir_[0] = '\0';
    return false;
  }
  return true;
#else
  return false;
#endif
}

void ScreenshotController::list_capture_dir_() {
#if ROVI_ENABLE_SCREENSHOTS
  if (!dir_[0]) return;
  if (!hal_.sdFsMounted()) return;

  File dir = hal_.sdFs().open(dir_);
  if (!dir || !dir.isDirectory()) {
    Serial.printf("Capture dir not found: %s\n", dir_);
    return;
  }

  Serial.printf("Captured files in %s:\n", dir_);
  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      Serial.printf(" - %s (%u bytes)\n", f.name(), static_cast<unsigned>(f.size()));
    }
    f = dir.openNextFile();
  }
#endif
}

void ScreenshotController::begin() {
#if ROVI_ENABLE_SCREENSHOTS
  dir_[0] = '\0';
  active_ = false;
  listed_ = false;
  counter_ = 0;
  last_frame_ = 0;
  target_cycle_ = 0;

  if (!dash_.demoReplayActive()) {
    Serial.println("Screenshots disabled: demo replay off");
    return;
  }
  if (!hal_.sdFsMounted()) {
    Serial.println("Screenshots disabled: SD missing");
    return;
  }
  if (!choose_next_capture_dir_()) {
    return;
  }

  active_ = true;
  target_cycle_ = dash_.demoCycle();
  counter_ = 0;
  last_frame_ = 0;
  listed_ = false;
  Serial.printf("Screenshots enabled: %s\n", dir_);
#endif
}

void ScreenshotController::tick() {
#if ROVI_ENABLE_SCREENSHOTS
  if (!active_) {
    return;
  }
  if (!dash_.demoReplayActive()) {
    return;
  }

  const uint32_t cycle = dash_.demoCycle();
  if (cycle > target_cycle_) {
    active_ = false;
    if (!listed_) {
      list_capture_dir_();
      listed_ = true;
    }
    return;
  }
  if (cycle < target_cycle_) {
    return;
  }

  const uint32_t frame = dash_.demoFrameIndex();
  if (frame == 0 || frame == last_frame_) {
    return;
  }
  last_frame_ = frame;

  ++counter_;
  char path[96];
  snprintf(path, sizeof(path), "%s/%u.bmp", dir_, static_cast<unsigned>(counter_));
  const bool ok = hal_.captureScreenshotBmp(path);
  if (!ok) {
    Serial.printf("Screenshot failed: %s\n", path);
    active_ = false;
    if (!listed_) {
      list_capture_dir_();
      listed_ = true;
    }
  }
#endif
}

} // namespace screenshot
