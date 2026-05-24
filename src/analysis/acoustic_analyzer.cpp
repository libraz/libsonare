#include "analysis/acoustic_analyzer.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <optional>
#include <vector>

#include "core/fft.h"
#include "core/window.h"
#include "filters/iir.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare {
namespace {

using sonare::constants::kSqrt2;

// Energy-domain floor (1e-20), intentionally smaller than the generic
// sonare::constants::kEpsilon (1e-10); used to guard log/division of energies.
constexpr float kEnergyEpsilon = 1e-20f;

float nan_value() { return std::numeric_limits<float>::quiet_NaN(); }

struct LinearFit {
  float slope = 0.0f;
  float r2 = 0.0f;
};

struct BlindRt60Estimate {
  float rt60 = nan_value();
  float confidence = 0.0f;
  double energy = 0.0;
  float center_hz = 0.0f;
};

struct DecayEstimateCandidate {
  BlindRt60Estimate estimate;
  float score = 0.0f;
  bool from_ml = false;
};

struct FrequencyRtModel {
  bool valid = false;
  float m0 = 0.0f;
  float b = 0.0f;
  float confidence = 0.0f;
  double energy = 0.0;
  std::vector<float> centers;
};

struct FrameEnergy {
  std::vector<float> times;
  std::vector<float> energy;
  std::vector<float> db;
};

struct DecayRegion {
  size_t first = 0;
  size_t last = 0;
};

struct SampleDecayRegion {
  size_t first = 0;
  size_t last = 0;
};

std::vector<double> squared_energy(const float* samples, size_t size) {
  std::vector<double> energy(size);
  for (size_t i = 0; i < size; ++i) {
    const double sample = static_cast<double>(samples[i]);
    energy[i] = sample * sample;
  }
  return energy;
}

std::optional<LinearFit> fit_line(const std::vector<float>& x, const std::vector<float>& y,
                                  size_t first, size_t last) {
  if (last <= first + 2 || last > x.size() || last > y.size()) {
    return std::nullopt;
  }

  double sum_x = 0.0;
  double sum_y = 0.0;
  double sum_xx = 0.0;
  double sum_xy = 0.0;
  const size_t count = last - first;
  for (size_t i = first; i < last; ++i) {
    sum_x += x[i];
    sum_y += y[i];
    sum_xx += static_cast<double>(x[i]) * x[i];
    sum_xy += static_cast<double>(x[i]) * y[i];
  }

  const double n = static_cast<double>(count);
  const double denominator = n * sum_xx - sum_x * sum_x;
  if (std::abs(denominator) < 1e-12) {
    return std::nullopt;
  }

  const double slope = (n * sum_xy - sum_x * sum_y) / denominator;
  const double intercept = (sum_y - slope * sum_x) / n;
  const double mean_y = sum_y / n;
  double ss_res = 0.0;
  double ss_tot = 0.0;
  for (size_t i = first; i < last; ++i) {
    const double predicted = intercept + slope * x[i];
    const double residual = y[i] - predicted;
    const double centered = y[i] - mean_y;
    ss_res += residual * residual;
    ss_tot += centered * centered;
  }

  LinearFit fit;
  fit.slope = static_cast<float>(slope);
  fit.r2 = ss_tot > 1e-12 ? static_cast<float>(std::clamp(1.0 - ss_res / ss_tot, 0.0, 1.0)) : 0.0f;
  return fit;
}

FrameEnergy compute_frame_energy(const float* samples, size_t size, int sample_rate) {
  const int frame_size = std::max(32, static_cast<int>(std::round(0.03f * sample_rate)));
  const int hop_size = std::max(1, static_cast<int>(std::round(0.01f * sample_rate)));
  if (size < static_cast<size_t>(frame_size)) {
    return {};
  }

  FrameEnergy result;
  for (size_t start = 0; start + static_cast<size_t>(frame_size) <= size;
       start += static_cast<size_t>(hop_size)) {
    double sum = 0.0;
    for (int i = 0; i < frame_size; ++i) {
      const double sample = samples[start + static_cast<size_t>(i)];
      sum += sample * sample;
    }
    result.energy.push_back(static_cast<float>(sum / frame_size));
    result.times.push_back((static_cast<float>(start) + 0.5f * frame_size) /
                           static_cast<float>(sample_rate));
  }

  const float reference =
      std::max(*std::max_element(result.energy.begin(), result.energy.end()), kEnergyEpsilon);
  result.db.reserve(result.energy.size());
  for (float value : result.energy) {
    result.db.push_back(10.0f * std::log10(std::max(value, kEnergyEpsilon) / reference));
  }
  return result;
}

float percentile(std::vector<float> values, float q) {
  if (values.empty()) {
    return nan_value();
  }
  q = std::clamp(q, 0.0f, 1.0f);
  const size_t index = static_cast<size_t>(std::round(q * static_cast<float>(values.size() - 1)));
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index),
                   values.end());
  return values[index];
}

