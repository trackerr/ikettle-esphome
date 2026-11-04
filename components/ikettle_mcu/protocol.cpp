#include "protocol.h"

#include <algorithm>
#include <utility>

namespace esphome {
namespace ikettle_mcu {

KettleSession::KettleSession(Send send) : send_(std::move(send)) {}

bool KettleSession::action_(Kind kind) {
  return kind == Kind::START || kind == Kind::FORMULA || kind == Kind::STOP || kind == Kind::STOP_WARM;
}
bool KettleSession::idle_read_(Kind kind) { return kind == Kind::DEFAULTS || kind == Kind::BASE || kind == Kind::WIFI; }
bool KettleSession::needs_ack_(Kind kind) { return action_(kind) || kind == Kind::DEFAULTS || kind == Kind::BASE; }
uint8_t KettleSession::response_(Kind kind) {
  switch (kind) {
    case Kind::IDENTITY: return 0x65;
    case Kind::DEFAULTS: return 0x2F;
    case Kind::BASE: return 0x2D;
    case Kind::WIFI: return 0x6B;
    default: return 0;
  }
}
const char *KettleSession::name_(Kind kind) {
  switch (kind) {
    case Kind::IDENTITY: return "identity";
    case Kind::DEFAULTS: return "defaults";
    case Kind::BASE: return "base";
    case Kind::WIFI: return "wifi_version";
    case Kind::START: return "start";
    case Kind::FORMULA: return "formula";
    case Kind::STOP: return "stop";
    case Kind::STOP_WARM: return "stop_keep_warm";
    default: return "none";
  }
}
void KettleSession::sync_() {
  diagnostics_.queued_reads = queue_size_;
  diagnostics_.in_flight = pending_.kind != Kind::NONE;
  diagnostics_.awaiting_start_status = awaiting_start_status_;
  diagnostics_.in_flight_name = name_(pending_.kind);
}
void KettleSession::error_(const char *code, uint32_t now) {
  diagnostics_.last_error.assign(code, std::min<size_t>(std::char_traits<char>::length(code), 48));
  diagnostics_.last_error_ms = now;
  ++state_.revision;
}
void KettleSession::command_status_(CommandStatus status) {
  if (diagnostics_.command_status != status) {
    diagnostics_.command_status = status;
    ++state_.revision;
  }
}
void KettleSession::finish_confirmation_(CommandStatus status) {
  if (confirmation_.kind == Kind::NONE) return;
  confirmation_ = CommandConfirmation{};
  command_status_(status);
}
void KettleSession::clear_queue_() {
  diagnostics_.dropped_reads += static_cast<uint32_t>(queue_size_);
  queue_size_ = 0;
  sync_();
}
void KettleSession::clear_state_() {
  const uint32_t revision = state_.revision + 1;
  state_ = KettleState{};
  state_.revision = revision;
}
void KettleSession::reset() {
  finish_confirmation_(CommandStatus::CONNECTION_LOST);
  clear_state_();
  clear_queue_();
  pending_ = Pending{};
  rx_.clear();
  rx_partial_ = false;
  modem_ready_ = false;
  writable_ = false;
  receiving_ = false;
  awaiting_start_status_ = start_acked_ = false;
  defaults_scheduled_ = base_scheduled_ = identity_blocked_ = retry_waiting_ = false;
  ++diagnostics_.session_epoch;
  ++diagnostics_.resets;
  sync_();
}
void KettleSession::recover_(const char *code, uint32_t now, bool preserve_status, bool preserve_rx) {
  const bool command_timed_out = action_(pending_.kind) &&
      static_cast<uint32_t>(now - pending_.sent_at) >= REQUEST_TIMEOUT_MS;
  finish_confirmation_(command_timed_out ? CommandStatus::NO_ACK : CommandStatus::CONNECTION_LOST);
  ++diagnostics_.recoveries;
  ++diagnostics_.session_epoch;
  error_(code, now);
  pending_ = Pending{};
  clear_queue_();
  awaiting_start_status_ = start_acked_ = false;
  if (!preserve_rx) { rx_.clear(); rx_partial_ = false; }
  state_.identity_known = false;
  state_.supported = false;
  state_.defaults_known = false;
  state_.base_known = false;
  state_.wifi_version.clear();
  if (!preserve_status) {
    state_.status_known = false;
    state_.on_base = false;
    state_.temp = std::numeric_limits<float>::quiet_NaN();
  }
  state_.profile_valid = false;
  defaults_scheduled_ = base_scheduled_ = identity_blocked_ = false;
  retry_waiting_ = true;
  retry_since_ = now;
  sync_();
}
bool KettleSession::status_fresh(uint32_t now) const {
  return modem_ready_ && state_.status_known && state_.profile_valid &&
         static_cast<uint32_t>(now - state_.last_status_ms) < STATUS_FRESH_MS;
}
bool KettleSession::idle_(uint32_t now) const { return status_fresh(now) && state_.state_code == 0; }
bool KettleSession::queued_(Kind kind) const {
  if (pending_.kind == kind) return true;
  for (size_t i = 0; i < queue_size_; ++i) if (queue_[i] == kind) return true;
  return false;
}
bool KettleSession::enqueue_(Kind kind) {
  if (queued_(kind)) return true;
  if (action_(kind) || queue_size_ >= MAX_QUEUED_READS) return false;
  queue_[queue_size_++] = kind;
  sync_();
  return true;
}
KettleSession::Kind KettleSession::pop_() {
  if (!queue_size_) return Kind::NONE;
  const Kind kind = queue_[0];
  for (size_t i = 1; i < queue_size_; ++i) queue_[i - 1] = queue_[i];
  --queue_size_;
  sync_();
  return kind;
}
void KettleSession::drop_idle_reads_() {
  size_t kept = 0;
  for (size_t i = 0; i < queue_size_; ++i) {
    const Kind kind = queue_[i];
    if (idle_read_(kind)) {
      ++diagnostics_.dropped_reads;
      if (kind == Kind::DEFAULTS && !state_.defaults_known) defaults_scheduled_ = false;
      if (kind == Kind::BASE && !state_.base_known) base_scheduled_ = false;
    } else queue_[kept++] = kind;
  }
  queue_size_ = kept;
  sync_();
}
bool KettleSession::action_allowed_(Kind kind, uint32_t now) {
  if (!modem_ready_) return reject_action_("not_ready", CommandStatus::NOT_READY, now);
  if (!writable_) return reject_action_("transport_busy", CommandStatus::BUSY, now);
  if (receiving_ || pending_.kind != Kind::NONE || queue_size_ || !rx_.empty())
    return reject_action_("busy", CommandStatus::BUSY, now);
  if (!state_.identity_known || !state_.supported)
    return reject_action_("not_identified", CommandStatus::NOT_IDENTIFIED, now);
  if (kind == Kind::START || kind == Kind::FORMULA) {
    if (awaiting_start_status_)
      return reject_action_("awaiting_post_start_status", CommandStatus::BUSY, now);
    if (!status_fresh(now))
      return reject_action_("not_fresh_idle_on_base", CommandStatus::NO_FRESH_STATUS, now);
    if (!state_.on_base)
      return reject_action_("not_fresh_idle_on_base", CommandStatus::OFF_BASE, now);
    if (state_.state_code != 0)
      return reject_action_("not_fresh_idle_on_base", CommandStatus::ALREADY_ACTIVE, now);
  }
  return true;
}
bool KettleSession::reject_action_(const char *code, CommandStatus status, uint32_t now) {
  ++diagnostics_.rejected_actions;
  error_(code, now);
  command_status_(status);
  return false;
}
bool KettleSession::begin_(Kind kind, uint32_t now, uint8_t temperature, uint8_t keep) {
  if (action_(kind) && !action_allowed_(kind, now)) return false;
  if (!modem_ready_ || !writable_ || pending_.kind != Kind::NONE || !rx_.empty()) return false;
  if (kind != Kind::IDENTITY && (!state_.identity_known || !state_.supported)) return false;
  if (idle_read_(kind) && !idle_(now)) return false;
  Bytes bytes;
  switch (kind) {
    case Kind::IDENTITY: bytes = {0x64, 0x7E}; break;
    case Kind::DEFAULTS: bytes = {0x2E, 0x7E}; break;
    case Kind::BASE: bytes = {0x2B, 0x7E}; break;
    case Kind::WIFI: bytes = {0x6A, 0x7E}; break;
    case Kind::START: bytes = {0x15, temperature, keep, 0x7E}; break;
    case Kind::FORMULA: bytes = {0x19, temperature, keep, 0x7E}; break;
    case Kind::STOP: bytes = {0x16, 0x7E}; break;
    case Kind::STOP_WARM: bytes = {0x30, 0x7E}; break;
    default: return false;
  }
  pending_ = Pending{};
  pending_.kind = kind;
  pending_.epoch = diagnostics_.session_epoch;
  pending_.sent_at = now;
  sync_();
  if (!send_ || !send_(bytes, now)) {
    ++diagnostics_.transport_rejections;
    if (action_(kind)) {
      pending_ = Pending{};
      sync_();
      return reject_action_("send_rejected", CommandStatus::SEND_REJECTED, now);
    } else recover_("send_rejected", now);
    return false;
  }
  ++diagnostics_.requests_sent;
  if (kind == Kind::START || kind == Kind::FORMULA) {
    awaiting_start_status_ = true;
    start_acked_ = false;
    sync_();
  }
  if (action_(kind)) {
    ++diagnostics_.actuator_requests;
    confirmation_ = CommandConfirmation{kind, now, false, status_fresh(now) && state_.state_code == 2 && state_.on_base};
    command_status_(CommandStatus::SENT);
  }
  ++state_.revision;
  return true;
}
void KettleSession::finish_() {
  if (pending_.kind == Kind::NONE || pending_.epoch != diagnostics_.session_epoch) return;
  if ((response_(pending_.kind) && !pending_.typed) || (needs_ack_(pending_.kind) && !pending_.ack)) return;
  switch (pending_.kind) {
    case Kind::DEFAULTS:
      state_.defaults_known = true;
      state_.default_temperature = pending_.temperature;
      state_.default_keep_minutes = pending_.keep;
      state_.default_formula_temperature = pending_.formula_temperature;
      break;
    case Kind::BASE: state_.base_known = true; state_.base_raw = pending_.base; break;
    case Kind::WIFI: state_.wifi_version = pending_.wifi; break;
    default: break;
  }
  if (pending_.kind == Kind::START || pending_.kind == Kind::FORMULA)
    start_acked_ = true;
  if (action_(pending_.kind)) {
    ++diagnostics_.actuator_acks;
    confirmation_.ack = true;
  }
  ++diagnostics_.requests_completed;
  pending_ = Pending{};
  sync_();
  ++state_.revision;
}
void KettleSession::confirm_from_status_(uint32_t now) {
  if (!confirmation_.ack) return;
  if (static_cast<uint32_t>(now - confirmation_.sent_at) >= STATUS_FRESH_MS) {
    finish_confirmation_(CommandStatus::NO_MATCHING_STATUS);
    return;
  }
  const bool idle = state_.state_code == 0 || state_.state_code == 3;
  switch (confirmation_.kind) {
    case Kind::START: case Kind::FORMULA:
      if (!state_.on_base) finish_confirmation_(CommandStatus::OFF_BASE);
      else if (state_.state_code == 1) finish_confirmation_(CommandStatus::HEATING);
      break;
    case Kind::STOP:
      if (idle) finish_confirmation_(CommandStatus::STOPPED);
      break;
    case Kind::STOP_WARM:
      if (confirmation_.was_keeping_warm && idle) finish_confirmation_(CommandStatus::KEEP_WARM_STOPPED);
      break;
    default: break;
  }
}
bool KettleSession::frame_(const Bytes &frame, uint32_t now) {
  ++diagnostics_.received_frames;
  const uint8_t code = frame[0];
  if (code == 0x14) {
    state_.status_known = true;
    state_.state_code = frame[1];
    state_.on_base = frame[2] != 0x7F;
    state_.temp = state_.on_base ? static_cast<float>(frame[2] < 128 ? frame[2] : static_cast<int>(frame[2]) - 256)
                                : std::numeric_limits<float>::quiet_NaN();
    state_.water_raw = static_cast<uint16_t>((static_cast<uint16_t>(frame[3]) << 8) | frame[4]);
    state_.last_status_ms = now;
    ++state_.status_sequence;
    ++state_.revision;
    state_.profile_valid = frame[1] <= 4 && frame[5] == 0;
    if (!state_.profile_valid) {
      ++diagnostics_.protocol_errors;
      recover_("profile_invalid", now, true, true);
      return false;
    }
    // The ACK is untagged and F19 can mask a rejected physical start. Until a
    // status AFTER its ACK arrives, the earlier idle frame cannot justify
    // another start/formula request. Stop remains an explicit best-effort action.
    if (awaiting_start_status_ && start_acked_) {
      awaiting_start_status_ = start_acked_ = false;
      sync_();
    }
    confirm_from_status_(now);
    if (frame[1] != 0) drop_idle_reads_();
    return true;
  }
  if (pending_.kind == Kind::NONE || pending_.epoch != diagnostics_.session_epoch) {
    ++diagnostics_.protocol_errors;
    recover_(code == 0x03 ? "unexpected_ack" : "unexpected_reply", now, false, true);
    return false;
  }
  if (code == 0x03) {
    if (frame[1] != 0) {
      ++diagnostics_.negative_acks;
      if (action_(pending_.kind)) {
        if (pending_.kind == Kind::START || pending_.kind == Kind::FORMULA)
          awaiting_start_status_ = start_acked_ = false;
        finish_confirmation_(CommandStatus::MCU_REJECTED);
        pending_ = Pending{};
        error_("ack_rejected", now);
        sync_();
      } else recover_("ack_rejected", now, false, true);
      return false;
    }
    if (!needs_ack_(pending_.kind) || pending_.ack || (response_(pending_.kind) && !pending_.typed)) {
      ++diagnostics_.protocol_errors;
      recover_("unexpected_ack", now, false, true);
      return false;
    }
    pending_.ack = true;
    finish_();
    return true;
  }
  if (code != response_(pending_.kind) || pending_.typed) {
    ++diagnostics_.protocol_errors;
    recover_("unexpected_reply", now, false, true);
    return false;
  }
  pending_.typed = true;
  if (code == 0x65) {
    const uint8_t type = frame[1], firmware = frame[2];
    if (type != 1 || firmware != 19) {
      ++diagnostics_.protocol_errors;
      recover_("unsupported_identity", now, false, true);
      state_.device_type = type;
      state_.firmware_version = firmware;
      state_.identity_known = true;
      identity_blocked_ = true;
      return false;
    }
    state_.device_type = type;
    state_.firmware_version = firmware;
    state_.identity_known = state_.supported = true;
    retry_waiting_ = false;
  } else if (code == 0x2F) {
    if (frame[1] > 100 || frame[2] > 30 || frame[3] > 100) {
      ++diagnostics_.protocol_errors;
      recover_("invalid_defaults", now, false, true);
      return false;
    }
    pending_.temperature = frame[1];
    pending_.keep = frame[2];
    pending_.formula_temperature = frame[3];
  } else if (code == 0x2D) {
    pending_.base = static_cast<uint16_t>((static_cast<uint16_t>(frame[1]) << 8) | frame[2]);
  } else if (code == 0x6B) {
    for (size_t i = 1; i + 1 < frame.size(); ++i) {
      const uint8_t value = frame[i];
      if (value != 9 && value != 10 && value != 13 && (value < 0x20 || value > 0x7D)) {
        ++diagnostics_.protocol_errors;
        recover_("invalid_wifi_text", now, false, true);
        return false;
      }
    }
    pending_.wifi.assign(frame.begin() + 1, frame.end() - 1);
  }
  finish_();
  return true;
}
bool KettleSession::receive(uint8_t link, const Bytes &payload, uint32_t now) {
  if (link != 1) { ++diagnostics_.ignored_links; return false; }
  if (receiving_ || payload.empty() || payload.size() > MAX_RX_BYTES - rx_.size()) {
    ++diagnostics_.protocol_errors;
    recover_("receive_bounds", now);
    return false;
  }
  receiving_ = true;
  rx_.insert(rx_.end(), payload.begin(), payload.end());
  bool ok = true, completed = false;
  while (!rx_.empty()) {
    size_t length = 0, tail = 0;
    switch (rx_[0]) {
      case 0x03: length = 3; break;
      case 0x14: length = 7; break;
      case 0x65: case 0x2D: length = 4; break;
      case 0x2F: length = 6; tail = 4; break;
      case 0x6B: {
        const auto end = std::find(rx_.begin() + 1, rx_.end(), static_cast<uint8_t>(0x7E));
        if (end == rx_.end()) {
          if (rx_.size() < 128) break;
        } else {
          length = static_cast<size_t>(end - rx_.begin()) + 1;
          if (length <= 128) break;
        }
        ++diagnostics_.protocol_errors;
        recover_("wifi_frame_bounds", now);
        receiving_ = false;
        return false;
      }
      default:
        ++diagnostics_.protocol_errors;
        recover_("unknown_frame", now);
        receiving_ = false;
        return false;
    }
    if (!length || rx_.size() < length) break;
    if (rx_[tail ? tail : length - 1] != 0x7E) {
      ++diagnostics_.protocol_errors;
      recover_("frame_boundary", now);
      receiving_ = false;
      return false;
    }
    Bytes frame(rx_.begin(), rx_.begin() + length);
    rx_.erase(rx_.begin(), rx_.begin() + length);
    completed = true;
    if (frame_(frame, now)) {
      diagnostics_.last_valid_reply_ms = now;
      diagnostics_.valid_reply_seen = true;
    } else {
      ok = false;
    }
  }
  if (rx_.empty()) rx_partial_ = false;
  else if (!rx_partial_ || completed) { rx_partial_ = true; rx_since_ = now; }
  receiving_ = false;
  sync_();
  return ok;
}
void KettleSession::tick(uint32_t now, bool modem_connected, bool writable) {
  if (!modem_connected) {
    if (modem_ready_ || pending_.kind != Kind::NONE || state_.identity_known || state_.status_known) reset();
    writable_ = false;
    return;
  }
  if (!modem_ready_) {
    clear_state_();
    clear_queue_();
    pending_ = Pending{};
    rx_.clear();
    rx_partial_ = false;
    defaults_scheduled_ = base_scheduled_ = identity_blocked_ = retry_waiting_ = false;
    ++diagnostics_.session_epoch;
    modem_ready_ = true;
    sync_();
  }
  writable_ = writable;
  if (pending_.kind != Kind::NONE && static_cast<uint32_t>(now - pending_.sent_at) >= REQUEST_TIMEOUT_MS) {
    ++diagnostics_.timeouts;
    recover_("request_timeout", now);
    return;
  }
  if (rx_partial_ && static_cast<uint32_t>(now - rx_since_) >= REQUEST_TIMEOUT_MS) {
    ++diagnostics_.timeouts;
    recover_("partial_frame_timeout", now);
    return;
  }
  if (confirmation_.ack && static_cast<uint32_t>(now - confirmation_.sent_at) >= STATUS_FRESH_MS)
    finish_confirmation_(CommandStatus::NO_MATCHING_STATUS);
  if (!writable_ || pending_.kind != Kind::NONE || !rx_.empty() || receiving_) return;
  if (!state_.identity_known || !state_.supported) {
    if (identity_blocked_) return;
    if (retry_waiting_ && static_cast<uint32_t>(now - retry_since_) < IDENTITY_BACKOFF_MS) return;
    // An explicit refresh may already have queued the same identity barrier.
    if (queue_size_ && queue_[0] == Kind::IDENTITY) pop_();
    begin_(Kind::IDENTITY, now);
    return;
  }
  if (!idle_(now)) drop_idle_reads_();
  else {
    if (!defaults_scheduled_) defaults_scheduled_ = enqueue_(Kind::DEFAULTS);
    if (!base_scheduled_) base_scheduled_ = enqueue_(Kind::BASE);
  }
  if (queue_size_) begin_(pop_(), now);
}
bool KettleSession::start(uint8_t temperature, uint8_t keep_minutes, bool formula, uint32_t now) {
  if (temperature < 20 || temperature > 100 || (keep_minutes != 0 && (keep_minutes < 5 || keep_minutes > 30)))
    return reject_action_("invalid_action_arguments", CommandStatus::INVALID_ARGUMENTS, now);
  return begin_(formula ? Kind::FORMULA : Kind::START, now, temperature, keep_minutes);
}
bool KettleSession::stop(uint32_t now) { return begin_(Kind::STOP, now); }
bool KettleSession::stop_keep_warm(uint32_t now) { return begin_(Kind::STOP_WARM, now); }
bool KettleSession::refresh(uint32_t now) {
  if (!modem_ready_) { error_("not_ready", now); return false; }
  if (!writable_) { error_("transport_busy", now); return false; }
  if (action_(pending_.kind)) { error_("busy", now); return false; }
  if (!state_.identity_known || !state_.supported) {
    identity_blocked_ = false;
    retry_waiting_ = false;  // Explicit operator request, not an automatic retry.
  }
  const bool metadata = state_.identity_known && state_.supported && idle_(now);
  if (!metadata) drop_idle_reads_();
  bool result = enqueue_(Kind::IDENTITY);
  if (metadata) result = result && enqueue_(Kind::DEFAULTS) && enqueue_(Kind::BASE) && enqueue_(Kind::WIFI);
  ++state_.revision;
  return result;
}

}  // namespace ikettle_mcu
}  // namespace esphome
