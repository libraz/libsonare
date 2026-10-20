#pragma once

/// @file svf_bandpass.h
/// @brief Topology-preserving-transform (TPT) state-variable bandpass filter.
///
/// A per-sample-tunable resonant bandpass used by the wah / auto-wah inserts:
/// the cutoff can be swept every sample (by an LFO or an envelope follower)
/// without the coefficient recomputation destabilising the filter, which is the
/// classic failure mode of a naive biquad wah. Header-only and allocation-free
/// so both inserts share one implementation.

#include <algorithm>
#include <cmath>

#include "rt/tail_budget.h"
#include "util/constants.h"
#include "util/non_finite_state.h"

namespace sonare::effects::modulation {

/// Single-channel TPT state-variable filter, bandpass tap (Zavalishin form).
class SvfBandpass {
 public:
  /// Highest cutoff the filter realizes, as a fraction of the sample rate. The TPT form is stable
  /// at any finite corner, so this only keeps tan() away from its pole at Nyquist.
  static constexpr double kMaxCutoffRatio = 0.49;

  /// @brief The cutoff, in hertz, that process(), set() and ring() clamp to at @p sample_rate.
  static float max_cutoff_hz(double sample_rate) noexcept {
    return static_cast<float>(kMaxCutoffRatio * sample_rate);
  }

  /// @brief Ring of the section at @p cutoff_hz and @p q, read from its equivalent biquad.
  static rt::TailBudget ring(float cutoff_hz, float q, double sample_rate) noexcept {
    const double rate = sample_rate > 0.0 ? sample_rate : sonare::constants::kDefaultDawSampleRate;
    const double fc = std::clamp(static_cast<double>(cutoff_hz), 10.0, kMaxCutoffRatio * rate);
    const double g = std::tan(::sonare::constants::kPiD * fc / rate);
    const double k = 1.0 / std::max(0.5, static_cast<double>(q));
    const double d = 1.0 + g * (g + k);
    rt::TailBudget tail;
    tail.biquad(2.0, 2.0 * (g * g - 1.0) / d, (1.0 - g * k + g * g) / d);
    return tail;
  }

  /// @brief Conservative ring bound for a swept section between two corners.
  ///
  /// The pole radius is not monotonic in the cutoff: it decreases toward the
  /// middle of the TPT range and rises again near Nyquist.  Both endpoints are
  /// therefore part of the bound, even when the caller supplies them reversed.
  static rt::TailBudget ring_range(float first_hz, float second_hz, float q,
                                   double sample_rate) noexcept {
    rt::TailBudget tail = ring(first_hz, q, sample_rate);
    return tail.alongside(ring(second_hz, q, sample_rate));
  }

  void prepare(double sample_rate) noexcept {
    sample_rate_ = sample_rate > 0.0 ? sample_rate : sonare::constants::kDefaultDawSampleRate;
    reset();
  }
  void reset() noexcept {
    ic1_ = 0.0f;
    ic2_ = 0.0f;
  }

  /// Returns the integrator pair to rest once a non-finite value has reached it
  /// (see util/non_finite_state.h). The owning insert calls this once per block.
  /// @return true when the pair was discarded.
  bool discard_non_finite() noexcept { return ::sonare::discard_group_if_non_finite(ic1_, ic2_); }

  /// Process one sample with the given centre frequency (Hz) and resonance Q.
  /// The bandpass output is scaled to unity peak gain at resonance; with
  /// @p lowpass the second integrator's state is returned instead.
  float process(float input, float cutoff_hz, float q, bool lowpass = false) noexcept {
    return tick(input, coefficients(cutoff_hz, q), lowpass);
  }

  /// Coefficients of one section; computed once per sample by coefficients() and shared by every
  /// channel that runs at the same corner.
  struct Coefficients {
    float k = 1.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float a3 = 0.0f;
  };

  /// Coefficients for a centre frequency (Hz) and resonance Q, clamped as process() clamps them.
  Coefficients coefficients(float cutoff_hz, float q) const noexcept {
    const float fc = std::clamp(cutoff_hz, 10.0f, max_cutoff_hz(sample_rate_));
    const float g = std::tan(static_cast<float>(::sonare::constants::kPiD) * fc /
                             static_cast<float>(sample_rate_));
    Coefficients c;
    c.k = 1.0f / std::max(0.5f, q);
    c.a1 = 1.0f / (1.0f + g * (g + c.k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
  }

  /// Sets the coefficients for tick(): centre frequency (Hz) and resonance Q,
  /// clamped exactly as process() clamps them.
  void set(float cutoff_hz, float q) noexcept { coeffs_ = coefficients(cutoff_hz, q); }

  /// Processes one sample with the coefficients of the last set(); the output
  /// is the unity-peak bandpass, as in process(), or the low-pass tap with @p lowpass.
  float tick(float input, bool lowpass = false) noexcept { return tick(input, coeffs_, lowpass); }

  /// Processes one sample with explicit coefficients, leaving the stored ones untouched.
  float tick(float input, const Coefficients& c, bool lowpass = false) noexcept {
    const float v3 = input - ic2_;
    const float v1 = c.a1 * ic1_ + c.a2 * v3;
    const float v2 = ic2_ + c.a2 * ic1_ + c.a3 * v3;
    ic1_ = 2.0f * v1 - ic1_;
    ic2_ = 2.0f * v2 - ic2_;
    if (lowpass) return v2;
    // v1 is the bandpass state; multiply by k so the resonant peak reaches unity.
    return c.k * v1;
  }

 private:
  double sample_rate_ = sonare::constants::kDefaultDawSampleRate;
  Coefficients coeffs_;
  float ic1_ = 0.0f;
  float ic2_ = 0.0f;
};

}  // namespace sonare::effects::modulation
