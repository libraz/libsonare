#pragma once

/// @file acoustic_tube.h
/// @brief Scattering-junction waveguide over a piecewise-constant area function
///        (Kelly-Lochbaum), with the section count fixed at compile time and the
///        physical length a runtime quantity.
///
/// Each section is half a sample long each way (`c / (2 fs)`). The chain runs as
/// a normalized ladder -- waves scaled by the square root of their section's area
/// and every junction an orthogonal rotation -- so a junction conserves energy
/// for any area, including areas that change between samples. The forward pass
/// carries no delay and each section's whole round trip sits in its backward
/// state; the transfer function is the half-sample tube's up to a pure delay.
///
/// NOT the model the wind voices use: they are single-delay-line waveguides with
/// a host-attached bore span (brass_voice.h names Smith/Cook), and a
/// near-cylindrical bore makes every scattering coefficient here zero. The area
/// function earns its cost where the profile IS the sound: vowels are area functions.

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "rt/fractional_delay.h"
#include "util/constants.h"

namespace sonare::midi::synth {

/// Half-width of the raised-cosine dip set_constriction() draws, in metres: a
/// tongue or lip constriction spans a few centimetres of the tract.
constexpr float kTubeConstrictionHalfWidthM = 0.02f;

/// kSections is sized for the LONGEST supported tract at the HIGHEST supported
/// rate; junctions past the active length run with zero reflection on silent
/// state rather than being skipped, so the per-sample cost is constant and equal
/// to the worst case. The state lives in member arrays rather than a
/// host-attached slab, unlike the wind voices: their slab exists because a bore
/// is sized for the lowest fundamental they support (reed_voice.h:
/// contrabassoon), while a tract is a fixed hundred sections.
template <int kSections>
class AcousticTube {
  static_assert(kSections >= 4, "a tube needs at least two active sections plus the mouth delay");

 public:
  /// Floor on a section's relative area. A syllable onset drives a section
  /// toward it, where the junction's reflection approaches 1; the near-closed
  /// case is a supported operating point, not a degenerate input.
  static constexpr float kMinArea = 1.0e-3f;

  /// @param damping Wall loss in nepers per metre of travel; 0 = lossless. The
  ///                per-section factor is built here from the section length, so
  ///                a tube decays the same in seconds at every rate.
  void configure(double sample_rate, float damping) noexcept {
    section_m_ = ::sonare::constants::kSoundSpeedMps / static_cast<float>(2.0 * sample_rate);
    section_loss_ = std::exp(-std::max(damping, 0.0f) * 2.0f * section_m_);
    std::fill(profile_, profile_ + kSections, 1.0f);
    constriction_position_ = 0.0f;
    constriction_degree_ = 0.0f;
    reset();
    active_ = 0;
    set_length(max_length());
  }

  /// Longest length the tube holds at the configured rate, in metres.
  float max_length() const noexcept {
    return (static_cast<float>(kSections) + kMouthMaxDelay) * section_m_;
  }

  /// Acoustic length from source to termination, in metres, clamped to what the
  /// tube holds. The sub-section remainder is a fractional delay
  /// (rt/fractional_delay.h) on the wave entering from the mouth, so the length
  /// is continuous.
  ///
  /// A per-block setter rather than a configure() argument: a player moves this
  /// while the note sounds, and neither a fingering nor a singer can afford a
  /// model rebuild. The mouth delay line and the sections carry the same
  /// backward wave, so crossing a section boundary hands one sample of it from
  /// one to the other: a leaving section's wave is read again from the delay
  /// line one sample older, and a joining section is seeded from the delay line
  /// at the age it would have had. Nothing is dropped or repeated.
  void set_length(float length_m) noexcept {
    const float min_m = (kMinActive + kMouthMinDelay) * section_m_;
    length_m_ = std::clamp(length_m, min_m, max_length() - section_m_ * kLengthHeadroom);
    // Round trip = sections + the one-sample mouth reflection + the fractional delay in [1, 2).
    const float round_trip = length_m_ / section_m_;
    const int active =
        std::clamp(static_cast<int>(std::floor(round_trip)) - 2, kMinActive, kSections);
    const float mouth_delay = round_trip - 1.0f - static_cast<float>(active);
    mouth_delay_q8_ = static_cast<int>(std::lround(mouth_delay * 256.0f));
    const int previous = active_;
    for (int i = active; i < kSections; ++i) back_[i] = 0.0f;
    for (int i = previous; i < active && previous > 0; ++i) {
      // Age of section i's wave behind the newest mouth sample, at the new split.
      back_[i] = section_loss_ * mouth_history(mouth_delay + static_cast<float>(active - 1 - i));
    }
    active_ = active;
    rebuild();
  }

  /// Constriction position (0 = source end, 1 = termination) and degree
  /// (0 = open, 1 = as closed as kMinArea allows), drawn over the area profile.
  /// The two axes a singer holds, and what a syllable onset moves transiently.
  void set_constriction(float position, float degree) noexcept {
    constriction_position_ = std::clamp(position, 0.0f, 1.0f);
    constriction_degree_ = std::clamp(degree, 0.0f, 1.0f);
    rebuild();
  }

  /// @param areas Relative area profile, source-to-mouth, sampled at kSections
  ///              evenly spaced points along the CURRENT length. Only ratios
  ///              between neighbours set the scattering. Resampled onto the
  ///              active sections, so one profile is one tract at every rate.
  void set_areas(const float (&areas)[kSections]) noexcept {
    for (int i = 0; i < kSections; ++i) profile_[i] = std::max(areas[i], kMinArea);
    rebuild();
  }

