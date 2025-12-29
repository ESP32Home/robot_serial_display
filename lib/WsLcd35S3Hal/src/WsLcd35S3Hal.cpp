#include "WsLcd35S3Hal.h"

#include <Arduino.h>
#include <FFat.h>
#include <Wire.h>
#include <SD_MMC.h>

#include <cstdio>
#include <new>
#include <cstring>
#include <memory>

#include <Arduino_GFX_Library.h>
#include <lvgl.h>

#include "TCA9554.h"
#include "TouchDrvFT6X36.hpp"

#include "esp_heap_caps.h"
#include "soc/soc_memory_types.h"

#ifndef ROVI_ENABLE_SCREENSHOTS
#define ROVI_ENABLE_SCREENSHOTS 0
#endif
#ifndef ROVI_BENCH_DRAW_BUF
#define ROVI_BENCH_DRAW_BUF 0
#endif

static constexpr bool kScreenshotsEnabled = (ROVI_ENABLE_SCREENSHOTS != 0);

namespace ws_lcd_35_s3_hal {
namespace {

// Board pins / wiring (matches existing examples in this repo)
static constexpr int kBacklightPin = 6;

static constexpr int kSpiMiso = 2;
static constexpr int kSpiMosi = 1;
static constexpr int kSpiSclk = 5;

static constexpr int kLcdCs = -1;
static constexpr int kLcdDc = 3;
static constexpr int kLcdRst = -1;
static constexpr int kLcdHorRes = 320;
static constexpr int kLcdVerRes = 480;

static constexpr int kI2cSda = 8;
static constexpr int kI2cScl = 7;

// SD (SD_MMC 1-bit mode) wiring from 14_lvgl_image sample
static constexpr int kSdClk = 11;
static constexpr int kSdCmd = 10;
static constexpr int kSdD0 = 9;

// Draw buffer placement: internal RAM (DMA-capable) for faster SPI transfers.

struct ArduinoFsFile {
  fs::File file;
};

TCA9554 g_tca(0x20);
TouchDrvFT6X36 g_touch;

Arduino_ESP32SPI g_bus(kLcdDc, kLcdCs, kSpiSclk, kSpiMosi, kSpiMiso);
Arduino_ST7796 g_gfx(&g_bus, kLcdRst, 0 /* rotation */, true /* IPS? */, kLcdHorRes, kLcdVerRes);

lv_disp_draw_buf_t g_draw_buf;
lv_color_t *g_disp_draw_buf1 = nullptr;
lv_color_t *g_disp_draw_buf2 = nullptr;
lv_disp_drv_t g_disp_drv;
lv_indev_drv_t g_indev_drv;

static void bench_draw_buffers_(uint16_t screen_width, uint16_t screen_height) {
#if ROVI_BENCH_DRAW_BUF
  const uint16_t w = screen_width;
  const uint16_t h = 120; // avoid huge internal allocs
  const uint32_t pixels = static_cast<uint32_t>(w) * h;
  const uint32_t bytes = pixels * sizeof(uint16_t);
  const int loops = 5;

  auto bench = [&](const char *label, uint32_t caps) {
    void *buf = heap_caps_malloc(bytes, caps);
    if (!buf) {
      Serial.printf("BENCH %s: alloc failed\n", label);
      return;
    }
    memset(buf, 0xA5, bytes);
    uint32_t start = micros();
    for (int i = 0; i < loops; ++i) {
      g_gfx.draw16bitRGBBitmap(0, 0, static_cast<uint16_t *>(buf), w, h);
    }
    uint32_t elapsed = micros() - start;
    float per = elapsed / static_cast<float>(loops);
    float mbps = (bytes / 1e6f) / (per / 1e6f);
    Serial.printf("BENCH %s: %d frames of %ux%u took %.1f ms total (%.2f ms/frame, %.2f MB/s)\n",
                  label,
                  loops,
                  w,
                  h,
                  elapsed / 1000.0f,
                  per / 1000.0f,
                  mbps);
    heap_caps_free(buf);
  };

  bench("INTERNAL|DMA", MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  bench("SPIRAM", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  (void)screen_width;
  (void)screen_height;
#endif
}

void lcd_reset() {
  g_tca.write1(1, 1);
  delay(10);
  g_tca.write1(1, 0);
  delay(10);
  g_tca.write1(1, 1);
  delay(200);
}

static void disp_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = static_cast<uint32_t>(area->x2 - area->x1 + 1);
  uint32_t h = static_cast<uint32_t>(area->y2 - area->y1 + 1);

#if (LV_COLOR_16_SWAP != 0)
  g_gfx.draw16bitBeRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t *>(&color_p->full), w, h);
#else
  g_gfx.draw16bitRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t *>(&color_p->full), w, h);
#endif

