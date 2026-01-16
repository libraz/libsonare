#include "streaming/stream_analyzer.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "analysis/chord_analyzer.h"
#include "analysis/chord_templates.h"
#include "analysis/key_profiles.h"
#include "core/fft.h"
#include "core/resample.h"
#include "core/window.h"
#include "feature/chroma.h"
#include "filters/chroma.h"
#include "filters/mel.h"
#include "util/math_utils.h"

namespace sonare {

namespace {

constexpr float kEpsilon = 1e-10f;
constexpr float kLogAmin = 1e-10f;

/// @brief Computes bin frequencies.
std::vector<float> compute_bin_frequencies(int n_bins, int sr, int n_fft) {
  std::vector<float> freqs(n_bins);
  float bin_width = static_cast<float>(sr) / static_cast<float>(n_fft);
  for (int i = 0; i < n_bins; ++i) {
    freqs[i] = static_cast<float>(i) * bin_width;
  }
  return freqs;
}

/// @brief Computes spectral centroid for a single frame.
float compute_centroid_frame(const float* magnitude, int n_bins, const float* frequencies) {
  float sum_weighted = 0.0f;
  float sum_mag = 0.0f;
  for (int k = 0; k < n_bins; ++k) {
    sum_weighted += frequencies[k] * magnitude[k];
    sum_mag += magnitude[k];
  }
  return sum_mag > kEpsilon ? sum_weighted / sum_mag : 0.0f;
}

/// @brief Computes spectral flatness for a single frame.
/// @details Flatness = geometric_mean / arithmetic_mean.
float compute_flatness_frame(const float* magnitude, int n_bins) {
  float sum = 0.0f;
  float log_sum = 0.0f;
  int count = 0;

  for (int k = 0; k < n_bins; ++k) {
    float val = magnitude[k];
    if (val > kEpsilon) {
      sum += val;
      log_sum += std::log(val);
      ++count;
    }
  }

  if (count == 0 || sum < kEpsilon) {
    return 0.0f;
  }

  float arithmetic_mean = sum / static_cast<float>(count);
  float geometric_mean = std::exp(log_sum / static_cast<float>(count));

  return geometric_mean / arithmetic_mean;
}

/// @brief Computes RMS for a single frame.
float compute_rms_frame(const float* samples, int n_fft) {
  float sum_sq = 0.0f;
  for (int i = 0; i < n_fft; ++i) {
    sum_sq += samples[i] * samples[i];
  }
  return std::sqrt(sum_sq / static_cast<float>(n_fft));
}

/// @brief BPM detection constants for streaming.
constexpr float kBpmMin = 60.0f;
constexpr float kBpmMax = 200.0f;
constexpr int kMinOnsetFrames = 100;  ///< Minimum frames for BPM estimation (~2-3 seconds)

/// @brief Converts lag (in frames) to BPM.
/// @param lag Lag in frames
/// @param sr Sample rate
/// @param hop_length Hop length
/// @return BPM value
float lag_to_bpm(int lag, int sr, int hop_length) {
  if (lag <= 0) return 0.0f;
  float seconds_per_beat = static_cast<float>(lag * hop_length) / static_cast<float>(sr);
  return 60.0f / seconds_per_beat;
}

/// @brief Converts BPM to lag (in frames).
/// @param bpm BPM value
/// @param sr Sample rate
/// @param hop_length Hop length
/// @return Lag in frames
int bpm_to_lag(float bpm, int sr, int hop_length) {
  if (bpm <= 0.0f) return 0;
  float seconds_per_beat = 60.0f / bpm;
  return static_cast<int>(seconds_per_beat * static_cast<float>(sr) /
                          static_cast<float>(hop_length));
}

/// @brief Computes autocorrelation using FFT (Wiener-Khinchin theorem).
/// @param signal Input signal
/// @param max_lag Maximum lag to compute
/// @return Normalized autocorrelation values [0, max_lag)
std::vector<float> compute_autocorrelation_streaming(const std::vector<float>& signal, int max_lag) {
  int n = static_cast<int>(signal.size());
  std::vector<float> autocorr(max_lag, 0.0f);

  if (n == 0 || max_lag <= 0) {
    return autocorr;
  }

  // Compute mean
  float mean_val = 0.0f;
  for (float val : signal) {
    mean_val += val;
  }
  mean_val /= static_cast<float>(n);

  // Compute variance
  float var = 0.0f;
  for (float val : signal) {
    float diff = val - mean_val;
    var += diff * diff;
  }

  if (var < kEpsilon) {
    return autocorr;
  }

  // Zero-pad to at least 2*n to avoid circular correlation artifacts
  int fft_size = next_power_of_2(2 * n);

  // Prepare zero-mean, zero-padded signal
  std::vector<float> padded(fft_size, 0.0f);
  for (int i = 0; i < n; ++i) {
    padded[i] = signal[i] - mean_val;
  }

  // FFT-based autocorrelation
  FFT fft(fft_size);
  int n_bins = fft.n_bins();

  std::vector<std::complex<float>> spectrum(n_bins);
  fft.forward(padded.data(), spectrum.data());

  // Compute power spectrum (|FFT(x)|^2)
  for (int i = 0; i < n_bins; ++i) {
    float re = spectrum[i].real();
    float im = spectrum[i].imag();
    spectrum[i] = std::complex<float>(re * re + im * im, 0.0f);
  }

  // Inverse FFT to get autocorrelation
  std::vector<float> raw_autocorr(fft_size);
  fft.inverse(spectrum.data(), raw_autocorr.data());

  // Normalize by variance and extract relevant lags
  float norm_factor = var * static_cast<float>(n);
  for (int lag = 0; lag < max_lag && lag < n; ++lag) {
    autocorr[lag] = raw_autocorr[lag] / norm_factor;
  }

  return autocorr;
}

/// @brief Finds the best tempo peak in autocorrelation.
/// @param autocorr Autocorrelation values
/// @param sr Sample rate
/// @param hop_length Hop length
/// @param bpm_min Minimum BPM
/// @param bpm_max Maximum BPM
/// @return Pair of (bpm, confidence)
std::pair<float, float> find_best_tempo(const std::vector<float>& autocorr, int sr,
                                         int hop_length, float bpm_min, float bpm_max) {
  int lag_min = bpm_to_lag(bpm_max, sr, hop_length);
  int lag_max = bpm_to_lag(bpm_min, sr, hop_length);

  lag_min = std::max(1, lag_min);
  lag_max = std::min(static_cast<int>(autocorr.size()) - 1, lag_max);

  if (lag_min >= lag_max) {
    return {120.0f, 0.0f};  // Default
  }

  // Find all local maxima and their weights
  std::vector<std::pair<float, float>> candidates;  // (bpm, weight)

  for (int lag = lag_min + 1; lag < lag_max - 1; ++lag) {
    if (autocorr[lag] > autocorr[lag - 1] && autocorr[lag] > autocorr[lag + 1] &&
        autocorr[lag] > 0.0f) {
      float bpm = lag_to_bpm(lag, sr, hop_length);
      if (bpm >= bpm_min && bpm <= bpm_max) {
        candidates.emplace_back(bpm, autocorr[lag]);
      }
    }
  }

  if (candidates.empty()) {
    return {120.0f, 0.0f};
  }

  // Find maximum weight
  float max_weight = 0.0f;
  for (const auto& c : candidates) {
    max_weight = std::max(max_weight, c.second);
  }

  // Prefer BPM in common range (80-180) with reasonable weight
  constexpr float kCommonBpmMin = 80.0f;
  constexpr float kCommonBpmMax = 180.0f;
  constexpr float kWeightThreshold = 0.3f;

  float best_bpm = 0.0f;
  float best_weight = 0.0f;

  // First, look for candidates in common range
  for (const auto& [bpm, weight] : candidates) {
    if (bpm >= kCommonBpmMin && bpm <= kCommonBpmMax &&
        weight >= kWeightThreshold * max_weight) {
      if (weight > best_weight) {
        best_bpm = bpm;
        best_weight = weight;
      }
    }
  }

  // If no good candidate in common range, take the best overall
  if (best_bpm == 0.0f) {
    for (const auto& [bpm, weight] : candidates) {
      if (weight > best_weight) {
        best_bpm = bpm;
        best_weight = weight;
      }
    }
  }

  // Confidence is the relative weight
  float confidence = (max_weight > 0.0f) ? best_weight / max_weight : 0.0f;

  return {best_bpm, confidence};
}

/// @brief Quantizes a float value to uint8 (0-255).
/// @param value Input value
/// @param min_val Minimum expected value (maps to 0)
/// @param max_val Maximum expected value (maps to 255)
/// @return Quantized value
inline uint8_t quantize_to_u8(float value, float min_val, float max_val) {
  float normalized = (value - min_val) / (max_val - min_val);
  normalized = std::max(0.0f, std::min(1.0f, normalized));
  return static_cast<uint8_t>(normalized * 255.0f + 0.5f);
}

/// @brief Quantizes a float value to int16 (-32768 to 32767).
/// @param value Input value
/// @param min_val Minimum expected value (maps to -32768)
/// @param max_val Maximum expected value (maps to 32767)
/// @return Quantized value
inline int16_t quantize_to_i16(float value, float min_val, float max_val) {
  float normalized = (value - min_val) / (max_val - min_val);
  normalized = std::max(0.0f, std::min(1.0f, normalized));
  return static_cast<int16_t>(normalized * 65535.0f - 32768.0f + 0.5f);
}

/// @brief Converts mel power to dB scale.
/// @param power Mel power value
/// @param ref Reference value (typically 1.0)
/// @param amin Minimum amplitude for clipping
/// @return dB value
inline float power_to_db(float power, float ref = 1.0f, float amin = 1e-10f) {
  return 10.0f * std::log10(std::max(power, amin) / ref);
}

/// @brief Counts shared notes between two triads.
/// @param root1, quality1 First chord (root 0-11, quality 0=Major, 1=Minor)
/// @param root2, quality2 Second chord
/// @return Number of shared notes (0-3)
int count_shared_notes(int root1, int quality1, int root2, int quality2) {
  // Build note sets for each chord
  // Major: root, root+4, root+7
  // Minor: root, root+3, root+7
  auto get_notes = [](int root, int quality) -> std::array<int, 3> {
    int third = (quality == 1) ? 3 : 4;  // Minor has minor 3rd
    return {{root % 12, (root + third) % 12, (root + 7) % 12}};
  };

  auto notes1 = get_notes(root1, quality1);
  auto notes2 = get_notes(root2, quality2);

  int shared = 0;
  for (int n1 : notes1) {
    for (int n2 : notes2) {
      if (n1 == n2) {
        ++shared;
        break;
      }
    }
  }
  return shared;
}

/// @brief Checks if two chords are "confusable" (share 2+ notes).
/// @details Am(A,C,E) and F(F,A,C) share A and C - easily confused.
bool are_chords_confusable(int root1, int quality1, int root2, int quality2) {
  return count_shared_notes(root1, quality1, root2, quality2) >= 2;
}

/// @brief Computes median-filtered chroma from history.
/// @details For each chroma bin, computes the median across all frames in history.
///          This is more robust to outliers than simple averaging.
/// @param history Deque of chroma arrays [n_frames][12]
/// @return Median-filtered chroma array [12]
std::array<float, 12> compute_median_chroma(
    const std::deque<std::array<float, 12>>& history) {
  std::array<float, 12> result = {};
  if (history.empty()) {
    return result;
  }

  size_t n_frames = history.size();
  std::vector<float> values(n_frames);

  for (int c = 0; c < 12; ++c) {
    // Collect values for this chroma bin
    for (size_t f = 0; f < n_frames; ++f) {
      values[f] = history[f][c];
    }

    // Sort to find median
    std::sort(values.begin(), values.end());

    // Compute median
    if (n_frames % 2 == 0) {
      result[c] = (values[n_frames / 2 - 1] + values[n_frames / 2]) * 0.5f;
    } else {
      result[c] = values[n_frames / 2];
    }
  }

  return result;
}

}  // namespace

StreamAnalyzer::StreamAnalyzer(const StreamConfig& config) : config_(config) {
  /// Determine if resampling is needed for high sample rates
  if (config_.sample_rate > kMaxDirectSampleRate) {
    needs_resampling_ = true;
    internal_sample_rate_ = kInternalSampleRate;
    resample_ratio_ = static_cast<float>(kInternalSampleRate) / config_.sample_rate;
  } else {
    needs_resampling_ = false;
    internal_sample_rate_ = config_.sample_rate;
    resample_ratio_ = 1.0f;
  }

  int n_bins = config_.n_bins();

  /// Initialize FFT
  fft_ = std::make_unique<FFT>(config_.n_fft);

  /// Cache window function
  window_ = get_window_cached(config_.window, config_.n_fft);

  /// Pre-compute mel filterbank (use internal sample rate)
  if (config_.compute_mel) {
    MelFilterConfig mel_config;
    mel_config.n_mels = config_.n_mels;
    mel_config.fmin = config_.fmin;
    mel_config.fmax = needs_resampling_
        ? std::min(config_.effective_fmax(), static_cast<float>(internal_sample_rate_ / 2))
        : config_.effective_fmax();
    mel_filterbank_ = create_mel_filterbank(internal_sample_rate_, config_.n_fft, mel_config);
  }

  /// Pre-compute chroma filterbank (use internal sample rate)
  if (config_.compute_chroma) {
    ChromaFilterConfig chroma_config;
    chroma_config.n_chroma = 12;
    /// Convert tuning_ref_hz to semitone offset: tuning = 12 * log2(ref/440)
    /// Positive tuning means audio is sharp, so we subtract to correct
    chroma_config.tuning =
        12.0f * std::log2(config_.tuning_ref_hz / 440.0f);
    /// Use C3 (~130 Hz) as minimum frequency to skip very low bass
    /// This helps avoid interference from sub-bass and low-frequency noise
    chroma_config.fmin = 65.0f;
    chroma_filterbank_ =
        create_chroma_filterbank(internal_sample_rate_, config_.n_fft, chroma_config);
  }

  /// Pre-compute frequencies for spectral features (use internal sample rate)
  if (config_.compute_spectral) {
    frequencies_ = compute_bin_frequencies(n_bins, internal_sample_rate_, config_.n_fft);
  }

  /// Allocate working buffers
  frame_buffer_.resize(config_.n_fft);
  spectrum_.resize(n_bins);
  magnitude_.resize(n_bins);
  power_.resize(n_bins);

  if (config_.compute_mel) {
    mel_buffer_.resize(config_.n_mels);
    mel_log_.resize(config_.n_mels);
    prev_mel_log_.resize(config_.n_mels, 0.0f);
  }

  if (config_.compute_chroma) {
    chroma_buffer_.resize(12);
    chroma_raw_.fill(0.0f);
    chroma_sum_.fill(0.0f);
    bar_chroma_sum_.fill(0.0f);
    bar_chord_votes_.fill(0);
    /// Initialize chord templates for chord detection
    chord_templates_ = generate_triad_templates();
  }

  /// Reserve overlap buffer capacity
  overlap_buffer_.reserve(config_.overlap());
}

StreamAnalyzer::~StreamAnalyzer() = default;

StreamAnalyzer::StreamAnalyzer(StreamAnalyzer&&) noexcept = default;
StreamAnalyzer& StreamAnalyzer::operator=(StreamAnalyzer&&) noexcept = default;

void StreamAnalyzer::process(const float* samples, size_t n_samples) {
  process_internal(samples, n_samples);
}

void StreamAnalyzer::process(const float* samples, size_t n_samples, size_t sample_offset) {
  /// Sync cumulative samples with external offset
  cumulative_samples_ = sample_offset;
  process_internal(samples, n_samples);
}

void StreamAnalyzer::process_internal(const float* samples, size_t n_samples) {
  if (samples == nullptr || n_samples == 0) {
    return;
  }

  const float* process_samples = samples;
  size_t process_n_samples = n_samples;

  /// Resample if needed (for high sample rates like 96000 Hz)
  if (needs_resampling_) {
    resample_buffer_ = resample(samples, n_samples, config_.sample_rate, internal_sample_rate_);
    process_samples = resample_buffer_.data();
    process_n_samples = resample_buffer_.size();
  }

  /// Append (resampled) samples to overlap buffer with normalization gain
  size_t prev_size = overlap_buffer_.size();
  overlap_buffer_.resize(prev_size + process_n_samples);
  if (normalization_gain_ != 1.0f) {
    for (size_t i = 0; i < process_n_samples; ++i) {
      overlap_buffer_[prev_size + i] = process_samples[i] * normalization_gain_;
    }
  } else {
    std::copy(process_samples, process_samples + process_n_samples,
              overlap_buffer_.begin() + prev_size);
  }

  /// Process complete frames
  int n_fft = config_.n_fft;
  int hop_length = config_.hop_length;

  while (overlap_buffer_.size() >= static_cast<size_t>(n_fft)) {
    /// Calculate sample offset for this frame (in original sample rate)
    size_t frame_sample_offset = cumulative_samples_;

    /// Process single frame
    StreamFrame frame = process_single_frame(overlap_buffer_.data(), frame_sample_offset);

    /// Check emit_every_n_frames
    ++emitted_frame_count_;
    if (emitted_frame_count_ >= config_.emit_every_n_frames) {
      emitted_frame_count_ = 0;
      output_buffer_.push_back(std::move(frame));
    }

    /// Slide buffer by hop_length
    overlap_buffer_.erase(overlap_buffer_.begin(), overlap_buffer_.begin() + hop_length);

    /// Update cumulative samples (in original sample rate)
    cumulative_samples_ += static_cast<size_t>(hop_length / resample_ratio_);
    ++frame_count_;

    /// Update progressive estimate if needed
    float current_time_sec = static_cast<float>(cumulative_samples_) / config_.sample_rate;
    update_progressive_estimate(current_time_sec);
  }
}

StreamFrame StreamAnalyzer::process_single_frame(const float* frame_start, size_t sample_offset) {
  StreamFrame frame;

  /// Calculate timestamp
  frame.timestamp = static_cast<float>(sample_offset) / static_cast<float>(config_.sample_rate);
  frame.frame_index = frame_count_;

  /// Compute STFT
  compute_stft(frame_start);

  /// Copy magnitude if requested
  if (config_.compute_magnitude) {
    int downsample = config_.magnitude_downsample;
    int output_bins = config_.n_bins() / downsample;
    frame.magnitude.resize(output_bins);
    for (int i = 0; i < output_bins; ++i) {
      frame.magnitude[i] = magnitude_[i * downsample];
    }
  }

  /// Compute mel spectrogram
  if (config_.compute_mel) {
    compute_mel();
    frame.mel = mel_buffer_;
  }

  /// Compute chroma
  if (config_.compute_chroma) {
    compute_chroma();
    frame.chroma = chroma_buffer_;

    /// Accumulate for key estimation
    for (int i = 0; i < 12; ++i) {
      chroma_sum_[i] += chroma_buffer_[i];
    }
    ++chroma_frame_count_;

    /// Accumulate chroma frame for batch-style chord analysis
    /// Store in column-major order: [chroma_bin][frame] for Chroma class compatibility
    for (int c = 0; c < 12; ++c) {
      accumulated_chroma_.push_back(chroma_buffer_[c]);
    }

    /// Detect chord for this frame using smoothed chroma
    if (!chord_templates_.empty() && chroma_buffer_.size() == 12) {
      /// Add current chroma to history
      std::array<float, 12> current_chroma;
      std::copy(chroma_buffer_.begin(), chroma_buffer_.end(), current_chroma.begin());
      chroma_history_.push_back(current_chroma);

      /// Keep history limited to smoothing window
      while (chroma_history_.size() > static_cast<size_t>(kChordSmoothingFrames)) {
        chroma_history_.pop_front();
      }

      /// Store to full chroma history for retroactive bar detection
      if (full_chroma_history_.size() < kMaxChromaHistoryFrames) {
        full_chroma_history_.push_back(current_chroma);
      }

      /// Compute median-filtered chroma (more robust to noise than averaging)
      std::array<float, 12> smoothed_chroma = compute_median_chroma(chroma_history_);

      /// Find best chord using smoothed chroma
      auto [best_chord, chord_corr] =
          find_best_chord(smoothed_chroma.data(), chord_templates_);

      /// Only report chord if confidence is above threshold
      if (chord_corr >= kChordConfidenceThreshold) {
        frame.chord_root = static_cast<int>(best_chord.root);
        frame.chord_quality = static_cast<int>(best_chord.quality);
        frame.chord_confidence = chord_corr;
      } else {
        /// Low confidence: keep previous chord or default to C major
        frame.chord_root = (prev_chord_root_ >= 0) ? prev_chord_root_ : 0;
        frame.chord_quality = (prev_chord_quality_ >= 0) ? prev_chord_quality_ : 0;
        frame.chord_confidence = std::max(0.0f, chord_corr);
      }
    }
  }

  /// Compute onset strength
  if (config_.compute_onset) {
    /// Save state before compute_onset() modifies it
    bool had_prev_frame = has_prev_frame_;
    frame.onset_strength = compute_onset();
    frame.onset_valid = had_prev_frame;

    /// Accumulate for BPM estimation
    if (frame.onset_valid) {
      onset_accumulator_.push_back(frame.onset_strength);
    }
  }

  /// Compute spectral features
  if (config_.compute_spectral) {
    compute_spectral_features(frame);
  }

  /// Compute RMS energy (from time-domain)
  frame.rms_energy = compute_rms_frame(frame_start, config_.n_fft);

  return frame;
}

void StreamAnalyzer::compute_stft(const float* frame_start) {
  /// Apply window
  for (int i = 0; i < config_.n_fft; ++i) {
    frame_buffer_[i] = frame_start[i] * window_[i];
  }

  /// Forward FFT
  fft_->forward(frame_buffer_.data(), spectrum_.data());

  /// Compute magnitude and power
  int n_bins = config_.n_bins();
  for (int k = 0; k < n_bins; ++k) {
    float re = spectrum_[k].real();
    float im = spectrum_[k].imag();
    magnitude_[k] = std::sqrt(re * re + im * im);
    power_[k] = re * re + im * im;
  }
}

void StreamAnalyzer::compute_mel() {
  /// Apply mel filterbank: mel = filterbank @ power
  int n_mels = config_.n_mels;
  int n_bins = config_.n_bins();

  for (int m = 0; m < n_mels; ++m) {
    float sum = 0.0f;
    const float* filter_row = mel_filterbank_.data() + m * n_bins;
    for (int k = 0; k < n_bins; ++k) {
      sum += filter_row[k] * power_[k];
    }
    mel_buffer_[m] = sum;
    mel_log_[m] = std::log(std::max(sum, kLogAmin));
  }
}

void StreamAnalyzer::compute_chroma() {
  /// Apply chroma filterbank: chroma = filterbank @ power
  int n_bins = config_.n_bins();

  for (int c = 0; c < 12; ++c) {
    float sum = 0.0f;
    const float* filter_row = chroma_filterbank_.data() + c * n_bins;
    for (int k = 0; k < n_bins; ++k) {
      sum += filter_row[k] * power_[k];
    }
    chroma_buffer_[c] = sum;
    chroma_raw_[c] = sum;  // Store raw (unnormalized) for accumulation
  }

  /// Normalize chroma using L2 norm (more robust than max)
  float l2_norm = 0.0f;
  for (int c = 0; c < 12; ++c) {
    l2_norm += chroma_buffer_[c] * chroma_buffer_[c];
  }
  l2_norm = std::sqrt(l2_norm);
  if (l2_norm > kEpsilon) {
    for (int c = 0; c < 12; ++c) {
      chroma_buffer_[c] /= l2_norm;
    }
  }
}

float StreamAnalyzer::compute_onset() {
  if (!config_.compute_mel) {
    return 0.0f;
  }

  float onset = 0.0f;

  if (has_prev_frame_) {
    /// Onset = sum of positive differences in log mel
    for (int m = 0; m < config_.n_mels; ++m) {
      float diff = mel_log_[m] - prev_mel_log_[m];
      if (diff > 0.0f) {
        onset += diff;
      }
    }
  }

  /// Store current mel_log for next frame
  prev_mel_log_ = mel_log_;
  has_prev_frame_ = true;

  return onset;
}

void StreamAnalyzer::compute_spectral_features(StreamFrame& frame) {
  int n_bins = config_.n_bins();

  /// Spectral centroid
  frame.spectral_centroid = compute_centroid_frame(magnitude_.data(), n_bins, frequencies_.data());

  /// Spectral flatness
  frame.spectral_flatness = compute_flatness_frame(magnitude_.data(), n_bins);
}

void StreamAnalyzer::update_progressive_estimate(float current_time) {
  current_estimate_.accumulated_seconds = current_time;
  current_estimate_.used_frames = frame_count_;
  current_estimate_.updated = false;

  /// Update key estimate using Krumhansl-Schmuckler correlation
  if (config_.compute_chroma && chroma_frame_count_ > 0) {
    float time_since_key_update = current_time - last_key_update_time_;
    if (time_since_key_update >= config_.key_update_interval_sec) {
      /// Normalize chroma_sum for key detection
      std::array<float, 12> mean_chroma;
      float sum = 0.0f;
      for (int c = 0; c < 12; ++c) {
        mean_chroma[c] = chroma_sum_[c] / chroma_frame_count_;
        sum += mean_chroma[c];
      }
      if (sum > kEpsilon) {
        for (int c = 0; c < 12; ++c) {
          mean_chroma[c] /= sum;
        }
      }

      /// Find best matching key using profile correlation
      int best_key = 0;
      bool best_minor = false;
      float best_corr = -2.0f;

      for (int root = 0; root < 12; ++root) {
        PitchClass pc = static_cast<PitchClass>(root);

        /// Check major key
        auto major_profile = normalize_profile(get_major_profile(pc));
        float major_corr = profile_correlation(mean_chroma, major_profile);
        if (major_corr > best_corr) {
          best_corr = major_corr;
          best_key = root;
          best_minor = false;
        }

        /// Check minor key
        auto minor_profile = normalize_profile(get_minor_profile(pc));
        float minor_corr = profile_correlation(mean_chroma, minor_profile);
        if (minor_corr > best_corr) {
          best_corr = minor_corr;
          best_key = root;
          best_minor = true;
        }
      }

      current_estimate_.key = best_key;
      current_estimate_.key_minor = best_minor;

      /// Confidence based on correlation strength and time
      float time_factor = std::min(1.0f, current_time / 30.0f);
      float corr_factor = (best_corr + 1.0f) / 2.0f;  // Normalize [-1, 1] to [0, 1]
      current_estimate_.key_confidence = corr_factor * time_factor;

      last_key_update_time_ = current_time;
      current_estimate_.updated = true;
    }

    /// Update chord estimate (every frame, using smoothed chroma)
    if (!chord_templates_.empty() && !chroma_history_.empty()) {
      /// Compute median-filtered chroma (more robust to noise than averaging)
      std::array<float, 12> smoothed_chroma = compute_median_chroma(chroma_history_);

      auto [best_chord, chord_corr] =
          find_best_chord(smoothed_chroma.data(), chord_templates_);
      int new_root = static_cast<int>(best_chord.root);
      int new_quality = static_cast<int>(best_chord.quality);
      float new_confidence = std::max(0.0f, chord_corr);

      /// Only update if confidence is above threshold
      if (new_confidence >= kChordConfidenceThreshold) {
        current_estimate_.chord_root = new_root;
        current_estimate_.chord_quality = new_quality;
        current_estimate_.chord_confidence = new_confidence;
      } else {
        /// Keep current estimate but update confidence
        current_estimate_.chord_confidence = new_confidence;
      }

      /// Track chord progression (only when confidence is high enough)
      if (new_confidence >= kChordConfidenceThreshold) {
        float frame_duration =
            static_cast<float>(config_.hop_length) / static_cast<float>(internal_sample_rate_);

        if (new_root == prev_chord_root_ && new_quality == prev_chord_quality_) {
          /// Same chord - accumulate stable time
          chord_stable_time_ += frame_duration;
        } else {
          /// Chord changed - check if previous chord was stable long enough
          if (prev_chord_root_ >= 0 && chord_stable_time_ >= kChordMinDuration) {
            /// Find the start time of the previous chord
            float chord_start = current_time - chord_stable_time_;

            /// Only add if it's a new chord or first chord
            if (current_estimate_.chord_progression.empty() ||
                current_estimate_.chord_progression.back().root != prev_chord_root_ ||
                current_estimate_.chord_progression.back().quality != prev_chord_quality_) {
              ChordChange change;
              change.root = prev_chord_root_;
              change.quality = prev_chord_quality_;
              change.start_time = chord_start;
              change.confidence = new_confidence;
              current_estimate_.chord_progression.push_back(change);
            }
          }

          /// Reset for new chord
          prev_chord_root_ = new_root;
          prev_chord_quality_ = new_quality;
          chord_stable_time_ = frame_duration;
        }
      }
    }
  }

  /// Update BPM estimate
  if (config_.compute_onset) {
    int n_onset = static_cast<int>(onset_accumulator_.size());
    current_estimate_.bpm_candidate_count = n_onset;

    float time_since_bpm_update = current_time - last_bpm_update_time_;
    if (time_since_bpm_update >= config_.bpm_update_interval_sec && n_onset >= kMinOnsetFrames) {
      /// Compute max lag based on minimum BPM (use internal sample rate)
      int max_lag = bpm_to_lag(kBpmMin, internal_sample_rate_, config_.hop_length);
      max_lag = std::min(max_lag, n_onset - 1);

      if (max_lag > 2) {
        /// Compute autocorrelation
        auto autocorr = compute_autocorrelation_streaming(onset_accumulator_, max_lag);

        /// Find best tempo (use internal sample rate)
        auto [bpm, rel_confidence] = find_best_tempo(autocorr, internal_sample_rate_,
                                                      config_.hop_length, kBpmMin, kBpmMax);

        current_estimate_.bpm = bpm;

        /// Combine relative confidence with time-based confidence
        /// Time factor: confidence increases as we get more data (up to 30 seconds)
        float time_factor = std::min(1.0f, current_time / 30.0f);
        current_estimate_.bpm_confidence = rel_confidence * time_factor;

        last_bpm_update_time_ = current_time;
        current_estimate_.updated = true;
      }
    }
  }

  /// Update chord progression using batch-style analysis (same as ChordAnalyzer)
  if (config_.compute_chroma && chroma_frame_count_ > 0) {
    float time_since_chord_analysis = current_time - last_chord_analysis_time_;
    constexpr float kChordAnalysisInterval = 2.0f;  // Update every 2 seconds
    constexpr int kMinFramesForAnalysis = 50;       // ~1 second of audio

    if (time_since_chord_analysis >= kChordAnalysisInterval &&
        chroma_frame_count_ >= kMinFramesForAnalysis) {
      /// Transpose accumulated chroma from [frame][chroma] to [chroma][frame]
      int n_frames = chroma_frame_count_;
      std::vector<float> transposed_chroma(12 * n_frames);
      for (int f = 0; f < n_frames; ++f) {
        for (int c = 0; c < 12; ++c) {
          transposed_chroma[c * n_frames + f] = accumulated_chroma_[f * 12 + c];
        }
      }

      /// Create Chroma object from accumulated data (use internal sample rate)
      Chroma chroma_obj(std::move(transposed_chroma), 12, n_frames, internal_sample_rate_,
                        config_.hop_length);

      /// Run ChordAnalyzer with same settings as batch analysis
      ChordConfig chord_config;
      chord_config.smoothing_window = 2.0f;  // Same as batch
      chord_config.min_duration = 0.3f;
      chord_config.use_triads_only = true;
      chord_config.use_beat_sync = false;  // No beat sync in streaming

      ChordAnalyzer chord_analyzer(chroma_obj, chord_config);

      /// Update chord progression from ChordAnalyzer results
      current_estimate_.chord_progression.clear();
      for (const auto& chord : chord_analyzer.chords()) {
        ChordChange change;
        change.root = static_cast<int>(chord.root);
        change.quality = static_cast<int>(chord.quality);
        change.start_time = chord.start;
        change.confidence = chord.confidence;
        current_estimate_.chord_progression.push_back(change);
      }

      last_chord_analysis_time_ = current_time;
      current_estimate_.updated = true;
    }
  }

  /// Update bar-synchronized chord tracking
  if (config_.compute_chroma) {
    update_bar_chord_tracking(current_time);
  }
}

void StreamAnalyzer::update_bar_chord_tracking(float current_time) {
  /// Check if BPM is stable enough to start bar tracking
  if (!bar_tracking_active_) {
    if (current_estimate_.bpm_confidence >= kBpmConfidenceThreshold && current_estimate_.bpm > 0.0f) {
      /// Start bar tracking
      bar_tracking_active_ = true;

      bar_duration_ = static_cast<float>(kBeatsPerBar) * 60.0f / current_estimate_.bpm;
      current_bar_index_ = 0;
      bar_start_time_ = current_time;

      /// Compute retroactive bar chords from stored chroma history
      compute_retroactive_bar_chords();

      /// Reset for live bar tracking (state already set by retroactive computation)
      bar_chroma_sum_.fill(0.0f);
      bar_chroma_count_ = 0;
      bar_chord_votes_.fill(0);
      bar_vote_count_ = 0;

      /// Update estimate with bar info
      current_estimate_.bar_duration = bar_duration_;
      current_estimate_.current_bar = current_bar_index_;
    }
    return;
  }

  /// Update bar duration if BPM changed significantly
  float new_bar_duration = static_cast<float>(kBeatsPerBar) * 60.0f / current_estimate_.bpm;
  if (std::abs(new_bar_duration - bar_duration_) > 0.1f) {
    bar_duration_ = new_bar_duration;
    current_estimate_.bar_duration = bar_duration_;
  }

  /// Vote for chord using current frame's smoothed chroma (from chroma_history_)
  /// This uses the same smoothed detection as per-frame chord output
  if (!chord_templates_.empty() && !chroma_history_.empty()) {
    /// Compute median-filtered chroma (more robust to noise than averaging)
    std::array<float, 12> smoothed_chroma = compute_median_chroma(chroma_history_);

    /// Detect chord for this frame
    auto [frame_chord, frame_corr] = find_best_chord(smoothed_chroma.data(), chord_templates_);

    /// Only vote if confidence is above threshold
    if (frame_corr >= kChordConfidenceThreshold) {
      int root = static_cast<int>(frame_chord.root);
      int quality = static_cast<int>(frame_chord.quality);

      /// Index: root * 4 + quality (12 roots × 4 qualities = 48)
      int vote_idx = root * 4 + quality;
      if (vote_idx >= 0 && vote_idx < 48) {
        ++bar_chord_votes_[vote_idx];
        ++bar_vote_count_;
      }
    }
  }
  ++bar_chroma_count_;

  /// Check if we've crossed a bar boundary
  if (current_time >= bar_start_time_ + bar_duration_) {
    /// Find chord with most votes
    if (bar_vote_count_ > 0) {
      int best_idx = 0;
      int best_votes = bar_chord_votes_[0];
      for (int i = 1; i < 48; ++i) {
        if (bar_chord_votes_[i] > best_votes) {
          best_votes = bar_chord_votes_[i];
          best_idx = i;
        }
      }

      int best_root = best_idx / 4;
      int best_quality = best_idx % 4;
      float confidence = static_cast<float>(best_votes) / static_cast<float>(bar_vote_count_);

      /// Add to bar chord progression
      BarChord bar_chord;
      bar_chord.bar_index = current_bar_index_;
      bar_chord.root = best_root;
      bar_chord.quality = best_quality;
      bar_chord.start_time = bar_start_time_;
      bar_chord.confidence = confidence;
      current_estimate_.bar_chord_progression.push_back(bar_chord);

      /// Update voted pattern and detect progression periodically (every 4 bars)
      if ((current_bar_index_ + 1) % 4 == 0) {
        compute_voted_pattern(4);
        detect_progression_pattern();
      }
    }

    /// Move to next bar
    ++current_bar_index_;
    bar_start_time_ = current_time;
    bar_chroma_sum_.fill(0.0f);
    bar_chroma_count_ = 0;
    bar_chord_votes_.fill(0);
    bar_vote_count_ = 0;

    /// Update estimate
    current_estimate_.current_bar = current_bar_index_;
  }
}

void StreamAnalyzer::compute_retroactive_bar_chords() {
  if (full_chroma_history_.empty() || bar_duration_ <= 0.0f) {
    return;
  }

  /// Calculate frames per bar (use internal sample rate)
  float seconds_per_frame = static_cast<float>(config_.hop_length) / internal_sample_rate_;
  int frames_per_bar = static_cast<int>(bar_duration_ / seconds_per_frame + 0.5f);

  if (frames_per_bar <= 0) {
    return;
  }

  /// How many complete bars can we detect from full history?
  int retroactive_frames = static_cast<int>(full_chroma_history_.size());
  int retroactive_bars = retroactive_frames / frames_per_bar;

  /// Clear existing bar progression and recompute from start
  current_estimate_.bar_chord_progression.clear();

  for (int bar = 0; bar < retroactive_bars; ++bar) {
    int start_frame = bar * frames_per_bar;
    int end_frame = std::min(start_frame + frames_per_bar, retroactive_frames);

    /// Vote for chord using frames in this bar
    std::array<int, 48> votes = {};
    int vote_count = 0;

    for (int f = start_frame; f < end_frame; ++f) {
      /// Average chroma over small window around this frame
      std::array<float, 12> smoothed = {};
      int smooth_start = std::max(0, f - kChordSmoothingFrames / 2);
      int smooth_end = std::min(retroactive_frames, f + kChordSmoothingFrames / 2);
      int smooth_count = smooth_end - smooth_start;

      for (int sf = smooth_start; sf < smooth_end; ++sf) {
        for (int c = 0; c < 12; ++c) {
          smoothed[c] += full_chroma_history_[sf][c];
        }
      }
      if (smooth_count > 0) {
        float inv = 1.0f / smooth_count;
        for (int c = 0; c < 12; ++c) {
          smoothed[c] *= inv;
        }
      }

      /// Detect chord
      auto [chord, corr] = find_best_chord(smoothed.data(), chord_templates_);
      if (corr >= kChordConfidenceThreshold) {
        int idx = static_cast<int>(chord.root) * 4 + static_cast<int>(chord.quality);
        if (idx >= 0 && idx < 48) {
          ++votes[idx];
          ++vote_count;
        }
      }
    }

    /// Find best chord
    int best_idx = 0;
    int best_votes = votes[0];
    for (int i = 1; i < 48; ++i) {
      if (votes[i] > best_votes) {
        best_votes = votes[i];
        best_idx = i;
      }
    }

    int best_root = best_idx / 4;
    int best_quality = best_idx % 4;
    float confidence = (vote_count > 0) ? static_cast<float>(best_votes) / vote_count : 0.0f;

    /// Create bar chord entry
    BarChord bc;
    bc.bar_index = bar;
    bc.root = best_root;
    bc.quality = best_quality;
    bc.start_time = bar * bar_duration_;
    bc.confidence = confidence;
    current_estimate_.bar_chord_progression.push_back(bc);
  }

  /// Update bar index to continue from where retroactive detection ended
  current_bar_index_ = retroactive_bars;
  bar_start_time_ = retroactive_bars * bar_duration_;

  /// Compute voted pattern from all detected bars
  compute_voted_pattern(4);

  /// Detect best matching progression pattern
  detect_progression_pattern();
}

void StreamAnalyzer::compute_voted_pattern(int pattern_length) {
  // Skip if pattern is already locked (detected with high confidence)
  if (pattern_locked_) {
    return;
  }

  const auto& bars = current_estimate_.bar_chord_progression;
  if (bars.empty() || pattern_length <= 0) {
    return;
  }

  current_estimate_.pattern_length = pattern_length;
  current_estimate_.voted_pattern.clear();
  current_estimate_.voted_pattern.resize(pattern_length);

  /// For each position in the pattern, vote across all repetitions
  for (int pos = 0; pos < pattern_length; ++pos) {
    /// Collect votes: chord index -> (total confidence, count)
    std::array<float, 48> confidence_sum = {};
    std::array<int, 48> vote_count = {};

    /// Go through all bars at this pattern position
    for (size_t bar_idx = pos; bar_idx < bars.size(); bar_idx += pattern_length) {
      const auto& bar = bars[bar_idx];
      int chord_idx = bar.root * 4 + bar.quality;
      if (chord_idx >= 0 && chord_idx < 48) {
        confidence_sum[chord_idx] += bar.confidence;
        ++vote_count[chord_idx];
      }
    }

    /// Find chord with highest weighted vote (confidence-weighted)
    /// Apply diatonic chord bonus based on detected key
    int detected_key = current_estimate_.key;
    bool key_minor = current_estimate_.key_minor;

    /// Diatonic chords in major key: I, ii, iii, IV, V, vi, vii°
    /// Semitones from root: 0(M), 2(m), 4(m), 5(M), 7(M), 9(m), 11(dim)
    /// In minor key: i, ii°, III, iv, v/V, VI, VII
    std::array<std::pair<int, int>, 7> diatonic_chords;
    if (!key_minor) {
      diatonic_chords = {{
          {0, 0},   // I (Major)
          {2, 1},   // ii (minor)
          {4, 1},   // iii (minor)
          {5, 0},   // IV (Major)
          {7, 0},   // V (Major)
          {9, 1},   // vi (minor)
          {11, 2},  // vii° (diminished)
      }};
    } else {
      diatonic_chords = {{
          {0, 1},   // i (minor)
          {2, 2},   // ii° (diminished)
          {3, 0},   // III (Major)
          {5, 1},   // iv (minor)
          {7, 0},   // V (Major) - often used
          {8, 0},   // VI (Major)
          {10, 0},  // VII (Major)
      }};
    }

    int best_idx = 0;
    float best_score = 0.0f;
    int total_votes = 0;

    for (int i = 0; i < 48; ++i) {
      total_votes += vote_count[i];

      float score = confidence_sum[i];
      if (score < 0.01f) continue;

      /// Apply diatonic bonus: +15% if chord is diatonic to detected key
      int chord_root = i / 4;
      int chord_quality = i % 4;
      int relative_root = (chord_root - detected_key + 12) % 12;

      for (const auto& [diatonic_degree, diatonic_quality] : diatonic_chords) {
        if (relative_root == diatonic_degree && chord_quality == diatonic_quality) {
          score *= 1.15f;  // 15% bonus for diatonic chords
          break;
        }
      }

      if (score > best_score) {
        best_score = score;
        best_idx = i;
      }
    }

    /// Create voted pattern entry
    BarChord& voted = current_estimate_.voted_pattern[pos];
    voted.bar_index = pos;
    voted.root = best_idx / 4;
    voted.quality = best_idx % 4;
    voted.start_time = 0.0f;  ///< Not meaningful for pattern

    /// Confidence = ratio of votes for this chord
    int votes_for_best = vote_count[best_idx];
    voted.confidence = (total_votes > 0)
                           ? static_cast<float>(votes_for_best) / total_votes
                           : 0.0f;
  }

  /// Try to correct voted pattern using known progression patterns
  correct_voted_pattern_by_known_patterns();
}

void StreamAnalyzer::correct_voted_pattern_by_known_patterns() {
  auto& voted = current_estimate_.voted_pattern;
  if (voted.size() < 4) return;

  // Calculate minimum bars needed before locking
  // If expected duration is known, use 25% of expected total bars
  // Otherwise, require at least 2 full repetitions (8 bars for 4-bar pattern)
  const auto& bars = current_estimate_.bar_chord_progression;
  int pattern_len = static_cast<int>(voted.size());
  int min_bars_for_lock;

  if (expected_duration_ > 0.0f && bar_duration_ > 0.0f) {
    int expected_total_bars = static_cast<int>(expected_duration_ / bar_duration_);
    // Lock after 25% of song, but at least 2 repetitions
    min_bars_for_lock = std::max(pattern_len * 2, expected_total_bars / 4);
  } else {
    // Default: 2 full repetitions
    min_bars_for_lock = pattern_len * 2;
  }

  bool can_lock = (static_cast<int>(bars.size()) >= min_bars_for_lock);

  const auto& patterns = get_known_patterns();
  int detected_key = current_estimate_.key;
  int pattern_length = static_cast<int>(voted.size());

  /// Find best matching known pattern
  std::string best_pattern_name;
  float best_match_score = 0.0f;
  std::vector<std::pair<int, int>> best_correction;  // position -> (new_root, new_quality)

  for (const auto& pattern : patterns) {
    int known_len = static_cast<int>(pattern.chords.size());
    if (known_len != pattern_length) continue;  // Only match same-length patterns

    int exact_matches = 0;
    int confusable_matches = 0;
    std::vector<std::pair<int, int>> corrections;

    for (int pos = 0; pos < pattern_length; ++pos) {
      int voted_root = voted[pos].root;
      int voted_quality = voted[pos].quality;

      // Expected chord from known pattern (relative to detected key)
      int expected_degree = pattern.chords[pos].first;
      int expected_quality = pattern.chords[pos].second;
      int expected_root = (detected_key + expected_degree) % 12;

      if (voted_root == expected_root && voted_quality == expected_quality) {
        ++exact_matches;
      } else if (are_chords_confusable(voted_root, voted_quality,
                                        expected_root, expected_quality)) {
        ++confusable_matches;
        corrections.emplace_back(pos, expected_root * 4 + expected_quality);
      }
    }

    // Score: exact matches count fully, confusable matches count partially
    float score = (exact_matches + confusable_matches * 0.7f) / pattern_length;

    // Require at least (n-1) positions to match (exact or confusable)
    // For 4-chord pattern: need 3+ matches
    int total_matches = exact_matches + confusable_matches;
    if (total_matches >= pattern_length - 1 && score > best_match_score) {
      best_match_score = score;
      best_pattern_name = pattern.name;
      best_correction = corrections;
    }
  }

  // Apply correction if we found a good match (at least 75% match score)
  if (best_match_score >= 0.75f && !best_correction.empty()) {
    for (const auto& [pos, chord_idx] : best_correction) {
      voted[pos].root = chord_idx / 4;
      voted[pos].quality = chord_idx % 4;
      // Keep original confidence but mark as corrected
      // (confidence remains from voting, but chord is corrected)
    }

    // Also set the detected pattern name based on the correction
    current_estimate_.detected_pattern_name = best_pattern_name;
    current_estimate_.detected_pattern_score = best_match_score;

    // Lock the pattern only if we have enough data (4+ repetitions)
    if (can_lock) {
      pattern_locked_ = true;
    }
  }
}

const std::vector<StreamAnalyzer::ProgressionPattern>& StreamAnalyzer::get_known_patterns() {
  // degree: 0=I, 1=bII, 2=II, 3=bIII, 4=III, 5=IV, 6=bV, 7=V, 8=bVI, 9=VI, 10=bVII, 11=VII
  // quality: 0=Major, 1=Minor
  static const std::vector<ProgressionPattern> patterns = {
      // Royal Road (王道進行): I - V - VIm - IV
      {"royalRoad", {{0, 0}, {7, 0}, {9, 1}, {5, 0}}},

      // Komuro (小室進行): VIm - IV - V - I
      {"komuro", {{9, 1}, {5, 0}, {7, 0}, {0, 0}}},

      // Canon (カノン進行): I - V - VIm - IIIm - IV - I - IV - V
      {"canon",
       {{0, 0}, {7, 0}, {9, 1}, {4, 1}, {5, 0}, {0, 0}, {5, 0}, {7, 0}}},

      // Just the Two of Us: IVM7 - IIIm7 - VIm
      {"justTheTwoOfUs", {{5, 0}, {4, 1}, {9, 1}}},

      // I - IV - V - I (Basic)
      {"basic145", {{0, 0}, {5, 0}, {7, 0}, {0, 0}}},

      // Blues (12-bar): I - I - I - I - IV - IV - I - I - V - IV - I - V
      {"blues12",
       {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {5, 0}, {5, 0}, {0, 0}, {0, 0}, {7, 0}, {5, 0}, {0, 0}, {7, 0}}},

      // Axis (VIm - IV - I - V)
      {"axis", {{9, 1}, {5, 0}, {0, 0}, {7, 0}}},

      // I - VIm - IV - V (50s)
      {"fifties", {{0, 0}, {9, 1}, {5, 0}, {7, 0}}},

      // I - V - VIm - IIIm (Sensitive)
      {"sensitive", {{0, 0}, {7, 0}, {9, 1}, {4, 1}}},
  };
  return patterns;
}

void StreamAnalyzer::detect_progression_pattern() {
  // Skip if pattern is already locked
  if (pattern_locked_) {
    return;
  }

  const auto& bars = current_estimate_.bar_chord_progression;
  if (bars.size() < 4) {
    return;
  }

  const auto& patterns = get_known_patterns();
  int detected_key = current_estimate_.key;

  std::string best_pattern_name;
  float best_score = 0.0f;
  current_estimate_.all_pattern_scores.clear();

  for (const auto& pattern : patterns) {
    int pattern_len = static_cast<int>(pattern.chords.size());
    if (pattern_len == 0) continue;

    /// Accumulate score across all repetitions of this pattern
    float total_score = 0.0f;
    float max_possible = 0.0f;

    for (size_t bar_idx = 0; bar_idx < bars.size(); ++bar_idx) {
      int pos = bar_idx % pattern_len;
      const auto& expected = pattern.chords[pos];

      /// Expected chord (relative to detected key)
      int expected_root = (detected_key + expected.first) % 12;
      int expected_quality = expected.second;

      /// Get detected bar chord
      const auto& bar = bars[bar_idx];
      float bar_conf = bar.confidence;

      /// Calculate similarity score (0.0 to 1.0)
      float similarity = 0.0f;

      if (bar.root == expected_root && bar.quality == expected_quality) {
        /// Exact match
        similarity = 1.0f;
      } else if (bar.root == expected_root) {
        /// Same root, different quality (e.g., C vs Cm)
        similarity = 0.6f;
      } else {
        /// Check for related chords
        int root_diff = std::abs(bar.root - expected_root);
        if (root_diff > 6) root_diff = 12 - root_diff;  /// Wrap around

        if (root_diff == 0) {
          similarity = 0.6f;  /// Same root
        } else if (root_diff == 7 || root_diff == 5) {
          /// Fifth or fourth relationship (e.g., C and G, or C and F)
          similarity = 0.3f;
        } else if (root_diff == 3 || root_diff == 4) {
          /// Third relationship (relative major/minor)
          similarity = 0.25f;
        } else if (root_diff == 2) {
          /// Second (close neighbor)
          similarity = 0.15f;
        } else if (root_diff == 1) {
          /// Semitone (very close, might be tuning issue)
          similarity = 0.2f;
        }

        /// Bonus if quality matches despite root mismatch
        if (bar.quality == expected_quality) {
          similarity += 0.1f;
        }
      }

      /// Weight by detection confidence
      total_score += similarity * bar_conf;
      max_possible += bar_conf;
    }

    float score = (max_possible > 0.0f) ? total_score / max_possible : 0.0f;

    /// Store all pattern scores
    current_estimate_.all_pattern_scores.emplace_back(pattern.name, score);

    if (score > best_score) {
      best_score = score;
      best_pattern_name = pattern.name;
    }
  }

  /// Sort by score descending
  std::sort(current_estimate_.all_pattern_scores.begin(),
            current_estimate_.all_pattern_scores.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  /// Only report pattern if score meets minimum threshold (75%)
  /// A low score means the pattern is a poor match, even if it's the "best" among patterns
  constexpr float kMinPatternScore = 0.75f;
  if (best_score >= kMinPatternScore) {
    current_estimate_.detected_pattern_name = best_pattern_name;
    current_estimate_.detected_pattern_score = best_score;
  } else if (current_estimate_.detected_pattern_name.empty()) {
    // Only clear if not already set by pattern-based correction
    current_estimate_.detected_pattern_score = best_score;
  }
}

size_t StreamAnalyzer::available_frames() const { return output_buffer_.size(); }

std::vector<StreamFrame> StreamAnalyzer::read_frames(size_t max_frames) {
  size_t count = std::min(max_frames, output_buffer_.size());
  std::vector<StreamFrame> result;
  result.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    result.push_back(std::move(output_buffer_.front()));
    output_buffer_.pop_front();
  }

  return result;
}

void StreamAnalyzer::read_frames_soa(size_t max_frames, FrameBuffer& buffer) {
  buffer.clear();

  size_t count = std::min(max_frames, output_buffer_.size());
  buffer.n_frames = count;

  if (count == 0) {
    return;
  }

  buffer.reserve(count, config_.n_mels);

  for (size_t i = 0; i < count; ++i) {
    StreamFrame& frame = output_buffer_.front();

    buffer.timestamps.push_back(frame.timestamp);
    buffer.onset_strength.push_back(frame.onset_strength);
    buffer.rms_energy.push_back(frame.rms_energy);
    buffer.spectral_centroid.push_back(frame.spectral_centroid);
    buffer.spectral_flatness.push_back(frame.spectral_flatness);
    buffer.chord_root.push_back(frame.chord_root);
    buffer.chord_quality.push_back(frame.chord_quality);
    buffer.chord_confidence.push_back(frame.chord_confidence);

    // Append mel (row-major)
    buffer.mel.insert(buffer.mel.end(), frame.mel.begin(), frame.mel.end());

    // Append chroma (row-major)
    buffer.chroma.insert(buffer.chroma.end(), frame.chroma.begin(), frame.chroma.end());

    output_buffer_.pop_front();
  }
}

void StreamAnalyzer::read_frames_quantized_u8(size_t max_frames, QuantizedFrameBufferU8& buffer,
                                               const QuantizeConfig& qconfig) {
  buffer.clear();

  size_t count = std::min(max_frames, output_buffer_.size());
  buffer.n_frames = count;

  if (count == 0) {
    return;
  }

  buffer.reserve(count, config_.n_mels);

  for (size_t i = 0; i < count; ++i) {
    StreamFrame& frame = output_buffer_.front();

    buffer.timestamps.push_back(frame.timestamp);

    // Quantize mel (convert to dB first)
    for (float mel_power : frame.mel) {
      float db = power_to_db(mel_power);
      buffer.mel.push_back(quantize_to_u8(db, qconfig.mel_db_min, qconfig.mel_db_max));
    }

    // Quantize chroma (already 0-1)
    for (float c : frame.chroma) {
      buffer.chroma.push_back(quantize_to_u8(c, 0.0f, 1.0f));
    }

    // Quantize scalar features
    buffer.onset_strength.push_back(quantize_to_u8(frame.onset_strength, 0.0f, qconfig.onset_max));
    buffer.rms_energy.push_back(quantize_to_u8(frame.rms_energy, 0.0f, qconfig.rms_max));
    buffer.spectral_centroid.push_back(
        quantize_to_u8(frame.spectral_centroid, 0.0f, qconfig.centroid_max));
    buffer.spectral_flatness.push_back(quantize_to_u8(frame.spectral_flatness, 0.0f, 1.0f));

    output_buffer_.pop_front();
  }
}

void StreamAnalyzer::read_frames_quantized_i16(size_t max_frames, QuantizedFrameBufferI16& buffer,
                                                const QuantizeConfig& qconfig) {
  buffer.clear();

  size_t count = std::min(max_frames, output_buffer_.size());
  buffer.n_frames = count;

  if (count == 0) {
    return;
  }

  buffer.reserve(count, config_.n_mels);

  for (size_t i = 0; i < count; ++i) {
    StreamFrame& frame = output_buffer_.front();

    buffer.timestamps.push_back(frame.timestamp);

    // Quantize mel (convert to dB first)
    for (float mel_power : frame.mel) {
      float db = power_to_db(mel_power);
      buffer.mel.push_back(quantize_to_i16(db, qconfig.mel_db_min, qconfig.mel_db_max));
    }

    // Quantize chroma (already 0-1)
    for (float c : frame.chroma) {
      buffer.chroma.push_back(quantize_to_i16(c, 0.0f, 1.0f));
    }

    // Quantize scalar features
    buffer.onset_strength.push_back(quantize_to_i16(frame.onset_strength, 0.0f, qconfig.onset_max));
    buffer.rms_energy.push_back(quantize_to_i16(frame.rms_energy, 0.0f, qconfig.rms_max));
    buffer.spectral_centroid.push_back(
        quantize_to_i16(frame.spectral_centroid, 0.0f, qconfig.centroid_max));
    buffer.spectral_flatness.push_back(quantize_to_i16(frame.spectral_flatness, 0.0f, 1.0f));

    output_buffer_.pop_front();
  }
}

void StreamAnalyzer::reset(size_t base_sample_offset) {
  cumulative_samples_ = base_sample_offset;
  frame_count_ = 0;
  emitted_frame_count_ = 0;

  overlap_buffer_.clear();
  output_buffer_.clear();

  /// Reset mel state
  if (config_.compute_mel) {
    std::fill(prev_mel_log_.begin(), prev_mel_log_.end(), 0.0f);
  }
  has_prev_frame_ = false;

  /// Reset progressive estimation
  onset_accumulator_.clear();
  chroma_sum_.fill(0.0f);
  chroma_frame_count_ = 0;
  accumulated_chroma_.clear();
  last_key_update_time_ = 0.0f;
  last_bpm_update_time_ = 0.0f;
  last_chord_analysis_time_ = 0.0f;
  current_estimate_ = ProgressiveEstimate();

  /// Reset chord progression tracking
  prev_chord_root_ = -1;
  prev_chord_quality_ = -1;
  chord_stable_time_ = 0.0f;
  chroma_history_.clear();

  /// Reset bar tracking state
  bar_tracking_active_ = false;
  bar_duration_ = 0.0f;
  current_bar_index_ = -1;
  bar_start_time_ = 0.0f;
  bar_chroma_sum_.fill(0.0f);
  bar_chroma_count_ = 0;
  bar_chord_votes_.fill(0);
  bar_vote_count_ = 0;

  /// Reset pattern lock (but keep expected_duration_)
  pattern_locked_ = false;
}

void StreamAnalyzer::set_expected_duration(float duration_seconds) {
  expected_duration_ = duration_seconds;
}

void StreamAnalyzer::set_normalization_gain(float gain) {
  // Clamp to reasonable range to avoid extreme values
  normalization_gain_ = std::clamp(gain, 0.01f, 100.0f);
}

void StreamAnalyzer::set_tuning_ref_hz(float ref_hz) {
  // Clamp to reasonable tuning range (A3 to A5)
  ref_hz = std::clamp(ref_hz, 220.0f, 880.0f);
  config_.tuning_ref_hz = ref_hz;

  // Recreate chroma filterbank with new tuning
  if (config_.compute_chroma) {
    ChromaFilterConfig chroma_config;
    chroma_config.n_chroma = 12;
    chroma_config.tuning = 12.0f * std::log2(ref_hz / 440.0f);
    chroma_config.fmin = 65.0f;  // Skip very low bass
    chroma_filterbank_ =
        create_chroma_filterbank(internal_sample_rate_, config_.n_fft, chroma_config);
  }
}

AnalyzerStats StreamAnalyzer::stats() {
  /// Compute final pattern detection before returning stats
  compute_voted_pattern(4);
  detect_progression_pattern();

  AnalyzerStats stats;
  stats.total_frames = frame_count_;
  stats.total_samples = cumulative_samples_;
  stats.duration_seconds = static_cast<float>(cumulative_samples_) / config_.sample_rate;
  stats.estimate = current_estimate_;

  return stats;
}

float StreamAnalyzer::current_time() const {
  return static_cast<float>(cumulative_samples_) / static_cast<float>(config_.sample_rate);
}

}  // namespace sonare
