#include "core/spectrum.h"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <random>

#include "core/fft.h"
#include "core/window.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"
#include "util/reflect_padding.h"

namespace sonare {

using sonare::constants::kTwoPi;

namespace {

std::vector<float> pad_center(const float* data, size_t size, int pad_length, PadMode pad_mode) {
  std::vector<float> padded(size + 2 * pad_length, 0.0f);
  if (data == nullptr || size == 0) {
    return padded;
  }
  if (pad_mode == PadMode::Constant) {
    std::copy(data, data + size, padded.begin() + pad_length);
    return padded;
  }
  for (size_t i = 0; i < padded.size(); ++i) {
    const int64_t src = static_cast<int64_t>(i) - pad_length;
    padded[i] = data[reflect_index(src, size)];
  }
  return padded;
}

}  // namespace

Spectrogram::Spectrogram()
    : n_bins_(0),
      n_frames_(0),
      n_fft_(0),
      hop_length_(0),
      sample_rate_(0),
      win_length_(0),
      center_(true) {}

Spectrogram::Spectrogram(std::vector<std::complex<float>> data, int n_bins, int n_frames, int n_fft,
                         int hop_length, int sample_rate, int win_length, bool center)
    : data_(std::move(data)),
      n_bins_(n_bins),
      n_frames_(n_frames),
      n_fft_(n_fft),
      hop_length_(hop_length),
      sample_rate_(sample_rate),
      win_length_(win_length > 0 ? win_length : n_fft),
      center_(center) {}

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

  // Get cached window (periodic for STFT, matching librosa/scipy fftbins=True).
  const std::vector<float>& window = get_window_cached(config.window, win_length, true);

  /// Pad window to n_fft if necessary
  std::vector<float> padded_window(n_fft, 0.0f);
  int win_offset = (n_fft - win_length) / 2;
  std::copy(window.begin(), window.end(), padded_window.begin() + win_offset);

  /// Prepare input signal
  const float* signal = audio.data();
  size_t signal_length = audio.size();

  std::vector<float> padded_signal;
  if (config.center) {
    int pad_length = n_fft / 2;
    padded_signal = pad_center(signal, signal_length, pad_length, config.pad_mode);
    signal = padded_signal.data();
    signal_length = padded_signal.size();
  }

  /// Calculate number of frames
  int n_frames = 1;
  if (signal_length >= static_cast<size_t>(n_fft)) {
    n_frames = 1 + static_cast<int>((signal_length - static_cast<size_t>(n_fft)) /
                                    static_cast<size_t>(hop_length));
  }

  int n_bins = n_fft / 2 + 1;

  /// Allocate output
  std::vector<std::complex<float>> spectrum(n_bins * n_frames);

  /// Create FFT processor
  FFT fft(n_fft);

  /// Temporary buffer for windowed frame
  std::vector<float> frame(n_fft);
  std::vector<std::complex<float>> frame_spectrum(n_bins);

  /// Progress reporting interval (every 5% or at least every 100 frames)
  int progress_interval = std::max(1, std::min(n_frames / 20, 100));

  /// Process each frame
  for (int t = 0; t < n_frames; ++t) {
    int start = t * hop_length;

    /// Extract and window frame - optimized by moving boundary check outside loop
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

  return Spectrogram(std::move(spectrum), n_bins, n_frames, n_fft, hop_length, audio.sample_rate(),
                     win_length, config.center);
}

Spectrogram Spectrogram::from_complex(const std::complex<float>* data, int n_bins, int n_frames,
                                      int n_fft, int hop_length, int sample_rate, bool center,
                                      int win_length) {
  std::vector<std::complex<float>> spectrum(data, data + n_bins * n_frames);
  return Spectrogram(std::move(spectrum), n_bins, n_frames, n_fft, hop_length, sample_rate,
                     win_length, center);
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
    if (!power_cache_.empty()) {
      // Derive magnitude from cached power via sqrt — cheaper than recomputing
      // re²+im² + sqrt from the complex spectrum.
      for (size_t i = 0; i < power_cache_.size(); ++i) {
        magnitude_cache_[i] = std::sqrt(power_cache_[i]);
      }
    } else {
      for (size_t i = 0; i < data_.size(); ++i) {
        magnitude_cache_[i] = std::abs(data_[i]);
      }
    }
  }
  return magnitude_cache_;
}

