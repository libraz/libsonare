#include "analysis/bpm_analyzer.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "feature/rhythm.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare {

using sonare::constants::kEpsilon;

std::vector<BpmHistogramBin> build_bpm_histogram(const std::vector<float>& candidates,
                                                 float bpm_min, float bpm_max, float bin_width) {
  if (candidates.empty() || bpm_min >= bpm_max || bin_width <= 0.0f) {
    return {};
  }

  /// Calculate number of bins
  int n_bins = static_cast<int>((bpm_max - bpm_min) / bin_width) + 1;

  /// Build histogram
  std::vector<int> hist(n_bins, 0);
  for (float bpm : candidates) {
    if (bpm >= bpm_min && bpm <= bpm_max) {
      int bin_idx = static_cast<int>((bpm - bpm_min) / bin_width);
      bin_idx = std::clamp(bin_idx, 0, n_bins - 1);
      hist[bin_idx]++;
    }
  }

  /// Convert to BpmHistogramBin and sort by votes descending
  std::vector<BpmHistogramBin> result;
  result.reserve(n_bins);
  for (int i = 0; i < n_bins; ++i) {
    if (hist[i] > 0) {
      result.push_back({bpm_min + i * bin_width, hist[i]});
    }
  }

  std::sort(result.begin(), result.end(),
            [](const BpmHistogramBin& a, const BpmHistogramBin& b) { return a.votes > b.votes; });

  return result;
}

HarmonicClusterMap harmonic_cluster(const std::vector<BpmHistogramBin>& top_bins) {
  HarmonicClusterMap clusters;

  /// Process bins in order of votes (descending, already sorted)
  for (const auto& bin : top_bins) {
    float bpm = bin.bpm_center;
    int votes = bin.votes;

    /// Try to find an existing cluster this BPM belongs to
    bool found_cluster = false;
    for (auto& [base, members] : clusters) {
      float ratio = bpm / base;

      /// Check if ratio matches any harmonic relationship
      for (float k : bpm_constants::kHarmonicRatios) {
        /// Check both ratio == k and ratio == 1/k
        if (std::abs(ratio - k) < bpm_constants::kHarmonicTolerance ||
            std::abs(ratio - 1.0f / k) < bpm_constants::kHarmonicTolerance) {
          members.emplace_back(bpm, votes);
          found_cluster = true;
          break;
        }
      }
      if (found_cluster) break;
    }

    /// Create new cluster if no match found
    if (!found_cluster) {
      clusters[bpm] = {{bpm, votes}};
    }
  }

  return clusters;
}

