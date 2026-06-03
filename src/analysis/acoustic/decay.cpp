#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "analysis/acoustic/internal.h"
#include "util/db.h"

namespace sonare::acoustic_detail {

std::vector<DecayRegion> detect_free_decay_regions(const FrameEnergy& frames, float min_decay_db,
                                                   float noise_floor_margin_db) {
  std::vector<DecayRegion> regions;
  if (frames.db.size() < 8) {
    return regions;
  }

  const float required_drop = std::clamp(min_decay_db, 10.0f, 30.0f);
  const float noise_floor = estimate_noise_floor_db(frames);
  const float minimum_db =
      std::isfinite(noise_floor) ? noise_floor + std::max(0.0f, noise_floor_margin_db) : -80.0f;
  const size_t min_frames = 7;
  const size_t max_gap = 2;

  for (size_t first = 0; first + min_frames < frames.db.size(); ++first) {
    if (!std::isfinite(frames.db[first]) || frames.db[first] < -3.0f ||
        frames.db[first] <= minimum_db) {
      continue;
    }
    if (first > 0 && frames.db[first] < frames.db[first - 1]) {
      continue;
    }

    size_t last = first + 1;
    size_t rising_gap = 0;
    float peak = frames.db[first];
    for (; last < frames.db.size(); ++last) {
      const float current = frames.db[last];
      if (!std::isfinite(current) || current <= minimum_db) {
        break;
      }
      peak = std::max(peak, current);
      if (current > frames.db[last - 1] + 0.75f) {
        ++rising_gap;
        if (rising_gap > max_gap) {
          break;
        }
      } else {
        rising_gap = 0;
      }
      if (peak - current >= required_drop) {
        ++last;
        break;
      }
    }

    if (last > first + min_frames && frames.db[first] - frames.db[last - 1] >= required_drop) {
      regions.push_back({first, last});
      first = last > 0 ? last - 1 : first;
    }
  }

  if (regions.empty()) {
    regions.push_back({0, frames.db.size()});
  }
  return regions;
}

std::vector<SampleDecayRegion> detect_lollmann_subframe_regions(const float* samples, size_t size,
                                                                int sample_rate) {
  std::vector<SampleDecayRegion> regions;
  if (samples == nullptr || size == 0 || sample_rate <= 0) {
    return regions;
  }

  const size_t frame_size =
      std::max<size_t>(64, static_cast<size_t>(std::round(4923.0 * sample_rate / 16000.0)));
  const size_t hop_size =
      std::max<size_t>(1, static_cast<size_t>(std::round(137.0 * sample_rate / 16000.0)));
  const size_t subframe_size =
      std::max<size_t>(16, static_cast<size_t>(std::round(547.0 * sample_rate / 16000.0)));
  const size_t min_consecutive = 2;
  if (size < frame_size || frame_size < subframe_size * 3) {
    return regions;
  }

  for (size_t frame_start = 0; frame_start + frame_size <= size; frame_start += hop_size) {
    const size_t subframes = frame_size / subframe_size;
    size_t run_start = 0;
    size_t run_length = 0;

    for (size_t l = 0; l + 1 < subframes; ++l) {
      const size_t first = frame_start + l * subframe_size;
      const size_t second = first + subframe_size;
      double energy_a = 0.0;
      double energy_b = 0.0;
      float peak_a = 0.0f;
      float peak_b = 0.0f;
      for (size_t i = 0; i < subframe_size; ++i) {
        const float a = samples[first + i];
        const float b = samples[second + i];
        energy_a += static_cast<double>(a) * a;
        energy_b += static_cast<double>(b) * b;
        peak_a = std::max(peak_a, std::abs(a));
        peak_b = std::max(peak_b, std::abs(b));
      }

      const bool decays = energy_a > energy_b && peak_a > peak_b;
      if (decays) {
        if (run_length == 0) {
          run_start = l;
        }
        ++run_length;
      } else {
        if (run_length >= min_consecutive) {
          const size_t start = frame_start + run_start * subframe_size;
          const size_t end =
              std::min(size, frame_start + (run_start + run_length + 1) * subframe_size);
          regions.push_back({start, end});
        }
        run_length = 0;
      }
    }

    if (run_length >= min_consecutive) {
      const size_t start = frame_start + run_start * subframe_size;
      const size_t end = std::min(size, frame_start + (run_start + run_length + 1) * subframe_size);
      regions.push_back({start, end});
    }
  }

  std::sort(regions.begin(), regions.end(),
            [](const SampleDecayRegion& a, const SampleDecayRegion& b) {
              return a.first < b.first || (a.first == b.first && a.last < b.last);
            });
  std::vector<SampleDecayRegion> merged;
  for (const auto& region : regions) {
    if (region.last <= region.first) {
      continue;
    }
    if (!merged.empty() && region.first <= merged.back().last) {
      merged.back().last = std::max(merged.back().last, region.last);
    } else {
      merged.push_back(region);
    }
  }
  return merged;
}

std::vector<DecayRegion> map_sample_regions_to_frames(const std::vector<SampleDecayRegion>& regions,
                                                      const FrameEnergy& frames, int sample_rate) {
  std::vector<DecayRegion> mapped;
  if (regions.empty() || frames.times.empty() || sample_rate <= 0) {
    return mapped;
  }

  for (const auto& region : regions) {
    const float start_time = static_cast<float>(region.first) / static_cast<float>(sample_rate);
    const float end_time = static_cast<float>(region.last) / static_cast<float>(sample_rate);
    auto first = std::lower_bound(frames.times.begin(), frames.times.end(), start_time);
    auto last = std::upper_bound(frames.times.begin(), frames.times.end(), end_time);
    const size_t first_index = static_cast<size_t>(std::distance(frames.times.begin(), first));
    const size_t last_index = static_cast<size_t>(std::distance(frames.times.begin(), last));
    if (last_index > first_index + 4) {
      mapped.push_back({first_index, last_index});
    }
  }
  std::sort(mapped.begin(), mapped.end(), [](const DecayRegion& a, const DecayRegion& b) {
    return a.first < b.first || (a.first == b.first && a.last < b.last);
  });
  return mapped;
}

std::optional<BlindRt60Estimate> estimate_exponential_decay_ml(const FrameEnergy& frames,
                                                               size_t first, size_t last,
                                                               float seed_rt60) {
  if (!std::isfinite(seed_rt60) || seed_rt60 <= 0.0f || last <= first + 4 ||
      last > frames.energy.size()) {
    return std::nullopt;
  }

  const size_t count = last - first;
  double mean_y = 0.0;
  double tail_floor = std::numeric_limits<double>::infinity();
  const size_t tail_start = first + (count * 3) / 4;
  for (size_t i = first; i < last; ++i) {
    mean_y += frames.energy[i];
    if (i >= tail_start) {
      tail_floor = std::min(tail_floor, static_cast<double>(frames.energy[i]));
    }
  }
  mean_y /= static_cast<double>(count);
  const double tail_noise = std::isfinite(tail_floor) ? tail_floor : 0.0;

  double ss_tot = 0.0;
  for (size_t i = first; i < last; ++i) {
    const double centered = frames.energy[i] - mean_y;
    ss_tot += centered * centered;
  }
  if (ss_tot <= 1e-20) {
    return std::nullopt;
  }

  float best_rt60 = seed_rt60;
  double best_noise = 0.0;
  double best_sse = std::numeric_limits<double>::infinity();
  const float min_rt60 = std::max(0.05f, seed_rt60 * 0.35f);
  const float max_rt60 = std::min(20.0f, seed_rt60 * 2.25f);
  const double noise_candidates[] = {0.0,
                                     tail_noise * 0.10,
                                     tail_noise * 0.25,
                                     tail_noise * 0.50,
                                     tail_noise * 0.75,
                                     tail_noise * 0.90};
  constexpr int kRt60GridSteps = 36;
  for (int step = 0; step <= kRt60GridSteps; ++step) {
    const float alpha = static_cast<float>(step) / static_cast<float>(kRt60GridSteps);
    const float rt60 = min_rt60 * std::pow(max_rt60 / min_rt60, alpha);
    const double lambda = std::log(1000000.0) / static_cast<double>(rt60);

    for (double noise : noise_candidates) {
      double numerator = 0.0;
      double denominator = 0.0;
      const double t0 = frames.times[first];
      for (size_t i = first; i < last; ++i) {
        const double basis = std::exp(-lambda * (frames.times[i] - t0));
        numerator += basis * std::max(0.0, static_cast<double>(frames.energy[i]) - noise);
        denominator += basis * basis;
      }
      if (denominator <= 1e-20) {
        continue;
      }

      const double amplitude = std::max(0.0, numerator / denominator);
      double sse = 0.0;
      for (size_t i = first; i < last; ++i) {
        const double basis = std::exp(-lambda * (frames.times[i] - t0));
        const double predicted = amplitude * basis + noise;
        const double residual = static_cast<double>(frames.energy[i]) - predicted;
        sse += residual * residual;
      }

      if (sse < best_sse) {
        best_sse = sse;
        best_rt60 = rt60;
        best_noise = noise;
      }
    }
  }

  const float r2 = static_cast<float>(std::clamp(1.0 - best_sse / ss_tot, 0.0, 1.0));
  if (r2 < 0.5f) {
    return std::nullopt;
  }

  BlindRt60Estimate estimate;
  estimate.rt60 = best_rt60;
  const double initial_energy =
      std::max(static_cast<double>(frames.energy[first]) - best_noise, 0.0);
  const double noise_separation =
      initial_energy > 1e-20 ? power_to_db_scalar(initial_energy / std::max(best_noise, 1e-20))
                             : 0.0;
  const float noise_score = static_cast<float>(std::clamp(noise_separation / 30.0, 0.0, 1.0));
  estimate.confidence = std::clamp(0.15f + 0.70f * r2 + 0.15f * noise_score, 0.0f, 1.0f);
  return estimate;
}

BlindRt60Estimate aggregate_decay_candidates(std::vector<DecayEstimateCandidate> candidates) {
  candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                  [](const DecayEstimateCandidate& candidate) {
                                    return !std::isfinite(candidate.estimate.rt60) ||
                                           candidate.estimate.rt60 <= 0.0f ||
                                           candidate.estimate.confidence <= 0.0f ||
                                           candidate.score <= 0.0f;
                                  }),
                   candidates.end());
  if (candidates.empty()) {
    return {};
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const DecayEstimateCandidate& a, const DecayEstimateCandidate& b) {
              return a.score > b.score;
            });

  constexpr float kHistogramBinSeconds = 0.10f;
  const int n_bins = static_cast<int>(std::ceil(20.0f / kHistogramBinSeconds)) + 1;
  std::vector<double> histogram(static_cast<size_t>(n_bins), 0.0);
  for (const auto& candidate : candidates) {
    const int bin =
        std::clamp(static_cast<int>(std::round(candidate.estimate.rt60 / kHistogramBinSeconds)), 0,
                   n_bins - 1);
    histogram[static_cast<size_t>(bin)] += candidate.score;
  }
  const int best_bin = static_cast<int>(
      std::distance(histogram.begin(), std::max_element(histogram.begin(), histogram.end())));
  const float histogram_reference = static_cast<float>(best_bin) * kHistogramBinSeconds;
  const float reference =
      histogram_reference > 0.0f ? histogram_reference : candidates.front().estimate.rt60;
  double weighted_sum = 0.0;
  double weight_sum = 0.0;
  float confidence_sum = 0.0f;
  int accepted = 0;
  for (const auto& candidate : candidates) {
    const float ratio = candidate.estimate.rt60 / reference;
    if (ratio < 0.65f || ratio > 1.55f) {
      continue;
    }
    const double weight =
        static_cast<double>(candidate.score) * std::max(0.001f, candidate.estimate.confidence);
    weighted_sum += static_cast<double>(candidate.estimate.rt60) * weight;
    weight_sum += weight;
    confidence_sum += candidate.estimate.confidence;
    ++accepted;
  }

  if (weight_sum <= 0.0 || accepted == 0) {
    return candidates.front().estimate;
  }

  BlindRt60Estimate result;
  result.rt60 = static_cast<float>(weighted_sum / weight_sum);
  const float diversity_bonus = std::min(0.12f, 0.03f * static_cast<float>(accepted - 1));
  result.confidence =
      std::clamp(confidence_sum / static_cast<float>(accepted) + diversity_bonus, 0.0f, 1.0f);
  result.energy = weight_sum;
  return result;
}