  /// The pressure wave arriving back at the source this sample, before process().
  /// An implicit source junction solves against it.
  float source_incoming() const noexcept { return back_[0] / root_area_[0]; }

  /// @param input            Pressure wave launched into the source end.
  /// @param mouth_reflection What the termination sent back from the previous sample's arrival.
  /// @return The pressure wave arriving at the termination.
  float process(float input, float mouth_reflection) noexcept {
    float forward = input * root_area_[0];
    float arriving = 0.0f;
    for (int i = 0; i < kSections - 1; ++i) {
      arriving += forward * end_mask_[i];
      const float gated = forward * gate_[i];
      const float backward = back_[i + 1];
      back_[i] = section_loss_ * (k_[i] * gated + c_[i] * backward);
      forward = c_[i] * gated - k_[i] * backward;
    }
    arriving += forward * end_mask_[kSections - 1];
    const int last = active_ - 1;
    const float entering = ::sonare::rt::lagrange3_fractional_delay(
        mouth_, kMouthTaps, mouth_write_, mouth_delay_q8_, mouth_reflection * root_area_[last]);
    back_[last] = section_loss_ * entering;
    return arriving / root_area_[last];
  }

  /// Stored acoustic energy in area-normalized units: the sections' backward
  /// state plus the mouth samples still in transit, the one the delay straddles
  /// counted by its fraction. The passivity test needs the COUPLED system's
  /// energy, so the tube reports its share.
  float stored_energy() const noexcept {
    float energy = 0.0f;
    for (int i = 0; i < kSections; ++i) energy += back_[i] * back_[i];
    const float delay = static_cast<float>(mouth_delay_q8_) / 256.0f;
    for (int age = 0; static_cast<float>(age) < delay; ++age) {
      const float s = mouth_history(static_cast<float>(age));
      energy += std::min(1.0f, delay - static_cast<float>(age)) * s * s;
    }
    return energy;
  }

  void reset() noexcept {
    std::fill(back_, back_ + kSections, 0.0f);
    std::fill(mouth_, mouth_ + kMouthTaps, 0.0f);
    mouth_write_ = 0;
  }

  /// Number of sections inside the current length (the rest carry silence).
  int active_sections() const noexcept { return active_; }
  /// Relative area at the termination; a pressure p there carries energy area * p^2.
  float mouth_area() const noexcept { return area_[active_ - 1]; }
  /// Length of one section at the configured rate, in metres.
  float section_length() const noexcept { return section_m_; }

 private:
  static constexpr int kMinActive = 2;
  static constexpr float kMouthMinDelay = 1.0f;
  static constexpr float kMouthMaxDelay = 3.0f;
  /// Keeps the mouth delay strictly under two samples at the longest length.
  static constexpr float kLengthHeadroom = 0.01f;
  static constexpr std::size_t kMouthTaps = 8;

  /// Linear read of the mouth delay line @p age samples behind its newest sample; 0 past its end.
  float mouth_history(float age) const noexcept {
    const int whole = static_cast<int>(std::floor(age));
    if (whole < 0 || whole + 1 >= static_cast<int>(kMouthTaps)) return 0.0f;
    const float frac = age - static_cast<float>(whole);
    const auto at = [&](int a) {
      return mouth_[(mouth_write_ + kMouthTaps - 1 - static_cast<std::size_t>(a)) % kMouthTaps];
    };
    return (1.0f - frac) * at(whole) + frac * at(whole + 1);
  }

  void rebuild() noexcept {
    const float centre_m = constriction_position_ * length_m_;
    for (int i = 0; i < kSections; ++i) {
      const int section = std::min(i, active_ - 1);
      const float x_m = (static_cast<float>(section) + 0.5f) * section_m_;
      const float t = std::min(x_m / length_m_, 1.0f) * static_cast<float>(kSections - 1);
      const int lo = std::min(static_cast<int>(t), kSections - 2);
      const float frac = t - static_cast<float>(lo);
      float area = profile_[lo] + frac * (profile_[lo + 1] - profile_[lo]);
      const float dist = std::fabs(x_m - centre_m);
      if (dist < kTubeConstrictionHalfWidthM) {
        const float w =
            0.5f * (1.0f + std::cos(::sonare::constants::kPi * dist / kTubeConstrictionHalfWidthM));
        area *= 1.0f - constriction_degree_ * w;
      }
      area_[i] = std::max(area, kMinArea);
      root_area_[i] = std::sqrt(area_[i]);
    }
    for (int i = 0; i < kSections; ++i) {
      const bool interior = i < active_ - 1;
      const float a = area_[i];
      const float b = interior ? area_[i + 1] : a;
      k_[i] = (a - b) / (a + b);
      c_[i] = std::sqrt(std::max(0.0f, 1.0f - k_[i] * k_[i]));
      gate_[i] = interior ? 1.0f : 0.0f;
      end_mask_[i] = i == active_ - 1 ? 1.0f : 0.0f;
    }
  }

  float section_m_ = 1.0f;
  float section_loss_ = 1.0f;
  float length_m_ = 0.0f;
  int active_ = kSections;
  int mouth_delay_q8_ = 256;
  float constriction_position_ = 0.0f;
  float constriction_degree_ = 0.0f;
  float profile_[kSections] = {};
  float area_[kSections] = {};
  float root_area_[kSections] = {};
  float k_[kSections] = {};
  float c_[kSections] = {};
  float gate_[kSections] = {};
  float end_mask_[kSections] = {};
  float back_[kSections] = {};
  /// Waves entering from the mouth, area-normalized; the fractional delay's line.
  float mouth_[kMouthTaps] = {};
  std::size_t mouth_write_ = 0;
};

}  // namespace sonare::midi::synth