std::pair<float, float> smart_choice(const HarmonicClusterMap& clusters, int total_votes) {
  /// Handle edge cases
  if (clusters.empty()) {
    return {120.0f, 0.0f};  ///< Default BPM with zero confidence
  }
  if (total_votes <= 0) {
    return {120.0f, 0.0f};
  }

  /// Find the base cluster with largest total votes
  float best_base = 0.0f;
  int best_base_votes = 0;
  for (const auto& [base, members] : clusters) {
    int cluster_votes = 0;
    for (const auto& [bpm, v] : members) {
      cluster_votes += v;
    }
    if (cluster_votes > best_base_votes) {
      best_base_votes = cluster_votes;
      best_base = base;
    }
  }

  /// Select from the base cluster
  const auto& base_members = clusters.at(best_base);
  if (base_members.empty()) {
    return {120.0f, 0.0f};
  }

  /// @details Musical tempo preference: prefer BPMs in common range (80-180) over extremes.
  /// This helps avoid octave errors (e.g., detecting 60 instead of 120).
  constexpr float kCommonBpmMin = 80.0f;
  constexpr float kCommonBpmMax = 180.0f;
  constexpr float kAcceptableBpmMin = 60.0f;
  constexpr float kAcceptableBpmMax = 200.0f;

  /// Categorize candidates
  std::vector<std::pair<float, int>> common_range;      ///< 80-180 BPM
  std::vector<std::pair<float, int>> acceptable_range;  ///< 60-200 BPM
  int max_votes_in_cluster = 0;

  for (const auto& [bpm, v] : base_members) {
    max_votes_in_cluster = std::max(max_votes_in_cluster, v);
    if (bpm >= kCommonBpmMin && bpm <= kCommonBpmMax) {
      common_range.emplace_back(bpm, v);
    } else if (bpm >= kAcceptableBpmMin && bpm <= kAcceptableBpmMax) {
      acceptable_range.emplace_back(bpm, v);
    }
  }

  float rep_bpm = 0.0f;

  auto choose_strongest_preferred = [](const std::vector<std::pair<float, int>>& values,
                                       int min_votes) {
    std::pair<float, int> best{0.0f, 0};
    for (const auto& [bpm, v] : values) {
      if (v < min_votes) continue;
      if (v > best.second || (v == best.second && bpm > best.first)) {
        best = {bpm, v};
      }
    }
    return best.first;
  };

  /// First priority: candidates in common range (80-180) with >= 30% of max votes
  if (!common_range.empty()) {
    const int min_votes =
        static_cast<int>(std::ceil(0.3f * static_cast<float>(max_votes_in_cluster)));
    rep_bpm = choose_strongest_preferred(common_range, min_votes);
  }

  /// Second priority: candidates in acceptable range (60-200) with >= 50% of max votes
  if (rep_bpm == 0.0f && !acceptable_range.empty()) {
    const int min_votes =
        static_cast<int>(std::ceil(0.5f * static_cast<float>(max_votes_in_cluster)));
    rep_bpm = choose_strongest_preferred(acceptable_range, min_votes);
  }

  /// Fall back to highest voted member if no preferred candidate found
  if (rep_bpm == 0.0f) {
    auto best_member =
        std::max_element(base_members.begin(), base_members.end(),
                         [](const auto& a, const auto& b) { return a.second < b.second; });
    rep_bpm = best_member->first;
  }

  float confidence = 100.0f * static_cast<float>(best_base_votes) / static_cast<float>(total_votes);

  return {rep_bpm, confidence};
}

float three_peak_octave_correction(const std::vector<BpmCandidate>& candidates, float bpm_min,
                                   float bpm_max, float tolerance) {
  if (candidates.empty()) {
    return 120.0f;
  }

  const int n = std::min(3, static_cast<int>(candidates.size()));
  std::vector<float> tempos;
  tempos.reserve(n);
  for (int i = 0; i < n; ++i) {
    const float bpm = candidates[i].bpm;
    if (bpm >= bpm_min && bpm <= bpm_max) {
      tempos.push_back(bpm);
    }
  }
  if (tempos.empty()) {
    return candidates.front().bpm;
  }

  std::sort(tempos.begin(), tempos.end());
  tempos.erase(
      std::unique(tempos.begin(), tempos.end(),
                  [](float a, float b) { return std::abs(a - b) / std::max(a, b) <= 0.01f; }),
      tempos.end());

  for (float low : tempos) {
    const float mid = low * 2.0f;
    const float high = low * 4.0f;
    auto has_close = [&](float target) {
      return std::any_of(tempos.begin(), tempos.end(),
                         [&](float bpm) { return std::abs(bpm - target) / target <= tolerance; });
    };
    if (mid >= bpm_min && mid <= bpm_max && has_close(mid) && has_close(high)) {
      return mid;
    }
  }

  for (int i = 0; i < n; ++i) {
    for (int j = i + 1; j < n; ++j) {
      const float a = candidates[i].bpm;
      const float b = candidates[j].bpm;
      const float low = std::min(a, b);
      const float high = std::max(a, b);
      if (low <= 0.0f) continue;
      const float ratio = high / low;
      if (std::abs(ratio - 2.0f) / 2.0f <= tolerance) {
        if (high >= 80.0f && high <= 180.0f) return high;
        if (low >= 80.0f && low <= 180.0f) return low;
        return std::clamp(high, bpm_min, bpm_max);
      }
    }
  }

  return candidates.front().bpm;
}

