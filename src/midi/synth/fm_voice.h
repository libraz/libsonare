#pragma once

/// @file fm_voice.h
/// @brief 2-4 operator FM (phase-modulation) core for the NativeSynth voice —
///        the e-piano / bell / brass / clav family (synthesis method (2) of
///        the instrument build plan; Chowning 1973).
///
/// What carries the realism beyond the textbook core:
///   - EXPONENTIAL operator envelopes (the shared DahdsrEnvelope): linear
///     index decay does not sound like an FM e-piano; the exponential index
///     fall-off is the DX-family signature.
///   - A feedback operator (self phase modulation, averaged over the last two
///     samples for stability) for brass / organ-like spectra.
///   - Velocity -> operator level scaling (velocity -> modulation index =
///     velocity -> brightness, not just amplitude).
///   - Key-rate scaling: higher notes decay faster (per-octave time scaling
///     of the decay/release stages).
///
/// The operator stack is driven by a small algorithm table (who modulates
/// whom, who is a carrier). Operators are evaluated from the highest index
/// down, so modulators always run before their consumers within one sample.
///
/// RT contract: start() only configures embedded state (no allocation);
/// render() is allocation-free. Determinism: operators start at phase 0, no
/// RNG anywhere.

#include <array>
#include <cstdint>

#include "midi/synth/envelope.h"

namespace sonare::midi::synth {

inline constexpr int kMaxFmOperators = 4;

/// One operator's patch parameters.
struct FmOperatorParams {
  /// Frequency ratio to the played note (1.0 = the note itself).
  float ratio = 1.0f;
  float detune_cents = 0.0f;
  /// Output level: modulation index (radians) when the operator modulates,
  /// linear gain when it is a carrier.
  float level = 0.0f;
  DahdsrConfig env;
  /// Velocity -> level amount in [0,1] (0 = level independent of velocity).
  float vel_to_level = 0.0f;
  /// Key-rate scaling in [0,1]: decay/release shorten by 2^(-krs * octaves
  /// above middle C).
  float key_rate_scale = 0.0f;
  /// Self phase-modulation depth (radians; brass/organ spectra).
  float feedback = 0.0f;
};

/// Operator wiring presets (modulator -> carrier graphs).
enum class FmAlgorithm : int {
  kStack2 = 0,   // op1 -> op0
  kStack3 = 1,   // op2 -> op1 -> op0
  kStack4 = 2,   // op3 -> op2 -> op1 -> op0
  kPair2x2 = 3,  // (op1 -> op0) + (op3 -> op2), two carriers
  kBright3 = 4,  // (op1 + op2) -> op0
  kAdd2 = 5,     // op0 + op1 both carriers (organ-ish additive pair)
};

/// FM section of a NativeSynthPatch (used when mode == kFm).
struct FmPatchParams {
  FmAlgorithm algorithm = FmAlgorithm::kStack2;
  std::array<FmOperatorParams, kMaxFmOperators> ops{};
};

/// Per-voice FM state, embedded in NativeSynthVoice. The voice's global
/// amplitude envelope / filter / mod matrix wrap around this core; render()
/// returns the raw operator-stack sample.
class FmVoiceCore {
 public:
  /// Configures the operators for @p note / @p velocity at @p sample_rate.
  void start(const FmPatchParams& params, double sample_rate, uint8_t note,
             uint8_t velocity) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (bend / vibrato / drift / glide), 1 = on pitch.
  float render(float pitch_ratio) noexcept;
  /// Note-off: release every operator envelope.
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

 private:
  struct Operator {
    double phase = 0.0;
    /// Phase increment at pitch_ratio == 1 (cycles per sample).
    float base_inc = 0.0f;
    float level = 0.0f;
    float feedback = 0.0f;
    DahdsrEnvelope env;
    /// Last two outputs (feedback average, DX-style).
    float prev1 = 0.0f;
    float prev2 = 0.0f;
  };

  std::array<Operator, kMaxFmOperators> ops_{};
  uint8_t mod_mask_[kMaxFmOperators] = {0, 0, 0, 0};
  uint8_t carrier_mask_ = 0x01;
  float carrier_norm_ = 1.0f;
};

}  // namespace sonare::midi::synth
