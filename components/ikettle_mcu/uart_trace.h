#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace esphome {
namespace ikettle_mcu {

enum class TraceDirection : uint8_t { MCU_TO_ESP, ESP_TO_MCU };
enum class TraceKind : uint8_t { AT_COMMAND, UART_OUTPUT, PAYLOAD, NOTICE };

struct TraceCounters {
  uint32_t rx_events{0}, tx_events{0}, seen_events{0}, emitted_events{0}, dropped_events{0};
  uint32_t rx_bytes{0}, tx_bytes{0};
};

// Passive, fail-closed semantic trace. Raw borrowed bytes are inspected only
// synchronously by record(); ONLY sanitized fixed-size lines enter the queue.
// No UART, socket, application command, persistence or heater access exists.
// Single-loop use; sinks/callbacks must not throw or re-enter the modem.
class UartTrace {
 public:
  static constexpr size_t QUEUE_CAPACITY = 8;
  static constexpr size_t MAX_LINE = 160;
  static constexpr size_t MAX_DRAIN = 2;
  using Sink = std::function<void(const char *)>;

  // Invalid settings return false and leave the previous settings untouched.
  // Settings are for the next enable window; configure does not extend a window.
  bool configure(uint32_t duration_seconds = 120, uint8_t rate = 8);
  // OFF is the construction/default state. Turning off clears pending lines.
  // Repeated ON while already on does not restart/extend the deadline.
  void set_enabled(bool enabled, uint32_t now);
  void tick(uint32_t now);
  bool enabled() const { return enabled_; }
  void record(TraceDirection direction, TraceKind kind, const uint8_t *data, size_t length, uint32_t now);
  // Calls sink at most twice, each pointer borrowed until its callback returns.
  size_t drain(uint32_t now, const Sink &sink);
  const TraceCounters &counters() const { return counters_; }
  size_t queued() const { return count_; }
  std::string summary() const;

 private:
  static std::string sanitize_(TraceDirection direction, TraceKind kind, const uint8_t *data, size_t length);
  void clear_();
  std::array<std::array<char, MAX_LINE + 1>, QUEUE_CAPACITY> queue_{};
  TraceCounters counters_;
  size_t head_{0}, count_{0};
  bool enabled_{false};
  uint32_t duration_ms_{120000}, active_duration_ms_{120000}, enabled_at_{0}, rate_window_at_{0};
  uint8_t rate_{8}, active_rate_{8}, rate_used_{0};
};

}  // namespace ikettle_mcu
}  // namespace esphome
