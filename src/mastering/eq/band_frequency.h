#pragma once

/// @file band_frequency.h
/// @brief The refusal for an equalizer band frequency outside (0, Nyquist).

#include <string>

#include "util/exception.h"
#include "util/number_format.h"

namespace sonare::mastering::eq {

/// @brief Throws the refusal for a band frequency outside (0, Nyquist) at @p sample_rate, naming
/// the ceiling.
[[noreturn]] inline void refuse_band_frequency(float frequency_hz, double sample_rate) {
  if (!(frequency_hz > 0.0f)) {
    throw SonareException(ErrorCode::InvalidParameter, "EQ band frequency must be above 0 Hz");
  }
  throw SonareException(
      ErrorCode::InvalidParameter,
      "EQ band frequency must be below " + sonare::util::format_general(sample_rate * 0.5, 12) +
          " Hz (Nyquist at " + sonare::util::format_general(sample_rate, 12) + " Hz)");
}

}  // namespace sonare::mastering::eq
