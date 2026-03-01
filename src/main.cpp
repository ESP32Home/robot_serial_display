/*Using LVGL with Arduino requires some extra steps:
 *Be sure to read the docs here: https://docs.lvgl.io/master/get-started/platforms/arduino.html  */

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
#include "esp32-hal-psram.h"
#include "esp_heap_caps.h"
#include "soc/soc_memory_types.h"
#endif

#include <LiveDashboard.h>
#include <ScreenshotController.h>
#include <WsLcd35S3Hal.h>

#ifndef ROVI_ENABLE_JSONL_DEMO_REPLAY
#define ROVI_ENABLE_JSONL_DEMO_REPLAY 0
#endif

// Serial RX diagnostics (set via build flags, e.g. `-D ROVI_RX_STATS_ENABLE=1`).
#ifndef ROVI_RX_LINE_TIMEOUT_MS
#define ROVI_RX_LINE_TIMEOUT_MS 500U
#endif

#ifndef ROVI_RX_STATS_ENABLE
#define ROVI_RX_STATS_ENABLE 0
#endif

#ifndef ROVI_RX_STATS_PERIOD_MS
#define ROVI_RX_STATS_PERIOD_MS 60000U
#endif

#ifndef ROVI_RX_ERROR_HEX_DUMP
#define ROVI_RX_ERROR_HEX_DUMP 0
#endif

static constexpr const char *kConfigPath = "/config.json";

static ws_lcd_35_s3_hal::WsLcd35S3Hal g_hal;
static live_dashboard::LiveDashboard g_dashboard;
static screenshot::ScreenshotController g_shots(g_hal, g_dashboard);
static bool g_dashboard_ready = false;

static void touch_allocation(void *ptr, size_t size) {
  if (ptr == nullptr || size == 0) {
    return;
  }
  volatile uint8_t *bytes = static_cast<volatile uint8_t *>(ptr);
  bytes[0] = 0xA5;
  bytes[size - 1] = 0x5A;
}

