#pragma once

#include <memory>

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/number/number.h"
#include "esphome/components/button/button.h"
#include "esphome/components/switch/switch.h"
#include "cycle_stats.h"
#include "drink_temp.h"
#include "modem.h"
#include "protocol.h"
#include "uart_trace.h"

namespace esphome::ikettle_mcu {

enum class ButtonAction : uint8_t { START, STOP, FORMULA, STOP_WARM, REFRESH };
enum class NumberKind : uint8_t { TARGET, KEEP_WARM };

class IKettleComponent;

class KettleNumber : public number::Number {
 public:
  void set_parent(IKettleComponent *parent) { parent_ = parent; }
  void set_kind(NumberKind kind) { kind_ = kind; }

 protected:
  void control(float value) override;
  IKettleComponent *parent_{nullptr};
  NumberKind kind_{NumberKind::TARGET};
};

class KettleButton : public button::Button {
 public:
  void set_parent(IKettleComponent *parent) { parent_ = parent; }
  void set_action(ButtonAction action) { action_ = action; }

 protected:
  void press_action() override;
  IKettleComponent *parent_{nullptr};
  ButtonAction action_{ButtonAction::REFRESH};
};

// Runtime-only diagnostic control. No restored ON state, UART write or action.
class KettleTraceSwitch : public switch_::Switch {
 public:
  void set_parent(IKettleComponent *parent) { parent_ = parent; }

 protected:
  void write_state(bool value) override;
  IKettleComponent *parent_{nullptr};
};

// Internal native-API adapter, not a transparent network-facing ESP-AT modem.
// Wi-Fi, credentials, AP and radio settings remain ESPHome's responsibility.
class IKettleComponent : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return 750.0f; }
  void set_initial_target(uint8_t value) { target_ = value; }
  void set_initial_keep_warm(uint8_t value) { keep_warm_ = value; }
  void set_preference_key(uint32_t value) { preference_key_ = value; }
  void set_restore_selection(bool value) { restore_selection_ = value; }
  bool select(NumberKind kind, float value);
  bool perform(ButtonAction action);
  bool check_drink_temperature(float threshold);
  void set_trace_timeout(uint32_t value) { trace_timeout_ms_ = value; }
  void set_trace_rate_limit(uint8_t value) { trace_rate_limit_ = value; }
  void set_uart_trace_switch(KettleTraceSwitch *value) { uart_trace_switch_ = value; }
  void set_uart_trace_enabled(bool value);

  SUB_SENSOR(temperature)
  SUB_SENSOR(water_raw)
  SUB_SENSOR(base_raw)
  SUB_SENSOR(default_temperature)
  SUB_SENSOR(default_keep_warm)
  SUB_SENSOR(default_formula_temperature)
  SUB_SENSOR(status_age)
  SUB_SENSOR(mcu_reply_age)
  SUB_SENSOR(serial_errors)
  SUB_SENSOR(cycle_start_temperature)
  SUB_SENSOR(cycle_heating_time)
  SUB_SENSOR(cycle_keep_warm_time)
  SUB_SENSOR(cycle_heating_energy)
  void set_heater_power_watts(float value) { heater_power_watts_ = value; }
  SUB_BINARY_SENSOR(on_base)
  SUB_BINARY_SENSOR(protocol_ready)
  SUB_TEXT_SENSOR(activity)
  SUB_TEXT_SENSOR(mcu_firmware)
  SUB_TEXT_SENSOR(protocol_error)
  SUB_TEXT_SENSOR(command_status)
  SUB_NUMBER(target_temperature)
  SUB_NUMBER(keep_warm_minutes)

 protected:
  struct Selection {
    uint8_t schema;
    uint8_t target;
    uint8_t keep;
  };
  void publish_(uint32_t now);
  void save_selection_();
  void publish_action_result_(bool accepted);
  void publish_command_status_();
  void publish_trace_summary_();
  void publish_cycle_stats_(const CycleStats &stats);
  CycleTracker cycle_tracker_;
  DrinkTemperatureAlert drink_temperature_alert_;
  float heater_power_watts_{0};
  UartTrace uart_trace_;
  KettleTraceSwitch *uart_trace_switch_{nullptr};
  std::unique_ptr<VirtualModem> modem_;
  std::unique_ptr<KettleSession> session_;
  ESPPreferenceObject selection_preference_;
  uint32_t preference_key_{0};
  uint32_t now_{0};
  uint32_t last_publish_{0};
  uint32_t last_revision_{0};
  uint8_t target_{90};
  uint8_t keep_warm_{0};
  uint32_t trace_timeout_ms_{120000};
  uint8_t trace_rate_limit_{8};
  bool trace_configured_{false};
  bool restore_selection_{false};
  bool last_fresh_{false};
};

}  // namespace esphome::ikettle_mcu
