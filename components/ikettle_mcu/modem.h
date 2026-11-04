#pragma once

#include "uart_trace.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace esphome {
namespace ikettle_mcu {

using Bytes = std::vector<uint8_t>;

struct ModemCallbacks {
  std::function<void(const Bytes &)> uart_tx;
  // Acceptance of the complete local transport payload, NOT successful decoding,
  // an actuator acknowledgement, or delivery over a real TCP connection.
  std::function<bool(uint8_t, const Bytes &)> payload;
  // Invalidates the owner's local application session. Must not call back into
  // this modem synchronously. Timeout/overflow poison is recovered by begin().
  std::function<void()> reset;
  // Optional passive semantic observer. Bytes are borrowed synchronously and
  // may contain credentials: pass ONLY to a fail-closed sanitizer, never logs.
  // Must not throw, write UART or re-enter this modem. Last/defaulted member
  // preserves existing three-field aggregate callback initializers.
  std::function<void(TraceDirection, TraceKind, const uint8_t *, size_t)> trace{};
};

struct ModemCounters {
  uint64_t rx_bytes{0};
  uint64_t tx_bytes{0};
  uint64_t at_commands{0};
  uint64_t unsupported_commands{0};
  uint64_t malformed_commands{0};
  uint64_t line_overflows{0};
  uint64_t payloads_accepted{0};
  uint64_t payloads_rejected{0};
  uint64_t payload_timeouts{0};
  uint64_t poisoned_bytes_dropped{0};
  uint64_t injections_sent{0};
  uint64_t injections_rejected{0};
  uint64_t resets{0};
  uint64_t connects{0};
  uint64_t warm_resumes{0};
  uint64_t invalid_mac_addresses{0};
};

struct ModemDiagnostics {
  ModemCounters counters;
  bool begun{false};
  bool poisoned{false};
  bool link_ready{false};
  bool mac_configured{false};
  bool at_header_pending{false};
  bool payload_pending{false};
  bool mux_seen{false};
  bool server_seen{false};
  bool udp_zero_reserved{false};
  bool rf_power_seen{false};
  bool warm_resume_hint{false};
  size_t payload_expected{0};
  size_t payload_received{0};
};

// Local, virtual AT compatibility channel for the unchanged F19 MCU. This is
// NOT a transparent ESP-AT implementation or a TCP/UDP server. ESPHome owns the
// actual radio, credentials and OTA. _DEF/RESTORE/CWMODE/RFPOWER handshakes below
// neither change those settings nor persist the credentials supplied by F19.
// Only local application peer 1 exists; slot 0 is reserved for UDP compatibility
// and has no remote peer. Real Wi-Fi events must never become unsolicited
// "WIFI CONNECTED" on this UART (F19's global input-flag 513 hazard).
//
// Call from one thread; callbacks must not re-enter VirtualModem. A uart_tx
// callback must consume/write the entire Bytes argument before returning.
class VirtualModem {
 public:
  static constexpr size_t MAX_AT_LINE = 256;
  static constexpr size_t MAX_PAYLOAD = 512;
  static constexpr uint32_t PAYLOAD_TIMEOUT_MS = 1500;
  static constexpr uint32_t QUARANTINE_MS = 6000;
  static constexpr uint32_t AT_SETTLE_MS = 100;

  explicit VirtualModem(ModemCallbacks callbacks);

  // Supply the real ESPHome device MAC in XX:XX:XX:XX:XX:XX form. Invalid input
  // clears the value; CIPAPMAC_CUR? returns ERROR rather than inventing an ID.
  void set_mac_address(std::string address);

  // Explicit local transport reset: clears poison/handshake/parser, not counters
  // or the supplied MAC; invalidates the application session and emits ready.
  // A hardware reset of the original MCU should also call begin().
  void begin(uint32_t now);
  void feed(const uint8_t *data, size_t size, uint32_t now);
  void tick(uint32_t now);

  bool can_inject(uint32_t now) const;
  // false means ZERO UART bytes written. Empty payloads are rejected: F19's
  // +IPD parser needs an application opcode, even though CIPSEND length 0 is
  // supported in the opposite direction. true writes one +IPD,1,N: envelope.
  bool inject(const Bytes &payload, uint32_t now);
  bool link_ready() const;
  const ModemCounters &counters() const;
  ModemDiagnostics diagnostics() const;

 private:
  void emit_(const Bytes &bytes);
  void trace_notice_(const char *code);
  void text_(const std::string &text);
  void ok_();
  void error_();
  void reset_state_(uint32_t now);
  void reset_handshake_();
  void pause_(uint32_t now, uint32_t duration);
  bool paused_(uint32_t now) const;
  void expire_payload_(uint32_t now);
  void poison_();
  void process_line_(uint32_t now);
  void process_send_(const std::string &command, uint32_t now);
  void finish_payload_();
  static bool unsigned_number_(const std::string &text, size_t limit, size_t &value);
  static bool quoted_(const std::string &text, size_t &position, size_t minimum,
                      size_t maximum, size_t &length);
  static bool joined_credentials_(const std::string &command);
  static bool ap_configuration_(const std::string &command);

  ModemCallbacks callbacks_;
  ModemCounters counters_;
  std::string mac_address_;
  std::string line_;
  Bytes payload_;
  bool begun_{false};
  bool poisoned_{false};
  bool line_cr_{false};
  bool discard_line_{false};
  bool payload_pending_{false};
  bool mux_seen_{false};
  bool server_seen_{false};
  bool udp_seen_{false};
  bool rf_seen_{false};
  bool warm_resume_hint_{false};
  bool link_announced_{false};
  bool pause_active_{false};
  size_t payload_expected_{0};
  uint8_t payload_link_{1};
  uint32_t payload_started_at_{0};
  uint32_t pause_until_{0};
};

}  // namespace ikettle_mcu
}  // namespace esphome
