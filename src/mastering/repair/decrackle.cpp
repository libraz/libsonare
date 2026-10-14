#include "mastering/repair/decrackle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <utility>
#include <vector>

#include "core/stereo_pair.h"
#include "mastering/repair/transient_envelope.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/validated.h"

namespace sonare::mastering::repair {
namespace {

float soft_threshold(float value, float threshold) {
  if (value > threshold) return value - threshold;
  if (value < -threshold) return value + threshold;
  return 0.0f;
}

float median_abs(const float* values, size_t count, std::vector<float>& scratch) {
  if (count == 0) return 0.0f;
  scratch.resize(count);
  for (size_t i = 0; i < count; ++i) scratch[i] = std::abs(values[i]);
  const auto middle = scratch.begin() + static_cast<std::ptrdiff_t>(count / 2);
  std::nth_element(scratch.begin(), middle, scratch.begin() + static_cast<std::ptrdiff_t>(count));
  return *middle;
}

float variance(const float* values, size_t count) {
  if (count == 0) return 0.0f;
  const double mean = std::accumulate(values, values + count, 0.0) / static_cast<double>(count);
  double sum = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const double centered = static_cast<double>(values[i]) - mean;
    sum += centered * centered;
  }
  return static_cast<float>(sum / static_cast<double>(count));
}

float bayes_shrink_threshold(const float* details, size_t count, float noise_sigma,
                             float max_threshold) {
  if (count == 0) return max_threshold;
  const float detail_variance = variance(details, count);
  const float noise_variance = noise_sigma * noise_sigma;
  const float signal_sigma = std::sqrt(std::max(0.0f, detail_variance - noise_variance));
  if (signal_sigma <= 1e-9f) {
    return max_threshold;
  }
  return std::min(max_threshold, noise_variance / signal_sigma);
}

/// The unnormalized Haar analysis. At each level [0, pairs) holds the
/// approximation and [pairs, 2*pairs) the raw detail; an odd tail sample is
/// carried through at 2*pairs. The approximation band is never shrunk and each
/// level transforms only the previous level's approximation, so the transform
/// and the shrinkage separate exactly -- which is what lets the shrinkage be
/// measured instead of only performed.
struct HaarAnalysis {
  std::vector<size_t> active_sizes;  ///< Band length at each level, level 0 first.
  float noise_sigma = 0.0f;          ///< MAD estimate from the level-0 detail band.
};

/// Working buffers one transform needs, sized once for the longest band and reused by every
/// level and every cycle spin.
struct HaarScratch {
  HaarAnalysis analysis;
  std::vector<float> temp;
  std::vector<float> abs_details;
};

void haar_forward(std::vector<float>& samples, int levels, HaarScratch& scratch) {
  HaarAnalysis& analysis = scratch.analysis;
  analysis.active_sizes.clear();
  analysis.noise_sigma = 0.0f;
  size_t active = samples.size();
  scratch.temp.resize(active);
  for (int level = 0; level < levels && active >= 2; ++level) {
    const size_t pairs = active / 2;
    float* temp = scratch.temp.data();
    for (size_t i = 0; i < pairs; ++i) {
      const float a = samples[2 * i];
      const float b = samples[2 * i + 1];
      temp[i] = 0.5f * (a + b);
      temp[pairs + i] = 0.5f * (a - b);
    }

    if (level == 0) {
      // Phi^-1(0.75): scales MAD to Gaussian sigma
      static constexpr float kMadGaussianConstant = 0.67448975f;
      analysis.noise_sigma =
          median_abs(temp + pairs, pairs, scratch.abs_details) / kMadGaussianConstant;
    }
    if (active % 2 != 0) {
      temp[active - 1] = samples[active - 1];
    }
    std::copy(temp, temp + active, samples.begin());
    analysis.active_sizes.push_back(active);
    active = pairs;
  }
}

void haar_shrink_details(std::vector<float>& samples, const HaarAnalysis& analysis, float threshold,
                         DecrackleReport* report) {
  for (size_t level = 0; level < analysis.active_sizes.size(); ++level) {
    const size_t pairs = analysis.active_sizes[level] / 2;
    float* details = samples.data() + pairs;
    // Each level averages the approximation band, so the white-noise standard
    // deviation in the detail band shrinks by 1/sqrt(2) per level. Reusing the
    // level-0 sigma verbatim would grossly over-threshold the coarser levels.
    float level_noise_sigma = analysis.noise_sigma;
    for (size_t l = 0; l < level; ++l) level_noise_sigma *= sonare::constants::kInvSqrt2;
    const float level_threshold =
        bayes_shrink_threshold(details, pairs, level_noise_sigma, threshold);
    for (size_t i = 0; i < pairs; ++i) {
      const float shrunk = soft_threshold(details[i], level_threshold);
      details[i] = shrunk;
      if (report == nullptr) continue;
      ++report->detail_coefficients;
      if (shrunk == 0.0f) ++report->shrunk_coefficients;
    }
  }
}

void haar_inverse(std::vector<float>& samples, const HaarAnalysis& analysis, HaarScratch& scratch) {
  for (int level = static_cast<int>(analysis.active_sizes.size()) - 1; level >= 0; --level) {
    const size_t reconstruct = analysis.active_sizes[static_cast<size_t>(level)];
    const size_t pairs = reconstruct / 2;
    if (pairs == 0) continue;
    float* temp = scratch.temp.data();
    for (size_t i = 0; i < pairs; ++i) {
      const float avg = samples[i];
      const float detail = samples[pairs + i];
      temp[2 * i] = avg + detail;
      temp[2 * i + 1] = avg - detail;
    }
    if (reconstruct % 2 != 0) temp[reconstruct - 1] = samples[reconstruct - 1];
    std::copy(temp, temp + reconstruct, samples.begin());
  }
}

/// Shrinks @p samples in place through one Haar transform.
void wavelet_shrink_once(std::vector<float>& samples, const DecrackleConfig& config,
                         HaarScratch& scratch, DecrackleReport* report) {
  haar_forward(samples, config.levels, scratch);
  const HaarAnalysis& analysis = scratch.analysis;
  haar_shrink_details(samples, analysis, config.threshold, report);
  haar_inverse(samples, analysis, scratch);
  if (report != nullptr) report->noise_sigma = analysis.noise_sigma;
}

/// This module's definition of the defect: a sample deviating from the median of
/// itself and its two neighbours by more than threshold. The detector and the
/// median repair both call it, so they cannot disagree about what crackle is.
bool is_crackle(const std::vector<float>& samples, size_t index, float threshold, float* median) {
  return detail::crackle_median(samples[index - 1], samples[index], samples[index + 1], threshold,
                                median);
}

/// How far a crackle sample's deviation must stand above the transient envelope beside it.
constexpr double kCrackleTransientContrast = 6.0;

/// Crackle samples the detector reports: the median criterion, less the samples that sit inside a
/// broadband transient of the programme rather than above it.
size_t count_crackle_defects(const std::vector<float>& samples, int sample_rate, float threshold) {
  const TransientEnvelope envelope(samples.data(), samples.size(), sample_rate);
  size_t count = 0;
  for (size_t i = 1; i + 1 < samples.size(); ++i) {
    float median = 0.0f;
    if (!is_crackle(samples, i, threshold, &median)) continue;
    const double deviation = std::abs(samples[i] - median);
    if (deviation > kCrackleTransientContrast * envelope.level(i, i + 1)) ++count;
  }
  return count;
}

CrackleDetection to_detection(size_t count, size_t size, int sample_rate) {
  CrackleDetection detection;
  detection.sample_count = count;
  if (size == 0) return detection;
  detection.sample_fraction = static_cast<float>(count) / static_cast<float>(size);
  detection.per_second =
      static_cast<float>(count) * static_cast<float>(sample_rate) / static_cast<float>(size);
  return detection;
}

std::vector<float> run_decrackle(const std::vector<float>& samples, int sample_rate,
                                 const DecrackleConfig& config, DecrackleReport* report) {
  if (report != nullptr) {
    *report = DecrackleReport{};
    report->detected = to_detection(count_crackle_defects(samples, sample_rate, config.threshold),
                                    samples.size(), sample_rate);
  }
  if (config.mode == DecrackleMode::WaveletShrinkage) {
    return detail::wavelet_shrink_spun(samples, config, detail::kCycleSpinShifts, report);
  }

  std::vector<float> output = samples;
  for (size_t i = 1; i + 1 < samples.size(); ++i) {
    float median = 0.0f;
    if (is_crackle(samples, i, config.threshold, &median)) {
      output[i] = median;
      if (report != nullptr) ++report->replaced_samples;
    }
  }
  return output;
}

}  // namespace

