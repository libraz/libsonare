#pragma once

/// @file free_reed_voice.h
/// @brief Free-reed core for the NativeSynth voice — the accordion / harmonica /
///        bandoneon / reed-organ family (GM 21-23). A free reed is a thin metal
///        tongue that swings freely through a close-fitting slot under bellows
///        pressure; unlike the beating reed of a clarinet (kReed, a pressure-
///        controlled valve on a resonant bore) it has no coupled air column, so
///        its pitch is set by the tongue itself and its tone is a bright,
///        buzzy, harmonic-rich drone with a characteristic slightly asymmetric
///        waveform.
///
/// The model is a driven tongue oscillator (not a bore waveguide): a phase
/// accumulator at the note fundamental generates the tongue's motion, a shaper
/// turns it into the buzzy harmonic spectrum, and a fixed body lowpass (the
/// reed-plate / cavity radiation) colours it. Two slightly detuned tongues per
/// note give the shimmering "musette" beating of an accordion. Because the
/// source is a feed-forward oscillator (no acoustic feedback loop), the model is
/// unconditionally stable and owns no host delay slab.
///
/// The shaper comes in two forms. The default is a soft-clipped asymmetric saw,
/// whose ladder falls monotonically from the fundamental and cannot be made to
/// do anything else. `slot_duty` swaps in the slot's own geometry — the tongue
/// swings through and lets air past twice per cycle, so the source is a pair of
/// flow humps — and radiates it, which is what puts the measured free reed's
/// second partial above its first and its null at the seventh.
///
/// RT contract: start()/render() are allocation-free. Determinism: the breath
/// noise is the counter-based (voice_index, note, age) stream, so identical
/// event streams render bit-identically.

#include <cstdint>

#include "midi/synth/voice_random.h"

namespace sonare::midi::synth {

/// Free-reed section of a NativeSynthPatch (used when mode == kFreeReed).
struct FreeReedPatchParams {
  /// Body/roll-off brightness in [0,1]: how much high harmonic content the reed
  /// plate radiates (0 = a mellow reed organ, 1 = a bright buzzy harmonica).
  float brightness = 0.6f;
  /// Reed stiffness in [0,1]: shapes the asymmetry / harmonic richness of the
  /// tongue nonlinearity (softer = rounder, stiffer = buzzier).
  float reed_stiffness = 0.5f;
  /// Steady bellows pressure in [0,1]: the dynamic level / drive.
  float breath_pressure = 0.7f;
  /// Note velocity -> bellows pressure in [0,1].
  float vel_to_breath = 0.5f;
  /// Musette detune in [0,1]: the beating between the two tongues per note
  /// (0 = a single tongue, no beating; higher = a wider wet-tuned accordion).
  float detune = 0.3f;
  /// Onset rise (ms): the bellows takes up before the reed speaks.
  float attack_ms = 20.0f;
  /// Release fall (ms): the bellows releases and the reed stops.
  float release_ms = 80.0f;
  /// Breath/air noise in [0,1]: the leakage air hiss around the reed.
  float breath_noise = 0.08f;
  /// Slot opening duty: the fraction of the period the tongue's first pass
  /// holds the slot open. The shipped shaper is a soft-clipped asymmetric saw,
  /// whose ladder falls monotonically and sits 10 to 16 dB under the measured
  /// references' by the sixth partial; no setting of the knobs above changes
  /// that, because a saw through a saturator has no other shape. This replaces
  /// it with the flow the slot actually passes, and the duty is what places the
  /// null the references all carry near the seventh partial. 0 = off -> the saw
  /// is used and the render is bit-identical.
  float slot_duty = 0.0f;
  /// The tongue's return pass through the slot, relative to the first (only
  /// when slot_duty > 0). 0 = one opening per cycle.
  float slot_return = 0.4f;
  /// Where the return pass falls inside the period (only when slot_duty > 0).
  /// Sets how much even-harmonic content the pair of humps carries.
  float slot_gap = 0.14f;
  /// How much of a free monopole the reed plate is (only when slot_duty > 0):
  /// 0 radiates the flow itself, 1 its time derivative, the +6 dB/octave tilt a
  /// small source radiates with. Normalised at the fundamental, so it is timbre
  /// only and moves no level. The harmonium and the bandoneon measure at 1, the
  /// harmonica — played inside the cupped hands — at 0.
  float radiation = 1.0f;
};

/// Per-voice free-reed state, embedded in NativeSynthVoice. The voice's
/// amplitude envelope / filter / mod matrix wrap around this core; render()
/// returns the raw reed sample.
class FreeReedVoiceCore {
 public:
  /// Configures the tongue oscillators for @p note / @p velocity and seeds the
  /// breath noise. @p seed drives the deterministic air hiss.
  void start(const FreeReedPatchParams& params, double sample_rate, uint8_t note, uint8_t velocity,
             uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (bend / drift / glide), 1 = on pitch.
  float render(float pitch_ratio) noexcept;
  /// Note-off: ramp the bellows pressure to zero over release_ms.
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

 private:
  double sample_rate_ = 48000.0;
  float base_freq_hz_ = 220.0f;

  // Two detuned tongue oscillators (phase accumulators). The second is skipped
  // when detune == 0 (bit-identical single tongue).
  float phase_a_ = 0.0f;
  float phase_b_ = 0.0f;
  float inc_a_ = 0.0f;
  float inc_b_ = 0.0f;
  bool dual_ = false;

  // Tongue nonlinearity shaping (asymmetry from reed_stiffness).
  float asymmetry_ = 0.0f;
  float drive_ = 1.0f;

  // Slot flow (gated): 0 duty keeps the asymmetric saw above.
  float slot_duty_ = 0.0f;
  float slot_return_ = 0.0f;
  float slot_return_width_ = 0.0f;
  float slot_gap_ = 0.0f;
  float slot_mean_ = 0.0f;
  float radiation_ = 0.0f;
  float radiation_norm_ = 0.0f;
  float prev_flow_ = 0.0f;

  // Body lowpass (reed-plate / cavity radiation): a one-pole roll-off.
  float body_alpha_ = 1.0f;
  float body_state_ = 0.0f;

  // Bellows level contour: a one-pole ramp toward the target (1 while blowing,
  // 0 once released).
  float level_target_ = 1.0f;
  float level_ = 0.0f;
  float attack_coeff_ = 0.0f;
  float release_coeff_ = 0.0f;
  bool releasing_ = false;

  // Breath/air noise.
  float breath_noise_ = 0.0f;
  VoiceRandomSequence noise_;
  uint64_t drive_index_ = 0;

  /// Output trim bringing the raw reed sample up to a musical voice level.
  float output_scale_ = 1.0f;
};

}  // namespace sonare::midi::synth