  if (disp_drv != nullptr && disp_drv->user_data != nullptr) {
    auto *hal = static_cast<WsLcd35S3Hal *>(disp_drv->user_data);
    hal->copyAreaToMirror_(area, color_p);
  }

  lv_disp_flush_ready(disp_drv);
}

static void touch_read_cb(lv_indev_drv_t *, lv_indev_data_t *data) {
  int16_t x[1], y[1];
  uint8_t touched = g_touch.getPoint(x, y, 1);

  if (touched) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x[0];
    data->point.y = y[0];
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

static void *lvgl_fs_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode) {
  if (drv == nullptr || path == nullptr) {
    return nullptr;
  }

  fs::FS *fs = static_cast<fs::FS *>(drv->user_data);
  if (fs == nullptr) {
    return nullptr;
  }

  const char *open_mode = "r";
  if (mode == LV_FS_MODE_WR) {
    open_mode = "w";
  } else if (mode == (LV_FS_MODE_RD | LV_FS_MODE_WR)) {
    open_mode = "r+";
  }

  char normalized[128];
  const char *open_path = path;
  if (path[0] != '/') {
    snprintf(normalized, sizeof(normalized), "/%s", path);
    open_path = normalized;
  }

  fs::File file = fs->open(open_path, open_mode);
  if (!file) {
    return nullptr;
  }

  void *mem = lv_mem_alloc(sizeof(ArduinoFsFile));
  if (mem == nullptr) {
    file.close();
    return nullptr;
  }

  ArduinoFsFile *handle = new (mem) ArduinoFsFile();
  handle->file = file;
  return handle;
}

static lv_fs_res_t lvgl_fs_close_cb(lv_fs_drv_t *, void *file_p) {
  if (file_p == nullptr) {
    return LV_FS_RES_OK;
  }

  ArduinoFsFile *handle = static_cast<ArduinoFsFile *>(file_p);
  handle->file.close();
  handle->~ArduinoFsFile();
  lv_mem_free(handle);
  return LV_FS_RES_OK;
}

static lv_fs_res_t lvgl_fs_read_cb(lv_fs_drv_t *, void *file_p, void *buf, uint32_t btr, uint32_t *br) {
  if (file_p == nullptr || buf == nullptr || br == nullptr) {
    return LV_FS_RES_INV_PARAM;
  }

  ArduinoFsFile *handle = static_cast<ArduinoFsFile *>(file_p);
  *br = static_cast<uint32_t>(handle->file.read(static_cast<uint8_t *>(buf), btr));
  return LV_FS_RES_OK;
}

static lv_fs_res_t lvgl_fs_write_cb(lv_fs_drv_t *, void *file_p, const void *buf, uint32_t btw, uint32_t *bw) {
  if (file_p == nullptr || buf == nullptr || bw == nullptr) {
    return LV_FS_RES_INV_PARAM;
  }

  ArduinoFsFile *handle = static_cast<ArduinoFsFile *>(file_p);
  *bw = static_cast<uint32_t>(handle->file.write(static_cast<const uint8_t *>(buf), btw));
  return LV_FS_RES_OK;
}

static lv_fs_res_t lvgl_fs_seek_cb(lv_fs_drv_t *, void *file_p, uint32_t pos, lv_fs_whence_t whence) {
  if (file_p == nullptr) {
    return LV_FS_RES_INV_PARAM;
  }

  ArduinoFsFile *handle = static_cast<ArduinoFsFile *>(file_p);
  uint32_t target = pos;
  if (whence == LV_FS_SEEK_CUR) {
    target = static_cast<uint32_t>(handle->file.position()) + pos;
  } else if (whence == LV_FS_SEEK_END) {
    target = static_cast<uint32_t>(handle->file.size()) + pos;
  }

  if (!handle->file.seek(target)) {
    return LV_FS_RES_UNKNOWN;
  }
  return LV_FS_RES_OK;
}

static lv_fs_res_t lvgl_fs_tell_cb(lv_fs_drv_t *, void *file_p, uint32_t *pos_p) {
  if (file_p == nullptr || pos_p == nullptr) {
    return LV_FS_RES_INV_PARAM;
  }

  ArduinoFsFile *handle = static_cast<ArduinoFsFile *>(file_p);
  *pos_p = static_cast<uint32_t>(handle->file.position());
  return LV_FS_RES_OK;
}

} // namespace

