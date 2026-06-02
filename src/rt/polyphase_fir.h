#pragma once

/// @file polyphase_fir.h
/// @brief Header-only FIR design and polyphase interpolation helpers.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "util/constants.h"

namespace sonare::rt {

struct PolyphaseFir {
  int phases = 1;
  int taps_per_phase = 1;
  std::vector<std::vector<float>> phase_taps;
};

inline double modified_bessel_i0(double x) {
  double sum = 1.0;
  double term = 1.0;
  const double half_x = x * 0.5;
  for (int k = 1; k < 32; ++k) {
    term *= (half_x / static_cast<double>(k)) * (half_x / static_cast<double>(k));
    sum += term;
    if (term < sum * 1.0e-12) {
      break;
    }
  }
  return sum;
}

inline std::vector<float> design_windowed_sinc_lowpass(int total_taps, int oversample_factor,
                                                       double kaiser_beta = 0.0,
                                                       bool use_kaiser = false) {
  std::vector<float> taps(static_cast<size_t>(total_taps), 0.0f);
  if (total_taps <= 0 || oversample_factor <= 0) {
    return taps;
  }

  const double center = (static_cast<double>(total_taps) - 1.0) * 0.5;
  const double cutoff = 1.0 / static_cast<double>(oversample_factor);
  const double i0_beta = use_kaiser ? modified_bessel_i0(kaiser_beta) : 1.0;
  for (int n = 0; n < total_taps; ++n) {
    const double m = static_cast<double>(n) - center;
    const double sinc_value =
        std::abs(m) < 1.0e-9
            ? cutoff
            : std::sin(::sonare::constants::kPiD * cutoff * m) / (::sonare::constants::kPiD * m);
    double window = 1.0;
    if (total_taps > 1) {
      if (use_kaiser) {
        const double r = (2.0 * static_cast<double>(n)) / static_cast<double>(total_taps - 1) - 1.0;
        window = modified_bessel_i0(kaiser_beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0_beta;
      } else {
        window = 0.5 - 0.5 * std::cos(2.0 * ::sonare::constants::kPiD * static_cast<double>(n) /
                                      static_cast<double>(total_taps - 1));
      }
    }
    taps[static_cast<size_t>(n)] = static_cast<float>(sinc_value * window);
  }

  double dc_gain = 0.0;
  for (float tap : taps) {
    dc_gain += tap;
  }
  if (std::abs(dc_gain) > 1.0e-12) {
    const float scale = static_cast<float>(static_cast<double>(oversample_factor) / dc_gain);
    for (float& tap : taps) {
      tap *= scale;
    }
  }
  return taps;
}

inline PolyphaseFir build_polyphase(const std::vector<float>& taps, int oversample_factor) {
  PolyphaseFir fir;
  fir.phases = oversample_factor;
  // Round up so a prototype whose length is not an exact multiple of the
  // oversample factor keeps its trailing taps instead of silently truncating
  // the filter (which would skew its DC gain). Phases that have no coefficient
  // at the final tap index keep the zero-initialized value.
  fir.taps_per_phase =
      oversample_factor <= 0
          ? 0
          : (static_cast<int>(taps.size()) + oversample_factor - 1) / oversample_factor;
  fir.phase_taps.assign(
      static_cast<size_t>(std::max(0, fir.phases)),
      std::vector<float>(static_cast<size_t>(std::max(0, fir.taps_per_phase)), 0.0f));
  for (int phase = 0; phase < fir.phases; ++phase) {
    for (int tap = 0; tap < fir.taps_per_phase; ++tap) {
      const size_t src =
          static_cast<size_t>(tap) * static_cast<size_t>(fir.phases) + static_cast<size_t>(phase);
      if (src >= taps.size()) {
        continue;
      }
      fir.phase_taps[static_cast<size_t>(phase)][static_cast<size_t>(tap)] = taps[src];
    }
  }
  return fir;
}

inline PolyphaseFir design_polyphase_lowpass(int oversample_factor, int total_taps,
                                             double kaiser_beta = 0.0, bool use_kaiser = false) {
  return build_polyphase(
      design_windowed_sinc_lowpass(total_taps, oversample_factor, kaiser_beta, use_kaiser),
      oversample_factor);
}

inline float interpolate_polyphase_sample(const float* data, size_t length, size_t index, int phase,
                                          const PolyphaseFir& fir) {
  if (data == nullptr || length == 0 || phase < 0 || phase >= fir.phases ||
      fir.taps_per_phase <= 0) {
    return 0.0f;
  }
  if (phase == 0) {
    return data[index];
  }

  const auto& h = fir.phase_taps[static_cast<size_t>(phase)];
  const int half = fir.taps_per_phase / 2;
  double accum = 0.0;
  for (int tap = 0; tap < fir.taps_per_phase; ++tap) {
    const long src = static_cast<long>(index) + static_cast<long>(tap) - static_cast<long>(half);
    if (src < 0 || src >= static_cast<long>(length)) {
      continue;
    }
    accum += static_cast<double>(h[static_cast<size_t>(tap)]) *
             static_cast<double>(data[static_cast<size_t>(src)]);
  }
  return static_cast<float>(accum);
}

}  // namespace sonare::rt
