#include "ikettle_mcu.h"

#include <cmath>
#include "esphome/core/alloc_helpers.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::ikettle_mcu {
static const char *const TAG = "ikettle_mcu";

static const char *command_status_text(CommandStatus status) {
  switch (status) {
    case CommandStatus::SENT: return "Command sent";
    case CommandStatus::HEATING: return "Heating";
    case CommandStatus::STOPPED: return "Stopped";
    case CommandStatus::KEEP_WARM_STOPPED: return "Keep warm stopped";
    case CommandStatus::NOT_READY: return "No connection to kettle controller";
    case CommandStatus::BUSY: return "Kettle communication busy";
    case CommandStatus::NOT_IDENTIFIED: return "Kettle controller not identified";
    case CommandStatus::NO_FRESH_STATUS: return "No recent status";
    case CommandStatus::OFF_BASE: return "Kettle is off the base";
    case CommandStatus::ALREADY_ACTIVE: return "Kettle is already active";
    case CommandStatus::INVALID_ARGUMENTS: return "Invalid temperature or keep-warm duration";
    case CommandStatus::SEND_REJECTED: return "Could not send command";
    case CommandStatus::MCU_REJECTED: return "Kettle controller rejected command";
    case CommandStatus::NO_ACK: return "No response to command";
    case CommandStatus::NO_MATCHING_STATUS: return "Kettle status did not confirm the command";
    case CommandStatus::CONNECTION_LOST: return "Connection to kettle controller lost";
    case CommandStatus::NONE: return "No command";
  }
  return "No command";
}

void KettleNumber::control(float value) {
  if (parent_ != nullptr)
    parent_->select(kind_, value);
}

void KettleButton::press_action() {
  if (parent_ != nullptr)
    parent_->perform(action_);
}

void KettleTraceSwitch::write_state(bool value) {
  if (parent_ != nullptr)
    parent_->set_uart_trace_enabled(value);
  else
    publish_state(false);
}

void IKettleComponent::set_uart_trace_enabled(bool value) {
  const uint32_t now = millis();
  const bool enable = value && trace_configured_;
  uart_trace_.set_enabled(enable, now);
  if (uart_trace_switch_ != nullptr)
    uart_trace_switch_->publish_state(uart_trace_.enabled());
  ESP_LOGI(TAG, "UART trace %s; diagnostic only, credentials redacted", enable ? "enabled" : "disabled");
  publish_trace_summary_();
}

void IKettleComponent::publish_trace_summary_() {
  ESP_LOGI(TAG, "%s", uart_trace_.summary().c_str());
  if (modem_ != nullptr) {
    const auto &c = modem_->counters();
    ESP_LOGI(TAG, "UART totals rx=%llu tx=%llu AT=%llu malformed=%llu overflow=%llu timeout=%llu",
             static_cast<unsigned long long>(c.rx_bytes), static_cast<unsigned long long>(c.tx_bytes),
             static_cast<unsigned long long>(c.at_commands), static_cast<unsigned long long>(c.malformed_commands),
             static_cast<unsigned long long>(c.line_overflows), static_cast<unsigned long long>(c.payload_timeouts));
  }
}

void IKettleComponent::setup() {
  trace_configured_ = uart_trace_switch_ != nullptr && trace_timeout_ms_ % 1000 == 0 &&
                      uart_trace_.configure(trace_timeout_ms_ / 1000, trace_rate_limit_);
  uart_trace_.set_enabled(false, millis());
  if (uart_trace_switch_ != nullptr)
    uart_trace_switch_->publish_state(false);
  // Restoring a selection publishes numbers only; it never starts heating.
  if (restore_selection_) {
    selection_preference_ = global_preferences->make_preference<Selection>(preference_key_, true);
    Selection saved{};
    if (selection_preference_.load(&saved) && saved.schema == 1 && saved.target >= 20 && saved.target <= 100 &&
        (saved.keep == 0 || (saved.keep >= 5 && saved.keep <= 30))) {
      target_ = saved.target;
      keep_warm_ = saved.keep;
    }
  }
  if (target_temperature_number_ != nullptr)
    target_temperature_number_->publish_state(target_);
  if (keep_warm_minutes_number_ != nullptr)
    keep_warm_minutes_number_->publish_state(keep_warm_);

  session_ = std::make_unique<KettleSession>([this](const Bytes &bytes, uint32_t now) {
    return modem_ != nullptr && modem_->inject(bytes, now);
  });
  ModemCallbacks callbacks;
  callbacks.uart_tx = [this](const Bytes &bytes) { this->write_array(bytes.data(), bytes.size()); };
  callbacks.payload = [this](uint8_t link, const Bytes &bytes) {
    if (link != 1 || session_ == nullptr)
      return false;
    // SEND OK confirms delivery to this local endpoint, not successful decoding
    // or physical heating. Keep these distinct even for unsupported MCU frames.
    session_->receive(link, bytes, now_);
    return true;
  };
  callbacks.reset = [this]() {
    if (session_ != nullptr)
      session_->reset();
  };
  callbacks.trace = [this](TraceDirection direction, TraceKind kind, const uint8_t *data, size_t size) {
    uart_trace_.record(direction, kind, data, size, now_);
  };
  modem_ = std::make_unique<VirtualModem>(std::move(callbacks));
  // A logical local interface identifier; no physical SoftAP is created.
  modem_->set_mac_address(get_mac_address_pretty());
  now_ = millis();
  modem_->begin(now_);
  publish_(now_);
}

