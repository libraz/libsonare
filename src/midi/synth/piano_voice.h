#pragma once

/// @file piano_voice.h
/// @brief Extended-waveguide acoustic-piano core for the NativeSynth voice —
///        the no-SF2 data-free grand sketch (synthesis method "piano" of the
///        instrument build plan; Bensa et al. 2003, Bank & Valimaki,
///        Jaffe & Smith).
///
/// Four elements separate "piano" from "guitar/organ", and all four are here:
///   1. STIFF-STRING DISPERSION: real strings are stiff, so partials stretch
///      sharp (f_n = n*f0*sqrt(1 + B*n^2), B rising from ~1e-4 in the bass to
///      ~1e-2 at the top). Implemented as a cascade of first-order allpasses
///      inside each waveguide loop (high frequencies travel faster), with
///      the EXACT allpass + loop-filter phase delay at the fundamental
///      compensated in the fractional loop length so the f0 tuning stays
///      accurate.
///   2. NONLINEAR FELT HAMMER: the felt spring F = K*y^p (p ~ 2-3) is folded
///      in analytically — the excitation is a raised-cosine force pulse
///      whose contact time shrinks as v^-((p-1)/(p+1)) and whose amplitude
///      grows as v^(2p/(p+1)) (the Hertz-contact scaling laws), so hard
///      strikes are shorter (= brighter) the way felt physics dictates, and
///      the strike-position comb is applied analytically to the pulse.
///   3. COUPLED UNISON STRINGS / TWO-STAGE DECAY: 2-3 micro-detuned string
///      loops share a bridge; the coherent (bridge-moving) component decays
///      at the fast "prompt sound" rate while the residual decays at the
///      slow "aftersound" rate — the double decay + shimmer signature.
///   4. SOUNDBOARD: a single shared modal bank (PianoSoundboard, in piano_resonance.h) driven
///      by the summed instrument output approximates the soundboard's dominant
///      radiating modes (the cheap end of commuted synthesis). It is host-owned
///      and instrument-wide, not per voice, so a chord shares one board the way
///      a real grand does; the per-note hammer knock still radiates through it.
///
/// The string delay slabs are NOT owned by the core: the host instrument
/// allocates one slab per voice slot in prepare() (the only allocation
/// site) and attach()es it before start().
///
/// RT contract: attach()/start()/render() are allocation-free. Determinism:
/// per-string detune jitter derives from the (voice_index, note, age)
/// stream; the hammer pulse is analytic.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "midi/control_value.h"
#include "midi/synth/piano_resonance.h"
#include "rt/biquad_design.h"

namespace sonare::midi::synth {

inline constexpr int kMaxPianoStrings = 3;

inline constexpr int kPianoDispersionStages = 4;
/// Lowest fundamental the piano string loops are sized for (A0 = 27.5 Hz).
inline constexpr float kPianoMinFundamentalHz = 26.0f;

/// Per-string delay capacity (samples) for @p sample_rate.
inline int piano_string_capacity(double sample_rate) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  return static_cast<int>(sr / kPianoMinFundamentalHz) + 8;
}

/// Whole-voice slab capacity (samples) the host must allocate.
inline int piano_slab_capacity(double sample_rate) noexcept {
  return kMaxPianoStrings * piano_string_capacity(sample_rate);
}

/// Stiff-string inharmonicity coefficient B for a MIDI @p note, where partial
/// n lands at f_n = n*f0*sqrt(1 + B*n^2). Fitted to a measured concert-grand
/// corpus: ~8e-4 around A4, a couple of percent at the top of the keyboard,
/// and a minimum near C2 (note 36) rather than at the bottom. Below that break
/// B climbs back to ~1e-4 at A0, because a wound bass string is a heavy core
/// on a scale too short to keep it flexible -- a monotonic curve reads seven
/// times too stiff-free down there, and the bass loses its growl. Drives the
/// per-note dispersion allpass design.
float piano_inharmonicity_b(uint8_t note) noexcept;

/// Number of coupled unison strings a real grand strings @p note with: a
/// single wound string in the deep bass, a wound bichord through the
/// bass-tenor region, and a plain trichord from the tenor break up. Used as
/// a per-register cap on the patch's string count, so the bass keeps its
/// single-stage decay (no unison aftersound) while the treble couples three.
int piano_unison_strings(uint8_t note) noexcept;