std::vector<float> suppress_stationary_noise_spectral(const float* samples, size_t size,
                                                      int sample_rate) {
  if (samples == nullptr || sample_rate <= 0 || size < 1024) {
    return samples == nullptr ? std::vector<float>{} : std::vector<float>(samples, samples + size);
  }

  const int n_fft = sample_rate <= 16000 ? 512 : 1024;
  const int hop = n_fft / 2;
  if (size < static_cast<size_t>(n_fft + hop)) {
    return std::vector<float>(samples, samples + size);
  }

  FFT fft(n_fft);
  const auto& window = get_window_cached(WindowType::Hann, n_fft);
  const size_t n_frames = 1 + (size - static_cast<size_t>(n_fft)) / static_cast<size_t>(hop);
  const int n_bins = fft.n_bins();
  std::vector<std::vector<float>> magnitudes(static_cast<size_t>(n_bins));
  for (auto& bin : magnitudes) {
    bin.reserve(n_frames);
  }

  std::vector<float> frame(static_cast<size_t>(n_fft), 0.0f);
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(n_bins));
  for (size_t frame_index = 0; frame_index < n_frames; ++frame_index) {
    const size_t start = frame_index * static_cast<size_t>(hop);
    for (int i = 0; i < n_fft; ++i) {
      frame[static_cast<size_t>(i)] = samples[start + static_cast<size_t>(i)] * window[i];
    }
    fft.forward(frame.data(), spectrum.data());
    for (int bin = 0; bin < n_bins; ++bin) {
      magnitudes[static_cast<size_t>(bin)].push_back(std::abs(spectrum[static_cast<size_t>(bin)]));
    }
  }

  std::vector<float> noise_floor(static_cast<size_t>(n_bins), 0.0f);
  for (int bin = 0; bin < n_bins; ++bin) {
    noise_floor[static_cast<size_t>(bin)] = percentile(magnitudes[static_cast<size_t>(bin)], 0.20f);
  }

  std::vector<float> output(size, 0.0f);
  std::vector<float> norm(size, 0.0f);
  std::vector<float> inverse_frame(static_cast<size_t>(n_fft), 0.0f);
  for (size_t frame_index = 0; frame_index < n_frames; ++frame_index) {
    const size_t start = frame_index * static_cast<size_t>(hop);
    for (int i = 0; i < n_fft; ++i) {
      frame[static_cast<size_t>(i)] = samples[start + static_cast<size_t>(i)] * window[i];
    }
    fft.forward(frame.data(), spectrum.data());
    for (int bin = 0; bin < n_bins; ++bin) {
      const float magnitude = std::abs(spectrum[static_cast<size_t>(bin)]);
      if (magnitude <= 1e-12f) {
        continue;
      }
      const float residual =
          std::max(0.0f, magnitude - 1.25f * noise_floor[static_cast<size_t>(bin)]);
      const float gain = std::clamp(residual / magnitude, 0.20f, 1.0f);
      spectrum[static_cast<size_t>(bin)] *= gain;
    }
    fft.inverse(spectrum.data(), inverse_frame.data());
    for (int i = 0; i < n_fft; ++i) {
      const size_t index = start + static_cast<size_t>(i);
      const float weight = window[i];
      output[index] += inverse_frame[static_cast<size_t>(i)] * weight;
      norm[index] += weight * weight;
    }
  }

  for (size_t i = 0; i < size; ++i) {
    if (norm[i] > 1e-8f) {
      output[i] /= norm[i];
    } else {
      output[i] = samples[i];
    }
  }
  return output;
}

