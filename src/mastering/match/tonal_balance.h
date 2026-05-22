#pragma once

/// @file tonal_balance.h
/// @brief Broad-band tonal balance comparison.

#include <vector>

#include "mastering/match/reference_spectrum.h"

namespace sonare::mastering::match {

struct TonalBalanceBand {
  float low_hz = 0.0f;
  float high_hz = 0.0f;
  float source_db = 0.0f;
  float reference_db = 0.0f;
  float deviation_db = 0.0f;
};

std::vector<TonalBalanceBand> tonal_balance(const ReferenceSpectrum& source,
                                            const ReferenceSpectrum& reference);
std::vector<TonalBalanceBand> tonal_balance_log_bands(const ReferenceSpectrum& source,
                                                      const ReferenceSpectrum& reference,
                                                      int bands_per_octave = 3,
                                                      float low_hz = 20.0f,
                                                      float high_hz = 20000.0f);

}  // namespace sonare::mastering::match
