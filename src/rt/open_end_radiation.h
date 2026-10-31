#pragma once

/// @file open_end_radiation.h
/// @brief Open-end termination of an acoustic tube. One physical boundary with
///        two outputs -- what returns into the tube and what leaves it. A caller
///        taking only one of the two is not modelling a boundary.
///
/// This is the low-flare case: one pole, one loss. Wavelengths below the cutoff
/// reflect inverted (pressure release), those above it leave, and the radiated
/// part is the complement `incoming + reflected`, so the two outputs cannot
/// drift apart. The wind voices spell their terminations with brightness-driven
/// ramped reflection and an independently tuned radiation pole, which this type
/// deliberately cannot express -- folding them in would lose the coupling, not
/// share it.
///
/// RT contract: process() is allocation-free and branch-free once configured.

#include <algorithm>
#include <limits>

#include "rt/biquad_design.h"

namespace sonare::rt {

class OpenEndRadiation {
 public:
  /// @param cutoff_hz Flare cutoff: wavelengths below it reflect, above it radiate.
  /// @param loss      Broadband loss at the termination in [0,1]; 0 = lossless.
  void configure(double sample_rate, float cutoff_hz, float loss) noexcept {
    lp_alpha_ = std::max(one_pole_lowpass_alpha_matched(cutoff_hz, sample_rate),
                         std::numeric_limits<float>::min());
    loss_ = std::clamp(loss, 0.0f, 1.0f);
  }

  struct Output {
    float reflected;  ///< Back into the tube, inverted (open end).
    float radiated;   ///< Out of the instrument.
  };

  /// One sample of the wave arriving at the termination. The reflection's
  /// magnitude never exceeds `1 - loss` at any frequency, so the boundary is passive.
  Output process(float incoming) noexcept {
    lp_state_ += lp_alpha_ * (incoming - lp_state_);
    const float reflected = -(1.0f - loss_) * lp_state_;
    return {reflected, incoming + reflected};
  }

  /// Energy held in the pole, `(1 - alpha) / alpha * state^2` in the incoming
  /// wave's squared units: with it, what returns plus what is stored never
  /// exceeds what arrived, so a coupled system can count the boundary's share.
  float stored_energy() const noexcept {
    return (1.0f - lp_alpha_) / lp_alpha_ * lp_state_ * lp_state_;
  }

  void reset() noexcept { lp_state_ = 0.0f; }

 private:
  float lp_state_ = 0.0f;
  float lp_alpha_ = 1.0f;
  float loss_ = 0.0f;
};

}  // namespace sonare::rt