float estimate_noise_floor_db(const FrameEnergy& frames) {
  if (frames.db.empty()) {
    return nan_value();
  }

  const size_t tail_start = frames.db.size() / 2;
  std::vector<float> tail;
  tail.reserve(frames.db.size() - tail_start);
  for (size_t i = tail_start; i < frames.db.size(); ++i) {
    if (std::isfinite(frames.db[i])) {
      tail.push_back(frames.db[i]);
    }
  }
  return percentile(std::move(tail), 0.20f);
}

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
      initial_energy > 1e-20 ? 10.0 * std::log10(initial_energy / std::max(best_noise, 1e-20))
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
                                                 bool suppress_stationary_noise = true) {
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

std::vector<float> schroeder_edc_db(const std::vector<double>& energy) {
  std::vector<float> edc(energy.size(), nan_value());
  if (energy.empty()) {
    return edc;
  }

  double cumulative = 0.0;
  for (size_t i = energy.size(); i-- > 0;) {
    cumulative += energy[i];
    edc[i] = static_cast<float>(cumulative);
  }

  const float reference = std::max(edc.front(), kEnergyEpsilon);
  for (float& value : edc) {
    value = 10.0f * std::log10(std::max(value, kEnergyEpsilon) / reference);
  }
  return edc;
}

float decay_time_from_range(const std::vector<float>& edc_db, int sample_rate, float upper_db,
                            float lower_db) {
  double sum_t = 0.0;
  double sum_y = 0.0;
  double sum_tt = 0.0;
  double sum_ty = 0.0;
  size_t count = 0;

  for (size_t i = 0; i < edc_db.size(); ++i) {
    const float y = edc_db[i];
    if (!std::isfinite(y) || y > upper_db || y < lower_db) {
      continue;
    }
    const double t = static_cast<double>(i) / static_cast<double>(sample_rate);
    sum_t += t;
    sum_y += y;
    sum_tt += t * t;
    sum_ty += t * y;
    ++count;
  }

  if (count < 2) {
    return nan_value();
  }

  const double n = static_cast<double>(count);
  const double denominator = n * sum_tt - sum_t * sum_t;
  if (std::abs(denominator) < 1e-12) {
    return nan_value();
  }

  const double slope = (n * sum_ty - sum_t * sum_y) / denominator;
  if (slope >= -1e-9) {
    return nan_value();
  }
  return static_cast<float>(-60.0 / slope);
}

double sum_range(const std::vector<double>& energy, size_t first, size_t last) {
  first = std::min(first, energy.size());
  last = std::min(last, energy.size());
  if (first >= last) {
    return 0.0;
  }
  return std::accumulate(energy.begin() + static_cast<std::ptrdiff_t>(first),
                         energy.begin() + static_cast<std::ptrdiff_t>(last), 0.0);
}

float clarity_db(const std::vector<double>& energy, int sample_rate, float boundary_sec) {
  const size_t boundary = static_cast<size_t>(std::round(boundary_sec * sample_rate));
  const double early = sum_range(energy, 0, boundary);
  const double late = sum_range(energy, boundary, energy.size());
  return static_cast<float>(10.0 * std::log10(std::max(early, static_cast<double>(kEnergyEpsilon)) /
                                              std::max(late, static_cast<double>(kEnergyEpsilon))));
}

float definition_d50(const std::vector<double>& energy, int sample_rate) {
  const size_t boundary = static_cast<size_t>(std::round(0.05f * sample_rate));
  const double early = sum_range(energy, 0, boundary);
  const double total = sum_range(energy, 0, energy.size());
  if (total <= 0.0) {
    return nan_value();
  }
  return static_cast<float>(early / total);
}

float estimate_confidence(float rt60, float edt, float min_decay_db) {
  if (!std::isfinite(rt60) || !std::isfinite(edt) || rt60 <= 0.0f || edt <= 0.0f) {
    return 0.0f;
  }
  const float agreement = 1.0f - std::min(std::abs(rt60 - edt) / std::max(rt60, 1e-6f), 1.0f);
  const float decay_coverage = std::clamp(min_decay_db / 30.0f, 0.0f, 1.0f);
  return std::clamp(0.4f + 0.4f * agreement + 0.2f * decay_coverage, 0.0f, 1.0f);
}

AcousticParameters analyze_band(const float* samples, size_t size, int sample_rate,
                                float min_decay_db) {
  const auto energy = squared_energy(samples, size);
  const auto edc_db = schroeder_edc_db(energy);

  AcousticParameters result;
  result.rt60 = decay_time_from_range(edc_db, sample_rate, -5.0f, -5.0f - min_decay_db);
  result.edt = decay_time_from_range(edc_db, sample_rate, 0.0f, -10.0f);
  result.c50 = clarity_db(energy, sample_rate, 0.05f);
  result.c80 = clarity_db(energy, sample_rate, 0.08f);
  result.d50 = definition_d50(energy, sample_rate);
  result.confidence = estimate_confidence(result.rt60, result.edt, min_decay_db);
  return result;
}

std::vector<float> filter_octave_band(const Audio& ir, float center_hz) {
  const float lower_hz = center_hz / kSqrt2;
  const float upper_hz = center_hz * kSqrt2;
  const float nyquist = static_cast<float>(ir.sample_rate()) * 0.5f;
  if (upper_hz >= nyquist || lower_hz <= 0.0f) {
    return {};
  }
  const auto coeffs = bandpass_coeffs(center_hz, upper_hz - lower_hz, ir.sample_rate());
  return apply_biquad_filtfilt(ir.data(), ir.size(), coeffs);
}

std::vector<float> filter_third_octave_band(const Audio& audio, float center_hz) {
  const float ratio = std::pow(2.0f, 1.0f / 6.0f);
  const float lower_hz = center_hz / ratio;
  const float upper_hz = center_hz * ratio;
  const float nyquist = static_cast<float>(audio.sample_rate()) * 0.5f;
  if (upper_hz >= nyquist || lower_hz <= 0.0f) {
    return {};
  }
  std::vector<float> centered(audio.data(), audio.data() + audio.size());
  const float mean = centered.empty() ? 0.0f
                                      : std::accumulate(centered.begin(), centered.end(), 0.0f) /
                                            static_cast<float>(centered.size());
  for (float& sample : centered) {
    sample -= mean;
  }
  const auto coeffs = bandpass_coeffs(center_hz, upper_hz - lower_hz, audio.sample_rate());
  return apply_biquad_filtfilt(centered.data(), centered.size(), coeffs);
}

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
  return 20.0 * std::log10(std::max(early_rms, 1e-12) / std::max(tail_rms, 1e-12)) >= 18.0;
}

}  // namespace

