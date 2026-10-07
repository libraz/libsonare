#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "util/constants.h"

namespace sonare::phase {

inline float wrap(float value) noexcept {
  return std::isfinite(value) ? std::remainder(value, constants::kTwoPi) : 0.0f;
}

inline double wrap(double value) noexcept {
  return std::isfinite(value) ? std::remainder(value, constants::kTwoPiD) : 0.0;
}

/// Writes one phase-vocoder frame's synthesis phases from the running accumulator,
/// optionally with identity phase locking; the accumulator is not advanced here.
/// Under locking the real-FFT endpoints are peak candidates and always keep their
/// own accumulated phase, snapped to 0 or pi so the inverse transform drops nothing.
inline void synthesize_locked_frame(const float* magnitude, const float* analysis_phase,
                                    const double* accumulator, int bin_count, bool phase_lock,
                                    double* synthesis_phase, std::vector<int>& peaks,
                                    std::vector<int>& nearest_peak) {
  if (!phase_lock) {
    std::copy(accumulator, accumulator + bin_count, synthesis_phase);
    return;
  }

  const int last = bin_count - 1;
  const auto real_phase = [](double value) {
    return std::cos(value) >= 0.0 ? 0.0 : constants::kPiD;
  };
  peaks.clear();
  for (int bin = 0; bin < bin_count; ++bin) {
    const bool above_left = bin == 0 || magnitude[bin] > magnitude[bin - 1];
    const bool above_right = bin == last || magnitude[bin] > magnitude[bin + 1];
    if (above_left && above_right) peaks.push_back(bin);
  }

  if (peaks.empty()) {
    std::copy(accumulator, accumulator + bin_count, synthesis_phase);
  } else {
    int peak_index = 0;
    for (int bin = 0; bin < bin_count; ++bin) {
      while (peak_index + 1 < static_cast<int>(peaks.size())) {
        const int boundary =
            (peaks[static_cast<size_t>(peak_index)] + peaks[static_cast<size_t>(peak_index + 1)]) /
            2;
        if (bin <= boundary) break;
        ++peak_index;
      }
      nearest_peak[static_cast<size_t>(bin)] = peaks[static_cast<size_t>(peak_index)];
    }
    nearest_peak[0] = 0;
    nearest_peak[static_cast<size_t>(last)] = last;
    for (int peak_bin : peaks) {
      synthesis_phase[peak_bin] = accumulator[peak_bin];
    }
    synthesis_phase[0] = accumulator[0];
    synthesis_phase[last] = accumulator[last];
  }
  synthesis_phase[0] = real_phase(synthesis_phase[0]);
  synthesis_phase[last] = real_phase(synthesis_phase[last]);
  if (peaks.empty()) return;

  for (int bin = 1; bin < last; ++bin) {
    const int peak_bin = nearest_peak[static_cast<size_t>(bin)];
    if (bin == peak_bin) continue;
    synthesis_phase[bin] =
        wrap(synthesis_phase[peak_bin] + static_cast<double>(analysis_phase[bin]) -
             static_cast<double>(analysis_phase[peak_bin]));
  }
}

}  // namespace sonare::phase
