#include "effects/phase_vocoder.h"

#include <cmath>
#include <complex>
#include <vector>

#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare {

namespace {

/// @brief Wraps phase to [-pi, pi].
/// @details Uses std::remainder for O(1) computation without loops.
///          Returns 0 for NaN/Inf inputs to prevent undefined behavior.
float wrap_phase(float phase) {
  if (!std::isfinite(phase)) {
    return 0.0f;
  }
  return std::remainder(phase, kTwoPi);
}

}  // namespace

std::vector<float> compute_instantaneous_frequency(const float* phase, const float* prev_phase,
                                                   int n_bins, int hop_length, int sample_rate) {
  SONARE_CHECK(phase != nullptr && prev_phase != nullptr, ErrorCode::InvalidParameter);

  std::vector<float> inst_freq(n_bins);

  float time_step = static_cast<float>(hop_length) / static_cast<float>(sample_rate);

  for (int k = 0; k < n_bins; ++k) {
    /// Expected phase advance based on bin frequency
    float bin_freq = static_cast<float>(k) * static_cast<float>(sample_rate) /
                     static_cast<float>((n_bins - 1) * 2);
    float expected_phase_advance = kTwoPi * bin_freq * time_step;

    /// Actual phase difference
    float phase_diff = phase[k] - prev_phase[k];

    /// Phase deviation from expected
    float phase_deviation = wrap_phase(phase_diff - expected_phase_advance);

    /// Instantaneous frequency
    inst_freq[k] = bin_freq + phase_deviation / (kTwoPi * time_step);
  }

  return inst_freq;
}

Spectrogram phase_vocoder(const Spectrogram& spec, float rate, const PhaseVocoderConfig& config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(spec.n_frames() >= 2, ErrorCode::InvalidParameter);
  SONARE_CHECK(rate > 0.0f, ErrorCode::InvalidParameter);

  int n_bins = spec.n_bins();
  int n_frames_in = spec.n_frames();
  int n_fft = spec.n_fft();
  int hop_length = config.hop_length > 0 ? config.hop_length : spec.hop_length();
  int sample_rate = spec.sample_rate();

  /// Calculate output number of frames
  int n_frames_out = static_cast<int>(std::ceil(static_cast<float>(n_frames_in) / rate));
  if (n_frames_out < 1) n_frames_out = 1;

  /// Get input complex spectrum
  const std::complex<float>* input = spec.complex_data();

  /// Output complex spectrum
  std::vector<std::complex<float>> output(n_bins * n_frames_out);

  /// Phase accumulator
  std::vector<float> phase_acc(n_bins, 0.0f);

  /// Time step ratio
  float time_step = static_cast<float>(hop_length) / static_cast<float>(sample_rate);

  for (int t_out = 0; t_out < n_frames_out; ++t_out) {
    /// Input time position
    float t_in_f = static_cast<float>(t_out) * rate;
    int t_in = static_cast<int>(t_in_f);
    float frac = t_in_f - static_cast<float>(t_in);

    /// Clamp to valid range
    if (t_in >= n_frames_in - 1) {
      t_in = n_frames_in - 2;
      frac = 1.0f;
    }
    if (t_in < 0) {
      t_in = 0;
      frac = 0.0f;
    }

    for (int k = 0; k < n_bins; ++k) {
      /// Get adjacent frames
      std::complex<float> frame0 = input[k * n_frames_in + t_in];
      std::complex<float> frame1 = input[k * n_frames_in + t_in + 1];

      /// Interpolate magnitude
      float mag0 = std::abs(frame0);
      float mag1 = std::abs(frame1);
      float mag = mag0 * (1.0f - frac) + mag1 * frac;

      /// Compute phase advance
      float phase0 = std::arg(frame0);
      float phase1 = std::arg(frame1);

      /// Expected phase advance based on bin frequency
      float bin_freq =
          static_cast<float>(k) * static_cast<float>(sample_rate) / static_cast<float>(n_fft);
      float expected_advance = kTwoPi * bin_freq * time_step;

      /// Phase difference with unwrapping
      float phase_diff = wrap_phase(phase1 - phase0 - expected_advance);
      float inst_freq = bin_freq + phase_diff / (kTwoPi * time_step);

      /// Accumulate phase
      if (t_out == 0) {
        phase_acc[k] = phase0 + frac * wrap_phase(phase1 - phase0);
      } else {
        phase_acc[k] += kTwoPi * inst_freq * time_step;
        phase_acc[k] = wrap_phase(phase_acc[k]);
      }

      /// Construct output complex value
      output[k * n_frames_out + t_out] = std::polar(mag, phase_acc[k]);
    }
  }

  return Spectrogram::from_complex(output.data(), n_bins, n_frames_out, n_fft, hop_length,
                                   sample_rate, spec.center(), spec.win_length());
}

