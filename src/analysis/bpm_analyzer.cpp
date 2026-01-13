#include "analysis/bpm_analyzer.h"

#include <algorithm>
#include <cmath>

#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "util/exception.h"

namespace sonare {

namespace {

/// @brief Computes autocorrelation of a signal.
std::vector<float> compute_autocorrelation(const std::vector<float>& signal, int max_lag) {
  int n = static_cast<int>(signal.size());
  std::vector<float> autocorr(max_lag, 0.0f);

  // Compute mean
  float mean = 0.0f;
  for (float val : signal) {
    mean += val;
  }
  mean /= static_cast<float>(n);

  // Compute variance
  float var = 0.0f;
  for (float val : signal) {
    float diff = val - mean;
    var += diff * diff;
  }

  if (var < 1e-10f) {
    return autocorr;
  }

  // Compute autocorrelation for each lag
  for (int lag = 0; lag < max_lag; ++lag) {
    float sum = 0.0f;
    for (int i = 0; i < n - lag; ++i) {
      sum += (signal[i] - mean) * (signal[i + lag] - mean);
    }
    autocorr[lag] = sum / var;
  }

  return autocorr;
}

/// @brief Converts lag (in frames) to BPM.
float lag_to_bpm(int lag, int sr, int hop_length) {
  if (lag <= 0) return 0.0f;
  float seconds_per_beat = static_cast<float>(lag * hop_length) / static_cast<float>(sr);
  return 60.0f / seconds_per_beat;
}

/// @brief Converts BPM to lag (in frames).
int bpm_to_lag(float bpm, int sr, int hop_length) {
  if (bpm <= 0.0f) return 0;
  float seconds_per_beat = 60.0f / bpm;
  return static_cast<int>(seconds_per_beat * static_cast<float>(sr) /
                          static_cast<float>(hop_length));
}

/// @brief Finds peaks in autocorrelation within BPM range.
std::vector<BpmCandidate> find_tempo_peaks(const std::vector<float>& autocorr, int sr,
                                           int hop_length, float bpm_min, float bpm_max) {
  std::vector<BpmCandidate> candidates;

  int lag_min = bpm_to_lag(bpm_max, sr, hop_length);
  int lag_max = bpm_to_lag(bpm_min, sr, hop_length);

  lag_min = std::max(1, lag_min);
  lag_max = std::min(static_cast<int>(autocorr.size()) - 1, lag_max);

  // Find local maxima
  for (int lag = lag_min + 1; lag < lag_max - 1; ++lag) {
    if (autocorr[lag] > autocorr[lag - 1] && autocorr[lag] > autocorr[lag + 1]) {
      float bpm = lag_to_bpm(lag, sr, hop_length);
      if (bpm >= bpm_min && bpm <= bpm_max) {
        candidates.push_back({bpm, autocorr[lag]});
      }
    }
  }

  // Sort by confidence (descending)
  std::sort(candidates.begin(), candidates.end(), [](const BpmCandidate& a, const BpmCandidate& b) {
    return a.confidence > b.confidence;
  });

  return candidates;
}

}  // namespace

BpmAnalyzer::BpmAnalyzer(const Audio& audio, const BpmConfig& config) : config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Compute onset strength
  MelConfig mel_config;
  mel_config.n_fft = config.n_fft;
  mel_config.hop_length = config.hop_length;
  mel_config.n_mels = 128;

  OnsetConfig onset_config;
  onset_config.lag = 1;
  onset_config.detrend = true;
  onset_config.center = false;

  std::vector<float> onset = compute_onset_strength(audio, mel_config, onset_config);

  analyze(onset, audio.sample_rate(), config.hop_length);
}

BpmAnalyzer::BpmAnalyzer(const std::vector<float>& onset_strength, int sr, int hop_length,
                         const BpmConfig& config)
    : config_(config) {
  analyze(onset_strength, sr, hop_length);
}

void BpmAnalyzer::analyze(const std::vector<float>& onset_strength, int sr, int hop_length) {
  if (onset_strength.empty()) {
    bpm_ = config_.start_bpm;
    confidence_ = 0.0f;
    return;
  }

  // Compute max lag based on minimum BPM
  int max_lag = bpm_to_lag(config_.bpm_min, sr, hop_length);
  max_lag = std::min(max_lag, static_cast<int>(onset_strength.size()) - 1);

  if (max_lag < 2) {
    bpm_ = config_.start_bpm;
    confidence_ = 0.0f;
    return;
  }

  // Compute autocorrelation
  autocorr_ = compute_autocorrelation(onset_strength, max_lag);

  // Find tempo peaks
  candidates_ = find_tempo_peaks(autocorr_, sr, hop_length, config_.bpm_min, config_.bpm_max);

  if (candidates_.empty()) {
    bpm_ = config_.start_bpm;
    confidence_ = 0.0f;
  } else {
    // Apply tempo prior (prefer tempos near start_bpm)
    float best_score = -1.0f;
    size_t best_idx = 0;

    for (size_t i = 0; i < candidates_.size(); ++i) {
      float prior_weight;

      if (config_.prior) {
        // Use custom prior function
        prior_weight = config_.prior(candidates_[i].bpm);
      } else {
        // Default: gaussian prior around start_bpm
        float tempo_ratio = candidates_[i].bpm / config_.start_bpm;
        prior_weight = std::exp(-0.5f * std::pow(std::log2(tempo_ratio), 2.0f) / 0.5f);
      }

      float score = candidates_[i].confidence * prior_weight;

      // Also consider octave errors (half/double tempo) - only with default prior
      if (!config_.prior && candidates_[i].bpm >= config_.bpm_min * 2.0f) {
        float half_bpm = candidates_[i].bpm / 2.0f;
        if (half_bpm >= config_.bpm_min) {
          float half_ratio = half_bpm / config_.start_bpm;
          float half_prior = std::exp(-0.5f * std::pow(std::log2(half_ratio), 2.0f) / 0.5f);
          if (half_prior > prior_weight) {
            score = candidates_[i].confidence * half_prior;
          }
        }
      }

      if (score > best_score) {
        best_score = score;
        best_idx = i;
      }
    }

    bpm_ = candidates_[best_idx].bpm;
    confidence_ = candidates_[best_idx].confidence;
  }

  // Simple tempogram (just store autocorrelation for now)
  tempogram_ = autocorr_;
}

std::vector<BpmCandidate> BpmAnalyzer::candidates(int top_n) const {
  int n = std::min(top_n, static_cast<int>(candidates_.size()));
  return std::vector<BpmCandidate>(candidates_.begin(), candidates_.begin() + n);
}

float detect_bpm(const Audio& audio, const BpmConfig& config) {
  BpmAnalyzer analyzer(audio, config);
  return analyzer.bpm();
}

}  // namespace sonare
