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

#include "midi/control_value.h"
#include "midi/synth/excitation_axes.h"
#include "midi/synth/voice_random.h"
#include "midi/synth/wind_breath.h"

namespace sonare::midi::synth {

/// Free-reed section of a NativeSynthPatch (used when mode == kFreeReed).
struct FreeReedPatchParams {
  /// Body/roll-off brightness in [0,1]: how much high harmonic content the reed
  /// plate radiates (0 = a mellow reed organ, 1 = a bright buzzy harmonica).
  float brightness = 0.6f;
  /// Reed stiffness in [0,1]: shapes the asymmetry / harmonic richness of the
  /// tongue nonlinearity (softer = rounder, stiffer = buzzier).
  /// @note Only the shaped-saw source reads it, so @ref slot_duty above 0 takes
  /// the sound away from it entirely. Every shipped free-reed patch sets one.
  float reed_stiffness = 0.5f;
  /// Steady bellows pressure in [0,1]: the dynamic level / drive.
  float breath_pressure = 0.7f;
  /// Note velocity -> bellows pressure in [0,1].
  float vel_to_breath = 0.5f;
  /// Musette detune in [0,1]: the beating between the two tongues per note
  /// (0 = a single tongue, no beating; higher = a wider wet-tuned accordion).
  float detune = 0.3f;
  /// Adds a third tongue as far flat as the musette partner is sharp, so the
  /// tremolo straddles the in-tune tongue instead of sitting above it. Read only
  /// when detune > 0. false = off -> bit-identical.
  bool musette_flat = false;
  /// Level of each musette partner against the in-tune tongue in [0,1]. Read
  /// only when detune > 0. 1 = equal tongues -> bit-identical.
  float musette_partner_level = 1.0f;
  /// How the musette offset follows the compass, in [0,1]: its cents scale by
  /// (A4/f0)^taper, so 0 holds the cents (the beat rate grows with f0) and 1
  /// holds the beat rate in Hz. Read only when detune > 0. 0 -> bit-identical.
  float musette_taper = 0.0f;
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
  /// Tongue swing at A4 in plate thicknesses (only when slot_duty > 0), scaled 1/f0:
  /// the slot passes the gap the tongue opens, saturated and odd-rich in the bass,
  /// near-sinusoidal in the treble. 0 keeps the hump pair.
  float slot_swing = 0.0f;
  /// How much of a free monopole the reed plate is (only when slot_duty > 0):
  /// 0 radiates the flow itself, 1 its time derivative, the +6 dB/octave tilt a
  /// small source radiates with. Normalised at the fundamental from 20 Hz to
  /// Nyquist, so it changes timbre without moving that level. The harmonium and
  /// bandoneon measure at 1, the harmonica — played inside cupped hands — at 0.
  float radiation = 1.0f;
  /// Share of the monopole's +6 dB per octave kept in the level across the
  /// compass, at the note's base pitch (only when slot_duty > 0 and radiation > 0):
  /// the differentiator is normalised at the fundamental and 1 restores the law. 0 = off.
  float radiation_keytrack = 0.0f;