const std::vector<float>& Spectrogram::power() const {
  if (power_cache_.empty() && !data_.empty()) {
    power_cache_.resize(data_.size());
    if (!magnitude_cache_.empty()) {
      // Magnitude already computed — squaring is cheaper than recomputing
      // re²+im² from the complex spectrum.
      for (size_t i = 0; i < magnitude_cache_.size(); ++i) {
        const float m = magnitude_cache_[i];
        power_cache_[i] = m * m;
      }
    } else {
      // re² + im² without sqrt (auto-vectorized by compiler — TIE with Eigen per §10.2.2)
      for (size_t i = 0; i < data_.size(); ++i) {
        const float re = data_[i].real();
        const float im = data_[i].imag();
        power_cache_[i] = re * re + im * im;
      }
    }
  }
  return power_cache_;
}

std::vector<float> Spectrogram::to_db(float ref, float amin, float top_db) const {
  const std::vector<float>& pwr = power();
  std::vector<float> db(pwr.size());
  power_to_db(pwr.data(), pwr.size(), ref, amin, top_db, db.data());
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

  // STFT analysis uses a periodic window (fftbins=True). iSTFT uses a symmetric
  // synthesis window and normalizes by analysis*synthesis overlap to preserve
  // reconstruction gain when the two window shapes differ.
  const std::vector<float>& analysis_win_short = get_window_cached(window_type, win_length_, true);
  const std::vector<float>& synthesis_win_short =
      get_window_cached(window_type, win_length_, false);

  // Zero-pad window to n_fft if win_length < n_fft (matches analysis padding)
  std::vector<float> analysis_window(n_fft_, 0.0f);
  std::vector<float> synthesis_window(n_fft_, 0.0f);
  int win_offset = (n_fft_ - win_length_) / 2;
  std::copy(analysis_win_short.begin(), analysis_win_short.end(),
            analysis_window.begin() + win_offset);
  std::copy(synthesis_win_short.begin(), synthesis_win_short.end(),
            synthesis_window.begin() + win_offset);

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

  // Eigen maps and precomputed analysis*synthesis (constant across frames).
  // Wrapping the synthesis / analysis windows in Eigen::Map lets the inner
  // overlap-add reuse SIMD-vectorized .segment().noalias() += expressions
  // instead of a hand-rolled scalar scatter-add.
  Eigen::Map<const Eigen::VectorXf> synthesis_window_vec(synthesis_window.data(), n_fft_);
  Eigen::Map<const Eigen::VectorXf> analysis_window_vec(analysis_window.data(), n_fft_);
  Eigen::Map<const Eigen::VectorXf> frame_vec(frame.data(), n_fft_);
  Eigen::Map<Eigen::VectorXf> output_vec(output.data(), full_length);
  Eigen::Map<Eigen::VectorXf> window_sum_vec(window_sum.data(), full_length);
  const Eigen::VectorXf window_product_vec = analysis_window_vec.cwiseProduct(synthesis_window_vec);

  // Process each frame with overlap-add
  for (int t = 0; t < n_frames_; ++t) {
    // Extract spectrum for this frame
    for (int f = 0; f < n_fft_ / 2 + 1; ++f) {
      frame_spectrum[f] = data_[f * n_frames_ + t];
    }

    // Inverse FFT
    fft.inverse(frame_spectrum.data(), frame.data());

    // Overlap-add with window. start >= 0 because hop_length_ > 0 and t >= 0;
    // start + n_fft_ <= full_length by construction (full_length =
    // (n_frames_ - 1) * hop_length_ + n_fft_), so the segment is fully inside
    // the output buffer and we can avoid the per-sample bounds check.
    const int start = t * hop_length_;
    output_vec.segment(start, n_fft_).noalias() += frame_vec.cwiseProduct(synthesis_window_vec);
    window_sum_vec.segment(start, n_fft_).noalias() += window_product_vec;
  }

  // Normalize by window sum (COLA condition). Use a vectorized select to keep
  // the original sample wherever window_sum is below the threshold, matching
  // the previous scalar behavior. The `.max(eps)` guard inside the division
  // keeps the divisor strictly positive for the lanes that get selected.
  const float eps = sonare::constants::kSpectrumEpsilon;
  output_vec =
      (window_sum_vec.array() > eps)
          .select(output_vec.array() / window_sum_vec.array().max(eps), output_vec.array());

  // Trim to requested length or remove center padding
  int trim_start = center_ ? n_fft_ / 2 : 0;
  int trim_end = center_ ? full_length - n_fft_ / 2 : full_length;

  if (length > 0) {
    std::vector<float> trimmed(static_cast<size_t>(length), 0.0f);
    if (trim_start < full_length) {
      int available = std::min(length, full_length - trim_start);
      if (available > 0) {
        std::copy(output.begin() + trim_start, output.begin() + trim_start + available,
                  trimmed.begin());
      }
    }
    return Audio::from_vector(std::move(trimmed), sample_rate_);
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
  std::uniform_real_distribution<float> dist(0.0f, kTwoPi);

  for (int f = 0; f < n_bins; ++f) {
    for (int t = 0; t < n_frames; ++t) {
      float mag = magnitude[f * n_frames + t];
      float phase = dist(rng);
      spectrum[f * n_frames + t] = std::polar(mag, phase);
    }
  }

  // Previous angles for momentum
  std::vector<float> prev_angles(n_bins * n_frames, 0.0f);
  const int target_length = std::max(0, (n_frames - 1) * hop_length);

  // Create spectrogram wrapper for iSTFT
  StftConfig stft_config;
  stft_config.n_fft = n_fft;
  stft_config.hop_length = hop_length;
  stft_config.center = true;

  // Iterate
  for (int iter = 0; iter < config.n_iter; ++iter) {
    // Create spectrogram and do iSTFT
    Spectrogram spec = Spectrogram::from_complex(spectrum.data(), n_bins, n_frames, n_fft,
                                                 hop_length, sample_rate, true);
    Audio reconstructed = spec.to_audio(target_length);

    // Forward STFT of reconstructed signal
    Spectrogram new_spec = Spectrogram::compute(reconstructed, stft_config);

    // Update phase while preserving magnitude
    for (int f = 0; f < n_bins; ++f) {
      for (int t = 0; t < n_frames; ++t) {
        int idx = f * n_frames + t;
        float target_mag = magnitude[idx];

        std::complex<float> new_val = new_spec.at(f, t);
        float new_angle = std::arg(new_val);

        // Apply momentum using the common phase-vocoder convention:
        //   rebuilt = (1 + momentum) * new_angles - momentum * prev_angles
        // High momentum extrapolates the phase update, accelerating convergence.
        float updated_angle = new_angle;
        if (iter > 0 && config.momentum > 0.0f) {
          updated_angle = (1.0f + config.momentum) * new_angle - config.momentum * prev_angles[idx];
        }

        prev_angles[idx] = new_angle;  // Store un-extrapolated angle for next iteration
        spectrum[idx] = std::polar(target_mag, updated_angle);
      }
    }
  }

  // Final reconstruction
  Spectrogram final_spec = Spectrogram::from_complex(spectrum.data(), n_bins, n_frames, n_fft,
                                                     hop_length, sample_rate, true);
  return final_spec.to_audio(target_length);
}

Audio griffin_lim(const std::vector<float>& magnitude, int n_bins, int n_frames, int n_fft,
                  int hop_length, int sample_rate, const GriffinLimConfig& config) {
  return griffin_lim(magnitude.data(), n_bins, n_frames, n_fft, hop_length, sample_rate, config);
}

MagPhase magphase(const std::complex<float>* spec, std::size_t n, float power) {
  if (power <= 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "magphase: power must be > 0");
  }
  if (n > 0 && spec == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "magphase: null input with non-zero length");
  }
  MagPhase out;
  out.magnitude.resize(n);
  out.phase.resize(n);
  // Use a tiny epsilon so we never divide by exactly zero. librosa returns 1+0j
  // for zero-magnitude bins; using a small eps keeps the same effect when the
  // magnitude is recombined later.
  constexpr float kPhaseEps = 1e-20f;
  for (std::size_t i = 0; i < n; ++i) {
    const float mag = std::abs(spec[i]);
    if (mag <= kPhaseEps) {
      out.phase[i] = std::complex<float>(1.0f, 0.0f);
    } else {
      out.phase[i] = spec[i] / mag;
    }
    out.magnitude[i] = (power == 1.0f) ? mag : std::pow(mag, power);
  }
  return out;
}

