#pragma once

/// @file constant_power_width.h
/// @brief Signal-independent gain compensation for mid/side width changes.

#include <cmath>

namespace sonare::mastering::stereo {

/// Constant-power compensation for an M/S side gain of @p width.
///
/// This assumes equal expected mid/side power and, unlike an instantaneous
/// energy ratio, never derives gain from the current samples. The operation is
/// therefore linear and cannot create intermodulation products.
inline float constant_power_width_gain(float width) noexcept {
  const double width_d = static_cast<double>(width);
  return static_cast<float>(std::sqrt(2.0 / (1.0 + width_d * width_d)));
}

}  // namespace sonare::mastering::stereo