void IKettleComponent::loop() {
  now_ = millis();
  const bool trace_was_enabled = uart_trace_.enabled();
  uart_trace_.tick(now_);
  if (trace_was_enabled && !uart_trace_.enabled()) {
    if (uart_trace_switch_ != nullptr)
      uart_trace_switch_->publish_state(false);
    ESP_LOGI(TAG, "UART trace disabled automatically after its time limit");
    publish_trace_summary_();
  }
  if (modem_ == nullptr || session_ == nullptr)
    return;
  uint8_t buffer[128];
  size_t count = 0;
  size_t budget = 512;
  while (budget-- != 0 && available() != 0) {
    if (!read_byte(&buffer[count]))
      break;
    if (++count == sizeof(buffer)) {
      modem_->feed(buffer, count, now_);
      count = 0;
    }
  }
  if (count != 0)
    modem_->feed(buffer, count, now_);
  modem_->tick(now_);
  session_->tick(now_, modem_->link_ready(), modem_->can_inject(now_));
  // Already-sanitized bounded records are drained after protocol service. No
  // second UART reader, raw text sensor, command injection or synchronous log
  // callback runs in the modem's critical reply path.
  uart_trace_.drain(now_, [](const char *line) { ESP_LOGI(TAG, "%s", line); });
  const auto &state = session_->state();
  const bool fresh = session_->status_fresh(now_) && state.profile_valid && state.supported;
  if (state.revision != last_revision_ || fresh != last_fresh_ || uint32_t(now_ - last_publish_) >= 1000) {
    publish_(now_);
    last_revision_ = state.revision;
    last_fresh_ = fresh;
    last_publish_ = now_;
  }
}

bool IKettleComponent::select(NumberKind kind, float value) {
  if (!std::isfinite(value) || std::floor(value) != value)
    return false;
  if (kind == NumberKind::TARGET) {
    if (value < 20 || value > 100)
      return false;
    target_ = static_cast<uint8_t>(value);
    if (target_temperature_number_ != nullptr)
      target_temperature_number_->publish_state(target_);
  } else {
    if (value != 0 && (value < 5 || value > 30)) {
      if (keep_warm_minutes_number_ != nullptr)
        keep_warm_minutes_number_->publish_state(keep_warm_);
      if (protocol_error_text_sensor_ != nullptr)
        protocol_error_text_sensor_->publish_state("keep_warm_requires_0_or_5_to_30");
      return false;
    }
    keep_warm_ = static_cast<uint8_t>(value);
    if (keep_warm_minutes_number_ != nullptr)
      keep_warm_minutes_number_->publish_state(keep_warm_);
  }
  save_selection_();
  return true;
}

void IKettleComponent::save_selection_() {
  if (!restore_selection_)
    return;
  Selection selection{1, target_, keep_warm_};
  if (!selection_preference_.save(&selection) && protocol_error_text_sensor_ != nullptr)
    protocol_error_text_sensor_->publish_state("selection_not_saved");
}

bool IKettleComponent::perform(ButtonAction action) {
  if (session_ == nullptr || modem_ == nullptr)
    return false;
  now_ = millis();
  // Re-check availability at the actual button event; never queue a deferred
  // actuator to be replayed after reboot, a scan, or a Wi-Fi reconnect.
  session_->tick(now_, modem_->link_ready(), modem_->can_inject(now_));
  bool accepted = false;
  switch (action) {
    case ButtonAction::START: accepted = session_->start(target_, keep_warm_, false, now_); break;
    case ButtonAction::STOP:
      accepted = session_->stop(now_);
      if (accepted)
        drink_temperature_alert_.cancel();
      break;
    case ButtonAction::FORMULA: accepted = session_->start(target_, keep_warm_, true, now_); break;
    case ButtonAction::STOP_WARM: accepted = session_->stop_keep_warm(now_); break;
    case ButtonAction::REFRESH: accepted = session_->refresh(now_); break;
  }
  publish_action_result_(accepted);
  if (action != ButtonAction::REFRESH)
    publish_command_status_();
  return accepted;
}

bool IKettleComponent::check_drink_temperature(float threshold) {
  if (session_ == nullptr)
    return false;
  const auto &state = session_->state();
  const bool fresh = session_->status_fresh(millis()) && state.profile_valid && state.supported;
  return drink_temperature_alert_.update(state, session_->diagnostics().session_epoch, fresh, threshold);
}

void IKettleComponent::publish_action_result_(bool accepted) {
  if (protocol_error_text_sensor_ != nullptr)
    protocol_error_text_sensor_->publish_state(accepted ? "request_sent_or_read_queued" :
                                               session_->diagnostics().last_error);
}

