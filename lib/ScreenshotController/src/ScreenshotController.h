#pragma once

#include <cstdint>

#include <LiveDashboard.h>
#include <WsLcd35S3Hal.h>

namespace screenshot {

class ScreenshotController {
public:
  ScreenshotController(ws_lcd_35_s3_hal::WsLcd35S3Hal &hal, live_dashboard::LiveDashboard &dash);

  void begin();
  void tick();

private:
  bool choose_next_capture_dir_();
  void list_capture_dir_();

  ws_lcd_35_s3_hal::WsLcd35S3Hal &hal_;
  live_dashboard::LiveDashboard &dash_;

  bool active_ = false;
  bool listed_ = false;
  uint32_t target_cycle_ = 0;
  uint32_t last_frame_ = 0;
  uint32_t counter_ = 0;
  char dir_[64]{};
};

} // namespace screenshot