  /// Onset bellows overshoot (dB of bellows pressure) at velocity 127: how far
  /// the bellows stands above its held level as the player opens the valve,
  /// before it settles. Applies only while no input streams the bellows.
  /// 0 = off -> bit-identical.
  float onset_breath_overshoot_db = 0.0f;
  /// Time the bellows overshoot takes to settle back onto the held level (ms, ~95%).
  float onset_breath_settle_ms = 40.0f;
  /// Change of the bellows overshoot per doubling of velocity (dB), read down
  /// from velocity 127: positive gives a soft note less of it.
  float onset_breath_overshoot_vel_db = 0.0f;
  /// Change of the bellows overshoot per octave above A4 (dB), floored at 0 dB:
  /// the measured accordion swells 4-7 dB at its bass notes and not at all in
  /// its treble, at every velocity. 0 = the same overshoot at every note -> bit-identical.
  float onset_breath_overshoot_key_db = 0.0f;
  /// Exponent of the voice-amplifier velocity curve, (velocity/127)^e; 2 is the
  /// SoundFont curve, bit-identical to it.
  float velocity_exponent = 2.0f;
};

/// Per-voice free-reed state, embedded in NativeSynthVoice. The voice's
/// amplitude envelope / filter / mod matrix wrap around this core; render()
/// returns the raw reed sample.
class FreeReedVoiceCore {
 public:
  /// Configures the tongue oscillators for @p note / @p velocity and seeds the
  /// breath noise. @p seed drives the deterministic air hiss.
  void start(const FreeReedPatchParams& params, double sample_rate, uint8_t note,
             Velocity16 velocity, uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (bend / drift / glide), 1 = on pitch.
  float render(float pitch_ratio) noexcept;
  /// Note-off: ramp the bellows pressure to zero over release_ms.
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

  // --- live continuous control (a free reed is driven for as long as the
  // bellows moves; the host drives these from MIDI CCs while the note sounds).
  // Each sets a smoothing TARGET the render ramps toward (no zipper); call
  // snap_excitation() to jump to the targets without a glide. ---

  /// Base axis positions in [0,1] from the patch or a CC. @p present names the
  /// fields the caller filled (ExcitationAxisMask); an axis it does not name
  /// keeps the value the note started with. Force is bellows pressure (CC2),
  /// which re-derives the saturator drive as well as the output trim — the two
  /// the note-on level set together — so leaning on the bellows buzzes the reed
  /// rather than only loudening it; brightness (CC74) opens the reed-plate body
  /// filter.
  void set_excitation_base(const ExcitationAxes& base, uint32_t present) noexcept;
  void restore_excitation_base() noexcept;
  /// Drops the onset overshoot: the bellows is driven by an input, or velocity
  /// means nothing, so the synthesized onset would play over the player.
  void hold_onset() noexcept { bellows_.hold_overshoot(); }
  /// Mod-matrix offsets on the same axes, in the same normalized units. Held
  /// apart from the base so the two compose rather than overwrite, and applied
  /// through the same smoothing ramp: this is a control-rate destination, not an
  /// audio-rate path into the tongue.
  void set_excitation_mod(const ExcitationAxes& offsets) noexcept;
  /// Jump the smoothed controls to their targets (seed a fresh note at the
  /// host's current CC positions without an audible glide).
  void snap_excitation() noexcept;

 private:
  /// Recomposes the two smoothing targets from their bases and the offsets, and
  /// arms the ramp if either moved.
  void refresh_excitation_targets() noexcept;
  /// Advances the ramp one sample and re-cuts the derived coefficients. Called
  /// only while a control is moving: the body pole costs a pow() and an exp(),
  /// so a voice nobody modulates keeps exactly the coefficients start() wrote.
  void advance_excitation() noexcept;
  /// Re-derives drive_, output_scale_ and body_alpha_ from the live axes.
  void apply_excitation() noexcept;
  /// The slot's flow at @p phase (cycles): the swinging tongue's gap when
  /// slot_swing is set, else the hump pair.
  float slot_flow_at(float phase) const noexcept;

  double sample_rate_ = 48000.0;
  float base_freq_hz_ = 220.0f;

  /// A musette tongue beside the in-tune one: its oscillator and its own copy of
  /// the radiation differentiator's normalisation and history.
  struct Partner {
    float phase = 0.0f;
    float inc = 0.0f;
    float halfangle = 0.0f;
    float norm = 0.0f;
    float mix_norm = 1.0f;
    float prev_flow = 0.0f;
  };
  static constexpr int kMaxPartners = 2;

  // The in-tune tongue's oscillator (phase accumulator) and its musette partners,
  // none when detune == 0 (bit-identical single tongue).
  float phase_a_ = 0.0f;
  float inc_a_ = 0.0f;
  Partner partners_[kMaxPartners];
  int partner_count_ = 0;
  // Each partner's level against the in-tune tongue, and the mix that averages them.
  float partner_level_ = 1.0f;
  float tongue_mix_ = 1.0f;

  // Tongue nonlinearity shaping (asymmetry from reed_stiffness).
  float asymmetry_ = 0.0f;
  float drive_ = 1.0f;
  // The stiffness half of drive_, held apart so the bellows half can be re-cut
  // live; likewise the slot shaper's output makeup.
  float drive_stiff_ = 1.0f;
  float slot_makeup_ = 1.0f;

  // Live excitation axes (bellows pressure, body brightness): base from the
  // patch or a CC, matrix offset, the composed target and the ramped value.
  // force01_base/bright01_base start at free reed's own 0.0/0.0
  // (ExcitationBases's in-class defaults are placeholders shared with other
  // engines, not this one's).
  ExcitationBases excite_{0.0f, 0.0f, 0.0f, 0.0f};
  float force01_target_ = 0.0f;
  float force01_ = 0.0f;
  float bright01_target_ = 0.0f;
  float bright01_ = 0.0f;
  float ctrl_coeff_ = 0.0f;
  bool excitation_live_ = false;

  // Slot flow (gated): 0 duty keeps the asymmetric saw above.
  float slot_duty_ = 0.0f;
  float slot_return_ = 0.0f;
  float slot_return_width_ = 0.0f;
  float slot_gap_ = 0.0f;
  float slot_mean_ = 0.0f;
  // The tongue's swing at this note in plate thicknesses; 0 keeps the hump pair.
  float swing_ = 0.0f;
  float radiation_ = 0.0f;
  // The radiated level's register law the fundamental normalisation took out; 1 keeps it out.
  float register_gain_ = 1.0f;
  // The differentiator's gain is normalised at the current sounding
  // fundamental, not only at the note's base pitch.  Keep the half-angle and
  // last ratio so an unchanged bend stays on the exact start() path.
  float radiation_halfangle_ = 0.0f;
  float radiation_ratio_ = 1.0f;
  float radiation_norm_ = 0.0f;
  float radiation_mix_norm_ = 1.0f;
  float prev_flow_ = 0.0f;
  bool radiation_history_ready_ = false;

  // Body lowpass (reed-plate / cavity radiation): a one-pole roll-off.
  float body_alpha_ = 1.0f;
  float body_state_ = 0.0f;

  // Bellows level contour: a one-pole ramp toward 1 while blowing, 0 once released.
  OnsetContour bellows_;

  // Breath/air noise.
  float breath_noise_ = 0.0f;
  VoiceRandomSequence noise_;
  uint64_t drive_index_ = 0;

  /// Output trim bringing the raw reed sample up to a musical voice level.
  float output_scale_ = 1.0f;
};

}  // namespace sonare::midi::synth
