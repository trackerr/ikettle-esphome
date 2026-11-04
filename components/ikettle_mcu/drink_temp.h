#pragma once

#include "protocol.h"

#include <cmath>

namespace esphome::ikettle_mcu {

class DrinkTemperatureAlert {
 public:
  void cancel() {
    phase_ = Phase::WAIT_FOR_IDLE;
    was_hot_ = false;
  }

  bool update(const KettleState &state, uint32_t epoch, bool fresh, float setting) {
    if (!fresh || !state.on_base || !std::isfinite(state.temp)) {
      cancel();
      return false;
    }
    if (epoch == epoch_ && state.status_sequence == last_status_)
      return false;
    if (epoch != epoch_ || state.status_sequence - last_status_ != 1 ||
        state.last_status_ms - last_status_ms_ >= KettleSession::STATUS_FRESH_MS)
      cancel();
    epoch_ = epoch;
    last_status_ = state.status_sequence;
    last_status_ms_ = state.last_status_ms;

    if (phase_ == Phase::WAIT_FOR_IDLE) {
      if (state.state_code == 0)
        phase_ = Phase::IDLE;
      return false;
    }
    if (state.state_code == 1 && phase_ != Phase::HEATING) {
      if (!std::isfinite(setting)) {
        cancel();
        return false;
      }
      phase_ = Phase::HEATING;
      threshold_ = setting;
      was_hot_ = false;
    }
    if (phase_ == Phase::IDLE)
      return false;

    // Arm above the threshold so idle water and sensor jitter cannot alert.
    if (phase_ == Phase::HEATING)
      was_hot_ = was_hot_ || state.temp >= threshold_ + 2.0f;
    if (state.state_code == 1)
      return false;
    if (!was_hot_) {
      phase_ = Phase::IDLE;
      return false;
    }
    phase_ = Phase::COOLING;
    if (state.state_code == 2 || state.temp > threshold_)
      return false;

    phase_ = Phase::IDLE;
    was_hot_ = false;
    return true;
  }

 private:
  enum class Phase : uint8_t { WAIT_FOR_IDLE, IDLE, HEATING, COOLING };
  Phase phase_{Phase::WAIT_FOR_IDLE};
  uint32_t epoch_{0};
  uint32_t last_status_{0};
  uint32_t last_status_ms_{0};
  float threshold_{0.0f};
  bool was_hot_{false};
};

}  // namespace esphome::ikettle_mcu
