#pragma once

/// @file field_domain.h
/// @brief Closed numeric domain of an effect-config field and the refusal raised outside it.
///
/// A processor states each field's domain once; construction, set_config and
/// realtime automation all test the same bounds. A value outside the domain,
/// or non-finite, is refused rather than clamped.

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "util/exception.h"

namespace sonare::effects::common {

/// Upper bound of a field that has only a lower limit.
inline constexpr float kNoUpperBound = std::numeric_limits<float>::infinity();

/// Closed interval a config field must lie in; a bound may be infinite.
struct FieldRange {
  float lo;
  float hi;
};

/// Any finite value: the field has no bound beyond not being NaN or infinite.
inline constexpr FieldRange kAnyFinite{std::numeric_limits<float>::lowest(),
                                       std::numeric_limits<float>::max()};

/// Whether @p value is finite and lies in the closed range [@p lo, @p hi].
inline bool in_domain(float value, float lo, float hi) noexcept {
  return std::isfinite(value) && value >= lo && value <= hi;
}

/// Throws InvalidParameter naming @p processor and @p field when @p value is outside [@p lo, @p hi].
inline void require_in_domain(const char* processor, const char* field, float value, float lo,
                              float hi) {
  if (in_domain(value, lo, hi)) return;
  char text[160];
  std::snprintf(text, sizeof(text), "%s: %s out of range [%g, %g], got %g", processor, field,
                static_cast<double>(lo), static_cast<double>(hi), static_cast<double>(value));
  throw SonareException(ErrorCode::InvalidParameter, text);
}

inline bool in_domain(float value, FieldRange range) noexcept {
  return in_domain(value, range.lo, range.hi);
}

inline void require_in_domain(const char* processor, const char* field, float value,
                              FieldRange range) {
  require_in_domain(processor, field, value, range.lo, range.hi);
}

}  // namespace sonare::effects::common
