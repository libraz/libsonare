#pragma once

/// @file dynamics_analyzer.h
/// @brief Dynamics and loudness analysis.

#include <vector>

#include "core/audio.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare {

/// @brief Dynamics analysis results.
struct Dynamics {
  float dynamic_range_db = 0.0f;   ///< Dynamic range in dB
  float peak_db = 0.0f;            ///< Peak level in dB
  float rms_db = 0.0f;             ///< RMS level in dB
  float crest_factor = 0.0f;       ///< Peak to RMS ratio
  float loudness_range_db = 0.0f;  ///< Loudness range (LRA) in dB
  bool is_compressed = false;      ///< True if audio appears heavily compressed; false for silence
};

/// @brief Loudness curve over time.
struct LoudnessCurve {
  std::vector<float> times;   ///< Time points in seconds
  std::vector<float> rms_db;  ///< RMS level in dB at each time point
};

/// @brief Configuration for dynamics analysis.
struct DynamicsConfig {
  float window_sec = 0.4f;             ///< RMS window in seconds; finite and positive
  int hop_length = 512;                ///< Hop length for loudness curve
  float compression_threshold = 6.0f;  ///< Dynamic range below this suggests compression
};

/// @brief Refuses a DynamicsConfig outside the argument domain every public surface shares.
/// @throws SonareException(InvalidParameter) unless window_sec is finite and positive, hop_length
///         is positive, and compression_threshold is finite and non-negative.
inline void validate_dynamics_config(const DynamicsConfig& config) {
  SONARE_CHECK_MSG(numeric::finite_positive(config.window_sec), ErrorCode::InvalidParameter,
                   "DynamicsConfig: window_sec must be finite and positive");
  SONARE_CHECK_MSG(config.hop_length > 0, ErrorCode::InvalidParameter,
                   "DynamicsConfig: hop_length must be positive");
  SONARE_CHECK_MSG(numeric::finite_non_negative(config.compression_threshold),
                   ErrorCode::InvalidParameter,
                   "DynamicsConfig: compression_threshold must be finite and non-negative");
}

/// @brief Dynamics analyzer for loudness and dynamic range.
class DynamicsAnalyzer {
 public:
  /// @brief Constructs dynamics analyzer from audio.
  /// @param audio Input audio
  /// @param config Dynamics configuration
  /// @throws SonareException(InvalidParameter) if @p audio is empty or `window_sec` is not
  ///         finite and positive.
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
