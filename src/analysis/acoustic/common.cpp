#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

#include "analysis/acoustic/internal.h"
#include "core/fft.h"
#include "core/window.h"
#include "util/db.h"

namespace sonare::acoustic_detail {

float nan_value() { return std::numeric_limits<float>::quiet_NaN(); }

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
    result.db.push_back(power_to_db_scalar(std::max(value, kEnergyEpsilon) / reference));
  }
  return result;
}

float percentile(std::vector<float> values, float q) {
  if (values.empty()) {
    return nan_value();
  }
  q = std::clamp(q, 0.0f, 1.0f);
  std::sort(values.begin(), values.end());
  const float position = q * static_cast<float>(values.size() - 1);
  const auto lower = static_cast<size_t>(std::floor(position));
  const auto upper = static_cast<size_t>(std::ceil(position));
  if (lower == upper) {
    return values[lower];
  }
  const float weight = position - static_cast<float>(lower);
  return values[lower] * (1.0f - weight) + values[upper] * weight;
}

// O(n) average percentile using std::nth_element on a mutable buffer.
// Caller owns the buffer; the contents are partially reordered on return.
// For linear interpolation between adjacent ranks we run nth_element twice,
// but the second call still operates on the partially partitioned buffer
// (so on average it remains linear time and avoids the full O(n log n) sort).
float percentile_nth_element(float* data, size_t count, float q) {
  if (data == nullptr || count == 0) {
    return nan_value();
  }
  q = std::clamp(q, 0.0f, 1.0f);
  const float position = q * static_cast<float>(count - 1);
  const auto lower = static_cast<size_t>(std::floor(position));
  const auto upper = static_cast<size_t>(std::ceil(position));
  std::nth_element(data, data + lower, data + count);
  const float lower_value = data[lower];
  if (lower == upper) {
    return lower_value;
  }
  // Elements at [lower+1, count) are all >= lower_value after the first call,
  // so the minimum of that tail is the (upper)th order statistic.
  const float upper_value = *std::min_element(data + lower + 1, data + count);
  const float weight = position - static_cast<float>(lower);
  return lower_value * (1.0f - weight) + upper_value * weight;
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
  // Flat [n_bins x n_frames] matrix in bin-major (row-major) layout so each
  // bin's per-frame magnitudes are contiguous for percentile/nth_element.
  // Single allocation replaces n_bins nested vector allocations.
  std::vector<float> magnitudes(static_cast<size_t>(n_bins) * n_frames, 0.0f);

  std::vector<float> frame(static_cast<size_t>(n_fft), 0.0f);
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(n_bins));
  for (size_t frame_index = 0; frame_index < n_frames; ++frame_index) {
    const size_t start = frame_index * static_cast<size_t>(hop);
    for (int i = 0; i < n_fft; ++i) {
      frame[static_cast<size_t>(i)] = samples[start + static_cast<size_t>(i)] * window[i];
    }
    fft.forward(frame.data(), spectrum.data());
    for (int bin = 0; bin < n_bins; ++bin) {
      magnitudes[static_cast<size_t>(bin) * n_frames + frame_index] =
          std::abs(spectrum[static_cast<size_t>(bin)]);
    }
  }

  std::vector<float> noise_floor(static_cast<size_t>(n_bins), 0.0f);
  for (int bin = 0; bin < n_bins; ++bin) {
    // nth_element is O(n) average vs std::sort O(n log n); we mutate the
    // bin's row in place since we don't need the magnitudes again afterward.
    noise_floor[static_cast<size_t>(bin)] = percentile_nth_element(
        magnitudes.data() + static_cast<size_t>(bin) * n_frames, n_frames, 0.20f);
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
    if (norm[i] > constants::kSpectrumEpsilon) {
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

}  // namespace sonare::acoustic_detail