Spectrogram phase_vocoder_phaselocked(const Spectrogram& spec, float rate,
                                      const PhaseVocoderConfig& config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(spec.n_frames() >= 2, ErrorCode::InvalidParameter);
  SONARE_CHECK(rate > 0.0f, ErrorCode::InvalidParameter);

  int n_bins = spec.n_bins();
  int n_frames_in = spec.n_frames();
  int n_fft = spec.n_fft();
  int hop_length = config.hop_length > 0 ? config.hop_length : spec.hop_length();
  int sample_rate = spec.sample_rate();

  int n_frames_out = static_cast<int>(std::ceil(static_cast<float>(n_frames_in) / rate));
  if (n_frames_out < 1) n_frames_out = 1;

  const std::complex<float>* input = spec.complex_data();
  std::vector<std::complex<float>> output(n_bins * n_frames_out);

  /// Synthesis phase accumulator (per bin).
  std::vector<float> phase_acc(n_bins, 0.0f);

  float time_step = static_cast<float>(hop_length) / static_cast<float>(sample_rate);

  /// Reused per-frame scratch buffers (avoid per-frame heap churn).
  std::vector<float> mag(n_bins, 0.0f);
  std::vector<float> ana_phase(n_bins, 0.0f);
  std::vector<float> inst_freq(n_bins, 0.0f);
  std::vector<int> peaks;
  peaks.reserve(n_bins);
  std::vector<int> nearest_peak(n_bins, -1);

  for (int t_out = 0; t_out < n_frames_out; ++t_out) {
    float t_in_f = static_cast<float>(t_out) * rate;
    int t_in = static_cast<int>(t_in_f);
    float frac = t_in_f - static_cast<float>(t_in);

    if (t_in >= n_frames_in - 1) {
      t_in = n_frames_in - 2;
      frac = 1.0f;
    }
    if (t_in < 0) {
      t_in = 0;
      frac = 0.0f;
    }

    /// Magnitude, analysis phase and instantaneous frequency per bin (same path as
    /// phase_vocoder()).
    for (int k = 0; k < n_bins; ++k) {
      std::complex<float> frame0 = input[k * n_frames_in + t_in];
      std::complex<float> frame1 = input[k * n_frames_in + t_in + 1];

      float mag0 = std::abs(frame0);
      float mag1 = std::abs(frame1);
      mag[k] = mag0 * (1.0f - frac) + mag1 * frac;

      float phase0 = std::arg(frame0);
      float phase1 = std::arg(frame1);
      ana_phase[k] = phase0 + frac * wrap_phase(phase1 - phase0);

      float bin_freq =
          static_cast<float>(k) * static_cast<float>(sample_rate) / static_cast<float>(n_fft);
      float expected_advance = kTwoPi * bin_freq * time_step;
      float phase_diff = wrap_phase(phase1 - phase0 - expected_advance);
      inst_freq[k] = bin_freq + phase_diff / (kTwoPi * time_step);
    }

    /// Detect spectral peaks: local maxima of the (interpolated) magnitude.
    peaks.clear();
    for (int k = 1; k < n_bins - 1; ++k) {
      if (mag[k] > mag[k - 1] && mag[k] > mag[k + 1]) {
        peaks.push_back(k);
      }
    }

    if (peaks.empty()) {
      /// Silence/DC: fall back to standard per-bin phase accumulation.
      for (int k = 0; k < n_bins; ++k) {
        if (t_out == 0) {
          phase_acc[k] = ana_phase[k];
        } else {
          phase_acc[k] = wrap_phase(phase_acc[k] + kTwoPi * inst_freq[k] * time_step);
        }
        output[k * n_frames_out + t_out] = std::polar(mag[k], phase_acc[k]);
      }
      continue;
    }

    /// Assign each bin to its nearest peak (region of influence). The boundary
    /// between two adjacent peaks is their midpoint bin.
    {
      int p_idx = 0;
      for (int k = 0; k < n_bins; ++k) {
        /// Advance to the peak whose region contains bin k.
        while (p_idx + 1 < static_cast<int>(peaks.size())) {
          int boundary = (peaks[p_idx] + peaks[p_idx + 1]) / 2;
          if (k > boundary) {
            ++p_idx;
          } else {
            break;
          }
        }
        nearest_peak[k] = peaks[p_idx];
      }
    }

    /// Accumulate synthesis phase at peak bins only.
    for (int peak_bin : peaks) {
      if (t_out == 0) {
        phase_acc[peak_bin] = ana_phase[peak_bin];
      } else {
        phase_acc[peak_bin] =
            wrap_phase(phase_acc[peak_bin] + kTwoPi * inst_freq[peak_bin] * time_step);
      }
    }

    /// Lock every bin (including peaks) and emit output.
    for (int k = 0; k < n_bins; ++k) {
      int k_p = nearest_peak[k];
      float synth_phase =
          (k == k_p) ? phase_acc[k_p] : phase_acc[k_p] + (ana_phase[k] - ana_phase[k_p]);
      phase_acc[k] = wrap_phase(synth_phase);
      output[k * n_frames_out + t_out] = std::polar(mag[k], phase_acc[k]);
    }
  }

  return Spectrogram::from_complex(output.data(), n_bins, n_frames_out, n_fft, hop_length,
                                   sample_rate, spec.center(), spec.win_length());
}

}  // namespace sonare