void IKettleComponent::publish_command_status_() {
  if (command_status_text_sensor_ == nullptr)
    return;
  const char *value = command_status_text(session_->diagnostics().command_status);
  if (command_status_text_sensor_->get_raw_state() != value)
    command_status_text_sensor_->publish_state(value);
}

void IKettleComponent::publish_cycle_stats_(const CycleStats &stats) {
  if (cycle_start_temperature_sensor_ != nullptr)
    cycle_start_temperature_sensor_->publish_state(stats.start_temperature);
  if (cycle_heating_time_sensor_ != nullptr)
    cycle_heating_time_sensor_->publish_state(float(stats.heating_ms) / 1000.0f);
  if (cycle_keep_warm_time_sensor_ != nullptr)
    cycle_keep_warm_time_sensor_->publish_state(float(stats.keep_warm_ms) / 1000.0f);
  if (cycle_heating_energy_sensor_ != nullptr)
    cycle_heating_energy_sensor_->publish_state(stats.heating_energy_wh(heater_power_watts_));
}

void IKettleComponent::publish_(uint32_t now) {
  const auto &s = session_->state();
  const bool fresh = session_->status_fresh(now) && s.profile_valid && s.supported;
  if (const auto stats = cycle_tracker_.observe(s, session_->diagnostics().session_epoch, fresh))
    publish_cycle_stats_(*stats);
  if (protocol_ready_binary_sensor_ != nullptr)
    protocol_ready_binary_sensor_->publish_state(fresh && modem_->can_inject(now));
  if (temperature_sensor_ != nullptr)
    temperature_sensor_->publish_state(fresh && s.on_base ? s.temp : NAN);
  if (water_raw_sensor_ != nullptr)
    water_raw_sensor_->publish_state(fresh && s.on_base ? float(s.water_raw) : NAN);
  if (on_base_binary_sensor_ != nullptr) {
    if (fresh)
      on_base_binary_sensor_->publish_state(s.on_base);
    else
      on_base_binary_sensor_->invalidate_state();
  }
  if (status_age_sensor_ != nullptr)
    status_age_sensor_->publish_state(s.status_known && s.profile_valid ?
        float(uint32_t(now - s.last_status_ms)) / 1000.0f : NAN);
  const auto &diagnostics = session_->diagnostics();
  if (mcu_reply_age_sensor_ != nullptr) {
    const float age = diagnostics.valid_reply_seen ? float(uint32_t(now - diagnostics.last_valid_reply_ms)) / 1000.0f
                                                   : NAN;
    mcu_reply_age_sensor_->publish_state(age);
  }
  if (serial_errors_sensor_ != nullptr) {
    const auto &serial = modem_->counters();
    // Receive/parser failures and timeouts on both layers, counted since ESP boot.
    const float errors = float(serial.malformed_commands + serial.line_overflows + serial.payload_timeouts +
                               diagnostics.protocol_errors + diagnostics.timeouts);
    if (!serial_errors_sensor_->has_state() || serial_errors_sensor_->get_raw_state() != errors)
      serial_errors_sensor_->publish_state(errors);
  }
  if (base_raw_sensor_ != nullptr)
    base_raw_sensor_->publish_state(s.base_known ? float(s.base_raw) : NAN);
  if (default_temperature_sensor_ != nullptr)
    default_temperature_sensor_->publish_state(s.defaults_known ? float(s.default_temperature) : NAN);
  if (default_keep_warm_sensor_ != nullptr)
    default_keep_warm_sensor_->publish_state(s.defaults_known ? float(s.default_keep_minutes) : NAN);
  if (default_formula_temperature_sensor_ != nullptr)
    default_formula_temperature_sensor_->publish_state(s.defaults_known ? float(s.default_formula_temperature) : NAN);
  if (mcu_firmware_text_sensor_ != nullptr)
    mcu_firmware_text_sensor_->publish_state(s.identity_known ? std::to_string(s.firmware_version) : "unknown");
  if (activity_text_sensor_ != nullptr) {
    const char *activity = "unavailable";
    if (fresh) {
      switch (s.state_code) {
        case 0: activity = "ready"; break;
        case 1: activity = "heating"; break;
        case 2: activity = "keep_warm"; break;
        case 3: activity = "cycle_finished_app_label"; break;
        case 4: activity = "formula_cooling"; break;
      }
    }
    activity_text_sensor_->publish_state(activity);
  }
  if (protocol_error_text_sensor_ != nullptr)
    protocol_error_text_sensor_->publish_state(session_->diagnostics().last_error);
  publish_command_status_();
}

void IKettleComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "iKettle SMK20/F19 native virtual-modem adapter");
  ESP_LOGCONFIG(TAG, "  MCU UART: 115200/8N1; original heater controller retained");
  ESP_LOGCONFIG(TAG, "  Radio/AP/credentials managed by ESPHome; no TCP/UDP2081 server");
  ESP_LOGCONFIG(TAG, "  Hardware mapping and recovery are not validated by this component");
  ESP_LOGCONFIG(TAG, "  UART trace: %s; OFF after boot, bounded and credentials redacted",
                trace_configured_ ? "available" : "not configured");
}

}  // namespace esphome::ikettle_mcu
