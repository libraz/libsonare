#include "analysis/timbre_analyzer.h"

#include <algorithm>
#include <cmath>

#include "core/spectrum.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "feature/spectral.h"
#include "util/exception.h"

namespace sonare {

namespace {

/// @brief Reference spectral centroid (Hz) mapped to maximum brightness (1.0).
/// @details Centroids at or above this frequency are perceived as maximally bright;
///          ~8 kHz approximates the upper edge of musically relevant brightness.
constexpr float kBrightnessCentroidRefHz = 8000.0f;
/// @brief Reference spectral flux mapped to maximum roughness (1.0).
/// @details Frame-to-frame spectral flux at or above this value saturates roughness.
constexpr float kRoughnessFluxRef = 100.0f;
/// @brief Reference MFCC standard deviation mapped to maximum complexity (1.0).
/// @details The RMS spread of MFCC variances at or above this value saturates complexity.
constexpr float kComplexityMfccStdRef = 50.0f;

}  // namespace

TimbreAnalyzer::TimbreAnalyzer(const Audio& audio, const TimbreConfig& config)
    : n_frames_(0), sr_(audio.sample_rate()), config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Compute spectrogram once and derive the mel spectrogram from it so the
  // STFT pass is not repeated inside MelSpectrogram::compute.
  Spectrogram spec = Spectrogram::compute(audio, make_stft_config(config.n_fft, config.hop_length));

  MelFilterConfig mel_filter_config;
  mel_filter_config.n_mels = config.n_mels;

  MelSpectrogram mel = MelSpectrogram::from_spectrogram(spec, sr_, mel_filter_config);

  // Delegate to shared implementation
  init_from_features(spec, mel);
}

TimbreAnalyzer::TimbreAnalyzer(const Spectrogram& spec, const MelSpectrogram& mel_spec,
                               const TimbreConfig& config)
    : n_frames_(0), sr_(spec.sample_rate()), config_(config) {
  init_from_features(spec, mel_spec);
}

void TimbreAnalyzer::init_from_features(const Spectrogram& spec, const MelSpectrogram& mel_spec) {
  n_frames_ = spec.n_frames();

  if (n_frames_ == 0) {
    timbre_ = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    return;
  }

  // Compute spectral features
  spectral_centroid_ = sonare::spectral_centroid(spec, sr_);
  spectral_flatness_ = sonare::spectral_flatness(spec);
  spectral_rolloff_ = sonare::spectral_rolloff(spec, sr_, 0.85f);
  spectral_flux_ = sonare::spectral_flux(spec);

  // Retain the per-frame MFCC matrix so timbre complexity can be computed over
  // each analysis window's frame range rather than from a single global figure.
  const int n_mfcc = std::max(0, config_.n_mfcc);
  const int mfcc_frames = mel_spec.n_frames();
  n_mfcc_ = 0;
  mfcc_frames_ = 0;
  mfcc_.clear();

  if (n_mfcc <= 0 || mfcc_frames <= 0 || mel_spec.n_mels() < n_mfcc) {
    analyze();
    return;
  }
  auto mfcc = mel_spec.mfcc(n_mfcc);
  if (mfcc.size() < static_cast<size_t>(n_mfcc * mfcc_frames)) {
    analyze();
    return;
  }

  n_mfcc_ = n_mfcc;
  mfcc_frames_ = mfcc_frames;
  mfcc_ = std::move(mfcc);

  analyze();
}

void TimbreAnalyzer::analyze() {
  // Compute overall timbre
  compute_overall_timbre();

  // Compute timbre over time
  int window_frames = static_cast<int>(config_.window_sec * sr_ / config_.hop_length);
  window_frames = std::max(1, window_frames);

  timbre_over_time_.clear();
  for (int start = 0; start < n_frames_; start += window_frames) {
    int end = std::min(start + window_frames, n_frames_);
    timbre_over_time_.push_back(compute_window_timbre(start, end));
  }
}

void TimbreAnalyzer::compute_overall_timbre() {
  if (n_frames_ == 0) {
    timbre_ = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    return;
  }

  timbre_ = compute_window_timbre(0, n_frames_);
}

Timbre TimbreAnalyzer::compute_window_timbre(int start_frame, int end_frame) const {
  Timbre t = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

  if (start_frame >= end_frame) {
    return t;
  }

  int count = end_frame - start_frame;

  // Brightness: based on spectral centroid
  // Higher centroid = brighter sound
  float centroid_sum = 0.0f;
  for (int f = start_frame; f < end_frame; ++f) {
    if (f < static_cast<int>(spectral_centroid_.size())) {
      centroid_sum += spectral_centroid_[f];
    }
  }
  float avg_centroid = centroid_sum / count;

  // Normalize centroid to [0, 1] relative to the brightness reference frequency.
  t.brightness = std::min(1.0f, avg_centroid / kBrightnessCentroidRefHz);

  // Warmth: inverse of brightness + low frequency energy emphasis
  // Low centroid = warmer sound
  t.warmth = 1.0f - t.brightness * 0.7f;

  // Density: based on spectral flatness
  // Higher flatness = more noise-like = denser
  float flatness_sum = 0.0f;
  for (int f = start_frame; f < end_frame; ++f) {
    if (f < static_cast<int>(spectral_flatness_.size())) {
      flatness_sum += spectral_flatness_[f];
    }
  }
  float avg_flatness = flatness_sum / count;
  t.density = std::min(1.0f, avg_flatness * 2.0f);

  // Roughness: based on spectral flux
  // Higher flux = more rapid spectral changes = rougher
  float flux_sum = 0.0f;
  for (int f = start_frame; f < end_frame; ++f) {
    if (f < static_cast<int>(spectral_flux_.size())) {
      flux_sum += spectral_flux_[f];
    }
  }
  float avg_flux = flux_sum / count;

  // Normalize flux to [0, 1] relative to the roughness reference.
  t.roughness = std::min(1.0f, avg_flux / kRoughnessFluxRef);

  // Complexity: based on per-window MFCC variance.
  // Higher variance across MFCCs within this window = more complex timbre.
  const int mfcc_start = std::max(0, std::min(start_frame, mfcc_frames_));
  const int mfcc_end = std::max(mfcc_start, std::min(end_frame, mfcc_frames_));
  const int mfcc_count = mfcc_end - mfcc_start;
  float avg_var = 0.0f;
  if (n_mfcc_ > 0 && mfcc_count > 0) {
    float total_var = 0.0f;
    for (int c = 0; c < n_mfcc_; ++c) {
      const float* row = &mfcc_[static_cast<size_t>(c) * static_cast<size_t>(mfcc_frames_)];
      float mean = 0.0f;
      for (int f = mfcc_start; f < mfcc_end; ++f) {
        mean += row[f];
      }
      mean /= static_cast<float>(mfcc_count);

      float var = 0.0f;
      for (int f = mfcc_start; f < mfcc_end; ++f) {
        const float diff = row[f] - mean;
        var += diff * diff;
      }
      total_var += var / static_cast<float>(mfcc_count);
    }
    avg_var = total_var / static_cast<float>(n_mfcc_);
  }

  // Normalize variance to [0, 1] relative to the complexity reference.
  t.complexity = std::min(1.0f, std::sqrt(avg_var) / kComplexityMfccStdRef);

  return t;
}

}  // namespace sonare
