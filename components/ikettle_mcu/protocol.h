#pragma once

#include "modem.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>

namespace esphome {
namespace ikettle_mcu {

struct KettleState {
  bool identity_known{false};
  bool supported{false};
  bool status_known{false};
  bool profile_valid{false};
  bool on_base{false};
  uint8_t state_code{0};
  float temp{std::numeric_limits<float>::quiet_NaN()};
  uint16_t water_raw{0};
  uint32_t last_status_ms{0};
  uint32_t status_sequence{0};
  bool defaults_known{false};
  uint8_t default_temperature{0};
  uint8_t default_keep_minutes{0};
  uint8_t default_formula_temperature{0};
  bool base_known{false};
  uint16_t base_raw{0};
  std::string wifi_version;
  uint8_t firmware_version{0};
  uint8_t device_type{0};
  // Every public state/diagnostic change, unlike status_sequence (14 only).
  uint32_t revision{0};
};

enum class CommandStatus : uint8_t {
  NONE, SENT, HEATING, STOPPED, KEEP_WARM_STOPPED, NOT_READY, BUSY, NOT_IDENTIFIED, NO_FRESH_STATUS,
  OFF_BASE, ALREADY_ACTIVE, INVALID_ARGUMENTS, SEND_REJECTED, MCU_REJECTED, NO_ACK,
  NO_MATCHING_STATUS, CONNECTION_LOST,
};

struct SessionDiagnostics {
  std::string last_error{"none"};  // Fixed codes only, maximum 48 characters.
  uint32_t last_error_ms{0};
  uint32_t requests_sent{0};
  uint32_t requests_completed{0};
  uint32_t timeouts{0};
  uint32_t protocol_errors{0};
  uint32_t rejected_actions{0};
  uint32_t actuator_requests{0};
  uint32_t actuator_acks{0};
  uint32_t negative_acks{0};
  uint32_t transport_rejections{0};
  uint32_t ignored_links{0};
  uint32_t received_frames{0};
  uint32_t last_valid_reply_ms{0};
  bool valid_reply_seen{false};
  uint32_t dropped_reads{0};
  uint32_t resets{0};
  uint32_t recoveries{0};
  uint32_t session_epoch{1};
  size_t queued_reads{0};
  bool in_flight{false};
  bool awaiting_start_status{false};
  std::string in_flight_name{"none"};
  CommandStatus command_status{CommandStatus::NONE};
};

class KettleSession {
 public:
  using Send = std::function<bool(const Bytes &, uint32_t)>;
  static constexpr uint32_t REQUEST_TIMEOUT_MS = 3000;
  static constexpr uint32_t IDENTITY_BACKOFF_MS = 2000;
  static constexpr uint32_t STATUS_FRESH_MS = 10000;
  static constexpr size_t MAX_QUEUED_READS = 8;
  static constexpr size_t MAX_RX_BYTES = 512;

  explicit KettleSession(Send send);
  void reset();
  bool receive(uint8_t link, const Bytes &payload, uint32_t now);
  void tick(uint32_t now, bool modem_connected, bool writable = true);
  // true means sent once to the modem, NOT ACKed or physically successful.
  // Actions are never queued. Busy/unsafe/offline actions return false.
  bool start(uint8_t temperature, uint8_t keep_minutes, bool formula, uint32_t now);
  bool stop(uint32_t now);
  bool stop_keep_warm(uint32_t now);
  // Coalesced readonly refresh: fresh idle adds metadata/6A; otherwise 64 only.
  // An explicit call may retry a previously unsupported/unidentified identity.
  bool refresh(uint32_t now);
  const KettleState &state() const { return state_; }
  bool status_fresh(uint32_t now) const;
  const SessionDiagnostics &diagnostics() const { return diagnostics_; }

 private:
  enum class Kind : uint8_t { NONE, IDENTITY, DEFAULTS, BASE, WIFI, START, FORMULA, STOP, STOP_WARM };
  struct Pending {
    Kind kind{Kind::NONE};
    uint32_t epoch{0};
    uint32_t sent_at{0};
    bool typed{false};
    bool ack{false};
    uint8_t temperature{0};
    uint8_t keep{0};
    uint8_t formula_temperature{0};
    uint16_t base{0};
    std::string wifi;
  };

  struct CommandConfirmation {
    Kind kind{Kind::NONE};
    uint32_t sent_at{0};
    bool ack{false};
    bool was_keeping_warm{false};
  };

  static bool action_(Kind kind);
  static bool idle_read_(Kind kind);
  static bool needs_ack_(Kind kind);
  static uint8_t response_(Kind kind);
  static const char *name_(Kind kind);
  bool idle_(uint32_t now) const;
  bool action_allowed_(Kind kind, uint32_t now);
  bool reject_action_(const char *code, CommandStatus status, uint32_t now);
  void command_status_(CommandStatus status);
  void finish_confirmation_(CommandStatus status);
  void confirm_from_status_(uint32_t now);
  bool begin_(Kind kind, uint32_t now, uint8_t temperature = 0, uint8_t keep = 0);
  bool enqueue_(Kind kind);
  Kind pop_();
  bool queued_(Kind kind) const;
  void clear_queue_();
  void drop_idle_reads_();
  void sync_();
  void error_(const char *code, uint32_t now);
  void clear_state_();
  void recover_(const char *code, uint32_t now, bool preserve_status = false, bool preserve_rx = false);
  void finish_();
  bool frame_(const Bytes &frame, uint32_t now);

  Send send_;
  KettleState state_;
  SessionDiagnostics diagnostics_;
  Pending pending_;
  CommandConfirmation confirmation_;
  std::array<Kind, MAX_QUEUED_READS> queue_{};
  size_t queue_size_{0};
  Bytes rx_;
  bool modem_ready_{false};
  bool writable_{false};
  bool receiving_{false};
  bool awaiting_start_status_{false};
  bool start_acked_{false};
  bool defaults_scheduled_{false};
  bool base_scheduled_{false};
  bool identity_blocked_{false};
  bool retry_waiting_{false};
  uint32_t retry_since_{0};
  bool rx_partial_{false};
  uint32_t rx_since_{0};
};

}  // namespace ikettle_mcu
}  // namespace esphome