MagPhase magphase(const Spectrogram& spec, float power) {
  const std::size_t n =
      static_cast<std::size_t>(spec.n_bins()) * static_cast<std::size_t>(spec.n_frames());
  return magphase(spec.complex_data(), n, power);
}

namespace {

/// @brief Computes STFT of @p signal with an arbitrary precomputed window.
std::vector<std::complex<float>> stft_with_window(const float* signal, size_t signal_length,
                                                  const std::vector<float>& padded_window,
                                                  int n_fft, int hop_length, bool center,
                                                  PadMode pad_mode, int* out_n_frames) {
  std::vector<float> padded_signal;
  if (center) {
    int pad_length = n_fft / 2;
    padded_signal = pad_center(signal, signal_length, pad_length, pad_mode);
    signal = padded_signal.data();
    signal_length = padded_signal.size();
  }
  int n_frames = 1;
  if (signal_length >= static_cast<size_t>(n_fft)) {
    n_frames = 1 + static_cast<int>((signal_length - n_fft) / hop_length);
  }
  const int n_bins = n_fft / 2 + 1;
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(n_bins) * n_frames);
  FFT fft(n_fft);
  std::vector<float> frame(n_fft);
  std::vector<std::complex<float>> frame_spectrum(n_bins);
  for (int t = 0; t < n_frames; ++t) {
    int start = t * hop_length;
    int valid = std::min(n_fft, static_cast<int>(signal_length) - start);
    if (valid >= n_fft) {
      for (int i = 0; i < n_fft; ++i) frame[i] = signal[start + i] * padded_window[i];
    } else if (valid > 0) {
      for (int i = 0; i < valid; ++i) frame[i] = signal[start + i] * padded_window[i];
      std::fill(frame.begin() + valid, frame.end(), 0.0f);
    } else {
      std::fill(frame.begin(), frame.end(), 0.0f);
    }
    fft.forward(frame.data(), frame_spectrum.data());
    for (int f = 0; f < n_bins; ++f) {
      spectrum[static_cast<size_t>(f) * n_frames + t] = frame_spectrum[f];
    }
  }
  *out_n_frames = n_frames;
  return spectrum;
}

}  // namespace

