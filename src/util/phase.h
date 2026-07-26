#pragma once

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

/// Advance one phase-vocoder synthesis frame, optionally with identity phase
/// locking. Shared by offline, streaming, and multichannel tempo-sync paths.
inline void synthesize_locked_frame(const float* magnitude, const float* analysis_phase,
                                    const float* instantaneous_frequency, int bin_count,
                                    bool phase_lock, bool first_frame, double time_step,
                                    std::vector<double>& phase_accumulator, std::vector<int>& peaks,
                                    std::vector<int>& nearest_peak) {
  peaks.clear();
  if (phase_lock) {
    for (int bin = 1; bin + 1 < bin_count; ++bin) {
      if (magnitude[bin] > magnitude[bin - 1] && magnitude[bin] > magnitude[bin + 1]) {
        peaks.push_back(bin);
      }
    }
  }

  if (!phase_lock || peaks.empty()) {
    for (int bin = 0; bin < bin_count; ++bin) {
      phase_accumulator[static_cast<size_t>(bin)] =
          first_frame ? static_cast<double>(analysis_phase[bin])
                      : wrap(phase_accumulator[static_cast<size_t>(bin)] +
                             constants::kTwoPiD *
                                 static_cast<double>(instantaneous_frequency[bin]) * time_step);
      nearest_peak[static_cast<size_t>(bin)] = bin;
    }
    return;
  }

  int peak_index = 0;
  for (int bin = 0; bin < bin_count; ++bin) {
    while (peak_index + 1 < static_cast<int>(peaks.size())) {
      const int boundary =
          (peaks[static_cast<size_t>(peak_index)] + peaks[static_cast<size_t>(peak_index + 1)]) / 2;
      if (bin <= boundary) break;
      ++peak_index;
    }
    nearest_peak[static_cast<size_t>(bin)] = peaks[static_cast<size_t>(peak_index)];
  }

  for (int peak_bin : peaks) {
    phase_accumulator[static_cast<size_t>(peak_bin)] =
        first_frame ? static_cast<double>(analysis_phase[peak_bin])
                    : wrap(phase_accumulator[static_cast<size_t>(peak_bin)] +
                           constants::kTwoPiD *
                               static_cast<double>(instantaneous_frequency[peak_bin]) * time_step);
  }

  for (int bin = 0; bin < bin_count; ++bin) {
    const int peak_bin = nearest_peak[static_cast<size_t>(bin)];
    const double synth_phase = bin == peak_bin ? phase_accumulator[static_cast<size_t>(peak_bin)]
                                               : phase_accumulator[static_cast<size_t>(peak_bin)] +
                                                     static_cast<double>(analysis_phase[bin]) -
                                                     static_cast<double>(analysis_phase[peak_bin]);
    phase_accumulator[static_cast<size_t>(bin)] = wrap(synth_phase);
  }
}

}  // namespace sonare::phase