WsLcd35S3Hal::WsLcd35S3Hal() : flash_fs_(&FFat) {}

bool WsLcd35S3Hal::begin() {
  Wire.begin(kI2cSda, kI2cScl);

  g_tca.begin();
  g_tca.pinMode1(1, OUTPUT);
  lcd_reset();

  if (!initTouch_()) {
    Serial.println("FATAL: Touch init failed");
    return false;
  }
  if (!initDisplay_()) {
    Serial.println("FATAL: Display init failed");
    return false;
  }

  flashfs_mounted_ = initFlashFs_();
  sd_fs_ = &SD_MMC;
  if (kScreenshotsEnabled) {
    sd_mounted_ = initSdCard_();
    if (!sd_mounted_) {
      Serial.println("WARN: SD card init failed (screenshots disabled)");
    }
  }

  lv_init();

  screen_width_ = static_cast<uint16_t>(g_gfx.width());
  screen_height_ = static_cast<uint16_t>(g_gfx.height());

  if (kScreenshotsEnabled && sd_mounted_) {
    const uint32_t fb_bytes = static_cast<uint32_t>(screen_width_) * static_cast<uint32_t>(screen_height_) * sizeof(lv_color_t);
    mirror_fb_ = static_cast<lv_color_t *>(heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (mirror_fb_ == nullptr) {
      Serial.println("WARN: Screenshot mirror buffer alloc failed (PSRAM)");
    } else {
      memset(mirror_fb_, 0, fb_bytes);
    }
  }

  const uint32_t caps = (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

  uint32_t buf_lines = 0;
  bool double_buffered = false;
  for (uint32_t try_lines : {480U, 440U, 400U, 360U, 320U, 300U, 280U, 260U, 240U, 200U, 160U, 120U, 100U, 80U, 60U, 40U}) {
    if (try_lines > screen_height_) continue;

    const uint32_t try_pixels = static_cast<uint32_t>(screen_width_) * try_lines;
    const uint32_t try_bytes = try_pixels * sizeof(lv_color_t);

    lv_color_t *buf1 = static_cast<lv_color_t *>(heap_caps_malloc(try_bytes, caps));
    if (buf1 == nullptr) {
      continue;
    }
    lv_color_t *buf2 = static_cast<lv_color_t *>(heap_caps_malloc(try_bytes, caps));

    g_disp_draw_buf1 = buf1;
    g_disp_draw_buf2 = buf2; // may be null => single buffering
    buf_lines = try_lines;
    double_buffered = (buf2 != nullptr);
    break;
  }

  if (g_disp_draw_buf1 == nullptr) {
    Serial.println("FATAL: LVGL draw buffers alloc failed");
    return false;
  }
  auto log_buf = [](const char *label, void *ptr) {
    Serial.printf("DRAW_BUF %s=%p ext=%d internal=%d dma=%d\n",
                  label,
                  ptr,
                  ptr != nullptr ? (esp_ptr_external_ram(ptr) ? 1 : 0) : 0,
                  ptr != nullptr ? (esp_ptr_internal(ptr) ? 1 : 0) : 0,
                  ptr != nullptr ? (esp_ptr_dma_capable(ptr) ? 1 : 0) : 0);
  };
  log_buf("buf1", g_disp_draw_buf1);
  log_buf("buf2", g_disp_draw_buf2);
  Serial.printf("DRAW_BUF caps: INTERNAL|DMA\n");
  Serial.printf("DRAW_BUF lines=%u mode=%s\n", static_cast<unsigned>(buf_lines), double_buffered ? "double" : "single");
  const uint32_t buf_pixels = static_cast<uint32_t>(screen_width_) * buf_lines;
  lv_disp_draw_buf_init(&g_draw_buf, g_disp_draw_buf1, g_disp_draw_buf2, buf_pixels);

  lv_disp_drv_init(&g_disp_drv);
  g_disp_drv.hor_res = screen_width_;
  g_disp_drv.ver_res = screen_height_;
  g_disp_drv.flush_cb = disp_flush_cb;
  g_disp_drv.draw_buf = &g_draw_buf;
  g_disp_drv.user_data = this;
  lv_disp_drv_register(&g_disp_drv);

  lv_indev_drv_init(&g_indev_drv);
  g_indev_drv.type = LV_INDEV_TYPE_POINTER;
  g_indev_drv.read_cb = touch_read_cb;
  g_indev_drv.user_data = this;
  lv_indev_drv_register(&g_indev_drv);

  if (flashfs_mounted_) {
    registerFlashFsWithLvgl_(lvgl_flash_drive_letter_);
  } else {
    Serial.println("WARN: FFat not mounted (no LVGL flash FS)");
  }

  bench_draw_buffers_(screen_width_, screen_height_);

  return true;
}

void WsLcd35S3Hal::loop() {
  lv_timer_handler();
  delay(1);
}

bool WsLcd35S3Hal::initDisplay_() {
  if (!g_gfx.begin()) {
    Serial.println("gfx.begin() failed");
    return false;
  }

  g_gfx.fillScreen(RGB565_BLACK);

  pinMode(kBacklightPin, OUTPUT);
  digitalWrite(kBacklightPin, HIGH);
  return true;
}

bool WsLcd35S3Hal::initTouch_() {
  if (!g_touch.begin(Wire, FT6X36_SLAVE_ADDRESS)) {
    Serial.println("Failed to find FT6X36 - check wiring");
    return false;
  }
  return true;
}

bool WsLcd35S3Hal::initFlashFs_() {
  if (!FFat.begin(false)) {
    Serial.println("FFat.begin(false) failed");
    return false;
  }
  return true;
}

bool WsLcd35S3Hal::initSdCard_() {
  if (!SD_MMC.setPins(kSdClk, kSdCmd, kSdD0)) {
    Serial.println("SD_MMC.setPins failed");
    return false;
  }
  if (!SD_MMC.begin("/sdcard", true /* 1-bit mode */)) {
    Serial.println("SD_MMC.begin failed");
    return false;
  }
  return true;
}

void WsLcd35S3Hal::copyAreaToMirror_(const lv_area_t *area, lv_color_t *color_p) {
  if (!kScreenshotsEnabled) {
    return;
  }
  if (mirror_fb_ == nullptr || area == nullptr || color_p == nullptr) {
    return;
  }

  const uint32_t w = static_cast<uint32_t>(area->x2 - area->x1 + 1);
  const uint32_t h = static_cast<uint32_t>(area->y2 - area->y1 + 1);
  const uint32_t stride_pixels = static_cast<uint32_t>(screen_width_);
  for (uint32_t row = 0; row < h; ++row) {
    const uint32_t dst_y = static_cast<uint32_t>(area->y1) + row;
    lv_color_t *dst = mirror_fb_ + (dst_y * stride_pixels + static_cast<uint32_t>(area->x1));
    const lv_color_t *src = color_p + (row * w);
    memcpy(dst, src, w * sizeof(lv_color_t));
  }
}

bool WsLcd35S3Hal::writeBmp_(fs::File &file) {
  if (!kScreenshotsEnabled || mirror_fb_ == nullptr) {
    return false;
  }

  const uint32_t width = screen_width_;
  const uint32_t height = screen_height_;
  const uint32_t row_bytes = width * sizeof(uint16_t);
  const uint32_t row_padded = (row_bytes + 3U) & ~3U; // BMP rows align to 4 bytes
  const uint32_t pixel_bytes = row_padded * height;
  const uint32_t header_bytes = 14U + 40U + 12U; // BITMAPFILEHEADER + BITMAPINFOHEADER + bit masks
  const uint32_t file_size = header_bytes + pixel_bytes;

  uint8_t header[66]{};
  // BITMAPFILEHEADER
  header[0] = 'B';
  header[1] = 'M';
  header[2] = static_cast<uint8_t>(file_size & 0xFF);
  header[3] = static_cast<uint8_t>((file_size >> 8) & 0xFF);
  header[4] = static_cast<uint8_t>((file_size >> 16) & 0xFF);
  header[5] = static_cast<uint8_t>((file_size >> 24) & 0xFF);
  header[10] = static_cast<uint8_t>(header_bytes); // pixel data offset

  // BITMAPINFOHEADER
  header[14] = 40; // biSize
  header[18] = static_cast<uint8_t>(width & 0xFF);
  header[19] = static_cast<uint8_t>((width >> 8) & 0xFF);
  header[20] = static_cast<uint8_t>((width >> 16) & 0xFF);
  header[21] = static_cast<uint8_t>((width >> 24) & 0xFF);
  header[22] = static_cast<uint8_t>(height & 0xFF);
  header[23] = static_cast<uint8_t>((height >> 8) & 0xFF);
  header[24] = static_cast<uint8_t>((height >> 16) & 0xFF);
  header[25] = static_cast<uint8_t>((height >> 24) & 0xFF);
  header[26] = 1;   // planes
  header[28] = 16;  // bit count
  header[30] = 3;   // compression BI_BITFIELDS
  header[34] = static_cast<uint8_t>(pixel_bytes & 0xFF);
  header[35] = static_cast<uint8_t>((pixel_bytes >> 8) & 0xFF);
  header[36] = static_cast<uint8_t>((pixel_bytes >> 16) & 0xFF);
  header[37] = static_cast<uint8_t>((pixel_bytes >> 24) & 0xFF);

  // Bit masks for RGB565
  constexpr uint32_t kMaskR = 0xF800;
  constexpr uint32_t kMaskG = 0x07E0;
  constexpr uint32_t kMaskB = 0x001F;
  header[54] = static_cast<uint8_t>(kMaskR & 0xFF);
  header[55] = static_cast<uint8_t>((kMaskR >> 8) & 0xFF);
  header[58] = static_cast<uint8_t>(kMaskG & 0xFF);
  header[59] = static_cast<uint8_t>((kMaskG >> 8) & 0xFF);
  header[62] = static_cast<uint8_t>(kMaskB & 0xFF);
  header[63] = static_cast<uint8_t>((kMaskB >> 8) & 0xFF);

  if (file.write(header, header_bytes) != header_bytes) {
    return false;
  }

  std::unique_ptr<uint8_t[]> row(new (std::nothrow) uint8_t[row_padded]);
  if (!row) {
    return false;
  }

  for (int32_t y = static_cast<int32_t>(height) - 1; y >= 0; --y) { // BMP writes bottom-up
    const lv_color_t *src = mirror_fb_ + (static_cast<uint32_t>(y) * width);
    memcpy(row.get(), src, row_bytes);
    if (row_padded > row_bytes) {
      memset(row.get() + row_bytes, 0, row_padded - row_bytes);
    }
    if (file.write(row.get(), row_padded) != row_padded) {
      return false;
    }
  }

  return true;
}

bool WsLcd35S3Hal::captureScreenshotBmp(const char *path) {
  if (!kScreenshotsEnabled) {
    Serial.println("Screenshots disabled at compile time (ROVI_ENABLE_SCREENSHOTS=0)");
    return false;
  }
  if (!sd_mounted_) {
    Serial.println("SD card not mounted");
    return false;
  }
  if (mirror_fb_ == nullptr) {
    Serial.println("Screenshot mirror buffer missing");
    return false;
  }
  if (path == nullptr || path[0] == '\0') {
    Serial.println("Invalid screenshot path");
    return false;
  }

  fs::File file = sd_fs_->open(path, FILE_WRITE);
  if (!file) {
    Serial.printf("Failed to open screenshot path: %s\n", path);
    return false;
  }

  const bool ok = writeBmp_(file);
  file.close();
  if (!ok) {
    Serial.println("Screenshot write failed");
  }
  return ok;
}

void WsLcd35S3Hal::registerFlashFsWithLvgl_(char drive_letter) {
  static lv_fs_drv_t fs_drv;
  lv_fs_drv_init(&fs_drv);
  fs_drv.letter = drive_letter;
  fs_drv.cache_size = 0;
  fs_drv.user_data = flash_fs_;

  fs_drv.open_cb = lvgl_fs_open_cb;
  fs_drv.close_cb = lvgl_fs_close_cb;
  fs_drv.read_cb = lvgl_fs_read_cb;
  fs_drv.write_cb = lvgl_fs_write_cb;
  fs_drv.seek_cb = lvgl_fs_seek_cb;
  fs_drv.tell_cb = lvgl_fs_tell_cb;

  lv_fs_drv_register(&fs_drv);
}

} // namespace ws_lcd_35_s3_hal
