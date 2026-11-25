#pragma once

/// @file band_frequency.h
/// @brief The refusals for an equalizer band frequency outside (0, Nyquist) and a band Q.

#include <cmath>

#include "util/exception.h"
#include "util/refusal_predicates.h"

namespace sonare::mastering::eq {

/// @brief True when @p frequency_hz lies in the open interval (0, Nyquist) at @p sample_rate.
inline bool band_frequency_in_range(float frequency_hz, double sample_rate) noexcept {
  return frequency_hz > 0.0f && frequency_hz < static_cast<float>(sample_rate * 0.5);
}

/// @brief True when @p q is a positive number (NaN is refused).
inline bool band_q_valid(float q) noexcept { return q > 0.0f; }

/// @brief Throws the refusal for a band frequency outside (0, Nyquist) at @p sample_rate.
[[noreturn]] inline void refuse_band_frequency(double sample_rate,
                                               const char* subject = "frequency_hz") {
  throw SonareParameterRefusal::predicate(
      ErrorCode::InvalidParameter, subject,
      util::refusal::interval_predicate(0.0, false, sample_rate * 0.5, false));
}

/// @brief Refuses a band Q that is not finite and positive.
inline void check_band_q(float q, const char* subject = "q") {
  if (!(std::isfinite(q) && band_q_valid(q))) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, subject,
                                            util::refusal::greater_than_predicate(0.0));
  }
}

}  // namespace sonare::mastering::eq
