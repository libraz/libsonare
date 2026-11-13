#pragma once

/// @file harmonic_peak.h
/// @brief Amplitude of one harmonic in a rendered tone, robust to a few cents of mistuning.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "util/constants.h"

namespace sonare::test {

/// Amplitude of the @p n th harmonic of @p f0 in @p x: a Hann window over @p periods periods,
/// the frequency scanned across +-0.5 % so a few cents of tuning cannot move the reading.
inline double harmonic_peak(const std::vector<float>& x, size_t from, int periods, double f0, int n,
                            double sample_rate) {
  const size_t count = static_cast<size_t>(std::lround(periods * sample_rate / f0));
  double best = 0.0;
  for (int s = -25; s <= 25; ++s) {
    const double hz = n * f0 * (1.0 + 0.0002 * s);
    std::complex<double> acc = 0.0;
    double window_sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
      const double w = 0.5 - 0.5 * std::cos(sonare::constants::kTwoPiD * static_cast<double>(i) /
                                            static_cast<double>(count));
      window_sum += w;
      acc +=
          w * static_cast<double>(x[from + i]) *
          std::polar(1.0, -sonare::constants::kTwoPiD * hz * static_cast<double>(i) / sample_rate);
    }
    best = std::max(best, 2.0 * std::abs(acc) / window_sum);
  }
  return best;
}

}  // namespace sonare::test
