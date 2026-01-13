#include "analysis/melody_analyzer.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "util/exception.h"

namespace sonare {

MelodyAnalyzer::MelodyAnalyzer(const Audio& audio, const MelodyConfig& config)
    : config_(config), sr_(audio.sample_rate()) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  analyze();

  // Process audio frame by frame
  const float* samples = audio.data();
  size_t n_samples = audio.size();

  int frame_size = config.frame_length;
  int hop = config.hop_length;

  for (size_t start = 0; start + frame_size <= n_samples; start += hop) {
    float time = static_cast<float>(start) / sr_;

    // Compute pitch using YIN
    float frequency = yin_pitch(samples + start, frame_size, sr_);

    PitchPoint point;
    point.time = time;
    point.frequency = frequency;

    // Compute confidence from YIN threshold
    if (frequency > 0.0f) {
      point.confidence = 1.0f - config.threshold;
    } else {
      point.confidence = 0.0f;
    }

    contour_.pitches.push_back(point);
  }

  compute_contour_features();
}

void MelodyAnalyzer::analyze() {
  // Initialize contour
  contour_.pitches.clear();
  contour_.pitch_range_octaves = 0.0f;
  contour_.pitch_stability = 0.0f;
  contour_.mean_frequency = 0.0f;
  contour_.vibrato_rate = 0.0f;
}

float MelodyAnalyzer::yin_pitch(const float* samples, int frame_size, int sr) const {
  // YIN algorithm simplified implementation
  // Based on: De Cheveigné, A., & Kawahara, H. (2002). YIN, a fundamental frequency estimator.

  int half_size = frame_size / 2;
  if (half_size < 2) {
    return 0.0f;
  }

  int tau_min = static_cast<int>(static_cast<float>(sr) / config_.fmax);
  int tau_max = static_cast<int>(static_cast<float>(sr) / config_.fmin);
  tau_min = std::max(1, tau_min);
  tau_max = std::min(tau_max, half_size);

  if (tau_min >= tau_max || tau_max <= 1) {
    return 0.0f;
  }

  // Compute difference function
  std::vector<float> diff(half_size, 0.0f);

  for (int tau = 0; tau < half_size; ++tau) {
    float sum = 0.0f;
    int limit = half_size;
    for (int j = 0; j < limit; ++j) {
      float delta = samples[j] - samples[j + tau];
      sum += delta * delta;
    }
    diff[tau] = sum;
  }

  // Cumulative mean normalized difference function
  std::vector<float> cmndf(half_size, 1.0f);
  float running_sum = 0.0f;

  for (int tau = 1; tau < half_size; ++tau) {
    running_sum += diff[tau];
    if (running_sum > 1e-10f) {
      cmndf[tau] = diff[tau] * static_cast<float>(tau) / running_sum;
    }
  }

  // Find threshold crossing
  int tau = tau_min;
  while (tau < tau_max - 1) {
    if (cmndf[tau] < config_.threshold) {
      // Find the local minimum
      while (tau + 1 < tau_max && cmndf[tau + 1] < cmndf[tau]) {
        ++tau;
      }
      break;
    }
    ++tau;
  }

  if (tau >= tau_max - 1) {
    return 0.0f;  // No pitch found
  }

  // Parabolic interpolation for sub-sample accuracy
  float refined_tau = parabolic_interpolation(cmndf.data(), half_size, tau);

  if (refined_tau <= 0.0f) {
    return 0.0f;
  }

  float frequency = static_cast<float>(sr) / refined_tau;

  // Check if frequency is within range
  if (frequency < config_.fmin || frequency > config_.fmax) {
    return 0.0f;
  }

  return frequency;
}

void MelodyAnalyzer::compute_difference_function(const float* samples, int frame_size,
                                                 float* diff) const {
  int tau_max = frame_size / 2;

  for (int tau = 0; tau < tau_max; ++tau) {
    float sum = 0.0f;
    for (int j = 0; j < frame_size - tau; ++j) {
      float delta = samples[j] - samples[j + tau];
      sum += delta * delta;
    }
    diff[tau] = sum;
  }
}

void MelodyAnalyzer::cumulative_mean_normalize(float* diff, int size) const {
  diff[0] = 1.0f;
  float running_sum = 0.0f;

  for (int tau = 1; tau < size; ++tau) {
    running_sum += diff[tau];
    if (running_sum > 1e-10f) {
      diff[tau] = diff[tau] / (running_sum / tau);
    } else {
      diff[tau] = 1.0f;
    }
  }
}

