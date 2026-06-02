#pragma once

/// @file lfo.h
/// @brief Lightweight low-frequency oscillator for modulation FX.

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare::effects::modulation {

class Lfo {
 public:
  void prepare(double sample_rate) noexcept {
    sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  }
  void reset(double phase = 0.0) noexcept { phase_ = phase - std::floor(phase); }
  void set_rate_hz(float rate_hz) noexcept { rate_hz_ = std::max(0.0f, rate_hz); }

  float process() noexcept {
    const float value = static_cast<float>(std::sin(phase_ * ::sonare::constants::kTwoPiD));
    phase_ += static_cast<double>(rate_hz_) / sample_rate_;
    phase_ -= std::floor(phase_);
    return value;
  }

 private:
  double sample_rate_ = 48000.0;
  double phase_ = 0.0;
  float rate_hz_ = 1.0f;
};

}  // namespace sonare::effects::modulation