namespace {

/// @brief Builds the three windows used by reassignment.
/// @details `padded_window` is the analysis window, zero-padded to n_fft.
///          `t_window` is the time-weighted window `(t - half) * w(t)`.
///          `dw_window` is the central-difference derivative of `w(t)`.
void build_reassignment_windows(const StftConfig& config, std::vector<float>& padded_window,
                                std::vector<float>& t_window, std::vector<float>& dw_window,
                                double& half_n) {
  const int n_fft = config.n_fft;
  const int win_length = config.actual_win_length();
  const std::vector<float>& window = get_window_cached(config.window, win_length, true);
  const int win_offset = (n_fft - win_length) / 2;
  padded_window.assign(n_fft, 0.0f);
  t_window.assign(n_fft, 0.0f);
  dw_window.assign(n_fft, 0.0f);
  std::copy(window.begin(), window.end(), padded_window.begin() + win_offset);
  half_n = 0.5 * static_cast<double>(win_length - 1);
  for (int i = 0; i < win_length; ++i) {
    const double t_sample = static_cast<double>(i) - half_n;
    t_window[win_offset + i] = static_cast<float>(t_sample * window[i]);
  }
  for (int i = 0; i < win_length; ++i) {
    const int idx = win_offset + i;
    const float prev = (i > 0) ? window[i - 1] : 0.0f;
    const float next = (i + 1 < win_length) ? window[i + 1] : 0.0f;
    dw_window[idx] = 0.5f * (next - prev);
  }
}

}  // namespace

