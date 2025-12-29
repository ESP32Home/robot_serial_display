#pragma once

#include <cstddef>
#include <cstdint>

#include <FS.h>

namespace live_dashboard {

#ifndef LIVE_DASHBOARD_MAX_TILES
#define LIVE_DASHBOARD_MAX_TILES 24
#endif

#ifndef LIVE_DASHBOARD_MAX_GAUGES
#define LIVE_DASHBOARD_MAX_GAUGES 24
#endif

#ifndef LIVE_DASHBOARD_MAX_BUTTONS
#define LIVE_DASHBOARD_MAX_BUTTONS 24
#endif

#ifndef LIVE_DASHBOARD_MAX_HZ_ROWS
#define LIVE_DASHBOARD_MAX_HZ_ROWS 24
#endif

#ifndef LIVE_DASHBOARD_ID_MAX_LEN
#define LIVE_DASHBOARD_ID_MAX_LEN 32
#endif

struct LiveDashboardOptions {
  bool demo_replay;
  const char *demo_path;
  uint32_t demo_period_ms;

  LiveDashboardOptions() : demo_replay(false), demo_path("/test.jsonl"), demo_period_ms(1000) {}
};

class LiveDashboard {
public:
  using ActionCallback = void (*)(const char *action_id, void *user);

  LiveDashboard() = default;

  bool begin(fs::FS &fs,
             const char *config_path,
             uint16_t screen_width,
             uint16_t screen_height,
             char lvgl_drive_letter,
             const LiveDashboardOptions &options = LiveDashboardOptions());
  void tick();

  bool publishGauge(const char *gauge_id, int32_t value, const char *text);
  bool ingestLine(char *line);
  bool ingestEventLine(char *line);
  bool onAction(const char *action_id, ActionCallback cb, void *user);

  const char *robotName() const;

  // Demo replay helpers (valid only if demo_replay=true in options)
  bool demoReplayActive() const;
  uint32_t demoFrameIndex() const; // increments per ingested demo line
  uint32_t demoCycle() const;      // increments each time the demo file loops
};

} // namespace live_dashboard
