#pragma once

#include <cstddef>
#include <vector>

namespace sonare::metering {

inline std::vector<float> bin_frequencies(int n_bins, int sample_rate, int n_fft) {
  std::vector<float> frequencies(static_cast<size_t>(n_bins));
  const float bin_width = static_cast<float>(sample_rate) / static_cast<float>(n_fft);
  for (int i = 0; i < n_bins; ++i) {
    frequencies[static_cast<size_t>(i)] = static_cast<float>(i) * bin_width;
  }
  return frequencies;
}

}  // namespace sonare::metering
