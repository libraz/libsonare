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

#include "util/constants.h"

namespace sonare::effects::modulation {

/// Single-channel TPT state-variable filter, bandpass tap (Zavalishin form).
class SvfBandpass {
 public:
  void prepare(double sample_rate) noexcept {
    sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
    reset();
  }
  void reset() noexcept {
    ic1_ = 0.0f;
    ic2_ = 0.0f;
  }

  /// Process one sample with the given centre frequency (Hz) and resonance Q.
  /// The bandpass output is scaled to unity peak gain at resonance.
  float process(float input, float cutoff_hz, float q) noexcept {
    const float nyquist = static_cast<float>(sample_rate_ * 0.5);
    const float fc = std::clamp(cutoff_hz, 10.0f, 0.49f * nyquist * 2.0f);
    const float g = std::tan(static_cast<float>(::sonare::constants::kPiD) *
                             std::min(fc, 0.49f * nyquist) / static_cast<float>(sample_rate_));
    const float k = 1.0f / std::max(0.5f, q);
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;
    const float v3 = input - ic2_;
    const float v1 = a1 * ic1_ + a2 * v3;
    const float v2 = ic2_ + a2 * ic1_ + a3 * v3;
    ic1_ = 2.0f * v1 - ic1_;
    ic2_ = 2.0f * v2 - ic2_;
    // v1 is the bandpass state; multiply by k so the resonant peak reaches unity.
    return k * v1;
  }

 private:
  double sample_rate_ = 48000.0;
  float ic1_ = 0.0f;
  float ic2_ = 0.0f;
};

}  // namespace sonare::effects::modulation
