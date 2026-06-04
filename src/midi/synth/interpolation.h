#pragma once

/// @file interpolation.h
/// @brief Sample / fractional-delay interpolation helpers for the voice
///        toolkit: linear and first-order allpass interpolators, plus a
///        position-based sample reader for pitched sample playback (SF2).
///
/// The 3rd-order Lagrange fractional delay already exists in
/// rt/fractional_delay.h (lagrange3_fractional_delay) and is reused as-is for
/// waveguide loops; this header only adds the cheaper linear / allpass forms
/// and the buffer-position reader the SF2 voice uses.
///
/// RT contract: everything here is allocation-free and header-only.

#include <cstddef>

namespace sonare::midi::synth {

/// Linear interpolation between two adjacent samples at fractional position
/// @p mu in [0,1).
inline float linear_interpolate(float y0, float y1, float mu) noexcept {
  return y0 + mu * (y1 - y0);
}

/// Reads @p data at fractional position @p pos with linear interpolation.
/// Out-of-range positions clamp to the buffer edges (no wrap) — loop handling
/// is the caller's job (the SF2 voice wraps `pos` before reading).
inline float read_sample_linear(const float* data, size_t size, double pos) noexcept {
  if (data == nullptr || size == 0) return 0.0f;
  if (pos <= 0.0) return data[0];
  const double last = static_cast<double>(size - 1);
  if (pos >= last) return data[size - 1];
  const size_t i0 = static_cast<size_t>(pos);
  const float mu = static_cast<float>(pos - static_cast<double>(i0));
  return linear_interpolate(data[i0], data[i0 + 1], mu);
}

/// First-order allpass fractional-delay interpolator (Thiran order 1).
/// Flat magnitude response (unlike linear interpolation's high-frequency
/// roll-off) at the cost of a state sample; suited to short modulated delays.
class AllpassInterpolator {
 public:
  /// @p mu is the fractional delay in [0,1). The classic stability/transient
  /// trade-off maps mu -> coefficient (1 - mu) / (1 + mu).
  void set_fraction(float mu) noexcept {
    if (mu < 0.0f) mu = 0.0f;
    if (mu > 0.999f) mu = 0.999f;
    coeff_ = (1.0f - mu) / (1.0f + mu);
  }

  void reset() noexcept {
    x1_ = 0.0f;
    y1_ = 0.0f;
  }

  float process(float x) noexcept {
    const float y = coeff_ * (x - y1_) + x1_;
    x1_ = x;
    y1_ = y;
    return y;
  }

 private:
  float coeff_ = 1.0f;
  float x1_ = 0.0f;
  float y1_ = 0.0f;
};

}  // namespace sonare::midi::synth
