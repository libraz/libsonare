#include "streaming/stream_analyzer_utils.h"

#include <algorithm>
#include <cmath>

#include "core/fft.h"
#include "filters/mel.h"
#include "util/math_utils.h"

namespace sonare::streaming_detail {

std::vector<float> compute_bin_frequencies(int n_bins, int sr, int n_fft) {
  std::vector<float> freqs(n_bins);
  float bin_width = static_cast<float>(sr) / static_cast<float>(n_fft);
  for (int i = 0; i < n_bins; ++i) {
    freqs[i] = static_cast<float>(i) * bin_width;
  }
  return freqs;
}

float compute_centroid_frame(const float* magnitude, int n_bins, const float* frequencies) {
  float sum_weighted = 0.0f;
  float sum_mag = 0.0f;
  for (int k = 0; k < n_bins; ++k) {
    sum_weighted += frequencies[k] * magnitude[k];
    sum_mag += magnitude[k];
  }
  return sum_mag > kEpsilon ? sum_weighted / sum_mag : 0.0f;
}

float compute_flatness_frame(const float* magnitude, int n_bins) {
  float sum = 0.0f;
  float log_sum = 0.0f;
  int count = 0;

  for (int k = 0; k < n_bins; ++k) {
    float val = magnitude[k];
    if (val > kEpsilon) {
      sum += val;
      log_sum += std::log(val);
      ++count;
    }
  }

  if (count == 0 || sum < kEpsilon) {
    return 0.0f;
  }

  float arithmetic_mean = sum / static_cast<float>(count);
  float geometric_mean = std::exp(log_sum / static_cast<float>(count));
  return geometric_mean / arithmetic_mean;
}

float compute_rms_frame(const float* samples, int n_fft) {
  float sum_sq = 0.0f;
  for (int i = 0; i < n_fft; ++i) {
    sum_sq += samples[i] * samples[i];
  }
  return std::sqrt(sum_sq / static_cast<float>(n_fft));
}

float lag_to_bpm(int lag, int sr, int hop_length) {
  if (lag <= 0) return 0.0f;
  float seconds_per_beat = static_cast<float>(lag * hop_length) / static_cast<float>(sr);
  return 60.0f / seconds_per_beat;
}

int bpm_to_lag(float bpm, int sr, int hop_length) {
  if (bpm <= 0.0f) return 0;
  float seconds_per_beat = 60.0f / bpm;
  return static_cast<int>(seconds_per_beat * static_cast<float>(sr) /
                          static_cast<float>(hop_length));
}

std::vector<float> compute_autocorrelation_streaming(const std::vector<float>& signal,
                                                     int max_lag) {
  std::vector<float> autocorr(max_lag, 0.0f);
  compute_autocorrelation(signal.data(), static_cast<int>(signal.size()), max_lag, autocorr.data());
  return autocorr;
}

std::pair<float, float> find_best_tempo(const std::vector<float>& autocorr, int sr, int hop_length,
                                        float bpm_min, float bpm_max) {
  int lag_min = bpm_to_lag(bpm_max, sr, hop_length);
  int lag_max = bpm_to_lag(bpm_min, sr, hop_length);

  lag_min = std::max(1, lag_min);
  lag_max = std::min(static_cast<int>(autocorr.size()) - 1, lag_max);

  if (lag_min >= lag_max) {
    return {120.0f, 0.0f};
  }

  std::vector<std::pair<float, float>> candidates;

  for (int lag = lag_min + 1; lag < lag_max - 1; ++lag) {
    if (autocorr[lag] > autocorr[lag - 1] && autocorr[lag] > autocorr[lag + 1] &&
        autocorr[lag] > 0.0f) {
      float bpm = lag_to_bpm(lag, sr, hop_length);
      if (bpm >= bpm_min && bpm <= bpm_max) {
        candidates.emplace_back(bpm, autocorr[lag]);
      }
    }
  }

  if (candidates.empty()) {
    return {120.0f, 0.0f};
  }

  float max_weight = 0.0f;
  for (const auto& candidate : candidates) {
    max_weight = std::max(max_weight, candidate.second);
  }

  constexpr float kCommonBpmMin = 80.0f;
  constexpr float kCommonBpmMax = 180.0f;
  constexpr float kWeightThreshold = 0.3f;

  float best_bpm = 0.0f;
  float best_weight = 0.0f;
  for (const auto& [bpm, weight] : candidates) {
    if (bpm >= kCommonBpmMin && bpm <= kCommonBpmMax && weight >= kWeightThreshold * max_weight) {
      if (weight > best_weight) {
        best_bpm = bpm;
        best_weight = weight;
      }
    }
  }

  if (best_bpm == 0.0f) {
    for (const auto& [bpm, weight] : candidates) {
      if (weight > best_weight) {
        best_bpm = bpm;
        best_weight = weight;
      }
    }
  }

  float confidence = (max_weight > 0.0f) ? best_weight / max_weight : 0.0f;
  return {best_bpm, confidence};
}

uint8_t quantize_to_u8(float value, float min_val, float max_val) {
  float normalized = (value - min_val) / (max_val - min_val);
  normalized = std::max(0.0f, std::min(1.0f, normalized));
  return static_cast<uint8_t>(normalized * 255.0f + 0.5f);
}

int16_t quantize_to_i16(float value, float min_val, float max_val) {
  float normalized = (value - min_val) / (max_val - min_val);
  normalized = std::max(0.0f, std::min(1.0f, normalized));
  return static_cast<int16_t>(normalized * 65535.0f - 32768.0f + 0.5f);
}

float single_power_to_db(float power_val, float ref, float amin) {
  float result;
  power_to_db(&power_val, 1, ref, amin, -1.0f, &result);
  return result;
}

int count_shared_notes(int root1, int quality1, int root2, int quality2) {
  auto get_notes = [](int root, int quality) -> std::array<int, 3> {
    int third = (quality == 1) ? 3 : 4;
    return {{root % 12, (root + third) % 12, (root + 7) % 12}};
  };

  auto notes1 = get_notes(root1, quality1);
  auto notes2 = get_notes(root2, quality2);

  int shared = 0;
  for (int n1 : notes1) {
    for (int n2 : notes2) {
      if (n1 == n2) {
        ++shared;
        break;
      }
    }
  }
  return shared;
}

bool are_chords_confusable(int root1, int quality1, int root2, int quality2) {
  return count_shared_notes(root1, quality1, root2, quality2) >= 2;
}

std::array<float, 12> compute_median_chroma(
    const std::deque<std::array<float, 12>>& history) {
  std::array<float, 12> result = {};
  if (history.empty()) {
    return result;
  }

  size_t n_frames = history.size();
  std::vector<float> values(n_frames);

  for (int c = 0; c < 12; ++c) {
    for (size_t f = 0; f < n_frames; ++f) {
      values[f] = history[f][c];
    }

    std::sort(values.begin(), values.end());

    if (n_frames % 2 == 0) {
      result[c] = (values[n_frames / 2 - 1] + values[n_frames / 2]) * 0.5f;
    } else {
      result[c] = values[n_frames / 2];
    }
  }

  return result;
}

}  // namespace sonare::streaming_detail
