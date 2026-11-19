#pragma once

/// @file field_domain.h
/// @brief Closed numeric domain of an effect-config field and the refusal raised outside it.
///
/// A processor states each field's domain once; construction, set_config and
/// realtime automation all test the same bounds. A value outside the domain,
/// or non-finite, is refused rather than clamped.

#include <cmath>
#include <limits>
#include <string>

#include "util/exception.h"
#include "util/refusal_predicates.h"

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

/// Throws a SonareParameterRefusal naming member @p field unless @p value is in [@p lo, @p hi].
/// The test runs in float; an infinite or float-extreme bound is stated as a one-sided domain.
inline void require_in_domain(const char* field, float value, float lo, float hi) {
  if (in_domain(value, lo, hi)) return;
  const bool no_lower = lo <= std::numeric_limits<float>::lowest();
  const bool no_upper = hi >= std::numeric_limits<float>::max();
  std::string predicate;
  if (no_lower && no_upper) {
    predicate = util::refusal::kFloatRangePredicate;
  } else if (no_upper) {
    predicate = util::refusal::at_least_predicate(lo);
  } else if (no_lower) {
    predicate = util::refusal::at_most_predicate(hi);
  } else {
    predicate = util::refusal::range_predicate(lo, hi);
  }
  throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, field, predicate);
}

inline bool in_domain(float value, FieldRange range) noexcept {
  return in_domain(value, range.lo, range.hi);
}

inline void require_in_domain(const char* field, float value, FieldRange range) {
  require_in_domain(field, value, range.lo, range.hi);
}

}  // namespace sonare::effects::common
