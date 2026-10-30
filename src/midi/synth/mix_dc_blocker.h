#pragma once

/// @file mix_dc_blocker.h
/// @brief Tail of the one-pole DC blocker the synth hosts run last on their mix bus.

#include <cmath>
#include <cstdint>

#include "rt/tail_budget.h"
#include "util/constants.h"

namespace sonare::midi::synth {

/// @brief Corner frequency of the mix-bus DC blocker in hertz.
inline constexpr double kMixDcBlockerCornerHz = 8.0;

/// @brief Pole of the mix-bus DC blocker at @p sample_rate.
inline float mix_dc_blocker_pole(double sample_rate) noexcept {
  return 1.0f - static_cast<float>(constants::kTwoPiD * kMixDcBlockerCornerHz / sample_rate);
}

/// @brief One sample of the DC blocker; updates the input and output histories.
inline float mix_dc_blocker_step(float in, float pole, float& x1, float& y1) noexcept {
  const float out = in - x1 + pole * y1;
  x1 = in;
  y1 = out;
  return out;
}

/// @brief Samples the mix-bus DC blocker with pole @p pole rings on once its input stops.
/// @details The impulse scale |1 - pole| covers a fresh input; the unit-state bound beside it
///   covers a y[n-1] the mix has already primed.
inline int64_t mix_dc_blocker_tail_samples(float pole) noexcept {
  const double radius = std::abs(static_cast<double>(pole));
  rt::TailBudget dc = rt::TailBudget().decay(radius, std::abs(1.0 - static_cast<double>(pole)));
  dc.alongside(rt::TailBudget().decay(radius, 1.0));
  return rt::tail_budget_samples(dc);
}

}  // namespace sonare::midi::synth
