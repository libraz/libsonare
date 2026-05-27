#include "analysis/dynamics_analyzer.h"

#include <algorithm>
#include <cmath>

#include "metering/lufs.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

using constants::kEpsilon;

namespace {
/// @brief Crest factor (dB) below which audio is considered heavily compressed.
/// @details Uncompressed program material typically exhibits a crest factor well above
///          ~8 dB; aggressive limiting/compression collapses peaks toward the RMS level.
constexpr float kCompressedCrestFactorDb = 8.0f;

float percentile_sorted(const std::vector<float>& sorted, float percentile) {
  if (sorted.empty()) return 0.0f;
  const float position = std::clamp(percentile, 0.0f, 1.0f) * static_cast<float>(sorted.size() - 1);
  const size_t lo = static_cast<size_t>(position);
  const size_t hi = std::min(lo + 1, sorted.size() - 1);
  const float frac = position - static_cast<float>(lo);
  return sorted[lo] * (1.0f - frac) + sorted[hi] * frac;
}
}  // namespace

DynamicsAnalyzer::DynamicsAnalyzer(const Audio& audio, const DynamicsConfig& config)
    : config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  analyze(audio);
}

void DynamicsAnalyzer::analyze(const Audio& audio) {
  const float* data = audio.data();
  size_t n_samples = audio.size();
  int sr = audio.sample_rate();

  // Compute peak and overall sum of squares in single pass
  float peak = 0.0f;
  double sum_sq = 0.0;
  for (size_t i = 0; i < n_samples; ++i) {
    float sample = data[i];
    peak = std::max(peak, std::abs(sample));
    sum_sq += static_cast<double>(sample) * static_cast<double>(sample);
  }
  float rms = static_cast<float>(std::sqrt(sum_sq / static_cast<double>(n_samples)));

  // Convert to dB
  constexpr float eps = kEpsilon;
  dynamics_.peak_db = 20.0f * std::log10(std::max(peak, eps));
  dynamics_.rms_db = 20.0f * std::log10(std::max(rms, eps));
  dynamics_.crest_factor = dynamics_.peak_db - dynamics_.rms_db;

  // Compute loudness curve using prefix sum for O(n) complexity
  int window_samples = static_cast<int>(config_.window_sec * sr);
  int hop_length = config_.hop_length;

  // Build prefix sum of squared samples: prefix_sum[i] = sum(data[0..i-1]^2)
  std::vector<double> prefix_sum(n_samples + 1);
  prefix_sum[0] = 0.0;
  for (size_t i = 0; i < n_samples; ++i) {
    prefix_sum[i + 1] = prefix_sum[i] + static_cast<double>(data[i]) * static_cast<double>(data[i]);
  }

  loudness_curve_.times.clear();
  loudness_curve_.rms_db.clear();

  // Estimate number of windows and reserve space
  size_t n_windows = (n_samples >= static_cast<size_t>(window_samples))
                         ? (n_samples - window_samples) / hop_length + 1
                         : 0;
  loudness_curve_.times.reserve(n_windows);
  loudness_curve_.rms_db.reserve(n_windows);

  std::vector<float> rms_values;
  rms_values.reserve(n_windows);

  for (size_t pos = 0; pos + window_samples <= n_samples; pos += hop_length) {
    // Compute RMS for this window using prefix sum: O(1) per window
    double win_sum_sq = prefix_sum[pos + window_samples] - prefix_sum[pos];
    float win_rms = static_cast<float>(std::sqrt(win_sum_sq / static_cast<double>(window_samples)));
    float win_rms_db = 20.0f * std::log10(std::max(win_rms, eps));

    const double center_sample = static_cast<double>(pos) + 0.5 * window_samples;
    float time = static_cast<float>(center_sample / static_cast<double>(sr));
    loudness_curve_.times.push_back(time);
    loudness_curve_.rms_db.push_back(win_rms_db);
    rms_values.push_back(win_rms_db);
  }

  // Compute dynamic range and loudness range
  if (!rms_values.empty()) {
    // Sort for percentile calculation
    std::vector<float> sorted_rms = rms_values;
    std::sort(sorted_rms.begin(), sorted_rms.end());

    // Dynamic range: difference between interpolated 95th and 10th percentiles.
    const float p10 = percentile_sorted(sorted_rms, 0.10f);
    const float p95 = percentile_sorted(sorted_rms, 0.95f);

    dynamics_.dynamic_range_db = p95 - p10;
  } else {
    dynamics_.dynamic_range_db = 0.0f;
  }

  // Loudness range (LRA): full EBU R128 / EBU Tech 3342 algorithm using K-weighted
  // short-term loudness (3 s windows, 100 ms hops, absolute + relative gating).
  dynamics_.loudness_range_db = metering::ebur128_loudness_range(audio);

  // Determine if compressed
  // Low dynamic range and low crest factor suggest compression
  dynamics_.is_compressed = (dynamics_.dynamic_range_db < config_.compression_threshold) ||
                            (dynamics_.crest_factor < kCompressedCrestFactorDb);
}

std::vector<int> DynamicsAnalyzer::loudness_histogram(int n_bins, float min_db,
                                                      float max_db) const {
  std::vector<int> histogram(n_bins, 0);

  float bin_width = (max_db - min_db) / static_cast<float>(n_bins);

  for (float rms : loudness_curve_.rms_db) {
    if (rms < min_db || rms > max_db) continue;

    int bin = static_cast<int>((rms - min_db) / bin_width);
    bin = std::min(bin, n_bins - 1);
    histogram[bin]++;
  }

  return histogram;
}

}  // namespace sonare
