#pragma once

#include <cstdint>

#include <FS.h>
#include <lvgl.h>
#include <SD_MMC.h>

namespace ws_lcd_35_s3_hal {

class WsLcd35S3Hal {
public:
  WsLcd35S3Hal();

  bool begin();
  void loop();

  uint16_t width() const { return screen_width_; }
  uint16_t height() const { return screen_height_; }

  bool flashFsMounted() const { return flashfs_mounted_; }
  fs::FS &flashFs() { return *flash_fs_; }
  bool sdFsMounted() const { return sd_mounted_; }
  fs::FS &sdFs() { return *sd_fs_; }

  char lvglFlashDriveLetter() const { return lvgl_flash_drive_letter_; }

  bool captureScreenshotBmp(const char *path);
  void copyAreaToMirror_(const lv_area_t *area, lv_color_t *color_p); // internal: called from flush_cb

private:
  bool initDisplay_();
  bool initTouch_();
  bool initFlashFs_();
  bool initSdCard_();
  bool writeBmp_(fs::File &file);
  void registerFlashFsWithLvgl_(char drive_letter);

  uint16_t screen_width_ = 0;
  uint16_t screen_height_ = 0;

  bool flashfs_mounted_ = false;
  fs::FS *flash_fs_ = nullptr;
  bool sd_mounted_ = false;
  fs::FS *sd_fs_ = nullptr;
  lv_color_t *mirror_fb_ = nullptr;
  char lvgl_flash_drive_letter_ = 'F';
};

} // namespace ws_lcd_35_s3_hal