namespace detail {

bool crackle_median(float before, float current, float after, float threshold, float* median) {
  std::array<float, 3> window = {before, current, after};
  std::sort(window.begin(), window.end());
  *median = window[1];
  return std::abs(current - *median) > threshold;
}

}  // namespace detail

namespace detail {

std::vector<float> wavelet_shrink_spun(const std::vector<float>& samples,
                                       const DecrackleConfig& config, int shifts,
                                       DecrackleReport* report) {
  const size_t size = samples.size();
  if (size == 0) return {};
  const size_t spins = std::min(static_cast<size_t>(std::max(1, shifts)), size);

  std::vector<double> accumulator(size, 0.0);
  std::vector<float> shifted(size, 0.0f);
  HaarScratch scratch;
  for (size_t shift = 0; shift < spins; ++shift) {
    // shifted[i] = samples[(i + shift) % size], as two contiguous runs.
    const size_t head = size - shift;
    std::copy(samples.begin() + static_cast<std::ptrdiff_t>(shift), samples.end(), shifted.begin());
    std::copy(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(shift),
              shifted.begin() + static_cast<std::ptrdiff_t>(head));
    wavelet_shrink_once(shifted, config, scratch, shift == 0 ? report : nullptr);
    // accumulator[(i + shift) % size] += shifted[i], as two contiguous runs.
    for (size_t i = 0; i < head; ++i) accumulator[shift + i] += shifted[i];
    for (size_t i = head; i < size; ++i) accumulator[i - head] += shifted[i];
  }

  std::vector<float> output(size, 0.0f);
  for (size_t i = 0; i < size; ++i) {
    output[i] = static_cast<float>(accumulator[i] / static_cast<double>(spins));
  }
  return output;
}

}  // namespace detail

