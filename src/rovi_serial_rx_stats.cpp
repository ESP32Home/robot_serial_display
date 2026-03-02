#include "rovi_serial_rx_stats.h"

#include <Arduino.h>

#if defined(ARDUINO_ARCH_ESP32) && ARDUINO_USB_CDC_ON_BOOT

#if !ARDUINO_USB_MODE
#include "USBCDC.h"

static volatile uint32_t g_cdc_rx_overflow_events = 0;
static volatile uint32_t g_cdc_rx_overflow_dropped_bytes = 0;

static void usb_cdc_event_cb(void *, esp_event_base_t, int32_t event_id, void *event_data) {
  if (event_id != ARDUINO_USB_CDC_RX_OVERFLOW_EVENT) {
    return;
  }

  ++g_cdc_rx_overflow_events;

  const auto *data = static_cast<const arduino_usb_cdc_event_data_t *>(event_data);
  if (data != nullptr) {
    g_cdc_rx_overflow_dropped_bytes += static_cast<uint32_t>(data->rx_overflow.dropped_bytes);
  }
}

#else
#include "HWCDC.h"

static volatile uint32_t g_hw_cdc_rx_events = 0;
static volatile uint32_t g_hw_cdc_rx_bytes = 0;
static volatile uint32_t g_hw_cdc_rx_max_len = 0;

static void hw_cdc_event_cb(void *, esp_event_base_t, int32_t event_id, void *event_data) {
  if (event_id != ARDUINO_HW_CDC_RX_EVENT) {
    return;
  }

  ++g_hw_cdc_rx_events;

  const auto *data = static_cast<const arduino_hw_cdc_event_data_t *>(event_data);
  const uint32_t len = (data != nullptr) ? static_cast<uint32_t>(data->rx.len) : 0U;
  g_hw_cdc_rx_bytes += len;
  if (len > g_hw_cdc_rx_max_len) {
    g_hw_cdc_rx_max_len = len;
  }
}
#endif

namespace rovi::serial_rx_stats {

void configure_before_serial_begin() {
  Serial.setRxBufferSize(1024);
#if !ARDUINO_USB_MODE
  Serial.onEvent(ARDUINO_USB_CDC_RX_OVERFLOW_EVENT, usb_cdc_event_cb);
#else
  Serial.onEvent(ARDUINO_HW_CDC_RX_EVENT, hw_cdc_event_cb);
#endif
}

void tick() {
#if !ARDUINO_USB_MODE
  static uint32_t last_overflow_events = 0;
  if (last_overflow_events != g_cdc_rx_overflow_events) {
    last_overflow_events = g_cdc_rx_overflow_events;
    Serial.printf("EVENT: USB CDC RX overflow events=%u dropped_bytes=%u\n",
                  static_cast<unsigned>(g_cdc_rx_overflow_events),
                  static_cast<unsigned>(g_cdc_rx_overflow_dropped_bytes));
  }
#else
  static uint32_t last_hw_rx_events = 0;
  if (last_hw_rx_events != g_hw_cdc_rx_events) {
    last_hw_rx_events = g_hw_cdc_rx_events;
    Serial.printf("EVENT: HW CDC RX events=%u bytes=%u max_len=%u\n",
                  static_cast<unsigned>(g_hw_cdc_rx_events),
                  static_cast<unsigned>(g_hw_cdc_rx_bytes),
                  static_cast<unsigned>(g_hw_cdc_rx_max_len));
  }
#endif
}

}  // namespace rovi::serial_rx_stats

#else

namespace rovi::serial_rx_stats {

void configure_before_serial_begin() {}
void tick() {}

}  // namespace rovi::serial_rx_stats

#endif
