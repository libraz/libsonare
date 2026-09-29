#pragma once

/// @file mix_law.h
/// @brief How a dry/wet control maps onto the dry and wet gains.

#include <algorithm>

namespace sonare::effects::common {

/// How dry and wet are combined by dry_wet.
enum class MixLaw {
  kCrossfade,  ///< dry = 1 - w, wet = w.
  kTwoRamps,   ///< dry and wet are independent linear gains, each reaching unity at w = 0.5.
};
inline constexpr int kMixLawCount = 2;

/// Dry and wet gains for a wet amount in [0, 1].
struct MixGains {
  float dry;
  float wet;
};

/// @brief Gains the given law assigns to a wet amount.
inline MixGains mix_gains(MixLaw law, float wet) noexcept {
  if (law == MixLaw::kTwoRamps) {
    return {std::min(1.0f, 2.0f * (1.0f - wet)), std::min(1.0f, 2.0f * wet)};
  }
  return {1.0f - wet, wet};
}

}  // namespace sonare::effects::common