void validate_config(const DecrackleConfig& config) {
  if (!std::isfinite(config.threshold) || !(config.threshold > 0.0f) ||
      config.threshold > kDecrackleMaxThreshold) {
    throw SonareException(ErrorCode::InvalidParameter, "threshold must be in (0, 1000]");
  }
  if (config.mode != DecrackleMode::Median && config.mode != DecrackleMode::WaveletShrinkage) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid decrackle mode");
  }
  if (config.levels < 1 || config.levels > kDecrackleMaxLevels) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "levels must be in [1, " + std::to_string(kDecrackleMaxLevels) + "]");
  }
}

CrackleDetection detect_crackle(const float* samples, size_t size, int sample_rate,
                                const DecrackleConfig& config) {
  const auto validated = Validated<DecrackleConfig>::make(config);
  if (sample_rate <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (samples == nullptr || size == 0) return {};

  const std::vector<float> buffer(samples, samples + size);
  return to_detection(count_crackle_defects(buffer, sample_rate, validated->threshold), size,
                      sample_rate);
}

Audio decrackle(const Audio& audio, const DecrackleConfig& config) {
  return decrackle(audio, config, nullptr);
}

Audio decrackle(const Audio& audio, const DecrackleConfig& config, DecrackleReport* report) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  const auto validated = Validated<DecrackleConfig>::make(config);

  const std::vector<float> samples(audio.data(), audio.data() + audio.size());
  std::vector<float> output = run_decrackle(samples, audio.sample_rate(), validated.get(), report);
  return Audio::from_vector(std::move(output), audio.sample_rate());
}

DecrackleStereoResult decrackle_stereo(const Audio& left, const Audio& right,
                                       const DecrackleConfig& config) {
  require_stereo_pair(left, right);
  const auto validated = Validated<DecrackleConfig>::make(config);
  const int sample_rate = left.sample_rate();

  const std::vector<float> left_samples(left.data(), left.data() + left.size());
  const std::vector<float> right_samples(right.data(), right.data() + right.size());

  DecrackleStereoResult result;
  std::vector<float> left_output =
      run_decrackle(left_samples, sample_rate, validated.get(), &result.left_report);
  std::vector<float> right_output =
      run_decrackle(right_samples, sample_rate, validated.get(), &result.right_report);
  result.left = Audio::from_vector(std::move(left_output), sample_rate);
  result.right = Audio::from_vector(std::move(right_output), sample_rate);
  return result;
}

}  // namespace sonare::mastering::repair