namespace {

/// @brief Computes autocorrelation of a signal using shared FFT-based implementation.
std::vector<float> compute_autocorrelation_local(const std::vector<float>& signal, int max_lag) {
  std::vector<float> autocorr(max_lag, 0.0f);
  compute_autocorrelation(signal.data(), static_cast<int>(signal.size()), max_lag, autocorr.data());
  return autocorr;
}

/// @brief Converts lag (in frames) to BPM.
float lag_to_bpm(float lag, int sr, int hop_length) {
  if (lag <= 0.0f) return 0.0f;
  float seconds_per_beat = lag * static_cast<float>(hop_length) / static_cast<float>(sr);
  return 60.0f / seconds_per_beat;
}

/// @brief Refines an autocorrelation peak location with parabolic interpolation.
float refine_peak_lag(const std::vector<float>& autocorr, int lag) {
  if (lag <= 0 || lag + 1 >= static_cast<int>(autocorr.size())) {
    return static_cast<float>(lag);
  }

  const float left = autocorr[lag - 1];
  const float center = autocorr[lag];
  const float right = autocorr[lag + 1];
  const float denominator = left - 2.0f * center + right;
  if (std::abs(denominator) < constants::kEpsilon) {
    return static_cast<float>(lag);
  }

  const float offset = 0.5f * (left - right) / denominator;
  return static_cast<float>(lag) + std::clamp(offset, -0.5f, 0.5f);
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
  lag_max = std::min(static_cast<int>(autocorr.size()) - 2, lag_max);

  /// Find local maxima. lag_min >= 1 and lag_max <= size-2, so accessing both
  /// autocorr[lag-1] and autocorr[lag+1] over the full [lag_min, lag_max] range
  /// stays in bounds while still admitting peaks near the bpm_min/bpm_max extremes.
  for (int lag = lag_min; lag <= lag_max; ++lag) {
    if (autocorr[lag] > autocorr[lag - 1] && autocorr[lag] > autocorr[lag + 1]) {
      float bpm = lag_to_bpm(refine_peak_lag(autocorr, lag), sr, hop_length);
      if (bpm >= bpm_min && bpm <= bpm_max) {
        candidates.push_back({bpm, autocorr[lag]});
      }
    }
  }

  /// Sort by confidence (descending)
  std::sort(candidates.begin(), candidates.end(), [](const BpmCandidate& a, const BpmCandidate& b) {
    return a.confidence > b.confidence;
  });

  return candidates;
}

/// @brief Nominal Fourier-tempogram analysis window length in seconds.
/// @details A multi-second window is needed to resolve tempo harmonics in the
/// onset envelope; matches the librosa default tempogram window scale.
constexpr float kFourierTempogramWindowSec = 12.0f;

std::vector<float> extract_fourier_local_bpm_curve(const std::vector<float>& onset_strength, int sr,
                                                   int hop_length, float bpm_min, float bpm_max,
                                                   std::vector<float>* tempogram_out) {
  if (onset_strength.empty()) {
    if (tempogram_out) tempogram_out->clear();
    return {};
  }

  TempogramConfig config;
  config.hop_length = hop_length;
  // Cap the nominal window to the available onset frames so short signals do not
  // request a window longer than the data (which would over-pad the tempogram).
  int nominal_win =
      std::max(32, static_cast<int>(std::round(kFourierTempogramWindowSec * static_cast<float>(sr) /
                                               static_cast<float>(hop_length))));
  nominal_win = std::min(nominal_win, static_cast<int>(onset_strength.size()));
  // fourier_tempogram requires win_length > 1; a 1-frame onset envelope would
  // otherwise cap nominal_win at 1 and throw. Clamp to the minimum valid window
  // so degenerate short clips degrade gracefully (normal-length inputs are
  // unaffected: their power-of-two window already exceeds 2).
  config.win_length = 2;
  while (config.win_length < nominal_win) {
    config.win_length *= 2;
  }
  config.center = true;
  config.norm = false;

  std::vector<float> ft = fourier_tempogram(onset_strength, sr, config);
  if (tempogram_out) {
    *tempogram_out = ft;
  }

  const int n_frames = static_cast<int>(onset_strength.size());
  const int n_bins = config.win_length / 2 + 1;
  std::vector<float> curve(static_cast<size_t>(n_frames), 0.0f);

  auto bin_to_bpm = [&](int bin) {
    return static_cast<float>(bin) / static_cast<float>(config.win_length) * 60.0f *
           static_cast<float>(sr) / static_cast<float>(hop_length);
  };

  for (int frame = 0; frame < n_frames; ++frame) {
    std::vector<BpmCandidate> frame_candidates;
    frame_candidates.reserve(3);
    for (int bin = 1; bin < n_bins; ++bin) {
      const float bpm = bin_to_bpm(bin);
      if (bpm < bpm_min || bpm > bpm_max) continue;
      const float value = ft[static_cast<std::size_t>(bin) * static_cast<std::size_t>(n_frames) +
                             static_cast<std::size_t>(frame)];
      frame_candidates.push_back({bpm, value});
    }

    std::sort(
        frame_candidates.begin(), frame_candidates.end(),
        [](const BpmCandidate& a, const BpmCandidate& b) { return a.confidence > b.confidence; });
    if (!frame_candidates.empty() && frame_candidates.front().confidence > 0.0f) {
      curve[static_cast<size_t>(frame)] =
          three_peak_octave_correction(frame_candidates, bpm_min, bpm_max);
    }
  }

  return curve;
}

float median_bpm(std::vector<float> values) {
  values.erase(std::remove_if(values.begin(), values.end(),
                              [](float v) { return !std::isfinite(v) || v <= 0.0f; }),
               values.end());
  if (values.empty()) return 0.0f;
  const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::nth_element(values.begin(), mid, values.end());
  return *mid;
}

}  // namespace

