#pragma once

/// @file pressure_valve.h
/// @brief Single-mass pressure-driven valve: flow through a gap whose width is set
///        by a mass on a spring driven by the Bernoulli pressure difference across
///        it. Serves the reed (closes as pressure rises) and the lip (opens).
///
/// Units are normalized: pressure acts on unit area, the displacement is in the
/// same units as the rest opening, and the flow is `o * sgn(drop) * sqrt|drop|`
/// with `o` the opening over the rest opening. A zero mass is the quasi-static
/// valve, whose opening follows the drop within the sample (`o = 1 -+ X`); a
/// positive mass integrates by symplectic Euler, so the opening the junction
/// solves against is fixed by the state the sample starts from. Either way the
/// closing pressure `stiffness * rest_opening` is the pressure scale the
/// PassiveCoupling has to be configured with.

#include <algorithm>
#include <cmath>
#include <limits>

#include "midi/synth/passive_coupling.h"

namespace sonare::midi::synth {

/// Orientation is a compile-time policy, not a runtime branch: a branch in the
/// per-sample path costs more than the instantiations.
enum class ValveOrientation { kInward, kOutward };

template <ValveOrientation kOrientation>
class PressureValve {
 public:
  struct Config {
    float mass;             ///< 0 = quasi-static; otherwise raised to what the integrator holds.
    float stiffness;        ///< Spring per unit displacement.
    float damping;          ///< Damping ratio of the mass on its spring.
    float rest_opening;     ///< Gap at zero drop.
    float collision_ratio;  ///< Extra stiffness, over `stiffness`, once the gap is shut.
  };

  void configure(double sample_rate, const Config& config) noexcept {
    dt_ = static_cast<float>(1.0 / sample_rate);
    stiffness_ = std::max(config.stiffness, std::numeric_limits<float>::min());
    rest_ = std::max(config.rest_opening, std::numeric_limits<float>::min());
    collision_ = std::max(config.collision_ratio, 0.0f);
    mass_ = std::max(config.mass, 0.0f);
    resistance_ = 0.0f;
    if (mass_ > 0.0f) {
      const float omega_dt = kMaxOmegaDt;
      mass_ = std::max(mass_, stiffness_ * (1.0f + collision_) * dt_ * dt_ / (omega_dt * omega_dt));
      resistance_ = 2.0f * std::max(config.damping, 0.0f) * std::sqrt(stiffness_ * mass_);
    }
    reset();
  }

  /// The drop at which the quasi-static valve shuts; the coupling's pressure scale.
  float closing_pressure() const noexcept { return stiffness_ * rest_; }

  /// The opening law for the sample about to be solved.
  ValveLaw law() const noexcept {
    if (mass_ == 0.0f) return {1.0f, kCloseSign, kCeiling};
    return {(rest_ + y_) / rest_, 0.0f, kCeiling};
  }

  /// @param upstream   Driving pressure.
  /// @param downstream Pressure at the tube mouth (source-tract interaction).
  /// @return Volume flow through the gap, evaluated before the mass moves.
  float process(float upstream, float downstream) noexcept {
    const float drop = upstream - downstream;
    const float x = drop / closing_pressure();
    const float flow =
        PassiveCoupling::opening(x, law()) * std::copysign(std::sqrt(std::fabs(drop)), drop);
    if (mass_ > 0.0f) {
      const float gap = rest_ + y_;
      float force = -kCloseSign * drop - stiffness_ * y_ - resistance_ * v_;
      if (gap < 0.0f) force -= stiffness_ * collision_ * gap;
      v_ += dt_ * force / mass_;
      y_ += dt_ * v_;
    }
    return flow;
  }

  /// Kinetic plus spring plus collision energy of the mass; 0 when quasi-static.
  float stored_energy() const noexcept {
    const float gap = rest_ + y_;
    const float contact = gap < 0.0f ? stiffness_ * collision_ * gap * gap : 0.0f;
    return 0.5f * (mass_ * v_ * v_ + stiffness_ * y_ * y_ + contact);
  }

  void reset() noexcept {
    y_ = 0.0f;
    v_ = 0.0f;
  }

 private:
  /// Largest `omega * dt` kept: symplectic Euler is stable below 2, and 1 leaves margin.
  static constexpr float kMaxOmegaDt = 1.0f;
  /// +1 when a positive drop closes the gap (reed), -1 when it opens it (lip).
  static constexpr float kCloseSign = kOrientation == ValveOrientation::kInward ? 1.0f : -1.0f;
  /// The inward valve keeps the shipped reed's ceiling at the rest opening; an
  /// outward valve has none.
  static constexpr float kCeiling =
      kOrientation == ValveOrientation::kInward ? 1.0f : std::numeric_limits<float>::infinity();

  float dt_ = 0.0f;
  float mass_ = 0.0f;
  float stiffness_ = 1.0f;
  float resistance_ = 0.0f;
  float rest_ = 1.0f;
  float collision_ = 0.0f;
  float y_ = 0.0f;
  float v_ = 0.0f;
};

}  // namespace sonare::midi::synth