/// Railsback stretch (cents) added to the equal-tempered pitch of @p note. A
/// real piano is tuned with progressively widened octaves so the inharmonically
/// sharp partials of the lower strings lock to the fundamentals above: sharp in
/// the treble, flat in the bass, zero at the A4 anchor. The perceptual
/// completion of the stiff-string inharmonicity (piano_inharmonicity_b).
///
/// Fitted to the same measured corpus and strongly asymmetric -- about ten
/// cents flat at A0 against fifty sharp at C8 -- so it is two power-law
/// branches rather than one odd function about the anchor. Note that this is
/// stretch only: an instrument tuned a little off A440 carries a constant
/// offset which is not part of the curve and must not be fitted into it.
float piano_stretch_cents(uint8_t note) noexcept;

/// Piano section of a NativeSynthPatch (used when mode == kPiano).
struct PianoPatchParams {
  /// Coupled unison strings per note (clamped to [1, kMaxPianoStrings]).
  int strings = 3;
  /// Full micro-detune spread between the outer unison strings (cents).
  float detune_cents = 1.6f;
  /// Prompt-sound (coupled) t60 at A4 in seconds.
  float decay_fast_s = 3.0f;
  /// Aftersound (residual) t60 at A4 in seconds.
  float decay_slow_s = 12.0f;
  /// t60 scales by 2^(stretch * octaves below A4).
  float decay_stretch = 0.7f;
  /// Loop-lowpass openness in [0,1] (frequency-dependent string damping).
  float brightness = 0.75f;
  /// Dispersion amount in [0,1]: scales the keyboard-graded stiffness
  /// stretch (0 = harmonic string).
  float dispersion = 1.0f;
  /// Hammer strike point as a fraction of the string period in [0, 0.5].
  float strike_position = 0.085f;
  /// Felt compression exponent p in F = K*y^p (sets the velocity scaling
  /// laws of contact time and force).
  float hammer_exponent = 2.5f;
  /// Hammer-felt contact time at A4 / mezzo-forte (ms).
  float hammer_contact_ms = 1.2f;
  /// Extra velocity-dependent felt compression in [0,1] (0 = off, bit-identical
  /// to the intrinsic Hertz-contact scaling). Above the intrinsic law, hard
  /// strikes shorten the felt contact and pass more high partials further, so
  /// the pp<->ff timbre spread widens; the shaping pivots at the mezzo-forte
  /// reference so the nominal voicing is preserved.
  float hammer_dynamics = 0.0f;
  /// Velocity exponent of a high shelf on the string-injected scrub, pivoting at
  /// mezzo-forte (0 = off, bit-identical). Moves the attack's 4-8 kHz share with the
  /// blow without moving the level law hammer_dynamics sets.
  float attack_hf_dynamics = 0.0f;
  /// Soundboard resonator mix in [0,1].
  float soundboard = 0.25f;
  /// Damped t60 in seconds applied at note-off (the damper falling back), for
  /// a note struck at the loud end of the velocity range. The strike velocity
  /// and the register both stretch it: felt damps a quiet string weakly, and
  /// the heavy wound bass strings hold on where the treble stops at once.
  float release_damp_s = 0.1f;
};

/// Per-voice piano state, embedded in NativeSynthVoice.
class PianoVoiceCore {
 public:
  /// Wiring: hands the core its delay slab (>= piano_slab_capacity()).
  void attach(float* buffer, int per_string_capacity) noexcept {
    slab_ = buffer;
    string_capacity_ = per_string_capacity;
  }