int MelodyAnalyzer::find_threshold_crossing(const float* diff, int size) const {
  int tau_min = static_cast<int>(sr_ / config_.fmax);
  tau_min = std::max(1, tau_min);

  for (int tau = tau_min; tau < size - 1; ++tau) {
    if (diff[tau] < config_.threshold) {
      // Find local minimum
      while (tau + 1 < size && diff[tau + 1] < diff[tau]) {
        ++tau;
      }
      return tau;
    }
  }

  return -1;  // No pitch found
}

float MelodyAnalyzer::parabolic_interpolation(const float* diff, int size, int tau) const {
  if (tau <= 0 || tau >= size - 1) {
    return static_cast<float>(tau);
  }

  float alpha = diff[tau - 1];
  float beta = diff[tau];
  float gamma = diff[tau + 1];

  float peak = 0.5f * (alpha - gamma) / (alpha - 2.0f * beta + gamma);

  return static_cast<float>(tau) + peak;
}

void MelodyAnalyzer::compute_contour_features() {
  if (contour_.pitches.empty()) {
    return;
  }

  // Filter out unvoiced frames
  std::vector<float> voiced_frequencies;
  for (const auto& p : contour_.pitches) {
    if (p.frequency > 0.0f) {
      voiced_frequencies.push_back(p.frequency);
    }
  }

  if (voiced_frequencies.empty()) {
    return;
  }

  // Mean frequency
  float sum = 0.0f;
  for (float f : voiced_frequencies) {
    sum += f;
  }
  contour_.mean_frequency = sum / voiced_frequencies.size();

  // Pitch range (in octaves)
  float min_freq = *std::min_element(voiced_frequencies.begin(), voiced_frequencies.end());
  float max_freq = *std::max_element(voiced_frequencies.begin(), voiced_frequencies.end());

  if (min_freq > 0.0f) {
    contour_.pitch_range_octaves = std::log2(max_freq / min_freq);
  }

  // Pitch stability (based on pitch variance)
  float mean_log = 0.0f;
  for (float f : voiced_frequencies) {
    mean_log += std::log2(f);
  }
  mean_log /= voiced_frequencies.size();

  float var_log = 0.0f;
  for (float f : voiced_frequencies) {
    float diff = std::log2(f) - mean_log;
    var_log += diff * diff;
  }
  var_log /= voiced_frequencies.size();

  // Convert variance to stability (lower variance = higher stability)
  // Typical variance range is 0 to 1 octave^2
  contour_.pitch_stability = std::max(0.0f, 1.0f - std::sqrt(var_log));

  // Estimate vibrato rate (simplified)
  // Look for periodic variations in pitch
  if (voiced_frequencies.size() >= 8) {
    // Compute pitch differences
    std::vector<float> pitch_diff;
    for (size_t i = 1; i < voiced_frequencies.size(); ++i) {
      pitch_diff.push_back(voiced_frequencies[i] - voiced_frequencies[i - 1]);
    }

    // Count zero crossings in pitch difference (rough vibrato estimate)
    int zero_crossings = 0;
    for (size_t i = 1; i < pitch_diff.size(); ++i) {
      if ((pitch_diff[i] >= 0.0f && pitch_diff[i - 1] < 0.0f) ||
          (pitch_diff[i] < 0.0f && pitch_diff[i - 1] >= 0.0f)) {
        zero_crossings++;
      }
    }

    // Convert to vibrato rate (Hz)
    float duration = contour_.pitches.back().time - contour_.pitches.front().time;
    if (duration > 0.0f) {
      contour_.vibrato_rate = static_cast<float>(zero_crossings) / (2.0f * duration);
    }
  }
}

std::vector<float> MelodyAnalyzer::pitch_times() const {
  std::vector<float> times;
  times.reserve(contour_.pitches.size());
  for (const auto& p : contour_.pitches) {
    times.push_back(p.time);
  }
  return times;
}

std::vector<float> MelodyAnalyzer::pitch_frequencies() const {
  std::vector<float> frequencies;
  frequencies.reserve(contour_.pitches.size());
  for (const auto& p : contour_.pitches) {
    frequencies.push_back(p.frequency);
  }
  return frequencies;
}

std::vector<float> MelodyAnalyzer::pitch_confidences() const {
  std::vector<float> confidences;
  confidences.reserve(contour_.pitches.size());
  for (const auto& p : contour_.pitches) {
    confidences.push_back(p.confidence);
  }
  return confidences;
}

}  // namespace sonare
