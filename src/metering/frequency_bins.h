#pragma once

#include "util/frequency_bins.h"

namespace sonare::metering {

inline std::vector<float> bin_frequencies(int n_bins, int sample_rate, int n_fft) {
  return util::bin_frequencies(n_bins, sample_rate, n_fft);
}

}  // namespace sonare::metering
