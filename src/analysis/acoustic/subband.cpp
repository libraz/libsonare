#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "analysis/acoustic/internal.h"
#include "util/db.h"

namespace sonare::acoustic_detail {

double mean_square_energy(const std::vector<float>& samples) {
  if (samples.empty()) {
    return 0.0;
  }
  double sum = 0.0;
  for (float sample : samples) {
    sum += static_cast<double>(sample) * sample;
  }
  return sum / static_cast<double>(samples.size());
}

std::vector<float> third_octave_centers(int count, int sample_rate) {
  std::vector<float> centers;
  centers.reserve(static_cast<size_t>(std::max(0, count)));
  const float nyquist = static_cast<float>(sample_rate) * 0.5f;
  float center = 125.0f;
  for (int i = 0; i < count; ++i) {
    if (center * std::pow(2.0f, 1.0f / 6.0f) >= nyquist) {
      break;
    }
    centers.push_back(center);
    center *= std::pow(2.0f, 1.0f / 3.0f);
  }
  return centers;
}

std::vector<BlindRt60Estimate> estimate_third_octave_rt60(const Audio& audio,
                                                          const AcousticConfig& config) {
  std::vector<BlindRt60Estimate> estimates;
  const auto centers = third_octave_centers(config.n_third_octave_subbands, audio.sample_rate());
  estimates.reserve(centers.size());

  for (float center_hz : centers) {
    std::vector<float> filtered = filter_third_octave_band(audio, center_hz);
    if (filtered.empty()) {
      BlindRt60Estimate empty;
      empty.center_hz = center_hz;
      estimates.push_back(empty);
      continue;
    }

    const double energy = mean_square_energy(filtered);
    if (energy < 1e-12) {
      BlindRt60Estimate empty;
      empty.center_hz = center_hz;
      estimates.push_back(empty);
      continue;
    }

    BlindRt60Estimate estimate =
        estimate_blind_rt60_from_decay(filtered.data(), filtered.size(), audio.sample_rate(),
                                       config.min_decay_db, config.noise_floor_margin_db, false);
    estimate.center_hz = center_hz;
    estimate.energy = energy;
    estimates.push_back(estimate);
  }

  return estimates;
}

BlindRt60Estimate weighted_subband_average(const std::vector<BlindRt60Estimate>& estimates,
                                           float min_hz, float max_hz) {
  std::vector<float> finite_rt60;
  finite_rt60.reserve(estimates.size());
  for (const auto& estimate : estimates) {
    if (std::isfinite(estimate.rt60) && estimate.confidence > 0.0f &&
        estimate.center_hz >= min_hz && estimate.center_hz <= max_hz) {
      finite_rt60.push_back(estimate.rt60);
    }
  }
  const float median_rt60 = percentile(finite_rt60, 0.5f);

  double weighted_sum = 0.0;
  double weight_sum = 0.0;
  float confidence_sum = 0.0f;
  int count = 0;

  for (const auto& estimate : estimates) {
    if (!std::isfinite(estimate.rt60) || estimate.confidence <= 0.0f ||
        estimate.center_hz < min_hz || estimate.center_hz > max_hz) {
      continue;
    }
    if (std::isfinite(median_rt60) && median_rt60 > 0.0f) {
      const float ratio = estimate.rt60 / median_rt60;
      if (ratio < 0.65f || ratio > 1.55f) {
        continue;
      }
    }
    const double weight = std::max(estimate.energy, static_cast<double>(kEnergyEpsilon)) *
                          static_cast<double>(estimate.confidence);
    weighted_sum += static_cast<double>(estimate.rt60) * weight;
    weight_sum += weight;
    confidence_sum += estimate.confidence;
    ++count;
  }

  BlindRt60Estimate result;
  if (weight_sum <= 0.0 || count == 0) {
    return result;
  }

  result.rt60 = static_cast<float>(weighted_sum / weight_sum);
  result.confidence = std::clamp(confidence_sum / static_cast<float>(count), 0.0f, 1.0f);
  result.energy = weight_sum;
  result.center_hz = 0.5f * (min_hz + max_hz);
  return result;
}

float frequency_model_value(float subband_index, float m0, float b) {
  static constexpr float kAlpha = 7.5f;
  if (subband_index <= 0.0f || b <= 0.0f) {
    return nan_value();
  }
  // Lollmann 2015 Eq. 19-22: fit a scaled Rayleigh-like frequency dependency
  // to more reliable upper subbands, then use it when low subband ML estimates fail.
  const float b2 = b * b;
  const float alpha2 = kAlpha * kAlpha;
  const float shaped = subband_index / (kAlpha * b2) *
                       std::exp(-(subband_index * subband_index) / (2.0f * alpha2 * b2));
  return m0 + shaped;
}

FrequencyRtModel fit_frequency_dependent_rt_model(const std::vector<BlindRt60Estimate>& estimates,
                                                  float min_fit_hz, float max_fit_hz) {
  FrequencyRtModel model;
  if (estimates.empty()) {
    return model;
  }
  model.centers.reserve(estimates.size());
  for (const auto& estimate : estimates) {
    model.centers.push_back(estimate.center_hz);
  }

  std::vector<size_t> fit_indices;
  fit_indices.reserve(estimates.size());
  double rt_sum = 0.0;
  float confidence_sum = 0.0f;
  for (size_t i = 0; i < estimates.size(); ++i) {
    const auto& estimate = estimates[i];
    if (std::isfinite(estimate.rt60) && estimate.rt60 > 0.0f && estimate.confidence > 0.0f &&
        estimate.center_hz >= min_fit_hz && estimate.center_hz <= max_fit_hz) {
      fit_indices.push_back(i);
      rt_sum += estimate.rt60;
      confidence_sum += estimate.confidence;
      model.energy += std::max(estimate.energy, static_cast<double>(kEnergyEpsilon));
    }
  }
  if (fit_indices.size() < 3) {
    return model;
  }

  model.m0 = static_cast<float>(rt_sum / static_cast<double>(fit_indices.size()));
  double best_error = std::numeric_limits<double>::infinity();
  float best_b = 0.0f;
  for (int step = 0; step <= 9; ++step) {
    const float b = 0.5f + 0.5f * static_cast<float>(step);
    double weighted_error = 0.0;
    double weight_sum = 0.0;
    for (size_t index : fit_indices) {
      const auto& estimate = estimates[index];
      const float predicted = frequency_model_value(static_cast<float>(index + 1), model.m0, b);
      if (!std::isfinite(predicted)) {
        continue;
      }
      const double weight = std::max(estimate.energy, static_cast<double>(kEnergyEpsilon)) *
                            std::max(0.001f, estimate.confidence);
      const double error = static_cast<double>(predicted) - estimate.rt60;
      weighted_error += weight * error * error;
      weight_sum += weight;
    }
    if (weight_sum > 0.0) {
      weighted_error /= weight_sum;
    }
    if (weighted_error < best_error) {
      best_error = weighted_error;
      best_b = b;
    }
  }

  if (!std::isfinite(best_error) || best_b <= 0.0f) {
    return model;
  }
  model.valid = true;
  model.b = best_b;
  model.confidence =
      std::clamp(confidence_sum / static_cast<float>(fit_indices.size()), 0.0f, 1.0f);
  return model;
}

BlindRt60Estimate estimate_from_frequency_model(const FrequencyRtModel& model, float center_hz) {
  BlindRt60Estimate result;
  if (!model.valid || model.centers.empty() || center_hz <= 0.0f) {
    return result;
  }

  size_t nearest = 0;
  float best_distance = std::numeric_limits<float>::infinity();
  for (size_t i = 0; i < model.centers.size(); ++i) {
    const float distance = std::abs(std::log2(std::max(model.centers[i], 1.0f) / center_hz));
    if (distance < best_distance) {
      best_distance = distance;
      nearest = i;
    }
  }

  const float rt60 = frequency_model_value(static_cast<float>(nearest + 1), model.m0, model.b);
  if (!std::isfinite(rt60) || rt60 <= 0.0f) {
    return result;
  }
  result.rt60 = rt60;
  result.confidence = std::clamp(model.confidence * 0.75f, 0.0f, 1.0f);
  result.energy = model.energy;
  result.center_hz = center_hz;
  return result;
}

BlindRt60Estimate extrapolate_low_frequency_rt60(const BlindRt60Estimate& high_band,
                                                 float center_hz) {
  BlindRt60Estimate result;
  if (!std::isfinite(high_band.rt60) || high_band.confidence <= 0.0f || center_hz <= 0.0f) {
    return result;
  }

  const float clamped_hz = std::clamp(center_hz, 125.0f, 1000.0f);
  const float octave_distance = std::log2(1000.0f / clamped_hz);
  const float multiplier = std::clamp(1.0f + 0.06f * octave_distance, 1.0f, 1.18f);
  result.rt60 = high_band.rt60 * multiplier;
  result.confidence = std::clamp(high_band.confidence * 0.65f, 0.0f, 1.0f);
  result.energy = high_band.energy;
  result.center_hz = center_hz;
  return result;
}

bool looks_like_impulse_response(const Audio& audio) {
  if (audio.empty() || audio.sample_rate() <= 0 ||
      audio.size() < static_cast<size_t>(audio.sample_rate() / 5)) {
    return false;
  }

  const size_t first_window =
      std::min(audio.size(), static_cast<size_t>(std::round(0.02f * audio.sample_rate())));
  const size_t late_start =
      std::min(audio.size(), static_cast<size_t>(std::round(0.10f * audio.sample_rate())));
  const size_t tail_start = audio.size() * 2 / 3;

  size_t peak_index = 0;
  float peak = 0.0f;
  for (size_t i = 0; i < audio.size(); ++i) {
    const float value = std::abs(audio.data()[i]);
    if (value > peak) {
      peak = value;
      peak_index = i;
    }
  }
  if (peak <= 1e-6f || peak_index >= first_window) {
    return false;
  }

  float later_peak = 0.0f;
  for (size_t i = late_start; i < audio.size(); ++i) {
    later_peak = std::max(later_peak, std::abs(audio.data()[i]));
  }
  if (later_peak > peak * 0.80f) {
    return false;
  }

  auto rms_range = [&audio](size_t first, size_t last) {
    first = std::min(first, audio.size());
    last = std::min(last, audio.size());
    if (first >= last) {
      return 0.0;
    }
    double sum = 0.0;
    for (size_t i = first; i < last; ++i) {
      const double sample = audio.data()[i];
      sum += sample * sample;
    }
    return std::sqrt(sum / static_cast<double>(last - first));
  };

  const double early_rms = rms_range(peak_index, std::min(audio.size(), peak_index + first_window));
  const double tail_rms = rms_range(tail_start, audio.size());
  if (early_rms <= 1e-12) {
    return false;
  }
  return linear_to_db(std::max(early_rms, 1e-12) / std::max(tail_rms, 1e-12)) >= 18.0;
}

}  // namespace sonare::acoustic_detail
