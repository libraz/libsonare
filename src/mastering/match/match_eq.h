#pragma once

/// @file match_eq.h
/// @brief Convert source/reference spectrum differences into EQ bands.

#include <cstddef>
#include <vector>

#include "mastering/eq/parametric.h"
#include "mastering/match/reference_spectrum.h"

namespace sonare::mastering::match {

struct MatchEqConfig {
  size_t max_bands = 8;
  float max_gain_db = 12.0f;
  float min_frequency_hz = 40.0f;
  float max_frequency_hz = 18000.0f;
  float q = 1.0f;
  int smoothing_bins = 2;
};

struct MatchEqCurve {
  std::vector<float> frequencies;
  std::vector<float> gain_db;
};

MatchEqCurve match_eq_curve(const ReferenceSpectrum& source, const ReferenceSpectrum& reference,
                            const MatchEqConfig& config = {});
std::vector<eq::EqBand> match_eq_bands(const ReferenceSpectrum& source,
                                       const ReferenceSpectrum& reference,
                                       const MatchEqConfig& config = {});

}  // namespace sonare::mastering::match
