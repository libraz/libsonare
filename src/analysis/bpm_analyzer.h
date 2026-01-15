#pragma once

/// @file bpm_analyzer.h
/// @brief BPM (tempo) detection with harmonic clustering.
///
/// @section bpm_algorithm Algorithm Overview
///
/// libsonare uses a harmonic clustering approach for BPM detection that differs
/// from librosa's implementation:
///
/// **librosa approach:**
/// - Uses tempogram (autocorrelation-based tempo salience)
/// - Aggregates tempogram across time to find dominant tempo
/// - Prior weighting based on start_bpm parameter
/// - Returns tempo from peak in aggregated tempogram
///
/// **libsonare approach:**
/// - Computes onset strength envelope and autocorrelation
/// - Builds BPM histogram from local maxima (0.5 BPM bins)
/// - Groups candidates into harmonic clusters (half, double, triplet, etc.)
/// - Uses smart selection: prefers musically common tempos (80-180 BPM)
/// - Avoids octave errors by preferring higher BPM in harmonic relationships
///
/// @section bpm_differences Key Differences from librosa
///
/// | Aspect              | librosa                  | libsonare              |
/// |---------------------|--------------------------|------------------------|
/// | Octave handling     | Prior weighting          | Harmonic clustering    |
/// | BPM selection       | Max peak                 | Smart musical choice   |
/// | Multi-tempo support | aggregate='median'       | Top 10 bins clustering |
/// | Confidence metric   | Not provided             | Vote-based [0,1]       |
///
/// Results may differ from librosa, especially for:
/// - Tracks with ambiguous tempo (multiple valid BPMs)
/// - Very slow (<60 BPM) or very fast (>200 BPM) tracks
/// - Tracks with strong half-time or double-time feels

#include <array>
#include <map>
#include <utility>
#include <vector>

#include "core/audio.h"

namespace sonare {

/// @brief Constants for BPM detection algorithm.
/// @details Parameters match bpm-detector Python implementation for consistency.
namespace bpm_constants {
/// @brief BPM histogram bin width (0.5 BPM resolution).
constexpr float kBinWidth = 0.5f;

/// @brief Tolerance for harmonic ratio matching (5%).
constexpr float kHarmonicTolerance = 0.05f;

/// @brief Threshold for preferring higher BPM clusters (15% of total votes).
constexpr float kThreshHigher = 0.15f;

/// @brief Number of top histogram bins to consider for clustering.
constexpr int kTopBins = 10;

/// @brief Harmonic ratios for BPM clustering.
/// @details Includes common tempo relationships: half, 2/3, 3/4, same, 4/3, 3/2, double, triple,
/// quadruple.
constexpr std::array<float, 9> kHarmonicRatios = {0.5f, 2.0f / 3.0f, 0.75f, 1.0f, 4.0f / 3.0f,
                                                   1.5f, 2.0f,       3.0f,  4.0f};
}  // namespace bpm_constants

/// @brief Configuration for BPM analysis.
/// @details Default values match librosa.
struct BpmConfig {
  float bpm_min = 30.0f;     ///< Minimum BPM to consider
  float bpm_max = 300.0f;    ///< Maximum BPM to consider
  float start_bpm = 120.0f;  ///< Initial BPM estimate (used as fallback)
  int n_fft = 2048;          ///< FFT size for onset detection
  int hop_length = 512;      ///< Hop length for onset detection
};

/// @brief BPM candidate with confidence score.
struct BpmCandidate {
  float bpm;         ///< BPM value
  float confidence;  ///< Confidence score [0, 1]
};

/// @brief BPM histogram bin with vote count.
struct BpmHistogramBin {
  float bpm_center;  ///< Center BPM of the bin
  int votes;         ///< Number of votes (tempo candidates in this bin)
};

/// @brief Harmonic cluster of related BPM candidates.
/// @details Key is the base BPM, value is list of (bpm, votes) pairs in the cluster.
using HarmonicClusterMap = std::map<float, std::vector<std::pair<float, int>>>;

/// @brief Builds BPM histogram from tempo candidates.
/// @param candidates BPM candidates from tempo estimation
/// @param bpm_min Minimum BPM
/// @param bpm_max Maximum BPM
/// @param bin_width Histogram bin width (default: 0.5 BPM)
/// @return Vector of histogram bins sorted by vote count (descending)
std::vector<BpmHistogramBin> build_bpm_histogram(const std::vector<float>& candidates,
                                                  float bpm_min, float bpm_max,
                                                  float bin_width = bpm_constants::kBinWidth);

/// @brief Groups BPM candidates into harmonic clusters.
/// @details Groups BPMs that are harmonically related (half, double, triplet, etc.).
///          Uses kHarmonicTolerance (5%) for ratio matching.
/// @param top_bins Top histogram bins sorted by votes
/// @return Map of base_bpm -> [(bpm, votes), ...] clusters
HarmonicClusterMap harmonic_cluster(const std::vector<BpmHistogramBin>& top_bins);

/// @brief Selects best BPM from harmonic clusters using smart selection.
/// @details Prefers higher BPM clusters if they have >= 15% of total votes,
///          otherwise selects the most voted BPM in the largest cluster.
/// @param clusters Harmonic clusters from harmonic_cluster()
/// @param total_votes Total number of votes across all bins
/// @return Pair of (best_bpm, confidence_percentage)
std::pair<float, float> smart_choice(const HarmonicClusterMap& clusters, int total_votes);

/// @brief BPM analyzer using autocorrelation with harmonic clustering.
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
