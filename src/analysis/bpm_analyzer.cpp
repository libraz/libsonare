#include "analysis/bpm_analyzer.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>

#include "core/fft.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare {

std::vector<BpmHistogramBin> build_bpm_histogram(const std::vector<float>& candidates,
                                                  float bpm_min, float bpm_max, float bin_width) {
  if (candidates.empty() || bpm_min >= bpm_max || bin_width <= 0.0f) {
    return {};
  }

  // Calculate number of bins
  int n_bins = static_cast<int>((bpm_max - bpm_min) / bin_width) + 1;

  // Build histogram
  std::vector<int> hist(n_bins, 0);
  for (float bpm : candidates) {
    if (bpm >= bpm_min && bpm <= bpm_max) {
      int bin_idx = static_cast<int>((bpm - bpm_min) / bin_width);
      bin_idx = std::clamp(bin_idx, 0, n_bins - 1);
      hist[bin_idx]++;
    }
  }

  // Convert to BpmHistogramBin and sort by votes descending
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

  // Process bins in order of votes (descending, already sorted)
  for (const auto& bin : top_bins) {
    float bpm = bin.bpm_center;
    int votes = bin.votes;

    // Try to find an existing cluster this BPM belongs to
    bool found_cluster = false;
    for (auto& [base, members] : clusters) {
      float ratio = bpm / base;

      // Check if ratio matches any harmonic relationship
      for (float k : bpm_constants::kHarmonicRatios) {
        // Check both ratio == k and ratio == 1/k
        if (std::abs(ratio - k) < bpm_constants::kHarmonicTolerance ||
            std::abs(ratio - 1.0f / k) < bpm_constants::kHarmonicTolerance) {
          members.emplace_back(bpm, votes);
          found_cluster = true;
          break;
        }
      }
      if (found_cluster) break;
    }

    // Create new cluster if no match found
    if (!found_cluster) {
      clusters[bpm] = {{bpm, votes}};
    }
  }

  return clusters;
}

