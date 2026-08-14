/// @file math_utils.cpp
/// @brief Implementation of math utility functions.

#include "util/math_utils.h"

#include <Eigen/Core>
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "core/db_convert.h"
#include "core/fft.h"
#include "util/exception.h"

namespace sonare {

using sonare::constants::kEpsilon;

float cosine_similarity(const float* a, const float* b, size_t size) {
  if (size == 0) return 0.0f;

  float dot = 0.0f;
  float norm_a = 0.0f;
  float norm_b = 0.0f;

  for (size_t i = 0; i < size; ++i) {
    dot += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }

  float denom = std::sqrt(norm_a) * std::sqrt(norm_b);
  if (denom < constants::kEpsilon) return 0.0f;
  return dot / denom;
}

float pearson_correlation(const float* a, const float* b, size_t size) {
  if (size < 2) return 0.0f;

  float mean_a = mean(a, size);
  float mean_b = mean(b, size);

  float num = 0.0f;
  float den_a = 0.0f;
  float den_b = 0.0f;

  for (size_t i = 0; i < size; ++i) {
    float da = a[i] - mean_a;
    float db = b[i] - mean_b;
    num += da * db;
    den_a += da * da;
    den_b += db * db;
  }

  float denom = std::sqrt(den_a * den_b);
  if (denom < constants::kEpsilon) return 0.0f;
  return num / denom;
}

float median(const float* data, size_t size) {
  if (size == 0) return 0.0f;

  std::vector<float> sorted(data, data + size);
  std::sort(sorted.begin(), sorted.end());

  if (size % 2 == 0) {
    return (sorted[size / 2 - 1] + sorted[size / 2]) / 2.0f;
  }
  return sorted[size / 2];
}

std::vector<float> unnormalized_autocorrelation(const float* input, size_t n, size_t max_lag) {
  if (n > 0 && input == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "unnormalized_autocorrelation: null input with non-zero length");
  }

  const size_t out_size = std::min(max_lag, n);
  std::vector<float> out(out_size, 0.0f);
  if (out_size == 0) return out;

  constexpr size_t kFftThreshold = 64;
  if (n < kFftThreshold) {
    for (size_t lag = 0; lag < out_size; ++lag) {
      double acc = 0.0;
      for (size_t i = 0; i + lag < n; ++i) {
        acc += static_cast<double>(input[i]) * static_cast<double>(input[i + lag]);
      }
      out[lag] = static_cast<float>(acc);
    }
    return out;
  }

  constexpr size_t kMaxAutocorrN = (static_cast<size_t>(1) << 29);
  if (n > kMaxAutocorrN) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "unnormalized_autocorrelation: input too large");
  }

  const size_t fft_size = next_power_of_2(2 * n - 1);

  std::vector<float> padded(fft_size, 0.0f);
  std::copy(input, input + n, padded.begin());

  FFT fft(static_cast<int>(fft_size));
  const int n_bins = fft.n_bins();
  std::vector<std::complex<float>> spectrum(static_cast<size_t>(n_bins));
  fft.forward(padded.data(), spectrum.data());

  for (int i = 0; i < n_bins; ++i) {
    const size_t index = static_cast<size_t>(i);
    const float re = spectrum[index].real();
    const float im = spectrum[index].imag();
    spectrum[index] = std::complex<float>(re * re + im * im, 0.0f);
  }

  std::vector<float> raw(fft_size);
  fft.inverse(spectrum.data(), raw.data());
  std::copy(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(out_size), out.begin());
  return out;
}

double percentile_sorted(const std::vector<float>& sorted, double percentile) {
  if (sorted.empty()) return 0.0;
  const double position = std::clamp(percentile, 0.0, 1.0) * static_cast<double>(sorted.size() - 1);
  const size_t low = static_cast<size_t>(std::floor(position));
  const size_t high = static_cast<size_t>(std::ceil(position));
  if (low == high) return static_cast<double>(sorted[low]);
  const double frac = position - static_cast<double>(low);
  return static_cast<double>(sorted[low]) * (1.0 - frac) + static_cast<double>(sorted[high]) * frac;
}

float percentile(const float* data, size_t size, float p) {
  if (size == 0) return 0.0f;

  std::vector<float> sorted(data, data + size);
  std::sort(sorted.begin(), sorted.end());

  // Clamp the requested percentile to [0, 100]. Out-of-range p would scale to a
  // fractional rank outside [0, size-1]; percentile_sorted clamps the fraction,
  // so the division below cannot produce an out-of-range rank.
  return static_cast<float>(
      percentile_sorted(sorted, static_cast<double>(std::clamp(p, 0.0f, 100.0f)) / 100.0));
}

void power_to_db(const float* power, size_t n, float ref, float amin, float top_db, float* out) {
  if (n == 0) {
    return;
  }

  // Delegate the core dB math (including the librosa ref<=0 => max(|S|) reference
  // resolution and the top_db clamp) to the single canonical implementation in
  // core/db_convert. This in-place / out-pointer overload merely copies the
  // result back, so it remains safe when `out` aliases `power`.
  std::vector<float> db = sonare::power_to_db(power, n, ref, amin, top_db);
  std::copy(db.begin(), db.end(), out);
}

void compute_autocorrelation(const float* input, int n, int max_lag, float* output) {
  if (n < 0 || max_lag < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "compute_autocorrelation: lengths must be non-negative");
  }
  if (n > 0 && input == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "compute_autocorrelation: null input with non-zero length");
  }
  if (max_lag > 0 && output == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "compute_autocorrelation: null output with non-zero length");
  }

  for (int i = 0; i < max_lag; ++i) {
    output[i] = 0.0f;
  }

  if (n == 0 || max_lag <= 0) {
    return;
  }

  // Compute mean
  const float mean_val = mean(input, static_cast<size_t>(n));

  // Compute variance
  float var = 0.0f;
  for (int i = 0; i < n; ++i) {
    float diff = input[i] - mean_val;
    var += diff * diff;
  }

  if (var < constants::kEpsilon) {
    return;
  }

  std::vector<float> zero_mean(static_cast<size_t>(n), 0.0f);
  for (int i = 0; i < n; ++i) {
    zero_mean[static_cast<size_t>(i)] = input[i] - mean_val;
  }

  const std::vector<float> raw_autocorr = unnormalized_autocorrelation(
      zero_mean.data(), zero_mean.size(), static_cast<size_t>(max_lag));
  // Normalize by variance (unnormalized sum of squared deviations).
  // Dividing by var gives output[0] = 1.0 (normalized autocorrelation).
  for (size_t lag = 0; lag < raw_autocorr.size(); ++lag) {
    output[lag] = raw_autocorr[lag] / var;
  }
}

}  // namespace sonare
