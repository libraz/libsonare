#include "analysis/timbre_analyzer.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "core/spectrum.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "feature/spectral.h"
#include "util/exception.h"

namespace sonare {

TimbreAnalyzer::TimbreAnalyzer(const Audio& audio, const TimbreConfig& config)
    : n_frames_(0), sr_(audio.sample_rate()), config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Compute spectrogram
  StftConfig stft_config;
  stft_config.n_fft = config.n_fft;
  stft_config.hop_length = config.hop_length;

  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  // Compute mel spectrogram
  MelConfig mel_config;
  mel_config.n_fft = config.n_fft;
  mel_config.hop_length = config.hop_length;
  mel_config.n_mels = config.n_mels;

  MelSpectrogram mel = MelSpectrogram::compute(audio, mel_config);

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

  // Compute MFCC for complexity analysis
  auto mfcc = mel_spec.mfcc(config_.n_mfcc);

  // Compute MFCC variance for complexity
  mfcc_variance_.resize(config_.n_mfcc, 0.0f);
  int mfcc_frames = mel_spec.n_frames();

  for (int c = 0; c < config_.n_mfcc; ++c) {
    float mean = 0.0f;
    for (int f = 0; f < mfcc_frames; ++f) {
      mean += mfcc[c * mfcc_frames + f];
    }
    mean /= static_cast<float>(mfcc_frames);

    float var = 0.0f;
    for (int f = 0; f < mfcc_frames; ++f) {
      float diff = mfcc[c * mfcc_frames + f] - mean;
      var += diff * diff;
    }
    mfcc_variance_[c] = var / static_cast<float>(mfcc_frames);
  }

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

  // Normalize centroid to [0, 1] (assuming max centroid ~ 8000 Hz)
  t.brightness = std::min(1.0f, avg_centroid / 8000.0f);

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

  // Normalize flux (assuming typical range)
  t.roughness = std::min(1.0f, avg_flux / 100.0f);

  // Complexity: based on MFCC variance
  // Higher variance across MFCCs = more complex timbre
  float total_var = 0.0f;
  for (float var : mfcc_variance_) {
    total_var += var;
  }
  float avg_var = total_var / std::max(1, static_cast<int>(mfcc_variance_.size()));

  // Normalize variance (assuming typical range)
  t.complexity = std::min(1.0f, std::sqrt(avg_var) / 50.0f);

  return t;
}

}  // namespace sonare
