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
#include <string>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

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
/// @details A peak is in range when its refined tempo lies inside [bpm_min, bpm_max]; every other
///          peak is evidence only. Allocation free.
template <typename Visit>
void for_each_tempo_peak(const float* autocorr, int size, int sr, int hop_length, float bpm_min,
                         float bpm_max, Visit&& visit) {
  const int lag_first = std::max(1, bpm_to_lag(3.0f * bpm_max, sr, hop_length));
  const int lag_last = std::min(size - 2, tempo_evidence_lag(sr, hop_length, bpm_min));
  for (int lag = lag_first; lag <= lag_last; ++lag) {
    TempoPeak peak;
    if (!measure_tempo_peak(autocorr, lag, sr, hop_length, peak)) continue;
    if (peak.bpm < 0.5f * bpm_min || peak.bpm > 3.0f * bpm_max) continue;
    peak.in_range = peak.bpm >= bpm_min && peak.bpm <= bpm_max;
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

/// @brief Common musical range a default search prefers when choosing between octaves.
constexpr float kCommonBpmMin = 80.0f;
constexpr float kCommonBpmMax = 180.0f;

/// @brief The default search range; any other range is the caller's constraint.
constexpr float kDefaultBpmMin = 60.0f;
constexpr float kDefaultBpmMax = 200.0f;
}  // namespace bpm_constants

/// @brief Configuration for BPM analysis.
/// @details The default 60-200 BPM search is the one every public tempo entry uses.
struct BpmConfig {
  /// A range other than the default is searched as given, and the unconstrained estimate is
  /// preferred when it lies inside it.
  float bpm_min = bpm_constants::kDefaultBpmMin;  ///< Minimum BPM to consider (inclusive)
  float bpm_max = bpm_constants::kDefaultBpmMax;  ///< Maximum BPM to consider (inclusive)
  /// Finite, positive beat-tracking prior in BPM. Standalone tempo detection ignores it
  /// and reports NaN when it finds no tempo.
  float start_bpm = 120.0f;
  int n_fft = 2048;      ///< Window length in samples at 22050 Hz, rescaled to the input rate
  int hop_length = 512;  ///< Hop length in samples of the input buffer
};

/// @brief Domain of the tempo search hop_length; n_fft takes @ref kStftNFftDomain.
inline constexpr util::check::NumericDomain kBpmCountDomain =
    util::check::integer_domain_of<int>(1);

/// @brief Refuses a tempo search outside the argument domain every public surface shares.
/// @details The rule @ref validate_bpm_config and validate_rhythm_config both apply.
/// @throws SonareParameterRefusal(InvalidParameter) unless bpm_min is finite and positive, bpm_max
///         is finite and above bpm_min, start_bpm is finite and positive, n_fft is even and in
///         @ref kStftNFftDomain, and hop_length is positive.
inline void validate_tempo_search(float bpm_min, float bpm_max, float start_bpm, int n_fft,
                                  int hop_length) {
  util::check::greater_than(bpm_min, 0.0, "bpm_min");
  if (!numeric::finite_ordered_range(bpm_min, bpm_max)) {
    util::check::refuse_relation({"bpm_max", "bpm_min"}, "{0} must be finite and greater than {1}");
  }
  util::check::greater_than(start_bpm, 0.0, "start_bpm");
  util::check::even_within(n_fft, kStftNFftDomain, "n_fft");
  util::check::integer_within<int>(hop_length, kBpmCountDomain, "hop_length");
}

/// @brief Refuses a BpmConfig outside the argument domain every public surface shares.
/// @throws SonareException(InvalidParameter) as @ref validate_tempo_search describes.
inline void validate_bpm_config(const BpmConfig& config) {
  validate_tempo_search(config.bpm_min, config.bpm_max, config.start_bpm, config.n_fft,
                        config.hop_length);
}

/// @brief Refuses a negative candidate count for BpmAnalyzer::candidates, which would clamp it.
/// @throws SonareException(InvalidParameter) when @p max_candidates is negative.
inline void validate_bpm_candidate_count(int max_candidates) {
  SONARE_CHECK_MSG(max_candidates >= 0, ErrorCode::InvalidParameter,
                   "max_candidates must be non-negative");
}

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
/// @param preferred_min,preferred_max The band candidates are preferred from: the common musical
///        range for a default search, the caller's range for a constrained one.
BeatTempoChoice choose_beat_tempo(const BpmHistogramBin* candidates, size_t n_candidates,
                                  const BpmHistogramBin* evidence, size_t n_evidence,
                                  float preferred_min = bpm_constants::kCommonBpmMin,
                                  float preferred_max = bpm_constants::kCommonBpmMax);

/// @brief Selects best BPM from harmonic clusters using smart selection.
/// @details @ref choose_beat_tempo over every cluster member and @p octave_evidence.
/// @param clusters Harmonic clusters from harmonic_cluster()
/// @param total_votes Total number of votes across all bins
/// @param octave_evidence Peaks outside the analysed range; they count as support and are never
///        chosen
/// @return Pair of (best_bpm, confidence_percentage), the confidence being the vote share of the
///         cluster holding the winning family
/// @param preferred_min,preferred_max See @ref choose_beat_tempo.
std::pair<float, float> smart_choice(const HarmonicClusterMap& clusters, int total_votes,
                                     const std::vector<BpmHistogramBin>& octave_evidence = {},
                                     float preferred_min = bpm_constants::kCommonBpmMin,
                                     float preferred_max = bpm_constants::kCommonBpmMax);

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
  /// @return Tempo in BPM, or NaN (kNotMeasured) when no tempo was found.
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
  // Whether the configured range differs from the default search.
  bool is_constrained() const noexcept;
  // A constrained search returns the unconstrained estimate when it lies inside the range.
  void prefer_unconstrained_estimate(const std::vector<float>& onset_strength, int sr,
                                     int hop_length);

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
/// @return Estimated BPM, or NaN when no tempo was found
float detect_bpm(const Audio& audio, const BpmConfig& config = BpmConfig());

}  // namespace sonare
