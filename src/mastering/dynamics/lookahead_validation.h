#pragma once

#include <cmath>
#include <limits>

#include "util/exception.h"

namespace sonare::mastering::dynamics {

// Validate the derived buffer length before a floating-point-to-int conversion
// or any mutation of a prepared processor. Oversampled callers pass the tighter
// bound required by their later integer multiplication.
inline int checked_lookahead_samples(double sample_rate, float lookahead_ms,
                                     int maximum = std::numeric_limits<int>::max()) {
  if (!std::isfinite(sample_rate) || sample_rate <= 0.0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be finite and positive");
  }
  const double samples = std::round(sample_rate * static_cast<double>(lookahead_ms) * 0.001);
  if (!std::isfinite(samples) || samples < 0.0 || samples > maximum) {
    throw SonareException(ErrorCode::InvalidParameter, "lookahead exceeds supported size");
  }
  return static_cast<int>(samples);
}

}  // namespace sonare::mastering::dynamics
