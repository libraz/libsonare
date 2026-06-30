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
///   4. SOUNDBOARD: a single shared modal bank (PianoSoundboard, below) driven
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

#include <array>
#include <cstddef>
#include <cstdint>

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

/// Physically-graded stiff-string inharmonicity coefficient B for a MIDI
/// @p note, where partial n lands at f_n = n*f0*sqrt(1 + B*n^2). B rises
/// roughly threefold per octave (Fletcher/Conklin), from ~2e-5 in the deep
/// bass through ~7e-4 around A4 to a few percent at the top of the keyboard,
/// with a small bass floor. Drives the per-note dispersion allpass design.
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
  float strike_position = 0.12f;
  /// Felt compression exponent p in F = K*y^p (sets the velocity scaling
  /// laws of contact time and force).
  float hammer_exponent = 2.5f;
  /// Hammer-felt contact time at A4 / mezzo-forte (ms).
  float hammer_contact_ms = 1.2f;
  /// Soundboard resonator mix in [0,1].
  float soundboard = 0.25f;
  /// Damped t60 in seconds applied at note-off (the damper falling back).
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
  void start(const PianoPatchParams& params, double sample_rate, uint8_t note, uint8_t velocity,
             uint64_t seed, bool una_corda = false) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor.
  float render(float pitch_ratio) noexcept;
  /// Note-off: the damper caps both decay stages at release_damp_s.
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

 private:
  struct String {
    float* buffer = nullptr;
    int size = 0;
    size_t write_index = 0;
    float base_period = 0.0f;  // ideal loop period / detune included
    float comp = 1.0f;         // loop delay not in the line (fb + lp + allpass)
    float lp_state = 0.0f;
    std::array<float, kPianoDispersionStages> ap_state{};
    float ap_a = 0.0f;  // shared first-order allpass coefficient
    float g_slow = 0.0f;
    float g_fast = 0.0f;
  };

  /// Analytic raised-cosine hammer force at sample @p n (0 outside contact).
  float hammer_force(int64_t n) const noexcept;

  float* slab_ = nullptr;
  int string_capacity_ = 0;

  std::array<String, kMaxPianoStrings> strings_{};
  int num_strings_ = 0;
  float loop_alpha_ = 1.0f;
  float bridge_ = 0.0f;
  /// Damper radius cap installed by release().
  float release_gain_ = 0.0f;

  // Hammer pulse (analytic; combed by the strike position, then the
  // velocity-driven felt-stiffness lowpass).
  int64_t exc_pos_ = 0;
  int contact_samples_ = 0;
  int comb_delay_ = 0;
  float hammer_amp_ = 0.0f;
  float exc_alpha_ = 1.0f;
  float exc_lp_ = 0.0f;
};

/// Pedal-gated sympathetic resonance: a small shared bank of string-mode
/// resonators driven by the bridge/voice mix while the dampers are lifted
/// (sustain pedal down). A reduced model of the undamped strings re-radiating
/// when the dampers are off (Lehtonen, Penttinen, Rauhala & Valimaki 2007).
/// One bank is shared by the whole instrument (not per voice); the host feeds
/// it the summed dry mix and adds the returned resonance back.
///
/// RT contract: prepare() is the only allocation site (it owns no heap, so it
/// is in fact allocation-free too); process()/reset() are allocation-free and
/// deterministic. The excitation is gated by a smoothed damper-open envelope,
/// and the resonators ring out with extra damping as the dampers fall.
class PianoResonanceBank {
 public:
  /// Tunes the mode bank for @p sample_rate and clears the state.
  void prepare(double sample_rate) noexcept;
  /// Clears the resonator state and the damper gate.
  void reset() noexcept;
  /// Adds the sympathetic resonance for one input sample. @p damper_open
  /// (sustain pedal down) gates the excitation through a smoothed envelope;
  /// returns the resonance to mix into the output.
  float process(float bridge_in, bool damper_open) noexcept;

 private:
  static constexpr int kResonanceModes = 16;
  struct Mode {
    float a1 = 0.0f;
    float a2 = 0.0f;
    float gain = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
  };
  std::array<Mode, kResonanceModes> modes_{};
  float gate_ = 0.0f;
  float gate_open_coeff_ = 1.0f;
  float gate_close_coeff_ = 1.0f;
  float ringout_ = 1.0f;
  float out_gain_ = 0.0f;
};

/// Shared modal soundboard: one fixed bank of second-order resonators, spread
/// across the soundboard's radiating range with a frequency-graded damping and
/// a low-mid radiation envelope, driven by the summed instrument output and
/// added back in parallel as a body coloration (the cheap, data-free end of
/// commuted synthesis). One board is shared by the whole instrument (not per
/// voice), so chords couple into a common body the way a real grand's strings
/// all drive one soundboard. The synthetic modal data is designed from plate
/// heuristics, so it needs no measured impulse response; the architecture is
/// the same if the coefficients are later refit to a measured admittance.
///
/// RT contract: prepare() is the only configuration site (it owns no heap);
/// process()/reset() are allocation-free and deterministic. Each resonator is
/// unity-peak normalized so the bank colours rather than rings away.
class PianoSoundboard {
 public:
  /// Tunes the mode bank for @p sample_rate and stores the patch @p mix in
  /// [0,1] (the soundboard return level); clears the resonator state.
  void prepare(double sample_rate, float mix) noexcept;
  /// Clears the resonator state.
  void reset() noexcept;
  /// Adds the soundboard colour for one summed input sample; returns the
  /// (mix-scaled) board contribution to fold back into the output.
  float process(float in) noexcept;

 private:
  static constexpr int kSoundboardModes = 28;
  struct Mode {
    float a1 = 0.0f;
    float a2 = 0.0f;
    float gain = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
  };
  std::array<Mode, kSoundboardModes> modes_{};
  float out_gain_ = 0.0f;
};

}  // namespace sonare::midi::synth
