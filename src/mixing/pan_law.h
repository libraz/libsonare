#pragma once

/// @file pan_law.h
/// @brief Stereo pan-law gain calculation.

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare::mixing {

enum class PanLaw {
  Const3dB,
  Const4p5dB,
  Const6dB,
  Linear0dB,
};

struct PanGains {
  float left = 1.0f;
  float right = 1.0f;
};

inline PanGains compute_pan_gains(float pan, PanLaw law = PanLaw::Const3dB) noexcept {
  const float p = std::clamp(pan, -1.0f, 1.0f);
  const float t = (p + 1.0f) * 0.5f;
  const float linear_left = 1.0f - t;
  const float linear_right = t;

  switch (law) {
    case PanLaw::Const3dB: {
      const float angle = t * ::sonare::constants::kHalfPi;
      return {std::cos(angle), std::sin(angle)};
    }
    case PanLaw::Const4p5dB: {
      const float angle = t * ::sonare::constants::kHalfPi;
      const float constant_left = std::cos(angle);
      const float constant_right = std::sin(angle);
      return {std::sqrt(linear_left * constant_left), std::sqrt(linear_right * constant_right)};
    }
    case PanLaw::Const6dB:
      return {linear_left, linear_right};
    case PanLaw::Linear0dB:
      return {p <= 0.0f ? 1.0f : 1.0f - p, p >= 0.0f ? 1.0f : 1.0f + p};
  }

  return {1.0f, 1.0f};
}

}  // namespace sonare::mixing