AcousticAnalyzer::AcousticAnalyzer(const Audio& audio, const AcousticConfig& config)
    : AcousticAnalyzer(
          audio, config,
          config.mode == AcousticConfig::Mode::ImpulseResponse ||
              (config.mode == AcousticConfig::Mode::Auto && looks_like_impulse_response(audio))) {}

AcousticAnalyzer AcousticAnalyzer::from_impulse_response(const Audio& ir,
                                                         const AcousticConfig& config) {
  AcousticConfig ir_config = config;
  ir_config.mode = AcousticConfig::Mode::ImpulseResponse;
  return AcousticAnalyzer(ir, ir_config, true);
}

AcousticAnalyzer::AcousticAnalyzer(const Audio& audio, const AcousticConfig& config,
                                   bool impulse_response)
    : config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(audio.sample_rate() > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config_.n_octave_bands >= 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(config_.min_decay_db > 0.0f, ErrorCode::InvalidParameter);
  SONARE_CHECK(config_.noise_floor_margin_db >= 0.0f, ErrorCode::InvalidParameter);

  if (impulse_response) {
    analyze_impulse_response(audio);
  } else {
    analyze_blind(audio);
  }
}

void AcousticAnalyzer::analyze_impulse_response(const Audio& ir) {
  parameters_ = analyze_band(ir.data(), ir.size(), ir.sample_rate(), config_.min_decay_db);
  parameters_.is_blind = false;

  static constexpr float kOctaveCenters[] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f};
  const int n_bands =
      std::min<int>(config_.n_octave_bands, static_cast<int>(std::size(kOctaveCenters)));
  parameters_.rt60_bands.reserve(n_bands);
  parameters_.edt_bands.reserve(n_bands);
  parameters_.c50_bands.reserve(n_bands);
  parameters_.c80_bands.reserve(n_bands);

  for (int i = 0; i < n_bands; ++i) {
    const std::vector<float> filtered = filter_octave_band(ir, kOctaveCenters[i]);
    if (filtered.empty()) {
      parameters_.rt60_bands.push_back(nan_value());
      parameters_.edt_bands.push_back(nan_value());
      parameters_.c50_bands.push_back(nan_value());
      parameters_.c80_bands.push_back(nan_value());
      continue;
    }

    const AcousticParameters band =
        analyze_band(filtered.data(), filtered.size(), ir.sample_rate(), config_.min_decay_db);
    parameters_.rt60_bands.push_back(band.rt60);
    parameters_.edt_bands.push_back(band.edt);
    parameters_.c50_bands.push_back(band.c50);
    parameters_.c80_bands.push_back(band.c80);
  }
}

