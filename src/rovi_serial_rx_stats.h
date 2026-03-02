#pragma once

namespace rovi::serial_rx_stats {

// Configure Serial RX buffering and register event callbacks.
// Must be called before `Serial.begin(...)`.
void configure_before_serial_begin();

// Print event stats when they change.
void tick();

}  // namespace rovi::serial_rx_stats

