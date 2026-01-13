#pragma once

/// @file bpm_analyzer.h
/// @brief BPM (tempo) detection.

#include <cmath>
#include <functional>
#include <vector>

#include "core/audio.h"

namespace sonare {

/// @brief Prior distribution function type for tempo estimation.
/// @details Maps BPM value to weight [0, 1]. Higher weight = more likely.
using TempoPrior = std::function<float(float bpm)>;

/// @brief Configuration for BPM analysis.
/// @details Default values match librosa.
struct BpmConfig {
  float bpm_min = 30.0f;     ///< Minimum BPM to consider
  float bpm_max = 300.0f;    ///< Maximum BPM to consider
  float start_bpm = 120.0f;  ///< Prior estimate for BPM (used when prior is null)
  int n_fft = 2048;          ///< FFT size for onset detection
  int hop_length = 512;      ///< Hop length for onset detection

  /// @brief Custom prior distribution for tempo estimation.
  /// @details If null, uses default gaussian prior around start_bpm.
  TempoPrior prior = nullptr;

  /// @brief Creates a uniform prior over a BPM range.
  /// @param min_bpm Minimum BPM (weight = 1.0)
  /// @param max_bpm Maximum BPM (weight = 1.0)
  /// @param falloff Weight for BPM outside range (default 0.1)
  /// @return Prior function
  static TempoPrior uniform_prior(float min_bpm, float max_bpm, float falloff = 0.1f) {
    return [=](float bpm) { return (bpm >= min_bpm && bpm <= max_bpm) ? 1.0f : falloff; };
  }

  /// @brief Creates a log-normal prior centered at a BPM (librosa-compatible).
  /// @param center_bpm Center BPM value
  /// @param sigma Standard deviation in log-space (default 1.0)
  /// @return Prior function
  /// @details Equivalent to scipy.stats.lognorm with loc=log(center_bpm).
  static TempoPrior lognorm_prior(float center_bpm, float sigma = 1.0f) {
    return [=](float bpm) {
      if (bpm <= 0.0f) return 0.0f;
      float log_ratio = std::log(bpm / center_bpm);
      return std::exp(-0.5f * (log_ratio * log_ratio) / (sigma * sigma));
    };
  }
};

/// @brief BPM candidate with confidence score.
struct BpmCandidate {
  float bpm;         ///< BPM value
  float confidence;  ///< Confidence score [0, 1]
};

/// @brief BPM analyzer using autocorrelation of onset strength.
class BpmAnalyzer {
 public:
  /// @brief Constructs BPM analyzer from audio.
  /// @param audio Input audio
  /// @param config BPM configuration
  explicit BpmAnalyzer(const Audio& audio, const BpmConfig& config = BpmConfig());

  /// @brief Constructs BPM analyzer from pre-computed onset strength.
  /// @param onset_strength Onset strength envelope
  /// @param sr Sample rate
  /// @param hop_length Hop length used for onset detection
  /// @param config BPM configuration
  BpmAnalyzer(const std::vector<float>& onset_strength, int sr, int hop_length,
              const BpmConfig& config = BpmConfig());

  /// @brief Returns the estimated BPM.
  float bpm() const { return bpm_; }

  /// @brief Returns confidence of the BPM estimate [0, 1].
  float confidence() const { return confidence_; }

  /// @brief Returns top BPM candidates.
  /// @param top_n Number of candidates to return
  /// @return Sorted list of BPM candidates
  std::vector<BpmCandidate> candidates(int top_n = 5) const;

  /// @brief Returns the autocorrelation function.
  const std::vector<float>& autocorrelation() const { return autocorr_; }

  /// @brief Returns the tempogram (tempo vs time).
  /// @return Tempogram values
  const std::vector<float>& tempogram() const { return tempogram_; }

 private:
  void analyze(const std::vector<float>& onset_strength, int sr, int hop_length);

  float bpm_;
  float confidence_;
  std::vector<float> autocorr_;
  std::vector<float> tempogram_;
  std::vector<BpmCandidate> candidates_;
  BpmConfig config_;
};

/// @brief Quick BPM detection function.
/// @param audio Input audio
/// @param config BPM configuration
/// @return Estimated BPM
float detect_bpm(const Audio& audio, const BpmConfig& config = BpmConfig());

}  // namespace sonare