static void print_memory_stats(const char *tag) {
#if defined(ARDUINO_ARCH_ESP32)
  const char *safe_tag = (tag != nullptr) ? tag : "?";
  Serial.printf("[%s] psramFound=%d size=%u free=%u\n",
                safe_tag,
                psramFound() ? 1 : 0,
                static_cast<unsigned>(ESP.getPsramSize()),
                static_cast<unsigned>(ESP.getFreePsram()));
  Serial.printf("[%s] free SPIRAM=%u\n",
                safe_tag,
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
#else
  (void)tag;
#endif
}

static void print_malloc_probe(const char *tag) {
#if defined(ARDUINO_ARCH_ESP32)
  const char *safe_tag = (tag != nullptr) ? tag : "?";

  Serial.printf("[%s] malloc probe\n", safe_tag);

#if defined(CONFIG_SPIRAM_USE_MALLOC) && defined(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)
  Serial.printf("[%s] CONFIG_SPIRAM_USE_MALLOC=%d ALWAYSINTERNAL=%d\n",
                safe_tag,
                CONFIG_SPIRAM_USE_MALLOC,
                CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
#endif

  Serial.printf("[%s] freeHeap=%u freeInternal=%u freeSpiram=%u\n",
                safe_tag,
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

  auto print_ptr = [&](const char *label, void *ptr, size_t size) {
    Serial.printf("[%s] %s=%p ext=%d internal=%d dma=%d\n",
                  safe_tag,
                  (label != nullptr) ? label : "?",
                  ptr,
                  ptr != nullptr ? (esp_ptr_external_ram(ptr) ? 1 : 0) : 0,
                  ptr != nullptr ? (esp_ptr_internal(ptr) ? 1 : 0) : 0,
                  ptr != nullptr ? (esp_ptr_dma_capable(ptr) ? 1 : 0) : 0);
    touch_allocation(ptr, size);
  };

  constexpr size_t kSmall = 32U * 1024U;
  void *m_small = malloc(kSmall);
  print_ptr("malloc(32KiB)", m_small, kSmall);
  if (m_small != nullptr) {
    free(m_small);
  }

  constexpr size_t kBig = 1024U * 1024U;
  void *m_big = malloc(kBig);
  print_ptr("malloc(1MiB)", m_big, kBig);
  if (m_big != nullptr) {
    free(m_big);
  }

  void *ps_big = ps_malloc(kBig);
  print_ptr("ps_malloc(1MiB)", ps_big, kBig);
  if (ps_big != nullptr) {
    free(ps_big);
  }

  void *dma_small = heap_caps_malloc(kSmall, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
  print_ptr("heap_caps_malloc(INTERNAL|DMA,32KiB)", dma_small, kSmall);
  if (dma_small != nullptr) {
    heap_caps_free(dma_small);
  }

  Serial.printf("[%s] after free freeInternal=%u freeSpiram=%u\n",
                safe_tag,
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
#else
  (void)tag;
#endif
}

static void rovi_action_cb(const char *action_id, void *) {
  Serial.printf("ROVI action requested: %s\n", action_id != nullptr ? action_id : "(null)");
}

static void poll_event_lines_from_serial() {
  static char rx[1024 + 1]{};
  static size_t rx_len = 0;
  static bool rx_drop = false;
  static uint32_t last_rx_ms = 0;
  static uint32_t last_stats_ms = 0;

  static uint32_t ok_lines = 0;
  static uint32_t ingest_fail = 0;
  static uint32_t overflow_count = 0;
  static uint32_t timeout_count = 0;
  static uint32_t resync_count = 0;
  static uint32_t dropped_bytes = 0;

  auto dump_ascii = [&](const char *label, const char *buf, size_t len, bool suffix) {
    if (buf == nullptr) return;
    constexpr size_t kSnip = 80;
    const size_t n = (len > kSnip) ? kSnip : len;
    size_t start = 0;
    if (suffix && len > kSnip) start = len - kSnip;

    char out[kSnip + 1]{};
    for (size_t i = 0; i < n; ++i) {
      char ch = buf[start + i];
      out[i] = (ch >= 32 && ch <= 126) ? ch : '.';
    }
    out[n] = '\0';

    Serial.printf("EVENT: RX %s %s ascii(%u/%u): %s\n",
                  label != nullptr ? label : "?",
                  suffix ? "suffix" : "prefix",
                  static_cast<unsigned>(n),
                  static_cast<unsigned>(len),
                  out);
  };

  auto dump_hex = [&](const char *label, const char *buf, size_t len, bool suffix) {
    (void)label;
    (void)buf;
    (void)len;
    (void)suffix;
#if ROVI_RX_ERROR_HEX_DUMP
    if (buf == nullptr) return;
    constexpr size_t kBytes = 32;
    const size_t n = (len > kBytes) ? kBytes : len;
    size_t start = 0;
    if (suffix && len > kBytes) start = len - kBytes;

    // "AA " * 32 = 96 chars worst-case + null.
    char out[kBytes * 3 + 1]{};
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
      const uint8_t b = static_cast<uint8_t>(buf[start + i]);
      snprintf(out + o, sizeof(out) - o, "%02X%s", b, (i + 1 < n) ? " " : "");
      o = strlen(out);
      if (o + 4 >= sizeof(out)) break;
    }
    Serial.printf("EVENT: RX %s %s hex(%u/%u): %s\n",
                  label != nullptr ? label : "?",
                  suffix ? "suffix" : "prefix",
                  static_cast<unsigned>(n),
                  static_cast<unsigned>(len),
                  out);
#endif
  };

#if ROVI_RX_STATS_ENABLE
  if (ROVI_RX_STATS_PERIOD_MS > 0) {
    const uint32_t now_ms = millis();
    if (last_stats_ms == 0) last_stats_ms = now_ms;
    if (now_ms - last_stats_ms >= ROVI_RX_STATS_PERIOD_MS) {
      Serial.printf("EVENT: RX stats ok=%u fail=%u overflow=%u timeout=%u resync=%u dropped=%u rx_len=%u drop=%u\n",
                    static_cast<unsigned>(ok_lines),
                    static_cast<unsigned>(ingest_fail),
                    static_cast<unsigned>(overflow_count),
                    static_cast<unsigned>(timeout_count),
                    static_cast<unsigned>(resync_count),
                    static_cast<unsigned>(dropped_bytes),
                    static_cast<unsigned>(rx_len),
                    rx_drop ? 1U : 0U);
      last_stats_ms = now_ms;
    }
  }
#endif

  while (Serial.available() > 0) {
    const uint32_t now_ms = millis();
    if ((rx_len > 0 || rx_drop) && last_rx_ms != 0 && (now_ms - last_rx_ms > ROVI_RX_LINE_TIMEOUT_MS)) {
      Serial.printf("EVENT: RX line timeout, resetting (len=%u drop=%u)\n",
                    static_cast<unsigned>(rx_len),
                    rx_drop ? 1U : 0U);
      ++timeout_count;
      rx_len = 0;
      rx_drop = false;
      dropped_bytes = 0;
    }

    int c = Serial.read();
    if (c < 0) {
      break;
    }
    last_rx_ms = now_ms;

    // Accept LF, CR, or CRLF as line terminators.
    if (c == '\n' || c == '\r') {
      if (rx_drop) {
        ++resync_count;
        Serial.printf("EVENT: RX resynced after dropping %u bytes\n", static_cast<unsigned>(dropped_bytes));
        dropped_bytes = 0;
      } else {
        rx[rx_len] = '\0';
        if (rx_len > 0) {
          if (g_dashboard.ingestLine(rx)) {
            ++ok_lines;
          } else {
            ++ingest_fail;
          }
        }
      }
      rx_len = 0;
      rx_drop = false;
      continue;
    }

    if (rx_drop) {
      ++dropped_bytes;
      continue;
    }

    if (rx_len < 1024) {
      rx[rx_len++] = static_cast<char>(c);
    } else {
      ++overflow_count;
      Serial.printf("EVENT: RX line too long (max 1024), dropping (rx_len=%u overflow=%u)\n",
                    static_cast<unsigned>(rx_len),
                    static_cast<unsigned>(overflow_count));
      dump_ascii("buffer", rx, rx_len, false);
      dump_ascii("buffer", rx, rx_len, true);
      dump_hex("buffer", rx, rx_len, false);
      dump_hex("buffer", rx, rx_len, true);
      rx_len = 0;
      rx_drop = true;
      dropped_bytes = 0;
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("ROVI dashboard (config-driven) example");
  print_memory_stats("boot");
  print_malloc_probe("boot");

  if (!g_hal.begin()) {
    Serial.println("FATAL: HAL bring-up failed");
    while (true) {
      delay(1000);
    }
  }
  print_memory_stats("after_hal");
  print_malloc_probe("after_hal");

  live_dashboard::LiveDashboardOptions options{};
  options.demo_replay = (ROVI_ENABLE_JSONL_DEMO_REPLAY != 0);
  options.demo_path = "/test.jsonl";
  options.demo_period_ms = 1000;

  if (!g_dashboard.begin(g_hal.flashFs(),
                         kConfigPath,
                         g_hal.width(),
                         g_hal.height(),
                         g_hal.lvglFlashDriveLetter(),
                         options)) {
    Serial.println("Setup done (config error)");
    return;
  }
  g_dashboard_ready = true;

  Serial.printf("Config loaded: robot=%s\n", g_dashboard.robotName());

  g_dashboard.onAction("shutdown", rovi_action_cb, nullptr);
  g_dashboard.onAction("restart", rovi_action_cb, nullptr);

  Serial.println("Setup done");

  g_shots.begin();
}

void loop() {
  if (!g_dashboard_ready) {
    g_hal.loop();
    return;
  }

  g_dashboard.tick();
  g_shots.tick();
  poll_event_lines_from_serial();

  g_hal.loop();
}
