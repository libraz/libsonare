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
    const double rate = sample_rate > 0.0 ? sample_rate : 48000.0;
    const double fc = std::clamp(static_cast<double>(cutoff_hz), 10.0, kMaxCutoffRatio * rate);
    const double g = std::tan(::sonare::constants::kPiD * fc / rate);
    const double k = 1.0 / std::max(0.5, static_cast<double>(q));
    const double d = 1.0 + g * (g + k);
    rt::TailBudget tail;
    tail.biquad(2.0, 2.0 * (g * g - 1.0) / d, (1.0 - g * k + g * g) / d);
    return tail;
  }

  void prepare(double sample_rate) noexcept {
    sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
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
    const float fc = std::clamp(cutoff_hz, 10.0f, max_cutoff_hz(sample_rate_));
    const float g = std::tan(static_cast<float>(::sonare::constants::kPiD) * fc /
                             static_cast<float>(sample_rate_));
    const float k = 1.0f / std::max(0.5f, q);
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;
    const float v3 = input - ic2_;
    const float v1 = a1 * ic1_ + a2 * v3;
    const float v2 = ic2_ + a2 * ic1_ + a3 * v3;
    ic1_ = 2.0f * v1 - ic1_;
    ic2_ = 2.0f * v2 - ic2_;
    if (lowpass) return v2;
    // v1 is the bandpass state; multiply by k so the resonant peak reaches unity.
    return k * v1;
  }

  /// Sets the coefficients for tick(): centre frequency (Hz) and resonance Q,
  /// clamped exactly as process() clamps them.
  void set(float cutoff_hz, float q) noexcept {
    const float fc = std::clamp(cutoff_hz, 10.0f, max_cutoff_hz(sample_rate_));
    const float g = std::tan(static_cast<float>(::sonare::constants::kPiD) * fc /
                             static_cast<float>(sample_rate_));
    k_ = 1.0f / std::max(0.5f, q);
    a1_ = 1.0f / (1.0f + g * (g + k_));
    a2_ = g * a1_;
    a3_ = g * a2_;
  }

  /// Processes one sample with the coefficients of the last set(); the output
  /// is the unity-peak bandpass, as in process().
  float tick(float input) noexcept {
    const float v3 = input - ic2_;
    const float v1 = a1_ * ic1_ + a2_ * v3;
    const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
    ic1_ = 2.0f * v1 - ic1_;
    ic2_ = 2.0f * v2 - ic2_;
    return k_ * v1;
  }

 private:
  double sample_rate_ = 48000.0;
  float k_ = 1.0f;
  float a1_ = 0.0f;
  float a2_ = 0.0f;
  float a3_ = 0.0f;
  float ic1_ = 0.0f;
  float ic2_ = 0.0f;
};

}  // namespace sonare::effects::modulation
