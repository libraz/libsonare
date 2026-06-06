#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include "analysis/acoustic/internal.h"
#include "filters/iir.h"
#include "util/db.h"

namespace sonare::acoustic_detail {

namespace {

size_t estimate_lundeby_truncation_index(const std::vector<double>& energy, int sample_rate) {
  const size_t n = energy.size();
  if (n < 64 || sample_rate <= 0) {
    return n;
  }

  const size_t peak = direct_sound_index(energy);
  const size_t tail_count = std::clamp(n / 10, static_cast<size_t>(16), n / 2);
  const double tail_sum = sum_range(energy, n - tail_count, n);
  const double noise_power = tail_sum / static_cast<double>(tail_count);
  if (!(noise_power > static_cast<double>(kEnergyEpsilon))) {
    return n;
  }

  const size_t max_window = std::max<size_t>(16, std::min<size_t>(2048, n / 4));
  const size_t window =
      std::clamp(static_cast<size_t>(std::lround(0.01 * static_cast<double>(sample_rate))),
                 static_cast<size_t>(16), max_window);
  if (window >= n) {
    return n;
  }

  double moving = sum_range(energy, 0, window);
  const double threshold = noise_power * 4.0;
  const size_t first = std::min(n - window, peak + window);
  const size_t last = n - window;
  for (size_t i = 0; i <= last; ++i) {
    if (i >= first && moving / static_cast<double>(window) <= threshold) {
      return i;
    }
    if (i == last) {
      break;
    }
    moving += energy[i + window] - energy[i];
  }
  return n;
}

std::vector<float> schroeder_edc_db_until(const std::vector<double>& energy, size_t end) {
  std::vector<float> edc(energy.size(), nan_value());
  if (energy.empty()) {
    return edc;
  }

  end = std::clamp(end, static_cast<size_t>(1), energy.size());
  double cumulative = 0.0;
  for (size_t i = end; i-- > 0;) {
    cumulative += energy[i];
    edc[i] = static_cast<float>(cumulative);
  }

  const float reference = std::max(edc.front(), kEnergyEpsilon);
  for (size_t i = 0; i < end; ++i) {
    edc[i] = power_to_db_scalar(std::max(edc[i], kEnergyEpsilon) / reference);
  }
  return edc;
}

}  // namespace

std::vector<float> schroeder_edc_db(const std::vector<double>& energy) {
  return schroeder_edc_db_until(energy, energy.size());
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

size_t direct_sound_index(const std::vector<double>& energy) {
  if (energy.empty()) {
    return 0;
  }
  return static_cast<size_t>(
      std::distance(energy.begin(), std::max_element(energy.begin(), energy.end())));
}

float clarity_db(const std::vector<double>& energy, int sample_rate, float boundary_sec,
                 size_t origin) {
  const size_t boundary = static_cast<size_t>(std::round(boundary_sec * sample_rate));
  const double early = sum_range(energy, origin, origin + boundary);
  const double late = sum_range(energy, origin + boundary, energy.size());
  return power_to_db_scalar(std::max(early, static_cast<double>(kEnergyEpsilon)) /
                            std::max(late, static_cast<double>(kEnergyEpsilon)));
}

float clarity_db(const std::vector<double>& energy, int sample_rate, float boundary_sec) {
  return clarity_db(energy, sample_rate, boundary_sec, 0);
}

float definition_d50(const std::vector<double>& energy, int sample_rate, size_t origin) {
  const size_t boundary = static_cast<size_t>(std::round(0.05f * sample_rate));
  const double early = sum_range(energy, origin, origin + boundary);
  const double total = sum_range(energy, origin, energy.size());
  if (total <= 0.0) {
    return nan_value();
  }
  return static_cast<float>(early / total);
}

float definition_d50(const std::vector<double>& energy, int sample_rate) {
  return definition_d50(energy, sample_rate, 0);
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
  const auto edc_db =
      schroeder_edc_db_until(energy, estimate_lundeby_truncation_index(energy, sample_rate));
  const size_t origin = direct_sound_index(energy);

  AcousticParameters result;
  result.rt60 = decay_time_from_range(edc_db, sample_rate, -5.0f, -5.0f - min_decay_db);
  if (!std::isfinite(result.rt60) && min_decay_db > 20.0f) {
    result.rt60 = decay_time_from_range(edc_db, sample_rate, -5.0f, -25.0f);
  }
  result.edt = decay_time_from_range(edc_db, sample_rate, 0.0f, -10.0f);
  result.c50 = clarity_db(energy, sample_rate, 0.05f, origin);
  result.c80 = clarity_db(energy, sample_rate, 0.08f, origin);
  result.d50 = definition_d50(energy, sample_rate, origin);
  result.confidence = estimate_confidence(result.rt60, result.edt, min_decay_db);
  return result;
}

// NOTE: octave / third-octave band filtering uses a single 2nd-order biquad
// bandpass (Q ~= 1.4) applied zero-phase via filtfilt. The resulting skirts are
// shallow, so adjacent-band energy leaks more than an IEC 61260 class filter
// would. A 4th-order Butterworth bandpass (two cascaded biquads) would sharpen
// the response, but it shifts every per-band RT60/EDT/clarity value and the
// existing acoustic tests / golden manifests are calibrated against this single
// section. The single-section approximation is intentional and kept stable.
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

// Forward biquad pass that subtracts a constant DC offset from each input
// sample in-line, fusing the DC removal step with the filter's natural copy
// and eliminating the dedicated `centered` buffer.
std::vector<float> apply_biquad_dc_removed(const float* input, size_t size, float dc_offset,
                                           const BiquadCoeffs& coeffs) {
  std::vector<float> output(size);
  float z1 = 0.0f;
  float z2 = 0.0f;
  for (size_t i = 0; i < size; ++i) {
    const float x = input[i] - dc_offset;
    const float y = coeffs.b0 * x + z1;
    z1 = coeffs.b1 * x - coeffs.a1 * y + z2;
    z2 = coeffs.b2 * x - coeffs.a2 * y;
    output[i] = y;
  }
  return output;
}

std::vector<float> filter_third_octave_band(const Audio& audio, float center_hz) {
  const float ratio = std::pow(2.0f, 1.0f / 6.0f);
  const float lower_hz = center_hz / ratio;
  const float upper_hz = center_hz * ratio;
  const float nyquist = static_cast<float>(audio.sample_rate()) * 0.5f;
  if (upper_hz >= nyquist || lower_hz <= 0.0f || audio.empty()) {
    return {};
  }
  // Compute DC offset directly from the source buffer (no copy).
  const float* src = audio.data();
  const size_t n = audio.size();
  double sum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    sum += static_cast<double>(src[i]);
  }
  const float mean = static_cast<float>(sum / static_cast<double>(n));
  const auto coeffs = bandpass_coeffs(center_hz, upper_hz - lower_hz, audio.sample_rate());
  // Forward pass with fused DC removal (replaces the explicit centered copy).
  std::vector<float> forward = apply_biquad_dc_removed(src, n, mean, coeffs);
  std::reverse(forward.begin(), forward.end());
  std::vector<float> backward = apply_biquad(forward.data(), n, coeffs);
  std::reverse(backward.begin(), backward.end());
  return backward;
}

}  // namespace sonare::acoustic_detail
