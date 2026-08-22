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
/// Share of the raw string signal a piano host keeps in the mix. The rest of
/// the note reaches the listener through PianoSoundboard::process(), whose
/// phase-diffusing radiation path breaks the waveform's periodicity — heard
/// directly, a phase-coherent string loop reads as a literally vibrating
/// string (a guitar), not as an instrument radiating through a board.
inline constexpr float kPianoDirectGain = 0.3f;

/// How long the shared piano body keeps radiating after the last string is
/// released (seconds). The modal soundboard and the pedal-gated sympathetic
/// bank ring well past the ~120 ms voice release, so both hosts fold this into
/// their tail estimate — and the SF2 host uses it to decide how long a part's
/// body still costs CPU — or a bounce cuts the bloom off the last chord.
inline constexpr float kPianoBodyRingS = 2.0f;
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
  void start(const PianoPatchParams& params, double sample_rate, uint8_t note, uint8_t velocity,
             uint64_t seed, bool una_corda = false) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor.
  float render(float pitch_ratio) noexcept;
  /// Note-off: the damper caps both decay stages at the t60 start() derived
  /// from release_damp_s for this note and strike velocity.
  void release() noexcept;
  /// Half-pedal: a damper resting partially on the string. @p strength in
  /// [0,1] sets the contact — 0 leaves the natural ring untouched, 1 reaches
  /// the full release() damping, and intermediate values cap the decay at a
  /// geometrically interpolated t60 (a light touch slows the ring gently).
  void damp(float strength) noexcept;
  /// Immediate silence.
  void kill() noexcept;

 private:
  struct String {
    float* buffer = nullptr;
    int size = 0;
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
  };

  float* slab_ = nullptr;
  int string_capacity_ = 0;

  std::array<String, kMaxPianoStrings> strings_{};
  int num_strings_ = 0;
  float loop_alpha_ = 1.0f;
  float bridge_ = 0.0f;
  /// Damper radius cap, derived in start() from the note and the strike
  /// velocity and applied by release() / damp().
  float release_gain_ = 0.0f;

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
  uint32_t noise_rng_ = 1u;
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
  /// Tunes the bank from an explicit set of @p count mode frequencies (Hz;
  /// clamped to kResonanceModes, the surplus zeroed) instead of the built-in
  /// piano register grid, with a @p ring_t60_s resonator decay and a @p out_gain
  /// coupling level. Used to share this bank as a plucked-string sympathetic
  /// resonator (the open guitar/harp strings ringing behind the played note):
  /// the caller drives process() with damper_open == true (plucked strings have
  /// no dampers), so the gate simply holds open. The piano prepare(double) path
  /// is untouched. RT contract identical to prepare().
  void prepare_custom(double sample_rate, const float* freqs, int count, float ring_t60_s,
                      float out_gain) noexcept;
  /// prepare_custom() preset for the plucked-string "sound halo": the bank is
  /// tuned to the standard-tuning open guitar strings (E2 A2 D3 G3 B3 E4) plus
  /// their low harmonics, ringing ~1.5 s at a weak coupling. Shared by every
  /// host that voices a ks.sympathetic patch so the tuning table lives in one
  /// place.
  void prepare_guitar_sympathetic(double sample_rate) noexcept;
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
  /// Radiates one summed input sample: returns the phase-diffused complement
  /// of the host's direct share plus the (mix-scaled) modal colour.
  float process(float in) noexcept;
  /// The phase-diffused sample computed by the last process() call. Feed
  /// resonance banks from this (not the raw dry) so their returns share the
  /// radiated path's phase field instead of partially cancelling it.
  float last_diffused() const noexcept { return in1_; }

 private:
  static constexpr int kSoundboardModes = 40;
  struct Mode {
    float a1 = 0.0f;
    float a2 = 0.0f;
    float gain = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
  };
  std::array<Mode, kSoundboardModes> modes_{};
  // Schroeder allpass diffusers (fixed capacity, no allocation: the lazy
  // fallback prepare runs on the audio thread; prepare() sets the active
  // lengths, clamped to the capacity at very high sample rates).
  static constexpr size_t kDiffuserCapacity = 2048;
  std::array<float, kDiffuserCapacity> diff_buf_[2]{};
  size_t diff_len_[2] = {0, 0};
  size_t diff_idx_[2] = {0, 0};
  float in1_ = 0.0f;
  float in2_ = 0.0f;
  float out_gain_ = 0.0f;
  // Sustain-air texture state (level-tracked bandpassed noise).
  float air_env_ = 0.0f;
  float air_lp_ = 0.0f;
  float air_hp_ = 0.0f;
  float air_attack_ = 0.0f;
  float air_release_ = 0.0f;
  float air_lp_a_ = 0.0f;
  float air_hp_a_ = 0.0f;
  uint32_t air_rng_ = 0x9E3779B9u;
};

}  // namespace sonare::midi::synth
