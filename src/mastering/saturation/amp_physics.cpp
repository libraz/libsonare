#include "mastering/saturation/amp_physics.h"

#include <algorithm>
#include <cmath>

#include "mastering/saturation/amp_sim.h"

namespace sonare::mastering::saturation {

float plate_dissipation_w(PowerTube tube) noexcept {
  switch (tube) {
    case PowerTube::kEL34:
      return 25.0f;
    case PowerTube::kEL84:
      return 12.0f;
    case PowerTube::k6V6:
      return 14.0f;  // 6V6GT; the older 6V6/6V6G is rated 12 W
    default:
      return 30.0f;  // 6L6GC
  }
}

float power_tube_scale(PowerTube tube) noexcept {
  const float dissipation = plate_dissipation_w(tube);
  if (!(dissipation > 0.0f)) return 1.0f;
  // Exactly 1.0 for the 6L6GC, so the default power stage is untouched.
  return std::sqrt(kReferencePlateDissipationW / dissipation);
}

float crossover_from_bias_fraction(float bias_fraction) noexcept {
  if (!std::isfinite(bias_fraction)) return 0.0f;
  const float span = kClassABHotBiasFraction - kClassABColdBiasFraction;
  return std::clamp((kClassABHotBiasFraction - bias_fraction) / span, 0.0f, 1.0f);
}

float rectifier_resistance_ohms(Rectifier rectifier) noexcept {
  switch (rectifier) {
    case Rectifier::kGz34:
      return 50.0f;
    case Rectifier::k5V4:
      return 100.0f;
    case Rectifier::k5U4:
      return 150.0f;
    case Rectifier::k5Y3:
      return 350.0f;
    default:
      return 0.0f;  // silicon
  }
}

float sag_from_supply(float supply_ohms, float full_output_current_a, float b_plus_v) noexcept {
  if (!std::isfinite(supply_ohms) || !std::isfinite(full_output_current_a) ||
      !std::isfinite(b_plus_v) || !(b_plus_v > 0.0f)) {
    return 0.0f;
  }
  return std::clamp(supply_ohms * full_output_current_a / b_plus_v, 0.0f, 1.0f);
}

}  // namespace sonare::mastering::saturation
