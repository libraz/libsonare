#pragma once

/// @file excess_delay.h
/// @brief Pure delay of an impulse response beyond its minimum-phase part.
///
/// A binaural renderer built as minimum-phase HRIR plus an ITD delay puts the
/// ITD entirely in the excess phase, so the difference of the two ears' excess
/// delays reads the ITD off the audio without knowing either HRIR. The excess
/// phase is the response's phase minus that of its minimum-phase counterpart
/// (real-cepstrum folding), and its slope over a low band is the delay.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "core/fft.h"
#include "util/constants.h"

namespace sonare::test {

/// Excess delay of @p ir in samples: the least-squares slope of the unwrapped
/// excess phase between @p low_hz and @p high_hz.
inline double excess_delay_samples(const std::vector<float>& ir, double sample_rate,
                                   double low_hz = 200.0, double high_hz = 1500.0) {
  constexpr int kSize = 16384;
  constexpr float kFloor = 1e-12f;
  const int bins = kSize / 2 + 1;
  FFT fft(kSize);
  std::vector<float> buffer(kSize, 0.0f);
  std::copy_n(ir.begin(), std::min(ir.size(), buffer.size()), buffer.begin());
  std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(bins));
  fft.forward(buffer.data(), spectrum.data());

  std::vector<std::complex<float>> log_magnitude(static_cast<std::size_t>(bins));
  for (int k = 0; k < bins; ++k) {
    log_magnitude[static_cast<std::size_t>(k)] =
        std::log(std::max(std::abs(spectrum[static_cast<std::size_t>(k)]), kFloor));
  }
  std::vector<float> cepstrum(kSize, 0.0f);
  fft.inverse(log_magnitude.data(), cepstrum.data());
  for (int n = 1; n < kSize / 2; ++n) cepstrum[static_cast<std::size_t>(n)] *= 2.0f;
  for (int n = kSize / 2 + 1; n < kSize; ++n) cepstrum[static_cast<std::size_t>(n)] = 0.0f;
  std::vector<std::complex<float>> minimum_log(static_cast<std::size_t>(bins));
  fft.forward(cepstrum.data(), minimum_log.data());

  const auto bin_of = [&](double hz) {
    return static_cast<int>(std::lround(hz * kSize / sample_rate));
  };
  const int last = std::min(bins - 1, bin_of(high_hz));
  const int first = std::max(1, bin_of(low_hz));
  double unwrapped = 0.0;
  double previous = 0.0;
  double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
  int count = 0;
  for (int k = 0; k <= last; ++k) {
    const auto i = static_cast<std::size_t>(k);
    const double phase =
        std::arg(std::complex<double>(spectrum[i]) *
                 std::exp(std::complex<double>(0.0, -static_cast<double>(minimum_log[i].imag()))));
    double step = phase - previous;
    step -= constants::kTwoPiD * std::round(step / constants::kTwoPiD);
    unwrapped += step;
    previous = phase;
    if (k >= first) {
      const double omega = constants::kTwoPiD * k / kSize;
      sx += omega;
      sy += unwrapped;
      sxx += omega * omega;
      sxy += omega * unwrapped;
      ++count;
    }
  }
  const double slope = (count * sxy - sx * sy) / (count * sxx - sx * sx);
  return -slope;
}

}  // namespace sonare::test
