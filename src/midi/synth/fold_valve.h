#pragma once

/// @file fold_valve.h
/// @brief Two-mass vocal fold: two masses coupled by a shear spring, both driven
///        by the transglottal pressure, with a collision constraint at closure.
///
/// Two masses rather than one because the register transition this instrument
/// exists to produce is carried by the PHASE DIFFERENCE between them. A single
/// mass plus a tube produces only the source-tract jump at F0 crossing a
/// formant -- vowel-dependent, and the thing singers train to avoid.
///
/// The masses sit one above the other along the flow (Ishizaka & Flanagan 1972):
/// the lower one, the body share, takes the pressure the converging glottis leaves
/// on it; the upper one, the cover share, takes the supraglottal pressure. The
/// glottis opens wider at the bottom while opening and at the top while closing,
/// which is what feeds the oscillation (Steinecke & Herzel 1995). Units are the
/// coupling's at pressure scale 1: pressure on unit area, displacement in opening
/// units. Same method vocabulary as PressureValve; a sibling, not a specialisation.

#include <algorithm>
#include <cmath>

#include "midi/synth/passive_coupling.h"

namespace sonare::midi::synth {

class FoldValve {
 public:
  struct Config {
    /// Total fold mass; the note-independent anatomy scale.
    float mass;
    /// Fraction of `mass` in the cover layer, in [0.05, 0.95]. This is the
    /// register variable: a thick cover leans chest, a thin one leans falsetto.
    float cover_fraction;
    /// Shear coupling between body and cover. Sets how much phase the two can
    /// hold apart, and so how sharp the transition is.
    float coupling_stiffness;
    /// Longitudinal tension, common to both masses. THIS is what sets F0: each
    /// mass gets `tension * share` of stiffness, so both ring at sqrt(tension / mass).
    float tension;
    float damping;          ///< Damping ratio of each mass on its spring.
    float rest_opening;     ///< Glottal opening at rest, the adduction.
    float collision_ratio;  ///< Extra stiffness, over a mass's own, once it is in contact.
  };

  void configure(double sample_rate, const Config& config) noexcept {
    dt_ = static_cast<float>(1.0 / sample_rate);
    mass_ = std::max(config.mass, kMinMass);
    coupling_ = std::max(config.coupling_stiffness, 0.0f);
    damping_ = std::max(config.damping, 0.0f);
    rest_ = config.rest_opening;
    collision_ = std::max(config.collision_ratio, 0.0f);
    tension_ = std::max(config.tension, 0.0f);
    cover_ = std::clamp(config.cover_fraction, kMinCover, kMaxCover);
    rebuild();
    reset();
  }

  /// Per-sample update of the two variables a player moves continuously.
  /// Separated from configure() so the RT path never rebuilds the whole model.
  void set_tension(float tension) noexcept {
    tension_ = std::max(tension, 0.0f);
    rebuild();
  }
  void set_cover_fraction(float fraction) noexcept {
    cover_ = std::clamp(fraction, kMinCover, kMaxCover);
    rebuild();
  }

  /// The opening law for the sample about to be solved: the narrower of the two
  /// gaps, fixed by the state for the sample.
  ValveLaw law() const noexcept {
    return {std::max(std::min(rest_ + x_[0], rest_ + x_[1]), 0.0f), 0.0f};
  }

  /// @param subglottal   Lung pressure below the folds.
  /// @param supraglottal Pressure the tract presents above them.
  /// @return Volume flow through the narrower gap, evaluated before the masses move.
  float process(float subglottal, float supraglottal) noexcept {
    const float drop = subglottal - supraglottal;
    const float lower = rest_ + x_[0];
    const float upper = rest_ + x_[1];
    const float narrowest = std::min(lower, upper);
    const float flow = std::max(narrowest, 0.0f) * std::copysign(std::sqrt(std::fabs(drop)), drop);
    float pressure[2] = {subglottal, supraglottal};
    if (lower > 0.0f && upper <= 0.0f) {
      pressure[1] = subglottal;
    } else if (lower > 0.0f) {
      // The jet separates at the narrowest gap; below it the converging flow keeps part of the
      // drop.
      const float ratio = narrowest / lower;
      pressure[0] = supraglottal + drop * (1.0f - ratio * ratio);
    }
    const float gaps[2] = {lower, upper};
    float force[2];
    for (int i = 0; i < 2; ++i) {
      const float shear = coupling_ * (x_[i] - x_[1 - i]);
      force[i] = pressure[i] - stiffness_[i] * x_[i] - shear - resistance_[i] * v_[i];
      if (gaps[i] < 0.0f) force[i] -= stiffness_[i] * collision_ * gaps[i];
    }
    for (int i = 0; i < 2; ++i) {
      v_[i] += dt_ * force[i] / masses_[i];
      x_[i] += dt_ * v_[i];
    }
    return flow;
  }

  /// Kinetic, spring, shear and collision energy of the two masses.
  float stored_energy() const noexcept {
    const float shear = x_[0] - x_[1];
    float energy = coupling_ * shear * shear;
    for (int i = 0; i < 2; ++i) {
      const float gap = rest_ + x_[i];
      const float contact = gap < 0.0f ? stiffness_[i] * collision_ * gap * gap : 0.0f;
      energy += masses_[i] * v_[i] * v_[i] + stiffness_[i] * x_[i] * x_[i] + contact;
    }
    return 0.5f * energy;
  }

  /// Phase-difference observables: the two gaps for the coming sample.
  float lower_gap() const noexcept { return rest_ + x_[0]; }
  float upper_gap() const noexcept { return rest_ + x_[1]; }

  void reset() noexcept {
    x_[0] = x_[1] = 0.0f;
    v_[0] = v_[1] = 0.0f;
  }

 private:
  static constexpr float kMinCover = 0.05f;
  static constexpr float kMaxCover = 0.95f;
  static constexpr float kMinMass = 1.0e-12f;

  void rebuild() noexcept {
    masses_[0] = (1.0f - cover_) * mass_;
    masses_[1] = cover_ * mass_;
    for (int i = 0; i < 2; ++i) {
      stiffness_[i] = tension_ * masses_[i] / mass_;
      resistance_[i] = 2.0f * damping_ * std::sqrt(stiffness_[i] * masses_[i]);
    }
  }

  float dt_ = 0.0f;
  float mass_ = 1.0f;
  float cover_ = 0.5f;
  float tension_ = 0.0f;
  float coupling_ = 0.0f;
  float damping_ = 0.0f;
  float rest_ = 0.0f;
  float collision_ = 0.0f;
  float masses_[2] = {0.5f, 0.5f};
  float stiffness_[2] = {};
  float resistance_[2] = {};
  float x_[2] = {};
  float v_[2] = {};
};

}  // namespace sonare::midi::synth