  /// @param una_corda soft-pedal (CC67) engaged at strike: the action shifts
  ///        onto a softer, less-grooved patch of felt, so the attack is
  ///        darker and a touch quieter (una corda voicing).
  void start(const PianoPatchParams& params, double sample_rate, uint8_t note, Velocity16 velocity,
             uint64_t seed, bool una_corda = false) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor.
  float render(float pitch_ratio) noexcept;
  /// Note-off: the damper caps both decay stages at the t60 start() derived
  /// from release_damp_s for this note and strike velocity.
  void release() noexcept;
  /// Half-pedal: the damper's current contact. @p strength in [0,1] — 0
  /// restores the natural ring, 1 applies release(), and intermediate values
  /// set the decay at a geometrically interpolated t60. A later call replaces
  /// an earlier one; energy already lost stays lost.
  void damp(float strength) noexcept;
  /// Immediate silence.
  void kill() noexcept;
  /// What this note's blow puts into the instrument's structure, as set by the
  /// last start(). The host hands it to the shared PianoSoundboard rather than
  /// mixing it into this voice's output, because a case network is struck once
  /// per blow and not driven by the note (see kCaseStrikeGain in the .cpp).
  float case_strike() const noexcept { return case_strike_; }
  /// The same blow into the board bank instead, which answers it over a
  /// fraction of a second where the case network answers over four.
  float board_strike() const noexcept { return board_strike_; }

 private:
  void refresh_modal_coefficients(float pitch_ratio) noexcept;

  struct String {
    float* buffer = nullptr;
    size_t write_index = 0;
    float base_period = 0.0f;     // ideal loop period / detune included
    float comp = 1.0f;            // loop delay not in the line (fb + lp + allpass)
    float strike_weight = 1.0f;   // uneven hammer energy across the unison
    float radiate_weight = 1.0f;  // uneven bridge coupling across the unison
    float lp_state = 0.0f;
    std::array<float, kPianoDispersionStages> ap_state{};
    float ap_a = 0.0f;  // shared first-order allpass coefficient
    float g_slow = 0.0f;
    float g_fast = 0.0f;
    // The two gains as start() set them: what a partial damper interpolates from.
    float g_slow_natural = 0.0f;
    float g_fast_natural = 0.0f;
  };

  float* slab_ = nullptr;
  int string_capacity_ = 0;

  std::array<String, kMaxPianoStrings> strings_{};
  int num_strings_ = 0;
  float loop_alpha_ = 1.0f;
  float bridge_ = 0.0f;
  /// The bridge signal the prompt-decay drain actually subtracts, and the
  /// one-pole that band-limits it (see kTwoStageDrainPartials). At the
  /// coefficient's transparent value of 1 this tracks `bridge_` exactly.
  float bridge_drain_ = 0.0f;
  float drain_lp_a_ = 1.0f;
  /// The drain's upper band, quoted in absolute frequency rather than in
  /// partials of this note (see kBridgeHfDrain). `bridge_hf_lp_` is the
  /// fixed-corner one-pole whose complement is the band, `drain_hi_w_` the
  /// weight it enters at, and `drain_out_` the sum the string loops actually
  /// subtract -- at a zero weight exactly `bridge_drain_`.
  float bridge_hf_lp_ = 0.0f;
  float drain_hf_a_ = 0.0f;
  float drain_hi_w_ = 0.0f;
  float drain_out_ = 0.0f;
  /// Damper radius cap, derived in start() from the note and the strike
  /// velocity and applied by release() / damp().
  float release_gain_ = 0.0f;

