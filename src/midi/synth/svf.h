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
/// RT contract: all methods are allocation-free; set() recomputes the three
/// coefficients with one tan() and may be called per-sample.

#include <algorithm>
#include <cmath>

namespace sonare::midi::synth {

class TptSvf {
 public:
  struct Outputs {
    float lp = 0.0f;
    float bp = 0.0f;
    float hp = 0.0f;
  };

  void prepare(double sample_rate) noexcept {
    sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
    set(cutoff_hz_, q_);
    reset();
  }

  /// Set cutoff (Hz, clamped to [10, 0.49 * sample_rate]) and resonance Q
  /// (clamped to [0.5, 100]; Q > ~20 approaches self-oscillation).
  void set(float cutoff_hz, float q) noexcept {
    cutoff_hz_ = std::clamp(cutoff_hz, 10.0f, static_cast<float>(0.49 * sample_rate_));
    q_ = std::clamp(q, 0.5f, 100.0f);
    const float g =
        std::tan(3.14159265358979323846f * cutoff_hz_ / static_cast<float>(sample_rate_));
    k_ = 1.0f / q_;
    a1_ = 1.0f / (1.0f + g * (g + k_));
    a2_ = g * a1_;
    a3_ = g * a2_;
  }

  float cutoff_hz() const noexcept { return cutoff_hz_; }
  float q() const noexcept { return q_; }

  void reset() noexcept {
    ic1_ = 0.0f;
    ic2_ = 0.0f;
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
  float q_ = 0.70710678f;
  float k_ = 1.41421356f;
  float a1_ = 0.0f;
  float a2_ = 0.0f;
  float a3_ = 0.0f;
  float ic1_ = 0.0f;
  float ic2_ = 0.0f;
};

}  // namespace sonare::midi::synth
