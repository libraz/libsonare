#pragma once

/// @file excitation_axes.h
/// @brief The synthesis-method tag and the excitation axes each method accepts.
///
/// A mod route reaches an engine's own sound generation through
/// ModDestination::kExcitationForce / kExcitationPosition /
/// kExcitationBrightness / kSpectrumMorph. Every engine that owns such an axis
/// takes the same three calls — set_excitation_base(), set_excitation_mod(),
/// snap_excitation() — so the voice dispatches one line per mode rather than a
/// per-engine spelling, and an engine reads only the axes its mask names.
///
/// The accept set is declared, never inferred. engine_axis_capability() carries
/// one row per SynthEngineMode and the switch behind it has no `default:`, so
/// adding a mode without declaring its axes fails to compile; a mode declared
/// continuous with an empty mask fails a static_assert. The tag lives here
/// rather than beside the patch struct because the row and the tag are one
/// fact, and a table keyed on an enum declared elsewhere is the parallel array
/// this design exists to avoid.

#include <cstdint>

namespace sonare::midi::synth {

/// Synthesis method tag. Every mode is implemented.
enum class SynthEngineMode : int {
  kSubtractive = 0,
  kFm = 1,              // operator-stack FM (fm_voice.h)
  kKarplusStrong = 2,   // plucked-string waveguide (ks_voice.h)
  kModal = 3,           // resonator-bank mallets/bells (modal_voice.h)
  kAdditive = 4,        // drawbar organ (additive_voice.h)
  kPercussion = 5,      // membrane modal + filtered noise (percussion_voice.h)
  kPiano = 6,           // extended waveguide piano (piano_voice.h)
  kPipeOrgan = 7,       // sustained waveguide flue pipe (pipe_organ_voice.h)
  kBowedString = 8,     // sustained waveguide bowed string (bowed_string_voice.h)
  kReed = 9,            // sustained waveguide reed woodwind (reed_voice.h)
  kBrass = 10,          // sustained waveguide brass / lip reed (brass_voice.h)
  kFlute = 11,          // sustained waveguide air-jet flute (flute_voice.h)
  kPluckedString = 12,  // buzzing-bridge plucked string (plucked_string_voice.h)
  kVocal = 13,          // source-filter glottal + formant voice (vocal_voice.h)
  kFreeReed = 14,       // driven free-reed accordion / harmonica (free_reed_voice.h)
  kHarpsichord = 15,    // jack-and-plectrum string choirs (harpsichord_voice.h)
  kSample = 16,         // host-supplied PCM through the subtractive chain (sample_voice.h)
};

/// Highest SynthEngineMode ordinal; the capability walk below runs to it.
inline constexpr int kSynthEngineModeMax = static_cast<int>(SynthEngineMode::kSample);

/// One offset per engine-owned axis, in the normalized units of the patch field
/// the axis moves. Used both for the base a patch or a CC sets and for the
/// mod-matrix offset composed on top of it; an engine ignores the fields its
/// mask does not name.
struct ExcitationAxes {
  /// Drive into the exciter: bow force, mouth pressure, bellows pressure.
  float force = 0.0f;
  /// Where the exciter meets the resonator (bowed string only).
  float position = 0.0f;
  /// Timbral opening of the radiating end, held apart from loudness.
  float brightness = 0.0f;
  /// Registration morph, for an engine whose spectrum is drawn rather than
  /// excited.
  float morph = 0.0f;
};

/// Which ExcitationAxes fields an engine reads. Also the `present` argument to
/// set_excitation_base(), where it says which fields the caller filled: the
/// base overrides a patch value, so an axis whose controller has not arrived
/// must be left alone rather than written as zero.
enum ExcitationAxisMask : uint32_t {
  kAxisNone = 0u,
  kAxisForce = 1u << 0,
  kAxisPosition = 1u << 1,
  kAxisBrightness = 1u << 2,
  kAxisMorph = 1u << 3,
};

/// What a mod route can reach inside one engine.
struct EngineAxisCapability {
  /// The engine models an exciter that keeps acting for as long as the note
  /// sounds, so a control moved mid-note reaches the sound generation itself.
  /// False for a struck or plucked exciter, which is finished before the second
  /// sample renders, and false for an engine with no exciter at all — a
  /// subtractive or FM stack sustains through its VCA, and the matrix reaches it
  /// through the generic destinations. An engine that says true must name at
  /// least one axis; the static_assert below is what keeps that pairing.
  bool continuous;
  /// Union of ExcitationAxisMask bits. kAxisNone declines every axis.
  uint32_t mask;
};

/// The accept set, one row per mode. No `default:` label: the exhaustiveness
/// warning is the whole mechanism, and a mode that declines every axis says so
/// with its own case rather than falling through.
constexpr EngineAxisCapability engine_axis_capability(SynthEngineMode mode) noexcept {
  switch (mode) {
    // Struck, plucked or non-physical: nothing per sample to reach.
    case SynthEngineMode::kSubtractive:
    case SynthEngineMode::kFm:
    case SynthEngineMode::kKarplusStrong:
    case SynthEngineMode::kModal:
    case SynthEngineMode::kPercussion:
    case SynthEngineMode::kPiano:
    case SynthEngineMode::kPluckedString:
    case SynthEngineMode::kHarpsichord:
    case SynthEngineMode::kSample:
      return {false, kAxisNone};
    // Drawbar tonewheels sustain but are not excited: what a route moves is the
    // registration, so the morph axis stands in for the exciter axes.
    case SynthEngineMode::kAdditive:
      return {false, kAxisMorph};
    case SynthEngineMode::kPipeOrgan:
      return {true, kAxisForce | kAxisBrightness};
    case SynthEngineMode::kBowedString:
      return {true, kAxisForce | kAxisPosition};
    case SynthEngineMode::kReed:
      return {true, kAxisForce | kAxisBrightness};
    case SynthEngineMode::kBrass:
      return {true, kAxisForce | kAxisBrightness};
    case SynthEngineMode::kFlute:
      return {true, kAxisForce | kAxisBrightness};
    case SynthEngineMode::kFreeReed:
      return {true, kAxisForce | kAxisBrightness};
    // Brightness only. Force has no target that is not already the brightness
    // tilt or the VCA, and modelling the real coupling would raise F0 with
    // pressure, which a note sounding at its note number cannot do.
    case SynthEngineMode::kVocal:
      return {true, kAxisBrightness};
  }
  return {false, kAxisNone};
}

/// True when the engine reads @p axis.
constexpr bool engine_accepts_axis(SynthEngineMode mode, ExcitationAxisMask axis) noexcept {
  return (engine_axis_capability(mode).mask & static_cast<uint32_t>(axis)) != 0u;
}

namespace excitation_detail {

/// A continuously excited engine that names no axis is an engine wired to
/// nothing — the defect this header exists to make impossible.
constexpr bool every_continuous_engine_names_an_axis() noexcept {
  for (int i = 0; i <= kSynthEngineModeMax; ++i) {
    const EngineAxisCapability cap = engine_axis_capability(static_cast<SynthEngineMode>(i));
    if (cap.continuous && cap.mask == kAxisNone) return false;
  }
  return true;
}

}  // namespace excitation_detail

static_assert(excitation_detail::every_continuous_engine_names_an_axis(),
              "a continuously excited engine must name at least one excitation axis");

}  // namespace sonare::midi::synth