ReassignedSpectrogram reassigned_spectrogram(const Audio& audio, const StftConfig& config,
                                             float ref_power, bool fill_nan) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  const int n_fft = config.n_fft;
  const int hop_length = config.hop_length;
  const int win_length = config.actual_win_length();
  SONARE_CHECK(win_length <= n_fft, ErrorCode::InvalidParameter);

  std::vector<float> padded_window, t_window, dw_window;
  double half_n = 0.0;
  build_reassignment_windows(config, padded_window, t_window, dw_window, half_n);

  const int sr = audio.sample_rate();
  const float* signal = audio.data();
  const size_t signal_len = audio.size();

  int n_frames = 0;
  std::vector<std::complex<float>> Sw =
      stft_with_window(signal, signal_len, padded_window, n_fft, hop_length, config.center,
                       config.pad_mode, &n_frames);
  int n_frames_t = 0;
  std::vector<std::complex<float>> Stw = stft_with_window(
      signal, signal_len, t_window, n_fft, hop_length, config.center, config.pad_mode, &n_frames_t);
  int n_frames_d = 0;
  std::vector<std::complex<float>> Sdw =
      stft_with_window(signal, signal_len, dw_window, n_fft, hop_length, config.center,
                       config.pad_mode, &n_frames_d);
  SONARE_CHECK(n_frames == n_frames_t && n_frames == n_frames_d, ErrorCode::InvalidParameter);

  const int n_bins = n_fft / 2 + 1;
  ReassignedSpectrogram out;
  out.magnitude.assign(static_cast<size_t>(n_bins) * n_frames, 0.0f);
  out.times.assign(static_cast<size_t>(n_bins) * n_frames, 0.0f);
  out.frequencies.assign(static_cast<size_t>(n_bins) * n_frames, 0.0f);

  const float bin_to_hz = static_cast<float>(sr) / static_cast<float>(n_fft);
  const float sample_to_sec = 1.0f / static_cast<float>(sr);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (int k = 0; k < n_bins; ++k) {
    for (int t = 0; t < n_frames; ++t) {
      const size_t idx = static_cast<size_t>(k) * n_frames + t;
      const std::complex<float> S = Sw[idx];
      const float power = std::norm(S);
      out.magnitude[idx] = std::sqrt(power);
      const float center_time =
          (static_cast<float>(t * hop_length) + (config.center ? 0.0f : half_n)) * sample_to_sec;
      const float center_freq = static_cast<float>(k) * bin_to_hz;
      if (power < ref_power) {
        out.times[idx] = fill_nan ? nan : center_time;
        out.frequencies[idx] = fill_nan ? nan : center_freq;
        continue;
      }
      const std::complex<float> r_time = Stw[idx] / S;
      out.times[idx] = center_time + r_time.real() * sample_to_sec;
      const std::complex<float> r_freq = Sdw[idx] / S;
      const float df_hz = static_cast<float>(-static_cast<double>(r_freq.imag()) *
                                             static_cast<double>(sr) / constants::kTwoPiD);
      out.frequencies[idx] = center_freq + df_hz;
    }
  }
  return out;
}

