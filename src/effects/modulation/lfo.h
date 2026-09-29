#pragma once

/// @file lfo.h
/// @brief Lightweight low-frequency oscillator for modulation FX.

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare::effects::modulation {

/// Waveform of a modulator, shared by every insert that offers a choice of shape.
enum class LfoShape {
  kSine,      ///< sin(2 pi t): the default everywhere.
  kTriangle,  ///< 0 at t = 0, +1 at a quarter turn, -1 at three quarters.
  kSquare,    ///< +1 for the first half turn, -1 for the second.
  kSawUp,     ///< rises from -1 to +1 over the turn.
  kSawDown,   ///< falls from +1 to -1 over the turn.
};
inline constexpr int kLfoShapeCount = 5;

/// Value in [-1, 1] of @p shape at @p phase, in turns (any real; wrapped for the non-sine shapes).
/// The sine branch is the exact expression the oscillators used before shapes existed.
inline float lfo_shape_value(LfoShape shape, double phase) noexcept {
  if (shape == LfoShape::kSine) {
    return static_cast<float>(std::sin(phase * ::sonare::constants::kTwoPiD));
  }
  const float t = static_cast<float>(phase - std::floor(phase));
  switch (shape) {
    case LfoShape::kTriangle:
      if (t < 0.25f) return 4.0f * t;
      if (t < 0.75f) return 2.0f - 4.0f * t;
      return 4.0f * t - 4.0f;
    case LfoShape::kSquare:
      return t < 0.5f ? 1.0f : -1.0f;
    case LfoShape::kSawUp:
      return 2.0f * t - 1.0f;
    case LfoShape::kSawDown:
      return 1.0f - 2.0f * t;
    case LfoShape::kSine:
      break;
  }
  return 0.0f;
}

class Lfo {
 public:
  void prepare(double sample_rate) noexcept {
    sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  }
  void reset(double phase = 0.0) noexcept { phase_ = phase - std::floor(phase); }
  void set_rate_hz(float rate_hz) noexcept { rate_hz_ = std::max(0.0f, rate_hz); }
  /// The phase the next process() call reads, in turns [0, 1).
  double phase() const noexcept { return phase_; }

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
