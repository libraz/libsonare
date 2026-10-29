#pragma once

#include <cmath>

#include "mastering/common/hysteresis_ja.h"
#include "rt/biquad_design.h"
#include "rt/tail_budget.h"

namespace sonare::mastering::common {

/// Zero-field relaxation tail in samples at the magnetic model's processing rate.
inline int hysteresis_relaxation_tail_samples(const JilesAthertonConfig& config, double sample_rate,
                                              double output_gain) noexcept {
  if (output_gain == 0.0) return 0;
  const float coupling = config.mean_field_coupling * config.saturation_magnetization /
                         (3.0f * config.anhysteretic_shape);
  if (!(config.viscosity_time_constant_s > 0.0f) || !(sample_rate > 0.0) ||
      !std::isfinite(sample_rate) || !(coupling < 1.0f) || !std::isfinite(output_gain)) {
    return rt::TailBudget::kUnbounded;
  }

  // Match the float relaxation arithmetic in JilesAtherton::process().
  const float rate = static_cast<float>(sample_rate);
  const float relax_rate =
      static_cast<float>(rt::one_pole_alpha_from_seconds_d(config.viscosity_time_constant_s, rate));
  const float relax_pole = 1.0f - relax_rate * (1.0f - coupling);
  if (!(relax_pole > 0.0f)) return 1;
  if (!(relax_pole < 1.0f) || !std::isfinite(relax_pole)) {
    return rt::TailBudget::kUnbounded;
  }

  // Bound both sides of the model's 1.2 * Ms clamp, plus one field-transition sample.
  const double scale = 2.4 * static_cast<double>(config.saturation_magnetization) * output_gain;
  rt::TailBudget tail;
  tail.delay(1.0).decay(static_cast<double>(relax_pole), scale);
  return tail.samples();
}

}  // namespace sonare::mastering::common