  // Modal top-octave bank: the same string, stated as its partials instead of
  // as a travelling wave.
  //
  // A waveguide loop is a delay line whose length IS the period, and every
  // element the loop needs -- the loss filter, the dispersion cascade, the
  // fractional-delay interpolator -- costs phase delay out of that length.
  // At C8 the whole loop is six samples. Four allpass stages and a one-pole
  // do not fit, the third-order fractional interpolator's magnitude droop is
  // paid four thousand times a second, and the result is a string that leaks
  // roughly seventy times the loss its t60 prescribes: measured, C8's second
  // partial falls 103 dB/s faster than its first where the reference holds
  // the two within a few decibels for the whole note. The failure is
  // structural rather than a setting -- no voicing knob in the engine moves
  // it by more than twenty -- and it is the documented limit of the method
  // (Smith, *Physical Audio Signal Processing*), whose stated remedy at very
  // high pitches is to leave the delay line for a small bank of second-order
  // resonators (Bank, Zambon & Fontana 2010 take a whole piano that way).
  //
  // So above the crossover each unison string is realized as its partials,
  // one two-pole resonator per (string, partial), driven by the SAME hammer
  // excitation and radiated through the same board. Three things follow:
  // every partial's decay becomes a declared quantity rather than a
  // by-product of loop arithmetic; the stiff-string stretch is exact instead
  // of faded out where the allpass cascade stopped fitting; and the top of
  // the keyboard gets cheaper, since a C8 needs about five partials against a
  // loop that still costs its interpolator and four allpasses per string.
  //
  // The mode amplitude is derived, not fitted. A unit impulse injected into a
  // loop of period P leaves an impulse train whose harmonic components each
  // have amplitude 2/P and decay at the loop gain, while a two-pole resonator
  // driven by the same impulse rings at gain/sin(w) -- so gain = 2*sin(w)/P
  // makes the two paths agree partial for partial, and the crossover is a
  // level match by construction.
  static constexpr int kModalPartials = 16;
  static constexpr int kModalModes = kMaxPianoStrings * kModalPartials;
  struct ModalMode {
    float omega0 = 0.0f;       // radians/sample at pitch_ratio == 1
    float r = 0.0f;            // per-sample decay radius
    float weight = 0.0f;       // strike/radiation weight before pitch normalization
    float base_period = 0.0f;  // ideal loop period / detune included
    float a1 = 0.0f;
    float a2 = 0.0f;
    float gain = 0.0f;  // pitch-adjusted 2*sin(w)/P, times strike/radiation weight
    float y1 = 0.0f;
    float y2 = 0.0f;
    bool audible = false;  // in-band in the last coefficient pass
  };
  std::array<ModalMode, kModalModes> modal_{};
  std::array<uint8_t, kModalModes> modal_active_indices_{};
  int num_modal_ = 0;
  uint8_t modal_active_count_ = 0;
  float modal_cached_ratio_ = 1.0f;
  /// Crossover weight in [0,1]: 0 leaves the waveguide alone, 1 skips it.
  float modal_mix_ = 0.0f;
  /// Damper on the modal bank. Applied as an output envelope rather than by
  /// retuning the poles: with the hammer long gone the bank's input is zero,
  /// so an exponential on the sum is exactly extra damping on every mode, and
  /// it costs one multiply instead of a trig sweep on the audio thread.
  float modal_env_ = 1.0f;
  float modal_damp_ = 1.0f;
  float modal_release_ = 1.0f;
  /// The prompt stage, as a second envelope on the same sum.
  ///
  /// A struck string loses most of a blow while its unison and the two
  /// polarizations of each string still move together, and rings on far more
  /// slowly once they have drifted apart. The loop carries that as a coupled
  /// drain between two loop gains; the bank has no loop and had no second rate
  /// at all, so above the crossover the whole top octave decayed at the
  /// aftersound rate from the first sample -- measured against three concert
  /// grands whose C8 falls at 65 to 68 dB/s for half a second, this voice's
  /// fell at 4.9 and put its knee at 3.7 s where theirs is at 0.5.
  ///
  /// An envelope on the sum rather than a second resonator per mode, for the
  /// reason `modal_env_` already gives: with the hammer gone the bank's input
  /// is zero, so multiplying the sum by (residue + (1 - residue) * e^-t/tau)
  /// gives every mode a genuine two-exponential decay whose fast rate is the
  /// sum of the two rates -- the same arithmetic the loop's t60_fast is built
  /// out of -- for one multiply-add per sample rather than per mode.
  float modal_prompt_ = 1.0f;
  float modal_prompt_r_ = 1.0f;
  /// Share of the amplitude that survives the prompt stage into the
  /// aftersound. 1 leaves the bank exactly as it was.
  float modal_residue_ = 1.0f;

  /// Ring capacity for the strike-position comb on the hammer force (covers
  /// a 0.5 * period tap up to ~32 Hz at 96 kHz; longer taps clamp).
  static constexpr int kHammerCombCapacity = 2048;

