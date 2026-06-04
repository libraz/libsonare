#include "metering/spectrum.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "core/fft.h"
#include "core/window.h"
#include "metering/frequency_bins.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/fractional_octave.h"

namespace sonare::metering {

namespace {

void validate_spectrum_config(const SpectrumConfig& config) {
  SONARE_CHECK(config.n_fft > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.octave_fraction > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.db_ref > 0.0f && config.db_amin > 0.0f, ErrorCode::InvalidParameter);
}

// Allocate frequencies/magnitude/power/db at full bin count, zeroed.
SpectrumResult make_empty_result(const Audio& audio, const SpectrumConfig& config) {
  SpectrumResult result;
  result.n_fft = config.n_fft;
  result.sample_rate = audio.sample_rate();
  const int n_bins = config.n_fft / 2 + 1;
  result.frequencies = bin_frequencies(n_bins, audio.sample_rate(), config.n_fft);
  result.magnitude.assign(n_bins, 0.0f);
  result.power.assign(n_bins, 0.0f);
  result.db.assign(n_bins, sonare::constants::kFloorDb);
  return result;
}

// Periodic Hann coherent-gain compensation so a bin-centered sinusoid reports
// the same FFT magnitude as an unwindowed frame. RMS gain would under-report the
// amplitude by ~1.76 dB for Hann and would carry that error into power=magnitude^2.
float window_coherent_norm(const std::vector<float>& window) {
  double window_sum = 0.0;
  for (float w : window) window_sum += static_cast<double>(w);
  return window_sum > 0.0 ? static_cast<float>(static_cast<double>(window.size()) / window_sum)
                          : 1.0f;
}

// Shared post-processing: derive power, optional fractional-octave smoothing, and
// the dB array from an already-populated magnitude spectrum.
void finalize_spectrum(SpectrumResult& result, const SpectrumConfig& config) {
  const int n_bins = static_cast<int>(result.magnitude.size());
  for (int i = 0; i < n_bins; ++i) {
    result.power[i] = result.magnitude[i] * result.magnitude[i];
  }

  if (config.apply_octave_smoothing) {
    result.magnitude =
        smooth_fractional_octave(result.magnitude, result.frequencies, config.octave_fraction);
    for (int i = 0; i < n_bins; ++i) {
      result.power[i] = result.magnitude[i] * result.magnitude[i];
    }
  }

  for (int i = 0; i < n_bins; ++i) {
    const float amplitude = std::max(config.db_amin, result.magnitude[i]);
    result.db[i] = linear_to_db(amplitude / config.db_ref);
  }
}

}  // namespace

SpectrumResult spectrum(const Audio& audio, const SpectrumConfig& config) {
  validate_spectrum_config(config);

  SpectrumResult result = make_empty_result(audio, config);
  const int n_bins = config.n_fft / 2 + 1;
  if (audio.empty()) return result;

  // Welch-style averaging: window each hop-advanced frame with a periodic Hann
  // window and average the power spectra across the whole signal, rather than
  // FFTing only the first n_fft samples with an implicit rectangular window
  // (which leaks energy and ignores most of the input).
  const size_t n_fft = static_cast<size_t>(config.n_fft);
  const std::vector<float>& window = get_window_cached(WindowType::Hann, config.n_fft, true);
  const float window_norm = window_coherent_norm(window);

  const size_t hop = std::max<size_t>(1, n_fft / 2);

  std::vector<float> frame(n_fft, 0.0f);
  std::vector<std::complex<float>> bins(n_bins);
  std::vector<double> power_accum(static_cast<size_t>(n_bins), 0.0);
  FFT fft(config.n_fft);

  size_t num_frames = 0;
  for (size_t start = 0;; start += hop) {
    const size_t available = start < audio.size() ? audio.size() - start : 0;
    const size_t copy_count = std::min(available, n_fft);
    for (size_t i = 0; i < copy_count; ++i) {
      frame[i] = audio.data()[start + i] * window[i];
    }
    for (size_t i = copy_count; i < n_fft; ++i) frame[i] = 0.0f;

    fft.forward(frame.data(), bins.data());
    for (int i = 0; i < n_bins; ++i) {
      const float mag = std::abs(bins[i]);
      power_accum[static_cast<size_t>(i)] += static_cast<double>(mag) * static_cast<double>(mag);
    }
    ++num_frames;

    // Stop once the frame has consumed the tail of the signal (the last frame is
    // zero-padded, mirroring center=false STFT behavior).
    if (start + n_fft >= audio.size()) break;
  }

  const float inv_frames = num_frames > 0 ? 1.0f / static_cast<float>(num_frames) : 1.0f;
  for (int i = 0; i < n_bins; ++i) {
    const float avg_power = static_cast<float>(power_accum[static_cast<size_t>(i)]) * inv_frames;
    result.magnitude[i] = std::sqrt(avg_power) * window_norm;
  }

  finalize_spectrum(result, config);
  return result;
}

SpectrumResult spectrum_frame(const Audio& audio, size_t frame_offset,
                              const SpectrumConfig& config) {
  validate_spectrum_config(config);

  SpectrumResult result = make_empty_result(audio, config);
  const int n_bins = config.n_fft / 2 + 1;
  if (audio.empty()) return result;

  // Single-frame: one Hann-windowed n_fft FFT at frame_offset, zero-padded past
  // the end of the buffer. No averaging, so transients are preserved.
  const size_t n_fft = static_cast<size_t>(config.n_fft);
  const std::vector<float>& window = get_window_cached(WindowType::Hann, config.n_fft, true);
  const float window_norm = window_coherent_norm(window);

  std::vector<float> frame(n_fft, 0.0f);
  std::vector<std::complex<float>> bins(n_bins);
  const size_t available = frame_offset < audio.size() ? audio.size() - frame_offset : 0;
  const size_t copy_count = std::min(available, n_fft);
  for (size_t i = 0; i < copy_count; ++i) {
    frame[i] = audio.data()[frame_offset + i] * window[i];
  }

  FFT fft(config.n_fft);
  fft.forward(frame.data(), bins.data());
  for (int i = 0; i < n_bins; ++i) {
    result.magnitude[i] = std::abs(bins[i]) * window_norm;
  }

  finalize_spectrum(result, config);
  return result;
}

std::vector<float> smooth_fractional_octave(const std::vector<float>& values,
                                            const std::vector<float>& frequencies,
                                            int octave_fraction) {
  return util::smooth_fractional_octave(values, frequencies, octave_fraction);
}

}  // namespace sonare::metering
