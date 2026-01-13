#include "analysis/dynamics_analyzer.h"

#include <algorithm>
#include <cmath>

#include "util/exception.h"

namespace sonare {

DynamicsAnalyzer::DynamicsAnalyzer(const Audio& audio, const DynamicsConfig& config)
    : config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  analyze(audio);
}

void DynamicsAnalyzer::analyze(const Audio& audio) {
  const float* data = audio.data();
  size_t n_samples = audio.size();
  int sr = audio.sample_rate();

  // Compute peak
  float peak = 0.0f;
  for (size_t i = 0; i < n_samples; ++i) {
    peak = std::max(peak, std::abs(data[i]));
  }

  // Compute overall RMS
  double sum_sq = 0.0;
  for (size_t i = 0; i < n_samples; ++i) {
    sum_sq += static_cast<double>(data[i]) * static_cast<double>(data[i]);
  }
  float rms = static_cast<float>(std::sqrt(sum_sq / static_cast<double>(n_samples)));

  // Convert to dB
  float eps = 1e-10f;
  dynamics_.peak_db = 20.0f * std::log10(std::max(peak, eps));
  dynamics_.rms_db = 20.0f * std::log10(std::max(rms, eps));
  dynamics_.crest_factor = dynamics_.peak_db - dynamics_.rms_db;

  // Compute loudness curve
  int window_samples = static_cast<int>(config_.window_sec * sr);
  int hop_length = config_.hop_length;

  loudness_curve_.times.clear();
  loudness_curve_.rms_db.clear();

  std::vector<float> rms_values;

  for (size_t pos = 0; pos + window_samples <= n_samples; pos += hop_length) {
    // Compute RMS for this window
    double win_sum_sq = 0.0;
    for (int i = 0; i < window_samples; ++i) {
      float sample = data[pos + i];
      win_sum_sq += static_cast<double>(sample) * static_cast<double>(sample);
    }
    float win_rms = static_cast<float>(std::sqrt(win_sum_sq / static_cast<double>(window_samples)));
    float win_rms_db = 20.0f * std::log10(std::max(win_rms, eps));

    float time = static_cast<float>(pos + window_samples / 2) / static_cast<float>(sr);
    loudness_curve_.times.push_back(time);
    loudness_curve_.rms_db.push_back(win_rms_db);
    rms_values.push_back(win_rms_db);
  }

  // Compute dynamic range and loudness range
  if (!rms_values.empty()) {
    // Sort for percentile calculation
    std::vector<float> sorted_rms = rms_values;
    std::sort(sorted_rms.begin(), sorted_rms.end());

    // Dynamic range: difference between 95th and 10th percentile
    size_t p10_idx = sorted_rms.size() / 10;
    size_t p95_idx = sorted_rms.size() * 95 / 100;

    float p10 = sorted_rms[p10_idx];
    float p95 = sorted_rms[p95_idx];

    dynamics_.dynamic_range_db = p95 - p10;

    // Loudness range (simplified LRA): difference between 95th and 10th percentile
    // of short-term loudness
    dynamics_.loudness_range_db = dynamics_.dynamic_range_db;
  } else {
    dynamics_.dynamic_range_db = 0.0f;
    dynamics_.loudness_range_db = 0.0f;
  }

  // Determine if compressed
  // Low dynamic range and low crest factor suggest compression
  dynamics_.is_compressed = (dynamics_.dynamic_range_db < config_.compression_threshold) ||
                            (dynamics_.crest_factor < 8.0f);
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