  // Dynamic felt hammer: a mass with a nonlinear spring (F = k * x^p, with a
  // hysteretic loss term) integrated per sample against the string's motion
  // at the strike point. Contact time, its velocity/register dependence and
  // the bass re-contact chatter all emerge from the interaction instead of
  // being prescribed as a pulse shape.
  float hammer_amp_ = 0.0f;
  bool ham_on_ = false;
  int ham_ttl_ = 0;
  float ham_y_ = 0.0f;
  float ham_v_ = 0.0f;
  float ham_k_ = 0.0f;
  float ham_p_ = 2.5f;
  float ham_mu_ = 0.0f;
  float ham_force_norm_ = 0.0f;
  float ham_exit_ = -1.0f;
  float ys_ = 0.0f;
  float ys_adm_ = 0.0f;
  float ys_limit_ = 0.0f;
  float last_force_ = 0.0f;
  int comb_delay_ = 0;
  int comb_idx_ = 0;
  int comb_tail_ = 0;
  std::array<float, kHammerCombCapacity> comb_hist_{};
  // Strike-position comb history for the injected scrub noise (same delay,
  // separate units/lifetime from the force comb).
  std::array<float, kHammerCombCapacity> noise_hist_{};
  float knock_gain_ = 0.6f;
  float case_strike_ = 0.0f;
  float board_strike_ = 0.0f;
  float knock_lp_ = 0.0f;
  float knock_lp2_ = 0.0f;
  float knock_lp3_ = 0.0f;
  float knock_lp3_a_ = 0.0f;
  float knock_lp_a_ = 0.0f;
  float bloom_ = 1.0f;
  float bloom_a_ = 1.0f;
  float exc_alpha_ = 1.0f;
  float exc_lp_ = 0.0f;
  float exc_lp2_ = 0.0f;
  /// The blow as the BODY feels it: the same felt-softened force pulse, but
  /// without the string's strike-position comb (see kKnockUncombed).
  float body_lp_ = 0.0f;
  float body_lp2_ = 0.0f;

  // Longitudinal string modes ("phantom partials"). Transverse motion stretches
  // the string, and the tension change it makes -- quadratic in the transverse
  // displacement -- launches waves at the LONGITUDINAL speed, which in steel is
  // tens of times the transverse one. They are inharmonic against the
  // transverse series, strongest in the bass where the fundamental radiates
  // almost nothing, and they are the metallic growl that tells the ear a low
  // note came from a piano rather than from a string. The drive is the squared
  // string sum, so the v^2 amplitude law and the doubled decay rate both come
  // out of the mechanism rather than being prescribed.
  static constexpr int kLongitudinalModes = 5;
  struct LongMode {
    float a1 = 0.0f;
    float a2 = 0.0f;
    float gain = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
  };
  std::array<LongMode, kLongitudinalModes> long_modes_{};
  float long_level_ = 0.0f;
  float long_prev_ = 0.0f;
  float long_hp_a_ = 1.0f;
  float long_x1_ = 0.0f;
  float long_x2_ = 0.0f;

  // Soundboard radiation highpass: two biquad sections (b2 == b0 in each)
  // forming a fourth-order Butterworth. The measured radiation transition of a
  // grand is about 24 dB/octave -- a bass fundamental sits 25 dB under its own
  // partial crown one octave below the break and 40-plus dB under it at A0 --
  // which a single 12 dB/octave section cannot reach without also thinning the
  // tenor, so the order is the thing that has to be right rather than the Q.
  static constexpr int kRadiationHpSections = 2;
  struct HpSection {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float x1 = 0.0f;
    float x2 = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
  };
  std::array<HpSection, kRadiationHpSections> hp_{};

  // Bridge-hill radiation emphasis (peaking biquad).
  float bh_b0_ = 1.0f;
  float bh_b1_ = 0.0f;
  float bh_b2_ = 0.0f;
  float bh_a1_ = 0.0f;
  float bh_a2_ = 0.0f;
  float bh_x1_ = 0.0f;
  float bh_x2_ = 0.0f;
  float bh_y1_ = 0.0f;
  float bh_y2_ = 0.0f;

  // Felt impact noise (broadband thump radiated with the knock at strike).
  int64_t noise_pos_ = 0;
  int noise_samples_ = 0;
  float noise_env_ = 0.0f;
  float noise_decay_ = 0.0f;
  float noise_alpha_ = 1.0f;
  float noise_inject_ = 0.0f;
  float noise_lp_ = 0.0f;
  float noise_lp2_ = 0.0f;
  float noise_lp3_ = 0.0f;
  float noise_alpha3_ = 1.0f;
  float noise_low_ = 0.0f;
  float noise_hp_a_ = 0.0f;
  // Two high-shelf sections on the injected scrub, half of this blow's gain each.
  float scrub_hf_gain_ = 1.0f;
  rt::BiquadState scrub_hi_[2]{};
  uint32_t noise_rng_ = 1u;
};

}  // namespace sonare::midi::synth