std::vector<float> reassign_frequencies(const Audio& audio, const StftConfig& config,
                                        float ref_power, bool fill_nan) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  const int n_fft = config.n_fft;
  const int hop_length = config.hop_length;
  const int win_length = config.actual_win_length();
  SONARE_CHECK(win_length <= n_fft, ErrorCode::InvalidParameter);

  std::vector<float> padded_window, t_window, dw_window;
  double half_n = 0.0;
  build_reassignment_windows(config, padded_window, t_window, dw_window, half_n);
  (void)t_window;  // Frequency reassignment only needs the analysis + derivative windows.

  const int sr = audio.sample_rate();
  const float* signal = audio.data();
  const size_t signal_len = audio.size();

  int n_frames = 0;
  std::vector<std::complex<float>> Sw =
      stft_with_window(signal, signal_len, padded_window, n_fft, hop_length, config.center,
                       config.pad_mode, &n_frames);
  int n_frames_d = 0;
  std::vector<std::complex<float>> Sdw =
      stft_with_window(signal, signal_len, dw_window, n_fft, hop_length, config.center,
                       config.pad_mode, &n_frames_d);
  SONARE_CHECK(n_frames == n_frames_d, ErrorCode::InvalidParameter);

  const int n_bins = n_fft / 2 + 1;
  std::vector<float> freqs(static_cast<size_t>(n_bins) * n_frames, 0.0f);
  const float bin_to_hz = static_cast<float>(sr) / static_cast<float>(n_fft);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (int k = 0; k < n_bins; ++k) {
    const float center_freq = static_cast<float>(k) * bin_to_hz;
    for (int t = 0; t < n_frames; ++t) {
      const size_t idx = static_cast<size_t>(k) * n_frames + t;
      const std::complex<float> S = Sw[idx];
      const float power = std::norm(S);
      if (power < ref_power) {
        freqs[idx] = fill_nan ? nan : center_freq;
        continue;
      }
      const std::complex<float> r_freq = Sdw[idx] / S;
      const float df_hz = static_cast<float>(-static_cast<double>(r_freq.imag()) *
                                             static_cast<double>(sr) / constants::kTwoPiD);
      freqs[idx] = center_freq + df_hz;
    }
  }
  return freqs;
}

std::vector<float> reassign_times(const Audio& audio, const StftConfig& config, float ref_power,
                                  bool fill_nan) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  const int n_fft = config.n_fft;
  const int hop_length = config.hop_length;
  const int win_length = config.actual_win_length();
  SONARE_CHECK(win_length <= n_fft, ErrorCode::InvalidParameter);

  std::vector<float> padded_window, t_window, dw_window;
  double half_n = 0.0;
  build_reassignment_windows(config, padded_window, t_window, dw_window, half_n);
  (void)dw_window;  // Time reassignment only needs the analysis + time-weighted windows.

  const int sr = audio.sample_rate();
  const float* signal = audio.data();
  const size_t signal_len = audio.size();

  int n_frames = 0;
  std::vector<std::complex<float>> Sw =
      stft_with_window(signal, signal_len, padded_window, n_fft, hop_length, config.center,
                       config.pad_mode, &n_frames);
  int n_frames_t = 0;
  std::vector<std::complex<float>> Stw = stft_with_window(
      signal, signal_len, t_window, n_fft, hop_length, config.center, config.pad_mode, &n_frames_t);
  SONARE_CHECK(n_frames == n_frames_t, ErrorCode::InvalidParameter);

  const int n_bins = n_fft / 2 + 1;
  std::vector<float> times(static_cast<size_t>(n_bins) * n_frames, 0.0f);
  const float sample_to_sec = 1.0f / static_cast<float>(sr);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (int k = 0; k < n_bins; ++k) {
    for (int t = 0; t < n_frames; ++t) {
      const size_t idx = static_cast<size_t>(k) * n_frames + t;
      const std::complex<float> S = Sw[idx];
      const float power = std::norm(S);
      const float center_time =
          (static_cast<float>(t * hop_length) + (config.center ? 0.0f : half_n)) * sample_to_sec;
      if (power < ref_power) {
        times[idx] = fill_nan ? nan : center_time;
        continue;
      }
      const std::complex<float> r_time = Stw[idx] / S;
      times[idx] = center_time + r_time.real() * sample_to_sec;
    }
  }
  return times;
}

}  // namespace sonare
