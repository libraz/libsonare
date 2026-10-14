#pragma once

/// @file mix_dc_blocker.h
/// @brief Tail of the one-pole DC blocker the synth hosts run last on their mix bus.

#include <cmath>
#include <cstdint>

#include "rt/tail_budget.h"

namespace sonare::midi::synth {

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
