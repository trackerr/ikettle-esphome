#pragma once

#include "protocol.h"

#include <optional>

namespace esphome::ikettle_mcu {

struct CycleStats {
  float start_temperature{0};
  uint32_t heating_ms{0};
  uint32_t keep_warm_ms{0};

  float heating_energy_wh(float watts) const { return watts * (float(heating_ms) / 3600000.0f); }
};

// Record cycles whose transition into heating and subsequent end were observed.
// Status 2 indicates the keep-warm phase, independently of relay duty.
class CycleTracker {
 public:
  std::optional<CycleStats> observe(const KettleState &state, uint32_t epoch, bool fresh) {
    if (!fresh) {
      observed_ = false;
      active_ = false;
      return std::nullopt;
    }

    if (observed_ && epoch == epoch_ && state.status_sequence == sequence_)
      return std::nullopt;

    // A reconnect, skipped frame or stale interval cannot account for phase time.
    const uint32_t elapsed = state.last_status_ms - status_ms_;
    const bool continuous = observed_ && epoch == epoch_ && state.status_sequence - sequence_ == 1 &&
                            elapsed < KettleSession::STATUS_FRESH_MS;
    const bool finished = state.state_code == 0 || state.state_code == 3 || !state.on_base;
    std::optional<CycleStats> completed;
    if (!continuous) {
      active_ = false;
    } else if (active_) {
      if (phase_ == 1)
        stats_.heating_ms += elapsed;
      else if (phase_ == 2)
        stats_.keep_warm_ms += elapsed;
      if (finished) {
        completed = stats_;
        active_ = false;
      }
    } else if (ready_on_base_ && state.on_base && state.state_code == 1) {
      stats_ = CycleStats{state.temp};
      active_ = true;
    }

    observed_ = true;
    epoch_ = epoch;
    sequence_ = state.status_sequence;
    status_ms_ = state.last_status_ms;
    phase_ = state.state_code;
    ready_on_base_ = finished && state.on_base;
    return completed;
  }

 private:
  CycleStats stats_;
  uint32_t epoch_{0};
  uint32_t sequence_{0};
  uint32_t status_ms_{0};
  uint8_t phase_{0};
  bool observed_{false};
  bool active_{false};
  bool ready_on_base_{false};
};

}  // namespace esphome::ikettle_mcu
