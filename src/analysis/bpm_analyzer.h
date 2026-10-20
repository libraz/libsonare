#pragma once

/// @file bpm_analyzer.h
/// @brief BPM (tempo) detection with harmonic clustering.
///
/// @section bpm_algorithm Algorithm Overview
///
/// libsonare uses a harmonic clustering approach for BPM detection:
///
/// **Current approach:**
/// - Computes onset strength envelope and autocorrelation
/// - Builds BPM histogram from local maxima (0.5 BPM bins)
/// - Groups candidates into harmonic clusters (half, double, triplet, etc.)
/// - Picks the octave family of the candidate best supported at its tempo, half of it and its
///   subdivision, counting peaks beyond the range
/// - Uses smart selection: prefers strongly supported musically common tempos (80-180 BPM)
/// - Avoids octave errors with harmonic relationships and bounded octave correction

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <utility>
#include <vector>

#include "core/audio.h"

namespace sonare {

/// @brief Converts a tempo to an autocorrelation lag in frames (0 when @p bpm is not positive).
/// @details Shared with the streaming analyzer so both read one lag grid.
int bpm_to_lag(float bpm, int sr, int hop_length);

/// @brief One positive autocorrelation peak, measured between integer lags.
struct TempoPeak {
  float bpm = 0.0f;       ///< Tempo at the parabolically refined lag
  float strength = 0.0f;  ///< Mean of the peak sample and its two neighbours, in [0, 1]
  int votes = 0;          ///< Votes it casts: strength x 100, in [1, 100]
  bool in_range = false;  ///< May be chosen; otherwise it is beat evidence only
};

/// @brief Measures the peak at @p lag when it is a positive local maximum.
/// @details A peak about two lags wide keeps its mass inside the three samples around it wherever
///          its apex falls, so a peak split across two lags weighs what an aligned one does.
///          Requires 1 <= @p lag and `lag + 1` inside @p autocorr. Allocation free.
/// @return False when @p lag is not a positive local maximum
bool measure_tempo_peak(const float* autocorr, int lag, int sr, int hop_length, TempoPeak& peak);

/// @brief Longest lag a tempo estimate over [@p bpm_min, ...] reads: lag(bpm_min / 2), so the
///        half tempo of the slowest candidate is visible as evidence.
int tempo_evidence_lag(int sr, int hop_length, float bpm_min);

/// @brief Calls @p visit(const TempoPeak&) for each peak of @p autocorr from three times
///        @p bpm_max down to half of @p bpm_min, the span beat evidence is read from.
/// @details A peak is in range inside [bpm_min, bpm_max] and below lag(bpm_min); the last two
///          lags of that range are evidence only. Allocation free.
template <typename Visit>
void for_each_tempo_peak(const float* autocorr, int size, int sr, int hop_length, float bpm_min,
                         float bpm_max, Visit&& visit) {
  const int range_lags = std::min(bpm_to_lag(bpm_min, sr, hop_length), size);
  const int lag_first = std::max(1, bpm_to_lag(3.0f * bpm_max, sr, hop_length));
  const int lag_last = std::min(size - 2, tempo_evidence_lag(sr, hop_length, bpm_min));
  for (int lag = lag_first; lag <= lag_last; ++lag) {
    TempoPeak peak;
    if (!measure_tempo_peak(autocorr, lag, sr, hop_length, peak)) continue;
    if (peak.bpm < 0.5f * bpm_min || peak.bpm > 3.0f * bpm_max) continue;
    peak.in_range = peak.bpm >= bpm_min && peak.bpm <= bpm_max && lag <= range_lags - 2;
    visit(peak);
  }
}

/// @brief Constants for BPM detection algorithm.
/// @details Parameters match bpm-detector Python implementation for consistency.
namespace bpm_constants {
/// @brief BPM histogram bin width (0.5 BPM resolution).
constexpr float kBinWidth = 0.5f;

/// @brief Tolerance for harmonic ratio matching (5%).
constexpr float kHarmonicTolerance = 0.05f;

/// @brief Number of top histogram bins to consider for clustering.
constexpr int kTopBins = 10;

/// @brief Harmonic ratios for BPM clustering.
/// @details Includes common tempo relationships: half, 2/3, 3/4, same, 4/3, 3/2, double, triple,
/// quadruple.
constexpr std::array<float, 9> kHarmonicRatios = {0.5f, 2.0f / 3.0f, 0.75f, 1.0f, 4.0f / 3.0f,
                                                  1.5f, 2.0f,        3.0f,  4.0f};
}  // namespace bpm_constants

/// @brief Configuration for BPM analysis.
/// @details Default values follow common MIR defaults.
struct BpmConfig {
  float bpm_min = 30.0f;     ///< Minimum BPM to consider
  float bpm_max = 300.0f;    ///< Maximum BPM to consider
  float start_bpm = 120.0f;  ///< Initial BPM estimate (used as fallback)
  int n_fft = 2048;          ///< Window length in samples at 22050 Hz, rescaled to the input rate
  int hop_length = 512;      ///< Hop length in samples of the input buffer
};

/// @brief BPM candidate with confidence score.
struct BpmCandidate {
  float bpm;         ///< BPM value
  float confidence;  ///< Confidence score [0, 1]
};

/// @brief BPM histogram bin with vote count.
struct BpmHistogramBin {
  float bpm_center;  ///< Vote-weighted mean of the tempos that landed in the bin
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

/// @brief Beat tempo chosen from in-range candidates and out-of-range evidence.
struct BeatTempoChoice {
  float bpm = 0.0f;     ///< Chosen tempo, 0 when no candidate is supported
  float anchor = 0.0f;  ///< Best supported candidate, whose octave family @ref bpm belongs to
  int votes = 0;        ///< Votes of the chosen candidate
};

/// @brief Allocation-free core of @ref smart_choice, shared with the streaming analyzer.
/// @details Scores each common-range candidate by the votes at its tempo, at half of it and at its
///          subdivision (twice or three times it, whichever holds more), across @p candidates and
///          @p evidence, then picks the strongest supported member of the winner's octave family
///          (tempi a power of two apart). A 2/3 or 3/2 relative is chosen only as the better
///          supported beat, never for out-voting the beat at its own tempo alone.
BeatTempoChoice choose_beat_tempo(const BpmHistogramBin* candidates, size_t n_candidates,
                                  const BpmHistogramBin* evidence, size_t n_evidence);

/// @brief Selects best BPM from harmonic clusters using smart selection.
/// @details @ref choose_beat_tempo over every cluster member and @p octave_evidence.
/// @param clusters Harmonic clusters from harmonic_cluster()
/// @param total_votes Total number of votes across all bins
/// @param octave_evidence Peaks outside the analysed range; they count as support and are never
///        chosen
/// @return Pair of (best_bpm, confidence_percentage), the confidence being the vote share of the
///         cluster holding the winning family
std::pair<float, float> smart_choice(const HarmonicClusterMap& clusters, int total_votes,
                                     const std::vector<BpmHistogramBin>& octave_evidence = {});

/// @brief Applies 3-peak octave correction to tempo candidates.
/// @details If a half/double-related pair exists among the strongest candidates,
///          returns the middle octave in the configured range.
/// @param candidates Tempo candidates sorted by confidence descending
/// @param bpm_min Minimum allowed BPM
/// @param bpm_max Maximum allowed BPM
/// @param tolerance Relative octave-ratio tolerance
/// @return Corrected BPM, or the strongest candidate if no octave relation is found
float three_peak_octave_correction(const std::vector<BpmCandidate>& candidates, float bpm_min,
                                   float bpm_max, float tolerance = 0.04f);

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
  /// @details When non-empty, the first entry is the chosen tempo (its `bpm` equals bpm()) and
  ///          the rest follow in descending confidence, each a histogram bin's vote share.
  /// @param top_n Number of candidates to return
  /// @return BPM candidates, chosen tempo first
  std::vector<BpmCandidate> candidates(int top_n = 5) const;

  /// @brief Returns the autocorrelation function.
  const std::vector<float>& autocorrelation() const { return autocorr_; }

  /// @brief Returns the tempogram (tempo vs time).
  /// @return Fourier tempogram values [n_bins x n_time_frames] when available
  const std::vector<float>& tempogram() const { return tempogram_; }

  /// @brief Returns the local BPM curve extracted from the Fourier tempogram.
  const std::vector<float>& local_bpm_curve() const { return local_bpm_curve_; }

 private:
  void analyze(const std::vector<float>& onset_strength, int sr, int hop_length);

  float bpm_;
  float confidence_;
  std::vector<float> autocorr_;
  std::vector<float> tempogram_;
  std::vector<float> local_bpm_curve_;
  std::vector<BpmCandidate> candidates_;
  BpmConfig config_;
};

/// @brief Quick BPM detection function.
/// @param audio Input audio
/// @param config BPM configuration
/// @return Estimated BPM
float detect_bpm(const Audio& audio, const BpmConfig& config = BpmConfig());

}  // namespace sonare
