#include "core/spectrum.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "core/fft.h"
#include "core/window.h"
#include "util/exception.h"

namespace sonare {

namespace {

/// @brief Pads signal with zeros for centered STFT (librosa compatible).
/// @details librosa uses pad_mode='constant' (zero padding) by default.
std::vector<float> pad_center(const float* data, size_t size, int pad_length) {
  std::vector<float> padded(size + 2 * pad_length, 0.0f);

  // Copy original data to center
  std::copy(data, data + size, padded.begin() + pad_length);

  return padded;
}

}  // namespace

Spectrogram::Spectrogram() : n_bins_(0), n_frames_(0), n_fft_(0), hop_length_(0), sample_rate_(0) {}

Spectrogram::Spectrogram(std::vector<std::complex<float>> data, int n_bins, int n_frames, int n_fft,
                         int hop_length, int sample_rate)
    : data_(std::move(data)),
      n_bins_(n_bins),
      n_frames_(n_frames),
      n_fft_(n_fft),
      hop_length_(hop_length),
      sample_rate_(sample_rate) {}

Spectrogram Spectrogram::compute(const Audio& audio, const StftConfig& config,
                                 SpectrogramProgressCallback progress_callback) {
  if (audio.empty()) {
    return Spectrogram();
  }

  SONARE_CHECK(config.n_fft > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config.hop_length > 0, ErrorCode::InvalidParameter);

  int n_fft = config.n_fft;
  int hop_length = config.hop_length;
  int win_length = config.actual_win_length();

  SONARE_CHECK(win_length <= n_fft, ErrorCode::InvalidParameter);

  // Get cached window
  const std::vector<float>& window = get_window_cached(config.window, win_length);

  // Pad window to n_fft if necessary
  std::vector<float> padded_window(n_fft, 0.0f);
  int win_offset = (n_fft - win_length) / 2;
  std::copy(window.begin(), window.end(), padded_window.begin() + win_offset);

  // Prepare input signal
  const float* signal = audio.data();
  size_t signal_length = audio.size();

  std::vector<float> padded_signal;
  if (config.center) {
    int pad_length = n_fft / 2;
    padded_signal = pad_center(signal, signal_length, pad_length);
    signal = padded_signal.data();
    signal_length = padded_signal.size();
  }

  // Calculate number of frames
  int n_frames = 1 + static_cast<int>((signal_length - n_fft) / hop_length);
  if (n_frames <= 0) {
    n_frames = 1;
  }

  int n_bins = n_fft / 2 + 1;

  // Allocate output
  std::vector<std::complex<float>> spectrum(n_bins * n_frames);

  // Create FFT processor
  FFT fft(n_fft);

  // Temporary buffer for windowed frame
  std::vector<float> frame(n_fft);
  std::vector<std::complex<float>> frame_spectrum(n_bins);

  // Progress reporting interval (every 5% or at least every 100 frames)
  int progress_interval = std::max(1, std::min(n_frames / 20, 100));

  // Process each frame
  for (int t = 0; t < n_frames; ++t) {
    int start = t * hop_length;

    // Extract and window frame - optimized by moving boundary check outside loop
    int valid_samples = std::min(n_fft, static_cast<int>(signal_length) - start);

    if (valid_samples >= n_fft) {
      // Fast path: no boundary handling needed (most common case)
      for (int i = 0; i < n_fft; ++i) {
        frame[i] = signal[start + i] * padded_window[i];
      }
    } else if (valid_samples > 0) {
      // Copy valid samples with windowing
      for (int i = 0; i < valid_samples; ++i) {
        frame[i] = signal[start + i] * padded_window[i];
      }
      // Zero-fill remainder
      std::fill(frame.begin() + valid_samples, frame.end(), 0.0f);
    } else {
      // All zeros (shouldn't happen with proper padding)
      std::fill(frame.begin(), frame.end(), 0.0f);
    }

    // Compute FFT
    fft.forward(frame.data(), frame_spectrum.data());

    // Store in output (column-major: [n_bins x n_frames])
    for (int f = 0; f < n_bins; ++f) {
      spectrum[f * n_frames + t] = frame_spectrum[f];
    }

    // Report progress
    if (progress_callback && (t % progress_interval == 0 || t == n_frames - 1)) {
      progress_callback(static_cast<float>(t + 1) / n_frames);
    }
  }

  return Spectrogram(std::move(spectrum), n_bins, n_frames, n_fft, hop_length, audio.sample_rate());
}

Spectrogram Spectrogram::from_complex(const std::complex<float>* data, int n_bins, int n_frames,
                                      int n_fft, int hop_length, int sample_rate) {
  std::vector<std::complex<float>> spectrum(data, data + n_bins * n_frames);
  return Spectrogram(std::move(spectrum), n_bins, n_frames, n_fft, hop_length, sample_rate);
}

float Spectrogram::duration() const {
  if (sample_rate_ == 0) {
    return 0.0f;
  }
  return static_cast<float>(n_frames_ * hop_length_) / sample_rate_;
}

MatrixView<std::complex<float>> Spectrogram::complex_view() const {
  return MatrixView<std::complex<float>>(data_.data(), n_bins_, n_frames_);
}

const std::complex<float>* Spectrogram::complex_data() const { return data_.data(); }

const std::vector<float>& Spectrogram::magnitude() const {
  if (magnitude_cache_.empty() && !data_.empty()) {
    magnitude_cache_.resize(data_.size());
    for (size_t i = 0; i < data_.size(); ++i) {
      magnitude_cache_[i] = std::abs(data_[i]);
    }
  }
  return magnitude_cache_;
}

const std::vector<float>& Spectrogram::power() const {
  if (power_cache_.empty() && !data_.empty()) {
    power_cache_.resize(data_.size());
    for (size_t i = 0; i < data_.size(); ++i) {
      float mag = std::abs(data_[i]);
      power_cache_[i] = mag * mag;
    }
  }
  return power_cache_;
}

std::vector<float> Spectrogram::to_db(float ref, float amin) const {
  const std::vector<float>& pwr = power();
  std::vector<float> db(pwr.size());

  float ref_power = ref * ref;
  for (size_t i = 0; i < pwr.size(); ++i) {
    float val = std::max(pwr[i], amin * amin);
    db[i] = 10.0f * std::log10(val / ref_power);
  }
  return db;
}

const std::complex<float>& Spectrogram::at(int bin, int frame) const {
  SONARE_CHECK(bin >= 0 && bin < n_bins_, ErrorCode::InvalidParameter);
  SONARE_CHECK(frame >= 0 && frame < n_frames_, ErrorCode::InvalidParameter);
  return data_[bin * n_frames_ + frame];
}

Audio Spectrogram::to_audio(int length, WindowType window_type) const {
  if (empty()) {
    return Audio();
  }

  // Get cached synthesis window
  const std::vector<float>& window = get_window_cached(window_type, n_fft_);

  // Calculate full reconstruction length (before trimming)
  int full_length = (n_frames_ - 1) * hop_length_ + n_fft_;

  // Allocate output and normalization buffers
  std::vector<float> output(full_length, 0.0f);
  std::vector<float> window_sum(full_length, 0.0f);

  // Create FFT processor
  FFT fft(n_fft_);

  // Temporary buffers
  std::vector<std::complex<float>> frame_spectrum(n_fft_ / 2 + 1);
  std::vector<float> frame(n_fft_);

  // Process each frame with overlap-add
  for (int t = 0; t < n_frames_; ++t) {
    // Extract spectrum for this frame
    for (int f = 0; f < n_fft_ / 2 + 1; ++f) {
      frame_spectrum[f] = data_[f * n_frames_ + t];
    }

    // Inverse FFT
    fft.inverse(frame_spectrum.data(), frame.data());

    // Overlap-add with window
    int start = t * hop_length_;
    for (int i = 0; i < n_fft_; ++i) {
      int idx = start + i;
      if (idx >= 0 && idx < full_length) {
        output[idx] += frame[i] * window[i];
        window_sum[idx] += window[i] * window[i];
      }
    }
  }

  // Normalize by window sum (COLA condition)
  constexpr float kEpsilon = 1e-8f;
  for (int i = 0; i < full_length; ++i) {
    if (window_sum[i] > kEpsilon) {
      output[i] /= window_sum[i];
    }
  }

  // Trim to requested length or remove center padding
  int trim_start = n_fft_ / 2;  // Remove padding added by center=true
  int trim_end = full_length - n_fft_ / 2;

  if (length > 0) {
    // Use requested length
    trim_end = std::min(trim_start + length, full_length);
  }

  if (trim_start < trim_end && trim_start < full_length) {
    std::vector<float> trimmed(output.begin() + trim_start,
                               output.begin() + std::min(trim_end, full_length));
    return Audio::from_vector(std::move(trimmed), sample_rate_);
  }

  return Audio::from_vector(std::move(output), sample_rate_);
}

Audio griffin_lim(const float* magnitude, int n_bins, int n_frames, int n_fft, int hop_length,
                  int sample_rate, const GriffinLimConfig& config) {
  SONARE_CHECK(magnitude != nullptr, ErrorCode::InvalidParameter);
  SONARE_CHECK(n_bins > 0 && n_frames > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(n_bins == n_fft / 2 + 1, ErrorCode::InvalidParameter);

  // Initialize with random phase
  std::vector<std::complex<float>> spectrum(n_bins * n_frames);
  std::mt19937 rng(42);  // Fixed seed for reproducibility
  std::uniform_real_distribution<float> dist(0.0f, 2.0f * 3.14159265358979323846f);

  for (int f = 0; f < n_bins; ++f) {
    for (int t = 0; t < n_frames; ++t) {
      float mag = magnitude[f * n_frames + t];
      float phase = dist(rng);
      spectrum[f * n_frames + t] = std::polar(mag, phase);
    }
  }

  // Previous angles for momentum
  std::vector<float> prev_angles(n_bins * n_frames, 0.0f);

  // Create spectrogram wrapper for iSTFT
  StftConfig stft_config;
  stft_config.n_fft = n_fft;
  stft_config.hop_length = hop_length;
  stft_config.center = true;

  // FFT processor (window not directly used in current implementation,
  // but kept for potential future optimization)
  FFT fft(n_fft);
  (void)fft;  // Suppress unused warning - used by Spectrogram methods internally

  // Iterate
  for (int iter = 0; iter < config.n_iter; ++iter) {
    // Create spectrogram and do iSTFT
    Spectrogram spec = Spectrogram::from_complex(spectrum.data(), n_bins, n_frames, n_fft,
                                                 hop_length, sample_rate);
    Audio reconstructed = spec.to_audio();

    // Forward STFT of reconstructed signal
    Spectrogram new_spec = Spectrogram::compute(reconstructed, stft_config);

    // Update phase while preserving magnitude
    for (int f = 0; f < n_bins; ++f) {
      for (int t = 0; t < n_frames; ++t) {
        int idx = f * n_frames + t;
        float target_mag = magnitude[idx];

        std::complex<float> new_val = new_spec.at(f, t);
        float new_angle = std::arg(new_val);

        // Apply momentum
        if (iter > 0 && config.momentum > 0.0f) {
          float angle_diff = new_angle - prev_angles[idx];
          // Wrap angle difference to [-pi, pi]
          while (angle_diff > 3.14159265f) angle_diff -= 2.0f * 3.14159265f;
          while (angle_diff < -3.14159265f) angle_diff += 2.0f * 3.14159265f;
          new_angle = prev_angles[idx] + angle_diff * (1.0f - config.momentum);
        }

        prev_angles[idx] = new_angle;
        spectrum[idx] = std::polar(target_mag, new_angle);
      }
    }
  }

  // Final reconstruction
  Spectrogram final_spec =
      Spectrogram::from_complex(spectrum.data(), n_bins, n_frames, n_fft, hop_length, sample_rate);
  return final_spec.to_audio();
}

Audio griffin_lim(const std::vector<float>& magnitude, int n_bins, int n_frames, int n_fft,
                  int hop_length, int sample_rate, const GriffinLimConfig& config) {
  return griffin_lim(magnitude.data(), n_bins, n_frames, n_fft, hop_length, sample_rate, config);
}

}  // namespace sonare