BlindRt60Estimate estimate_blind_rt60_from_decay(const float* samples, size_t size, int sample_rate,
                                                 float min_decay_db, float noise_floor_margin_db,
                                                 bool suppress_stationary_noise) {
  std::vector<float> denoised;
  const float* analysis_samples = samples;
  if (suppress_stationary_noise) {
    denoised = suppress_stationary_noise_spectral(samples, size, sample_rate);
    if (!denoised.empty()) {
      analysis_samples = denoised.data();
    }
  }
  const FrameEnergy frames = compute_frame_energy(analysis_samples, size, sample_rate);
  if (frames.db.size() < 8) {
    return {};
  }

  const float required_drop = std::clamp(min_decay_db, 10.0f, 30.0f);
  const size_t start_stride = frames.db.size() > 200 ? 8 : 4;
  const size_t last_stride = frames.db.size() > 200 ? 3 : 1;
  const size_t max_window_frames = static_cast<size_t>(std::round(12.0f / 0.01f));
  auto decay_regions = detect_free_decay_regions(frames, required_drop, noise_floor_margin_db);
  auto lollmann_regions = map_sample_regions_to_frames(
      detect_lollmann_subframe_regions(analysis_samples, size, sample_rate), frames, sample_rate);
  decay_regions.insert(decay_regions.end(), lollmann_regions.begin(), lollmann_regions.end());
  std::sort(decay_regions.begin(), decay_regions.end(),
            [](const DecayRegion& a, const DecayRegion& b) {
              return a.first < b.first || (a.first == b.first && a.last < b.last);
            });
  float best_score = -1.0f;
  BlindRt60Estimate best;
  std::vector<DecayEstimateCandidate> candidates;

  for (const DecayRegion& region : decay_regions) {
    const size_t region_last = std::min(region.last, frames.db.size());
    for (size_t first = region.first; first + 7 < region_last; first += start_stride) {
      if (frames.db[first] < -3.0f) {
        continue;
      }
      const size_t last_limit = std::min(region_last, first + max_window_frames);
      for (size_t last = first + 7; last <= last_limit; last += last_stride) {
        const float drop = frames.db[first] - frames.db[last - 1];
        if (drop < required_drop) {
          continue;
        }

        const auto fit = fit_line(frames.times, frames.db, first, last);
        if (!fit || fit->slope >= -1e-6f) {
          continue;
        }

        const float rt60 = -60.0f / fit->slope;
        if (!std::isfinite(rt60) || rt60 <= 0.05f || rt60 > 20.0f) {
          continue;
        }

        const float drop_score = std::min(drop / required_drop, 1.0f);
        float chosen_rt60 = rt60;
        float chosen_confidence = std::clamp(0.2f + 0.8f * fit->r2 * drop_score, 0.0f, 1.0f);
        bool chosen_from_ml = false;
        const float window_duration = frames.times[last - 1] - frames.times[first];
        const auto ml_estimate = estimate_exponential_decay_ml(frames, first, last, rt60);
        if (ml_estimate) {
          const float rt60_ratio = ml_estimate->rt60 / rt60;
          const bool close_to_regression = rt60_ratio >= 0.65f && rt60_ratio <= 1.55f;
          const bool enough_decay_window =
              window_duration >= std::max(0.08f, std::min(rt60, ml_estimate->rt60) / 8.0f);
          if (close_to_regression && enough_decay_window) {
            chosen_rt60 = ml_estimate->rt60;
            chosen_confidence =
                std::clamp(ml_estimate->confidence * (0.70f + 0.30f * drop_score), 0.0f, 1.0f);
            chosen_from_ml = true;
          }
        }

        const float duration_score =
            std::clamp(window_duration / std::max(0.1f, chosen_rt60 / 6.0f), 0.0f, 1.0f);
        chosen_confidence *= duration_score;
        const float score = chosen_confidence + (chosen_from_ml ? 0.03f : 0.0f) +
                            0.001f * std::min(chosen_rt60, 5.0f);
        if (score > best_score) {
          best_score = score;
          best.rt60 = chosen_rt60;
          best.confidence = std::clamp(chosen_confidence, 0.0f, 1.0f);
        }
        candidates.push_back(
            {BlindRt60Estimate{chosen_rt60, std::clamp(chosen_confidence, 0.0f, 1.0f), 0.0, 0.0f},
             score, chosen_from_ml});
      }
    }
  }

  if (!candidates.empty()) {
    best = aggregate_decay_candidates(std::move(candidates));
  }
  if (best.confidence < 0.5f) {
    return {};
  }
  return best;
}

}  // namespace sonare::acoustic_detail
