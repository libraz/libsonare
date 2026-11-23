#pragma once

/// @file dynamics_analyzer.h
/// @brief Dynamics and loudness analysis.

#include <vector>

#include "core/audio.h"
#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

namespace sonare {

/// @brief Dynamics analysis results.
struct Dynamics {
  float dynamic_range_db =
      0.0f;  ///< Dynamic range in dB; +inf over a silent low percentile, NaN when both are silent
  float peak_db = 0.0f;            ///< Peak level in dB; -inf for exact silence
  float rms_db = 0.0f;             ///< RMS level in dB; -inf for exact silence
  float crest_factor = 0.0f;       ///< Peak to RMS ratio in dB; NaN for silence
  float loudness_range_lu = 0.0f;  ///< Loudness range (LRA) in dB; NaN with no gated block
  bool is_compressed = false;      ///< True if audio appears heavily compressed; false for silence
};

/// @brief Loudness curve over time.
struct LoudnessCurve {
  std::vector<float> times;   ///< Time points in seconds
  std::vector<float> rms_db;  ///< RMS level in dB at each time point; -inf for an all-zero window
};

/// @brief Configuration for dynamics analysis.
struct DynamicsConfig {
  float window_sec = 0.4f;             ///< RMS window in seconds; finite and positive
  int hop_length = 512;                ///< Hop length for loudness curve
  float compression_threshold = 6.0f;  ///< Dynamic range below this suggests compression
};

/// @brief Domains of the DynamicsConfig members, stated once for the core check and the readers.
inline constexpr util::check::NumericDomain kDynamicsWindowSecDomain =
    util::check::real_domain_above(0.0, false);
inline constexpr util::check::NumericDomain kDynamicsHopDomain =
    util::check::integer_domain_of<int>(1);
inline constexpr util::check::NumericDomain kDynamicsCompressionThresholdDomain =
    util::check::real_domain_above(0.0, true);

/// @brief Refuses a DynamicsConfig outside the argument domain every public surface shares.
/// @throws SonareException(InvalidParameter) unless window_sec is finite and positive, hop_length
///         is positive, and compression_threshold is finite and non-negative.
inline void validate_dynamics_config(const DynamicsConfig& config) {
  util::check::within(config.window_sec, kDynamicsWindowSecDomain, "window_sec");
  util::check::within(config.hop_length, kDynamicsHopDomain, "hop_length");
  util::check::within(config.compression_threshold, kDynamicsCompressionThresholdDomain,
                      "compression_threshold");
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
