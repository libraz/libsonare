#pragma once

#include <limits>

#include "util/db.h"

namespace sonare::mastering::dynamics {

/**
 * @brief Detector level in dB for a threshold decision.
 *
 * linear_to_db() clamps silence to the -120 dB reporting floor, which lies above any accepted
 * threshold below it. Zero or negative energy is therefore -infinity here, below every finite
 * threshold; a NaN level still propagates.
 */
inline float detector_level_db(float linear) noexcept {
  return linear <= 0.0f ? -std::numeric_limits<float>::infinity() : linear_to_db(linear);
}

}  // namespace sonare::mastering::dynamics
