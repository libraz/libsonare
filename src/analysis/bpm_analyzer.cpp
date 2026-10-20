#include "analysis/bpm_analyzer.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "analysis/analysis_rate.h"
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
  std::vector<double> bpm_sum(n_bins, 0.0);
  for (float bpm : candidates) {
    if (bpm >= bpm_min && bpm <= bpm_max) {
      int bin_idx = static_cast<int>((bpm - bpm_min) / bin_width);
      bin_idx = std::clamp(bin_idx, 0, n_bins - 1);
      hist[bin_idx]++;
      bpm_sum[bin_idx] += bpm;
    }
  }

  /// Convert to BpmHistogramBin and sort by votes descending
  std::vector<BpmHistogramBin> result;
  result.reserve(n_bins);
  for (int i = 0; i < n_bins; ++i) {
    if (hist[i] > 0) {
      // A bin reports the mean of the tempos it holds, not its lower edge.
      result.push_back({static_cast<float>(bpm_sum[i] / hist[i]), hist[i]});
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

BeatTempoChoice choose_beat_tempo(const BpmHistogramBin* candidates, size_t n_candidates,
                                  const BpmHistogramBin* evidence, size_t n_evidence) {
  /// @details Musical tempo preference: prefer BPMs in common range (80-180) over extremes.
  /// This helps avoid octave errors (e.g., detecting 60 instead of 120).
  constexpr float kCommonBpmMin = 80.0f;
  constexpr float kCommonBpmMax = 180.0f;
  constexpr float kAcceptableBpmMin = 60.0f;
  constexpr float kAcceptableBpmMax = 200.0f;
  const auto in_common_range = [&](float bpm) {
    return bpm >= kCommonBpmMin && bpm <= kCommonBpmMax;
  };
  const auto in_acceptable_range = [&](float bpm) {
    return bpm >= kAcceptableBpmMin && bpm <= kAcceptableBpmMax;
  };

  // The tempo comes from the octave family (bins a power of two apart) of the beat candidate with
  // the most votes at its own tempo, half of it, and its subdivision (twice or three times it,
  // whichever holds more). A 2/3 or 3/2 relative is then only chosen when it is the better beat.
  const auto same_octave_family = [](float a, float b) {
    const float ratio = a / b;
    const float octave = std::exp2(std::round(std::log2(ratio)));
    return std::abs(ratio / octave - 1.0f) < bpm_constants::kHarmonicTolerance;
  };
  const auto votes_near = [&](float target) {
    int votes = 0;
    const auto add = [&](const BpmHistogramBin* bins, size_t n) {
      for (size_t i = 0; i < n; ++i) {
        if (std::abs(bins[i].bpm_center / target - 1.0f) < bpm_constants::kHarmonicTolerance) {
          votes += bins[i].votes;
        }
      }
    };
    add(candidates, n_candidates);
    add(evidence, n_evidence);
    return votes;
  };
  const auto beat_support = [&](float c) {
    return votes_near(c) + votes_near(0.5f * c) +
           std::max(votes_near(2.0f * c), votes_near(3.0f * c));
  };

  bool any_common = false;
  for (size_t i = 0; i < n_candidates; ++i) {
    any_common = any_common || in_common_range(candidates[i].bpm_center);
  }
  BeatTempoChoice choice;
  int best_support = 0;
  for (size_t i = 0; i < n_candidates; ++i) {
    const float bpm = candidates[i].bpm_center;
    if (any_common && !in_common_range(bpm)) continue;
    const int support = beat_support(bpm);
    if (support > best_support) {
      best_support = support;
      choice.anchor = bpm;
    }
  }
  if (best_support <= 0) {
    return {};
  }

  int max_votes_in_family = 0;
  for (size_t i = 0; i < n_candidates; ++i) {
    if (same_octave_family(candidates[i].bpm_center, choice.anchor)) {
      max_votes_in_family = std::max(max_votes_in_family, candidates[i].votes);
    }
  }

  /// The strongest family member passing @p in_range with at least @p share of the family's
  /// largest vote, preferring the faster tempo on a tie.
  const auto choose_strongest_preferred = [&](auto in_range, float share) {
    const int min_votes =
        static_cast<int>(std::ceil(share * static_cast<float>(max_votes_in_family)));
    BpmHistogramBin best{0.0f, 0};
    for (size_t i = 0; i < n_candidates; ++i) {
      const BpmHistogramBin& bin = candidates[i];
      if (!same_octave_family(bin.bpm_center, choice.anchor) || !in_range(bin.bpm_center) ||
          bin.votes < min_votes) {
        continue;
      }
      if (bin.votes > best.votes || (bin.votes == best.votes && bin.bpm_center > best.bpm_center)) {
        best = bin;
      }
    }
    return best;
  };

  /// First priority: candidates in common range (80-180) with >= 30% of max votes
  BpmHistogramBin chosen = choose_strongest_preferred(in_common_range, 0.3f);
  /// Second priority: candidates in acceptable range (60-200) with >= 50% of max votes
  if (chosen.bpm_center == 0.0f) {
    chosen = choose_strongest_preferred(
        [&](float bpm) { return !in_common_range(bpm) && in_acceptable_range(bpm); }, 0.5f);
  }
  /// Fall back to highest voted member if no preferred candidate found
  if (chosen.bpm_center == 0.0f) {
    chosen = choose_strongest_preferred([](float) { return true; }, 1.0f);
  }
  choice.bpm = chosen.bpm_center;
  choice.votes = chosen.votes;
  return choice;
}

std::pair<float, float> smart_choice(const HarmonicClusterMap& clusters, int total_votes,
                                     const std::vector<BpmHistogramBin>& octave_evidence) {
  /// Handle edge cases
  if (clusters.empty()) {
    return {120.0f, 0.0f};  ///< Default BPM with zero confidence
  }
  if (total_votes <= 0) {
    return {120.0f, 0.0f};
  }

  std::vector<BpmHistogramBin> members;
  for (const auto& [base, cluster] : clusters) {
    (void)base;
    for (const auto& [bpm, v] : cluster) members.push_back({bpm, v});
  }
  const BeatTempoChoice choice = choose_beat_tempo(members.data(), members.size(),
                                                   octave_evidence.data(), octave_evidence.size());
  if (choice.bpm <= 0.0f) {
    return {120.0f, 0.0f};
  }

  // The confidence is the vote share of the cluster that holds the winning beat candidate.
  int cluster_votes = 0;
  for (const auto& [base, cluster] : clusters) {
    (void)base;
    const bool holds_anchor = std::any_of(cluster.begin(), cluster.end(), [&](const auto& member) {
      return member.first == choice.anchor;
    });
    if (!holds_anchor) continue;
    for (const auto& [bpm, v] : cluster) {
      (void)bpm;
      cluster_votes += v;
    }
    break;
  }
  const float confidence =
      100.0f * static_cast<float>(cluster_votes) / static_cast<float>(total_votes);
  return {choice.bpm, confidence};
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

// One comparator rather than an equivalent lambda per call site: a lambda has
// its own closure type, so each one instantiates std::sort's machinery again for
// the same element type.
bool bpm_candidate_more_confident(const BpmCandidate& a, const BpmCandidate& b) {
  return a.confidence > b.confidence;
}

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
  /// A peak is only a tempo candidate when the correlation itself is positive:
  /// the value doubles as the candidate's confidence, and a signal with no
  /// periodic onset structure has local maxima inside a wholly negative
  /// correlation whose values would otherwise surface as negative confidence.
  /// The weighted-histogram pass below applies the same non-negative gate.
  for (int lag = lag_min; lag <= lag_max; ++lag) {
    if (autocorr[lag] > 0.0f && autocorr[lag] > autocorr[lag - 1] &&
        autocorr[lag] > autocorr[lag + 1]) {
      TempoPeak peak;
      if (measure_tempo_peak(autocorr.data(), lag, sr, hop_length, peak) && peak.bpm >= bpm_min &&
          peak.bpm <= bpm_max) {
        candidates.push_back({peak.bpm, peak.strength});
      }
    }
  }

  /// Sort by confidence (descending)
  std::sort(candidates.begin(), candidates.end(), bpm_candidate_more_confident);

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

    std::sort(frame_candidates.begin(), frame_candidates.end(), bpm_candidate_more_confident);
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

int bpm_to_lag(float bpm, int sr, int hop_length) {
  if (bpm <= 0.0f) return 0;
  float seconds_per_beat = 60.0f / bpm;
  return static_cast<int>(seconds_per_beat * static_cast<float>(sr) /
                          static_cast<float>(hop_length));
}

bool measure_tempo_peak(const float* autocorr, int lag, int sr, int hop_length, TempoPeak& peak) {
  const float left = autocorr[lag - 1];
  const float center = autocorr[lag];
  const float right = autocorr[lag + 1];
  if (!(center > 0.0f && center > left && center > right)) return false;

  const float denominator = left - 2.0f * center + right;
  const float offset = std::abs(denominator) < constants::kEpsilon
                           ? 0.0f
                           : std::clamp(0.5f * (left - right) / denominator, -0.5f, 0.5f);
  peak.bpm = lag_to_bpm(static_cast<float>(lag) + offset, sr, hop_length);
  peak.strength = std::clamp((left + center + right) / 3.0f, 0.0f, 1.0f);
  /// Clamp the upper bound to 100 so a pure-tone signal with an
  /// extremely strong autocorrelation peak cannot dominate unboundedly.
  peak.votes = std::min(100, std::max(1, static_cast<int>(peak.strength * 100.0f)));
  return true;
}

int tempo_evidence_lag(int sr, int hop_length, float bpm_min) {
  return bpm_to_lag(0.5f * bpm_min, sr, hop_length);
}

BpmAnalyzer::BpmAnalyzer(const Audio& audio, const BpmConfig& config) : config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  const std::vector<float> onset = analysis_onset_strength(audio, config.n_fft, config.hop_length);

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

  const int evidence_lag =
      std::max(max_lag, std::min(tempo_evidence_lag(sr, hop_length, config_.bpm_min),
                                 static_cast<int>(onset_strength.size()) - 1));
  const std::vector<float> evidence_autocorr =
      compute_autocorrelation_local(onset_strength, evidence_lag);
  autocorr_.assign(evidence_autocorr.begin(), evidence_autocorr.begin() + max_lag);

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
  std::vector<double> tempo_hist_bpm_sum(tempo_hist.size(), 0.0);
  std::vector<BpmHistogramBin> octave_evidence;
  for_each_tempo_peak(
      evidence_autocorr.data(), static_cast<int>(evidence_autocorr.size()), sr, hop_length,
      config_.bpm_min, config_.bpm_max, [&](const TempoPeak& peak) {
        if (!peak.in_range) {
          octave_evidence.push_back({peak.bpm, peak.votes});
        } else if (!tempo_hist.empty()) {
          int bin_idx = static_cast<int>((peak.bpm - config_.bpm_min) / bpm_constants::kBinWidth);
          bin_idx = std::clamp(bin_idx, 0, static_cast<int>(tempo_hist.size()) - 1);
          tempo_hist[static_cast<size_t>(bin_idx)] += peak.votes;
          tempo_hist_bpm_sum[static_cast<size_t>(bin_idx)] +=
              static_cast<double>(peak.bpm) * peak.votes;
        }
      });

  // A bin reports the vote-weighted mean of the refined tempos in it, not its lower edge.
  const auto bin_bpm = [&](size_t i) {
    return static_cast<float>(tempo_hist_bpm_sum[i] / static_cast<double>(tempo_hist[i]));
  };
  std::vector<BpmHistogramBin> histogram;
  histogram.reserve(tempo_hist.size());
  for (size_t i = 0; i < tempo_hist.size(); ++i) {
    if (tempo_hist[i] > 0) {
      histogram.push_back({bin_bpm(i), tempo_hist[i]});
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
    auto [rep_bpm, conf_percent] = smart_choice(clusters, total_votes, octave_evidence);

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
    if (bpm_ < 80.0f && bpm_ > 0.0f && !top_bins.empty()) {
      const float strongest_peak = top_bins.front().bpm_center;
      // Only promote the strongest common-range bin when it is octave/harmonic
      // related to the detected tempo, matching the relationship gate used by the
      // middle-octave correction above. Without this check a genuinely slow tempo
      // (ballad/downtempo) whose strongest bin is unrelated would be blindly
      // doubled to that unrelated value.
      const float peak_ratio = strongest_peak / bpm_;
      const bool peak_octave_related = std::abs(peak_ratio - 0.5f) <= 0.04f ||
                                       std::abs(peak_ratio - 1.0f) <= 0.04f ||
                                       std::abs(peak_ratio - 2.0f) <= 0.08f;
      if (strongest_peak >= 80.0f && strongest_peak <= 180.0f && peak_octave_related) {
        bpm_ = strongest_peak;
      }
    }
    confidence_ = conf_percent / 100.0f;  ///< Convert percentage to [0, 1]

    /// Chosen tempo first with its own bin's vote share, then the other top bins by vote share.
    auto vote_share = [total_votes](int votes) {
      return total_votes > 0 ? static_cast<float>(votes) / static_cast<float>(total_votes) : 0.0f;
    };
    const int chosen_bin =
        std::clamp(static_cast<int>((bpm_ - config_.bpm_min) / bpm_constants::kBinWidth), 0,
                   static_cast<int>(tempo_hist.size()) - 1);
    const size_t chosen_index = static_cast<size_t>(chosen_bin);
    const bool chosen_has_votes = tempo_hist[chosen_index] > 0;
    candidates_.clear();
    candidates_.push_back({bpm_, vote_share(tempo_hist[chosen_index])});
    for (const auto& bin : top_bins) {
      if (chosen_has_votes && bin.bpm_center == bin_bpm(chosen_index)) continue;
      candidates_.push_back({bin.bpm_center, vote_share(bin.votes)});
    }
  }

  if (tempogram_.empty()) {
    tempogram_ = autocorr_;
  }
}

std::vector<BpmCandidate> BpmAnalyzer::candidates(int top_n) const {
  int n = std::clamp(top_n, 0, static_cast<int>(candidates_.size()));
  return std::vector<BpmCandidate>(candidates_.begin(), candidates_.begin() + n);
}

float detect_bpm(const Audio& audio, const BpmConfig& config) {
  BpmAnalyzer analyzer(audio, config);
  return analyzer.bpm();
}

}  // namespace sonare