std::pair<float, float> smart_choice(const HarmonicClusterMap& clusters, int total_votes) {
  // Handle edge cases
  if (clusters.empty()) {
    return {120.0f, 0.0f};  // Default BPM with zero confidence
  }
  if (total_votes <= 0) {
    return {120.0f, 0.0f};
  }

  // Find the base cluster with largest total votes
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

  // Find higher BPM clusters and their votes
  std::vector<std::pair<float, int>> higher_clusters;
  for (const auto& [base, members] : clusters) {
    if (base > best_base) {
      int cluster_votes = 0;
      for (const auto& [bpm, v] : members) {
        cluster_votes += v;
      }
      higher_clusters.emplace_back(base, cluster_votes);
    }
  }

  // Sort higher clusters by votes descending
  std::sort(higher_clusters.begin(), higher_clusters.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  // Check if strongest higher cluster has enough votes (>= 15% of total)
  if (!higher_clusters.empty() &&
      static_cast<float>(higher_clusters[0].second) / static_cast<float>(total_votes) >=
          bpm_constants::kThreshHigher) {
    float rep_bpm = higher_clusters[0].first;
    float confidence = 100.0f * static_cast<float>(higher_clusters[0].second) /
                       static_cast<float>(total_votes);
    return {rep_bpm, confidence};
  }

  // Select from the base cluster
  const auto& base_members = clusters.at(best_base);
  if (base_members.empty()) {
    return {120.0f, 0.0f};
  }

  // Musical tempo preference: prefer BPMs in common range (80-180) over extremes
  // This helps avoid octave errors (e.g., detecting 60 instead of 120)
  constexpr float kCommonBpmMin = 80.0f;
  constexpr float kCommonBpmMax = 180.0f;
  constexpr float kAcceptableBpmMin = 60.0f;
  constexpr float kAcceptableBpmMax = 200.0f;

  // Categorize candidates
  std::vector<std::pair<float, int>> common_range;    // 80-180 BPM
  std::vector<std::pair<float, int>> acceptable_range; // 60-200 BPM
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

  // First priority: candidates in common range (80-180) with >= 30% of max votes
  if (!common_range.empty()) {
    for (const auto& [bpm, v] : common_range) {
      if (static_cast<float>(v) >= 0.3f * static_cast<float>(max_votes_in_cluster)) {
        // Among candidates meeting threshold, prefer higher BPM (avoid octave-down)
        if (rep_bpm == 0.0f || bpm > rep_bpm) {
          rep_bpm = bpm;
        }
      }
    }
  }

  // Second priority: candidates in acceptable range (60-200) with >= 50% of max votes
  if (rep_bpm == 0.0f && !acceptable_range.empty()) {
    for (const auto& [bpm, v] : acceptable_range) {
      if (static_cast<float>(v) >= 0.5f * static_cast<float>(max_votes_in_cluster)) {
        if (rep_bpm == 0.0f || bpm > rep_bpm) {
          rep_bpm = bpm;
        }
      }
    }
  }

  // Fall back to highest voted member if no preferred candidate found
  if (rep_bpm == 0.0f) {
    auto best_member =
        std::max_element(base_members.begin(), base_members.end(),
                         [](const auto& a, const auto& b) { return a.second < b.second; });
    rep_bpm = best_member->first;
  }

  float confidence =
      100.0f * static_cast<float>(best_base_votes) / static_cast<float>(total_votes);

  return {rep_bpm, confidence};
}

namespace {

/// @brief Computes autocorrelation of a signal using FFT (O(n log n)).
/// @details Uses the Wiener-Khinchin theorem: autocorr(x) = IFFT(|FFT(x)|^2)
///          Zero-pads to avoid circular correlation artifacts.
std::vector<float> compute_autocorrelation(const std::vector<float>& signal, int max_lag) {
  int n = static_cast<int>(signal.size());
  std::vector<float> autocorr(max_lag, 0.0f);

  if (n == 0 || max_lag <= 0) {
    return autocorr;
  }

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

  // Zero-pad to at least 2*n to avoid circular correlation artifacts
  int fft_size = next_power_of_2(2 * n);

  // Prepare zero-mean, zero-padded signal
  std::vector<float> padded(fft_size, 0.0f);
  for (int i = 0; i < n; ++i) {
    padded[i] = signal[i] - mean;
  }

  // FFT-based autocorrelation
  FFT fft(fft_size);
  int n_bins = fft.n_bins();

  std::vector<std::complex<float>> spectrum(n_bins);
  fft.forward(padded.data(), spectrum.data());

  // Compute power spectrum (|FFT(x)|^2)
  for (int i = 0; i < n_bins; ++i) {
    float re = spectrum[i].real();
    float im = spectrum[i].imag();
    spectrum[i] = std::complex<float>(re * re + im * im, 0.0f);
  }

  // Inverse FFT to get autocorrelation
  std::vector<float> raw_autocorr(fft_size);
  fft.inverse(spectrum.data(), raw_autocorr.data());

  // Normalize by variance and extract relevant lags
  // raw_autocorr[0] should equal var after normalization
  float norm_factor = var * static_cast<float>(n);
  for (int lag = 0; lag < max_lag && lag < n; ++lag) {
    autocorr[lag] = raw_autocorr[lag] / norm_factor;
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

  // Find all tempo peaks
  candidates_ = find_tempo_peaks(autocorr_, sr, hop_length, config_.bpm_min, config_.bpm_max);

  if (candidates_.empty()) {
    bpm_ = config_.start_bpm;
    confidence_ = 0.0f;
    tempogram_ = autocorr_;
    return;
  }

  // Use harmonic clustering for octave error handling

  // Generate tempo candidates from all local maxima, weighted by autocorrelation
  std::vector<float> tempo_candidates;
  int lag_min = bpm_to_lag(config_.bpm_max, sr, hop_length);
  int lag_max_inner = bpm_to_lag(config_.bpm_min, sr, hop_length);
  lag_min = std::max(1, lag_min);
  lag_max_inner = std::min(static_cast<int>(autocorr_.size()) - 1, lag_max_inner);

  for (int lag = lag_min + 1; lag < lag_max_inner - 1; ++lag) {
    if (autocorr_[lag] > autocorr_[lag - 1] && autocorr_[lag] > autocorr_[lag + 1] &&
        autocorr_[lag] > 0.0f) {
      float bpm = lag_to_bpm(lag, sr, hop_length);
      if (bpm >= config_.bpm_min && bpm <= config_.bpm_max) {
        // Add candidate multiple times based on autocorrelation strength
        int weight = std::max(1, static_cast<int>(autocorr_[lag] * 100.0f));
        for (int w = 0; w < weight; ++w) {
          tempo_candidates.push_back(bpm);
        }
      }
    }
  }

  if (tempo_candidates.empty()) {
    // Fall back to highest confidence candidate
    bpm_ = candidates_[0].bpm;
    confidence_ = candidates_[0].confidence;
  } else {
    // Build histogram with 0.5 BPM resolution
    auto histogram = build_bpm_histogram(tempo_candidates, config_.bpm_min, config_.bpm_max,
                                         bpm_constants::kBinWidth);

    // Take top bins for clustering
    std::vector<BpmHistogramBin> top_bins;
    int n_top = std::min(bpm_constants::kTopBins, static_cast<int>(histogram.size()));
    top_bins.assign(histogram.begin(), histogram.begin() + n_top);

    // Apply harmonic clustering
    auto clusters = harmonic_cluster(top_bins);

    // Calculate total votes
    int total_votes = 0;
    for (const auto& bin : histogram) {
      total_votes += bin.votes;
    }

    // Smart choice selection
    auto [rep_bpm, conf_percent] = smart_choice(clusters, total_votes);

    bpm_ = rep_bpm;
    confidence_ = conf_percent / 100.0f;  // Convert percentage to [0, 1]

    // Update candidates_ for API compatibility
    candidates_.clear();
    for (const auto& bin : top_bins) {
      float bin_conf =
          (total_votes > 0) ? static_cast<float>(bin.votes) / static_cast<float>(total_votes)
                            : 0.0f;
      candidates_.push_back({bin.bpm_center, bin_conf});
    }
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