void AcousticAnalyzer::analyze_blind(const Audio& audio) {
  parameters_.rt60 = nan_value();
  parameters_.edt = nan_value();
  parameters_.c50 = nan_value();
  parameters_.c80 = nan_value();
  parameters_.d50 = nan_value();
  parameters_.confidence = 0.0f;
  parameters_.is_blind = true;

  const std::vector<BlindRt60Estimate> subband_estimates =
      estimate_third_octave_rt60(audio, config_);
  const FrequencyRtModel frequency_model =
      fit_frequency_dependent_rt_model(subband_estimates, 1000.0f, 10000.0f);
  BlindRt60Estimate subband = weighted_subband_average(subband_estimates, 1000.0f, 4000.0f);
  if (subband.confidence <= 0.0f) {
    subband = weighted_subband_average(subband_estimates, 125.0f, 10000.0f);
  }

  const BlindRt60Estimate fullband =
      estimate_blind_rt60_from_decay(audio.data(), audio.size(), audio.sample_rate(),
                                     config_.min_decay_db, config_.noise_floor_margin_db);
  BlindRt60Estimate global = subband;
  if (subband.confidence > 0.0f && fullband.confidence > 0.0f) {
    const float ratio = subband.rt60 / fullband.rt60;
    if (ratio < 0.75f || ratio > 1.35f) {
      global = fullband;
    }
  } else if (fullband.confidence > 0.0f) {
    global = fullband;
  }

  if (global.confidence > 0.0f) {
    parameters_.rt60 = global.rt60;
    parameters_.edt = global.rt60;
    parameters_.confidence = global.confidence;
  }

  static constexpr float kOctaveCenters[] = {125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f};
  const int n_bands =
      std::min<int>(config_.n_octave_bands, static_cast<int>(std::size(kOctaveCenters)));
  parameters_.rt60_bands.reserve(n_bands);
  parameters_.edt_bands.reserve(n_bands);

  for (int i = 0; i < n_bands; ++i) {
    const std::vector<float> filtered = filter_octave_band(audio, kOctaveCenters[i]);
    if (filtered.empty()) {
      parameters_.rt60_bands.push_back(nan_value());
      parameters_.edt_bands.push_back(nan_value());
      continue;
    }

    const float ratio = kSqrt2;
    BlindRt60Estimate band = weighted_subband_average(subband_estimates, kOctaveCenters[i] / ratio,
                                                      kOctaveCenters[i] * ratio);
    const BlindRt60Estimate low_frequency_anchor =
        std::isfinite(parameters_.rt60) && parameters_.confidence > 0.0f ? global : subband;
    if (kOctaveCenters[i] < 1000.0f &&
        (band.confidence <= 0.0f ||
         (std::isfinite(low_frequency_anchor.rt60) && std::isfinite(band.rt60) &&
          band.rt60 < low_frequency_anchor.rt60 * 0.7f))) {
      band = estimate_from_frequency_model(frequency_model, kOctaveCenters[i]);
      if (band.confidence <= 0.0f) {
        band = extrapolate_low_frequency_rt60(low_frequency_anchor, kOctaveCenters[i]);
      }
    }
    if (band.confidence <= 0.0f) {
      band = estimate_blind_rt60_from_decay(filtered.data(), filtered.size(), audio.sample_rate(),
                                            config_.min_decay_db, config_.noise_floor_margin_db,
                                            false);
    }
    parameters_.rt60_bands.push_back(band.rt60);
    parameters_.edt_bands.push_back(band.confidence > 0.0f ? band.rt60 : nan_value());
  }
}

void AcousticAnalyzer::set_unsupported_blind_result() {
  parameters_.rt60 = nan_value();
  parameters_.edt = nan_value();
  parameters_.c50 = nan_value();
  parameters_.c80 = nan_value();
  parameters_.d50 = nan_value();
  parameters_.confidence = 0.0f;
  parameters_.is_blind = true;
}

AcousticParameters detect_acoustic(const Audio& audio, const AcousticConfig& config) {
  return AcousticAnalyzer(audio, config).parameters();
}

AcousticParameters analyze_impulse_response(const Audio& ir, const AcousticConfig& config) {
  return AcousticAnalyzer::from_impulse_response(ir, config).parameters();
}

}  // namespace sonare