BpmAnalyzer::BpmAnalyzer(const Audio& audio, const BpmConfig& config) : config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  /// Compute onset strength
  MelConfig mel_config;
  mel_config.n_fft = config.n_fft;
  mel_config.hop_length = config.hop_length;
  mel_config.n_mels = constants::kDefaultNMels;

  OnsetConfig onset_config;
  onset_config.lag = 1;
  onset_config.detrend = true;

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
    tempogram_.clear();
    local_bpm_curve_.clear();
    return;
  }

  local_bpm_curve_ = extract_fourier_local_bpm_curve(onset_strength, sr, hop_length,
                                                     config_.bpm_min, config_.bpm_max, &tempogram_);

  /// Compute max lag based on minimum BPM
  int max_lag = bpm_to_lag(config_.bpm_min, sr, hop_length);
  max_lag = std::min(max_lag, static_cast<int>(onset_strength.size()) - 1);

  if (max_lag < 2) {
    bpm_ = config_.start_bpm;
    confidence_ = 0.0f;
    if (tempogram_.empty()) tempogram_ = onset_strength;
    return;
  }

  /// Compute autocorrelation
  autocorr_ = compute_autocorrelation_local(onset_strength, max_lag);

  /// Find all tempo peaks
  candidates_ = find_tempo_peaks(autocorr_, sr, hop_length, config_.bpm_min, config_.bpm_max);

  if (candidates_.empty()) {
    const float local_median = median_bpm(local_bpm_curve_);
    bpm_ = local_median > 0.0f ? local_median : config_.start_bpm;
    confidence_ = local_median > 0.0f ? 0.25f : 0.0f;
    if (tempogram_.empty()) tempogram_ = autocorr_;
    return;
  }

  /// Use harmonic clustering for octave error handling

  /// Generate a weighted tempo histogram from all local maxima without expanding
  /// each peak into repeated candidate values.
  const int n_hist_bins =
      static_cast<int>((config_.bpm_max - config_.bpm_min) / bpm_constants::kBinWidth) + 1;
  std::vector<int> tempo_hist(n_hist_bins > 0 ? static_cast<size_t>(n_hist_bins) : 0u, 0);
  int lag_min = bpm_to_lag(config_.bpm_max, sr, hop_length);
  int lag_max_inner = bpm_to_lag(config_.bpm_min, sr, hop_length);
  lag_min = std::max(1, lag_min);
  lag_max_inner = std::min(static_cast<int>(autocorr_.size()) - 2, lag_max_inner);

  /// Iterate the full [lag_min, lag_max_inner] range (neighbor access stays in
  /// bounds because lag_min >= 1 and lag_max_inner <= size-2, keeping both
  /// autocorr_[lag-1] and autocorr_[lag+1] valid) so peaks at the documented
  /// bpm_min/bpm_max boundaries are not dropped.
  for (int lag = lag_min; lag <= lag_max_inner; ++lag) {
    if (autocorr_[lag] > autocorr_[lag - 1] && autocorr_[lag] > autocorr_[lag + 1] &&
        autocorr_[lag] > 0.0f) {
      float bpm = lag_to_bpm(refine_peak_lag(autocorr_, lag), sr, hop_length);
      if (bpm >= config_.bpm_min && bpm <= config_.bpm_max) {
        /// Add weighted votes based on autocorrelation strength.
        /// Clamp the upper bound to 100 so a pure-tone signal with an
        /// extremely strong autocorrelation peak cannot dominate unboundedly.
        int weight = std::min(100, std::max(1, static_cast<int>(autocorr_[lag] * 100.0f)));
        if (!tempo_hist.empty()) {
          int bin_idx = static_cast<int>((bpm - config_.bpm_min) / bpm_constants::kBinWidth);
          bin_idx = std::clamp(bin_idx, 0, static_cast<int>(tempo_hist.size()) - 1);
          tempo_hist[static_cast<size_t>(bin_idx)] += weight;
        }
      }
    }
  }

  std::vector<BpmHistogramBin> histogram;
  histogram.reserve(tempo_hist.size());
  for (size_t i = 0; i < tempo_hist.size(); ++i) {
    if (tempo_hist[i] > 0) {
      histogram.push_back(
          {config_.bpm_min + static_cast<float>(i) * bpm_constants::kBinWidth, tempo_hist[i]});
    }
  }
  std::sort(histogram.begin(), histogram.end(),
            [](const BpmHistogramBin& a, const BpmHistogramBin& b) { return a.votes > b.votes; });

  if (histogram.empty()) {
    /// Fall back to highest confidence candidate
    bpm_ = candidates_[0].bpm;
    confidence_ = candidates_[0].confidence;
  } else {
    /// Take top bins for clustering
    std::vector<BpmHistogramBin> top_bins;
    int n_top = std::min(bpm_constants::kTopBins, static_cast<int>(histogram.size()));
    top_bins.assign(histogram.begin(), histogram.begin() + n_top);

    /// Apply harmonic clustering
    auto clusters = harmonic_cluster(top_bins);

    /// Calculate total votes
    int total_votes = 0;
    for (const auto& bin : histogram) {
      total_votes += bin.votes;
    }

    /// Smart choice selection
    auto [rep_bpm, conf_percent] = smart_choice(clusters, total_votes);

    const float corrected =
        three_peak_octave_correction(candidates_, config_.bpm_min, config_.bpm_max);
    const float correction_ratio = corrected > 0.0f && rep_bpm > 0.0f ? corrected / rep_bpm : 0.0f;
    const bool octave_related = std::abs(correction_ratio - 0.5f) <= 0.04f ||
                                std::abs(correction_ratio - 1.0f) <= 0.04f ||
                                std::abs(correction_ratio - 2.0f) <= 0.08f;
    const bool corrected_common = corrected >= 80.0f && corrected <= 180.0f;
    const bool correction_downshifts_common =
        corrected_common && correction_ratio < 0.75f && rep_bpm >= 80.0f && rep_bpm <= 180.0f;
    bpm_ =
        (corrected_common && octave_related && !correction_downshifts_common) ? corrected : rep_bpm;
    if (bpm_ < 80.0f && !top_bins.empty()) {
      const float strongest_peak = top_bins.front().bpm_center;
      if (strongest_peak >= 80.0f && strongest_peak <= 180.0f) {
        bpm_ = strongest_peak;
      }
    }
    confidence_ = conf_percent / 100.0f;  ///< Convert percentage to [0, 1]

    /// Update candidates_ for API compatibility
    candidates_.clear();
    for (const auto& bin : top_bins) {
      float bin_conf = (total_votes > 0)
                           ? static_cast<float>(bin.votes) / static_cast<float>(total_votes)
                           : 0.0f;
      candidates_.push_back({bin.bpm_center, bin_conf});
    }
  }

  if (tempogram_.empty()) {
    tempogram_ = autocorr_;
  }
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
