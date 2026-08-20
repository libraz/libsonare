#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include "analysis/acoustic/internal.h"
#include "filters/iir.h"
#include "util/db.h"

namespace sonare::acoustic_detail {

namespace {

// ISO 3382 early/late split points, measured from the direct-sound arrival:
// 50 ms for C50 and D50, 80 ms for C80.
constexpr float kEarlyBoundary50Sec = 0.05f;
constexpr float kEarlyBoundary80Sec = 0.08f;

// Level drop the Schroeder fit spans to report a reverberation time.
constexpr double kDecayRangeDb = 60.0;

// Lundeby noise-floor crossing, searched forward of the direct sound. Both the
// noise estimate and the moving-average window are measured over the post-origin
// span, so the returned index shifts by exactly the amount of leading silence
// the container carries and the anchored window [origin, end) is unchanged.
size_t lundeby_truncation_index(const std::vector<double>& energy, size_t origin, int sample_rate) {
  const size_t n = energy.size();
  const size_t span = n - origin;
  if (span < 64 || sample_rate <= 0) {
    return n;
  }

  const size_t tail_count = std::clamp(span / 10, static_cast<size_t>(16), span / 2);
  const double tail_sum = sum_range(energy, n - tail_count, n);
  const double noise_power = tail_sum / static_cast<double>(tail_count);
  if (!(noise_power > static_cast<double>(kEnergyEpsilon))) {
    return n;
  }

  const size_t max_window = std::max<size_t>(16, std::min<size_t>(2048, span / 4));
  const size_t window =
      std::clamp(static_cast<size_t>(std::lround(0.01 * static_cast<double>(sample_rate))),
                 static_cast<size_t>(16), max_window);
  if (window >= span) {
    return n;
  }

  double moving = sum_range(energy, origin, origin + window);
  const double threshold = noise_power * 4.0;
  const size_t first = origin + window;
  const size_t last = n - window;
  for (size_t i = origin; i <= last; ++i) {
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

// Backward energy integration over [origin, end), in dB relative to the energy
// at the origin. Index 0 of the result is the direct-sound arrival, so two
// inputs differing only in leading silence produce the same curve.
std::vector<float> schroeder_edc_db(const std::vector<double>& energy, size_t origin, size_t end) {
  std::vector<float> edc(end - origin);
  double cumulative = 0.0;
  for (size_t i = end; i-- > origin;) {
    cumulative += energy[i];
    edc[i - origin] = static_cast<float>(cumulative);
  }

  const float reference = std::max(edc.front(), kEnergyEpsilon);
  for (float& value : edc) {
    value = power_to_db_scalar(std::max(value, kEnergyEpsilon) / reference);
  }
  return edc;
}

}  // namespace

AnchoredDecay AnchoredDecay::from_band(const float* samples, size_t size, int sample_rate) {
  AnchoredDecay decay;
  decay.sample_rate_ = sample_rate;
  decay.energy_ = squared_energy(samples, size);
  if (decay.energy_.empty()) {
    return decay;  // origin_ == end_, so every metric reports NaN
  }

  decay.origin_ = direct_sound_index(decay.energy_);
  // A crossing found at or before the direct sound would invert the window; keep
  // one sample so [origin, end) stays well formed and the metrics degrade to NaN
  // rather than reading past the end of the buffer.
  decay.end_ = std::max(lundeby_truncation_index(decay.energy_, decay.origin_, sample_rate),
                        decay.origin_ + 1);
  decay.edc_db_ = schroeder_edc_db(decay.energy_, decay.origin_, decay.end_);
  return decay;
}

float AnchoredDecay::decay_time(float upper_db, float lower_db) const {
  if (sample_rate_ <= 0) {
    return nan_value();
  }

  double sum_t = 0.0;
  double sum_y = 0.0;
  double sum_tt = 0.0;
  double sum_ty = 0.0;
  size_t count = 0;

  for (size_t i = 0; i < edc_db_.size(); ++i) {
    const float y = edc_db_[i];
    if (!std::isfinite(y) || y > upper_db || y < lower_db) {
      continue;
    }
    const double t = static_cast<double>(i) / static_cast<double>(sample_rate_);
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
  return static_cast<float>(-kDecayRangeDb / slope);
}

float AnchoredDecay::clarity_db(float boundary_sec) const {
  const size_t split = split_index(boundary_sec);
  const double early = sum_range(energy_, origin_, split);
  const double late = sum_range(energy_, split, end_);
  // A truncation landing at or before the split leaves no late window. Reporting
  // the epsilon-floor ratio there would claim extreme clarity; the honest answer
  // is that the late energy was not measurable.
  if (!(early > 0.0) || !(late > 0.0)) {
    return nan_value();
  }
  return static_cast<float>(power_to_db_scalar(early / late));
}

float AnchoredDecay::definition_d50() const {
  const size_t split = split_index(kEarlyBoundary50Sec);
  if (split >= end_) {
    return nan_value();  // no late window: the ratio would be pinned at 1 by construction
  }
  const double early = sum_range(energy_, origin_, split);
  const double total = sum_range(energy_, origin_, end_);
  if (!(total > 0.0)) {
    return nan_value();
  }
  // `early` sums a prefix of the same non-negative range as `total`, so the
  // ratio cannot leave [0, 1] on any exit path.
  return static_cast<float>(early / total);
}

size_t AnchoredDecay::split_index(float boundary_sec) const {
  const size_t boundary = static_cast<size_t>(
      std::max<long>(0, std::lround(static_cast<double>(boundary_sec) * sample_rate_)));
  return std::min(origin_ + boundary, end_);
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
  const AnchoredDecay decay = AnchoredDecay::from_band(samples, size, sample_rate);

  AcousticParameters result;
  result.rt60 = decay.decay_time(-5.0f, -5.0f - min_decay_db);
  if (!std::isfinite(result.rt60) && min_decay_db > 20.0f) {
    result.rt60 = decay.decay_time(-5.0f, -25.0f);
  }
  result.edt = decay.decay_time(0.0f, -10.0f);
  result.c50 = decay.clarity_db(kEarlyBoundary50Sec);
  result.c80 = decay.clarity_db(kEarlyBoundary80Sec);
  result.d50 = decay.definition_d50();
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
