#pragma once

/// @file svf.h
/// @brief Topology-preserving-transform (TPT / zero-delay-feedback) state
///        variable filter with simultaneous LP/BP/HP outputs.
///
/// The existing filters/iir.h RBJ biquads are designed for FIXED cutoffs
/// (coefficient design per setting). Voice filters sweep their cutoff every
/// few samples (envelope / LFO -> Fc); the TPT SVF stays stable and zipper-free
/// under such audio-rate modulation and self-oscillates cleanly at high
/// resonance, which is why it is the toolkit's modulated-cutoff filter.
/// Reference: Zavalishin, "The Art of VA Filter Design" (TPT SVF).
///
/// RT contract: all methods are allocation-free; set() may be called per-sample
/// and designs the three coefficients (one tan()) only when the clamped cutoff
/// or Q differs from the last design at the current rate.

#include <algorithm>
#include <cmath>

#include "rt/tail_budget.h"
#include "util/constants.h"

namespace sonare::midi::synth {

class TptSvf {
 public:
  struct Outputs {
    float lp = 0.0f;
    float bp = 0.0f;
    float hp = 0.0f;
  };

  /// Conservative decay bound for the bandpass state at a fixed setting.
  ///
  /// Build the denominator from the live float state transition. The bandpass
  /// input/output taps have an absolute sum bounded by two.
  static rt::TailBudget ring(float cutoff_hz, float q, double sample_rate) noexcept {
    const double rate = sample_rate > 0.0 ? sample_rate : constants::kDefaultDawSampleRate;
    // Match set()'s float clamps and arithmetic before converting the matrix.
    const float rate_f = static_cast<float>(rate);
    const float fc = std::clamp(cutoff_hz, 10.0f, static_cast<float>(0.49 * rate));
    const float k = 1.0f / std::clamp(q, 0.5f, 100.0f);
    const float g = std::tan(constants::kPi * fc / rate_f);
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;
    // process() advances [ic1, ic2] by this matrix once its input is zero.
    const float m00 = 2.0f * a1 - 1.0f;
    const float m01 = -2.0f * a2;
    const float m10 = 2.0f * a2;
    const float m11 = 1.0f - 2.0f * a3;
    const double trace = static_cast<double>(m00) + static_cast<double>(m11);
    const double determinant = static_cast<double>(m00) * static_cast<double>(m11) -
                               static_cast<double>(m01) * static_cast<double>(m10);
    rt::TailBudget tail;
    tail.biquad(2.0, -trace, determinant);
    return tail;
  }

  void prepare(double sample_rate) noexcept {
    sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
    designed_ = false;
    set(cutoff_hz_, q_);
    reset();
  }

  /// Set cutoff (Hz, clamped to [10, 0.49 * sample_rate]) and resonance Q
  /// (clamped to [0.5, 100]; Q > ~20 approaches self-oscillation).
  void set(float cutoff_hz, float q) noexcept {
    const float cutoff = std::clamp(cutoff_hz, 10.0f, static_cast<float>(0.49 * sample_rate_));
    const float clamped_q = std::clamp(q, 0.5f, 100.0f);
    if (designed_ && cutoff == cutoff_hz_ && clamped_q == q_) return;
    cutoff_hz_ = cutoff;
    q_ = clamped_q;
    designed_ = true;
    ++design_count_;
    const float g = std::tan(constants::kPi * cutoff_hz_ / static_cast<float>(sample_rate_));
    k_ = 1.0f / q_;
    a1_ = 1.0f / (1.0f + g * (g + k_));
    a2_ = g * a1_;
    a3_ = g * a2_;
  }

  float cutoff_hz() const noexcept { return cutoff_hz_; }
  float q() const noexcept { return q_; }
  /// Number of coefficient designs performed since construction.
  unsigned design_count() const noexcept { return design_count_; }

  void reset() noexcept {
    ic1_ = 0.0f;
    ic2_ = 0.0f;
  }

  /// Puts the state at the steady state of a constant input @p x (ic1 = 0, ic2 = x), so the
  /// next process(x) returns lp = x, bp = 0, hp = 0 with no transient.
  void settle(float x) noexcept {
    ic1_ = 0.0f;
    ic2_ = x;
  }

  /// Advance one sample; returns the simultaneous LP/BP/HP outputs.
  Outputs process(float x) noexcept {
    const float v3 = x - ic2_;
    const float v1 = a1_ * ic1_ + a2_ * v3;
    const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
    ic1_ = 2.0f * v1 - ic1_;
    ic2_ = 2.0f * v2 - ic2_;
    Outputs out;
    out.lp = v2;
    out.bp = v1;
    out.hp = x - k_ * v1 - v2;
    return out;
  }

 private:
  double sample_rate_ = 48000.0;
  float cutoff_hz_ = 1000.0f;
  float q_ = constants::kButterworthQ;
  float k_ = constants::kSqrt2;
  float a1_ = 0.0f;
  float a2_ = 0.0f;
  float a3_ = 0.0f;
  float ic1_ = 0.0f;
  float ic2_ = 0.0f;
  bool designed_ = false;
  unsigned design_count_ = 0;
};

}  // namespace sonare::midi::synth
