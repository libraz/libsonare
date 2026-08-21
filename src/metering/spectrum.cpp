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
#include "util/resource_limits.h"

namespace sonare::metering {

namespace {

void validate_spectrum_config(const SpectrumConfig& config) {
  SONARE_CHECK(resource::spectrum_shape_fits(config.n_fft), ErrorCode::InvalidParameter);
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

// One-sided spectrum doubling for every bin except DC and (for an even n_fft)
// Nyquist, which have no mirrored partner. The 1/n_fft that used to live here
// is gone: the coherent normalization is now per frame, against the window
// weights that actually multiplied samples.
float one_sided_bin_scale(int bin, int n_fft) {
  const bool edge_bin = bin == 0 || (n_fft % 2 == 0 && bin == n_fft / 2);
  return edge_bin ? 1.0f : 2.0f;
}

// The same doubling folded together with the 1/n_fft term, for spectrum_frame.
// That entry point documents zero-padding as its contract (a caller asks for one
// frame at an offset and gets what that frame contains), so it keeps the
// whole-window coherent gain.
float one_sided_amplitude_scale(int bin, int n_fft) {
  return one_sided_bin_scale(bin, n_fft) / static_cast<float>(n_fft);
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
    result.db[i] = std::max(sonare::constants::kFloorDb, linear_to_db(amplitude / config.db_ref));
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
  const auto window_handle = get_window_cached(WindowType::Hann, config.n_fft, true);
  const std::vector<float>& window = *window_handle;

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

    // Coherent gain over the POPULATED span, not the whole window. A frame
    // shorter than n_fft is zero-padded, so only window[0, copy_count) ever
    // multiplied a sample; dividing by the full window sum then under-reports
    // by exactly the ratio of the two sums. For a periodic Hann with n_fft=2048
    // and a 512-sample buffer that ratio is about 0.09, i.e. roughly -21 dB --
    // enough to make a short one-shot unusable against the peak/RMS meters
    // measured on the same buffer.
    double frame_window_sum = 0.0;
    for (size_t i = 0; i < copy_count; ++i) frame_window_sum += static_cast<double>(window[i]);
    const double frame_norm = frame_window_sum > 0.0 ? 1.0 / frame_window_sum : 0.0;

    fft.forward(frame.data(), bins.data());
    for (int i = 0; i < n_bins; ++i) {
      const double mag = static_cast<double>(std::abs(bins[i])) * frame_norm;
      power_accum[static_cast<size_t>(i)] += mag * mag;
    }
    ++num_frames;

    // Stop once the frame has consumed the tail of the signal (the last frame is
    // zero-padded, mirroring center=false STFT behavior).
    if (start + n_fft >= audio.size()) break;
  }

  const float inv_frames = num_frames > 0 ? 1.0f / static_cast<float>(num_frames) : 1.0f;
  for (int i = 0; i < n_bins; ++i) {
    const float avg_power = static_cast<float>(power_accum[static_cast<size_t>(i)]) * inv_frames;
    // The per-frame coherent normalization above already divided by the window
    // sum, so only the one-sided doubling remains here.
    result.magnitude[i] = std::sqrt(avg_power) * one_sided_bin_scale(i, config.n_fft);
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
  const auto window_handle = get_window_cached(WindowType::Hann, config.n_fft, true);
  const std::vector<float>& window = *window_handle;
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
    result.magnitude[i] =
        std::abs(bins[i]) * window_norm * one_sided_amplitude_scale(i, config.n_fft);
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
