#pragma once

/// @file dynamics_analyzer.h
/// @brief Dynamics and loudness analysis.

#include <vector>

#include "core/audio.h"

namespace sonare {

/// @brief Dynamics analysis results.
struct Dynamics {
  float dynamic_range_db;   ///< Dynamic range in dB
  float peak_db;            ///< Peak level in dB
  float rms_db;             ///< RMS level in dB
  float crest_factor;       ///< Peak to RMS ratio
  float loudness_range_db;  ///< Loudness range (LRA) in dB
  bool is_compressed;       ///< True if audio appears heavily compressed
};

/// @brief Loudness curve over time.
struct LoudnessCurve {
  std::vector<float> times;   ///< Time points in seconds
  std::vector<float> rms_db;  ///< RMS level in dB at each time point
};

/// @brief Configuration for dynamics analysis.
struct DynamicsConfig {
  float window_sec = 0.4f;             ///< Window size for RMS calculation in seconds
  int hop_length = 512;                ///< Hop length for loudness curve
  float compression_threshold = 6.0f;  ///< Dynamic range below this suggests compression
};

/// @brief Dynamics analyzer for loudness and dynamic range.
class DynamicsAnalyzer {
 public:
  /// @brief Constructs dynamics analyzer from audio.
  /// @param audio Input audio
  /// @param config Dynamics configuration
  explicit DynamicsAnalyzer(const Audio& audio, const DynamicsConfig& config = DynamicsConfig());

  /// @brief Returns dynamics analysis results.
  const Dynamics& dynamics() const { return dynamics_; }

  /// @brief Returns dynamic range in dB.
  float dynamic_range_db() const { return dynamics_.dynamic_range_db; }

  /// @brief Returns peak level in dB.
  float peak_db() const { return dynamics_.peak_db; }

  /// @brief Returns RMS level in dB.
  float rms_db() const { return dynamics_.rms_db; }

  /// @brief Returns crest factor (peak/RMS ratio in dB).
  float crest_factor() const { return dynamics_.crest_factor; }

  /// @brief Returns true if audio appears to be heavily compressed.
  bool is_compressed() const { return dynamics_.is_compressed; }

  /// @brief Returns loudness curve over time.
  const LoudnessCurve& loudness_curve() const { return loudness_curve_; }

  /// @brief Returns histogram of loudness values.
  /// @param n_bins Number of histogram bins
  /// @param min_db Minimum dB value
  /// @param max_db Maximum dB value
  /// @return Histogram counts
  std::vector<int> loudness_histogram(int n_bins = 100, float min_db = -60.0f,
                                      float max_db = 0.0f) const;

 private:
  void analyze(const Audio& audio);

  Dynamics dynamics_;
  LoudnessCurve loudness_curve_;
  DynamicsConfig config_;
};

}  // namespace sonare
