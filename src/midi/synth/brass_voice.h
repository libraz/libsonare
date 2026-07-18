#pragma once

/// @file brass_voice.h
/// @brief Brass (lip-reed, breath-excited) core for the NativeSynth voice — the
///        data-free brass model (trumpet / trombone / tuba / french horn, and
///        lip-driven brass in general; McIntyre, Schumacher & Woodhouse 1983,
///        Cook's TBone / STK Brass waveguide). Like the reed woodwind it is a
///        BREATH-excited sustained tone, but where the reed valve is
///        INWARD-striking (blown-closed: rising bore pressure pinches the reed
///        SHUT and the bore sets the pitch), the brass lip is an OUTWARD-striking
///        (blown-open, "swinging-door") valve whose OWN resonance selects the
///        played partial — the player buzzes the lips and their tension picks the
///        note (Fletcher 1979). A note-driven synth locks both the lip resonance
///        and the bore to the note frequency, so each note speaks at its
///        fundamental.
///
/// The model is the Smith/Cook single-delay-line brass waveguide, the same one
/// STK's Brass implements. The air column is a travelling-wave delay line (the
/// bore); at the mouthpiece a RESONANT NONLINEAR LIP VALVE couples the player's
/// breath to the bore. Unlike the reed's memoryless table, the lip is a damped
/// two-pole resonator (a mass-spring, the buzzing lip) tuned to the note, driven
/// by the pressure difference across the lips (mouth pressure minus the pressure
/// reflected back up the bore). The lip's displacement opens a valve whose flow
/// mixes the mouth pressure into the bore — this one resonant nonlinearity, with
/// the bore's resonance, is the whole instrument:
///   1. LIP VALVE (resonant): the pressure difference dp = bore - mouth drives a
///      two-pole lip resonator (a bandpass, b0/a1/a2) tuned to the note; its
///      displacement modulates a reflection coefficient (clamped to [-1,1]) that
///      gates the mouth pressure into the bore. The injection is the reed-style
///      flow inj = mouth + dp*coeff — the same topology as the reed table, but
///      the coefficient is the RESONANT buzzing lip rather than a memoryless
///      table, so the harmonic drive (the octave and above) survives into the
///      bore. The coupling sign is NEGATIVE: because the lip is OUTWARD-striking
///      the note speaks just ABOVE the lip resonance, and this sign is what locks
///      the buzz to the fundamental instead of a mistuned inter-harmonic mode.
///   2. BORE: a travelling-wave delay line, a POSITIVE-feedback comb of the full
///      period, so the bore resonances land on the FULL harmonic series (f0, 2f0,
///      3f0 …) the way a bell-and-mouthpiece-corrected brass tube does — the lip
///      buzzes the fundamental and the tube reinforces every harmonic. The
///      @c conical flag darkens the reflection (french horn / tuba are conical
///      and rounder than a cylindrical trumpet / trombone); both are full-
///      harmonic, since the lip valve excites all harmonics regardless.
///   3. BELL TERMINATION: the flaring bell is a strong high-frequency radiator
///      that reflects only the low end, modelled as a one-pole loss lowpass with
///      a loss gain < 1 (the bore's round-trip damping and the energy the bell
///      radiates). Brightness voices the bell filter; damping sets the loss.
///   4. BREATH CONTOUR: the breath rises into the note (the lips take a few
///      periods to lock into oscillation) and falls when the player tongues off.
///      A small internal envelope drives the mouth pressure; the lip valve's
///      rectifying clamp self-regulates the amplitude, so the loop stays bounded
///      without explicit level normalisation. The lips only speak ABOVE a breath
///      threshold — below it the valve never buzzes and the note is silent — so
///      the contour is calibrated to cross into the oscillating region promptly.
///
/// The delay buffer is NOT owned by the core: the host allocates one slab per
/// voice slot in prepare() (one bore span) and attach()es it before start().
/// Radiation/formant voicing (the bell) is the shared BodyResonator on the
/// voice, enabled per preset; the core emits the raw bore pressure. Self-
/// sustained but unconditionally stable: the lip reflection coefficient is
/// bounded to [-1,1] and the bell loss gain is < 1. The linear waveguide is
/// deliberately dark (a soft/medium brass is round); the bright, brassy "cuivré"
/// edge is the amplitude-dependent nonlinear steepening added off-by-default in a
/// later phase.
///
/// RT contract: attach()/start()/render() are allocation-free (start zeroes /
/// seeds the attached span). Determinism: the breath turbulence and the onset
/// chiff are drawn from the counter-based (voice_index, note, age) stream, so
/// identical event streams render bit-identically.

#include <cstddef>
#include <cstdint>

#include "midi/synth/voice_random.h"
#include "rt/adaa.h"
#include "rt/nonlinearities.h"

namespace sonare::midi::synth {

/// Lowest fundamental the bore delay line is sized for; covers the tuba /
/// contrabass-brass low range with pitch-bend headroom (a brass never sounds
/// below this, but the margin keeps a bent-down low note inside the buffer).
inline constexpr float kBrassMinFundamentalHz = 20.0f;

/// Per-bore buffer capacity (samples) for @p sample_rate: a full period at the
/// lowest fundamental (the brass bore is a full-period positive-feedback comb;
/// sizing for the whole period leaves bend-down headroom).
inline int brass_buffer_capacity(double sample_rate) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  return static_cast<int>(sr / kBrassMinFundamentalHz) + 8;
}

/// Whole-voice slab capacity (samples) the host must allocate: one bore span.
inline int brass_slab_capacity(double sample_rate) noexcept {
  return brass_buffer_capacity(sample_rate);
}

/// Brass (lip-reed) section of a NativeSynthPatch (used when mode == kBrass).
struct BrassPatchParams {
  /// Steady mouth pressure in [0,1]: how hard the player blows, i.e. the dynamic
  /// level. It sets the breath that drives the lip valve; the lips only buzz
  /// above a threshold, so very low values approach silence.
  float breath_pressure = 0.7f;
  /// Note velocity -> breath pressure in [0,1]: how much the struck velocity
  /// opens the breath (0 = velocity-independent dynamics, 1 = velocity is the
  /// dynamic).
  float vel_to_breath = 0.6f;
  /// Lip tension in [0,1]: fine-tunes the lip resonance around the note (the
  /// embouchure "centre" of the note). Tighter = the lip resonance sits a touch
  /// above the bore, the outward-striking oscillation point that gives the brass
  /// "edge"; looser = a touch below, a broader, more slotted centre.
  float lip_tension = 0.5f;
  /// Lip damping in [0,1] (the lip resonator's pole radius): low = a tight,
  /// sharp, buzzing lip (a bright, edgy tone); high = a soft, loose, rounded lip
  /// (a broad, mellow tone).
  float lip_damping = 0.5f;
  /// Conical bore: false = a cylindrical-bodied brass (trumpet / trombone),
  /// brighter and more brilliant; true = a conical-bodied brass (french horn /
  /// tuba / cornet / flugelhorn), darker and rounder. Both radiate the full
  /// harmonic series (the lip valve excites all harmonics); the flag colours the
  /// bell reflection.
  bool conical = false;
  /// Bell reflection-filter openness in [0,1]: how brightly the bore reflects at
  /// the bell (1 = bright/brilliant, 0 = dark/covered). The loop lowpass.
  float brightness = 0.5f;
  /// Bore damping in [0,1]: the bell loop loss. Low = a purer, more sustained,
  /// sharply pitched tone; high = a more damped, quicker-speaking tone. The
  /// breath replenishes the loss either way (the note is sustained).
  float damping = 0.4f;
  /// Breath rise on note-on (ms): the lips take a few periods to lock into
  /// oscillation, so the mouth pressure ramps in rather than stepping.
  float attack_ms = 25.0f;
  /// Breath fall on note-off (ms): the player tongues off and the bore rings
  /// down.
  float release_ms = 90.0f;
  /// Breath turbulence in [0,1]: a subtle deterministic noise on the mouth
  /// pressure (the air rushing past the lips), 0 = a perfectly steady breath.
  /// Kept small; the noise is the seeded per-voice stream so bounces stay
  /// bit-identical.
  float breath_noise = 0.1f;
  /// Onset speech transient (chiff) in [0,1]: the brief bright noise as the lips
  /// start to buzz (the tonguing attack, the "pfft"), so the note articulates
  /// rather than swelling in.
  float chiff = 0.35f;
  /// Chiff decay time constant (ms).
  float chiff_ms = 10.0f;

  // --- off-by-default advanced physics (Phase 4; C-ABI non-exposed, gated) ---
  /// Cuivré / brassiness in [0,1]: the bright, blaring "brassy" edge of a loud
  /// brass. Physically it is the cumulative NONLINEAR wave steepening as a
  /// high-amplitude wave travels the bore — the compression phase outruns the
  /// rarefaction, the wavefront sharpens toward a shock, and the spectrum blooms
  /// with upper harmonics (Hirschberg 1996, Menguy-Gilbert 2000). The linear
  /// Phase-1 waveguide is deliberately dark; this adds the amplitude-DEPENDENT
  /// harmonic bloom so the tone opens up only as it is played louder (pp stays
  /// round, ff blares), realised as a bounded radiation-side waveshaper (the
  /// practical bounded form of the shock, the same output-side strategy the reed's
  /// growth cone uses to stay stable). The shaper is antialiased with first-order
  /// antiderivative antialiasing (ADAA), so the bloomed upper harmonics do not
  /// fold back as aliasing in the high register. The steepening depth follows the
  /// played dynamic when @c cuivre_dynamics is on. 0 = off -> render is
  /// bit-identical (the shaper is skipped).
  float brassiness = 0.0f;

  /// Cuivré dynamics in [0,1]: how strongly the played dynamic (note-on velocity,
  /// baked into the breath level, plus the live CC2 breath) scales the effective
  /// @c brassiness, so the shock blooms with loudness — pp stays round, ff blares
  /// (the amplitude-dependent brightening a real brass has, over and above the
  /// amp VCA). The signal's own level cannot drive this (the self-limiting loop
  /// does not track dynamics), so the played breath is the source. 0 = off ->
  /// @c brassiness is static and the render is bit-identical.
  float cuivre_dynamics = 0.0f;

  /// Mute in [0,1]: a straight / cup / harmon mute over the bell reshapes its
  /// radiation into a nasal, honky timbre with a strong upper formant and a
  /// scooped low-mid (the muted-trumpet colour). Modelled as a gated output
  /// formant/notch shaping of the radiated tone. 0 = off -> render is
  /// bit-identical (the mute filter is skipped).
  float mute = 0.0f;

  /// Half-valve in [0,1]: pressing a valve half-way (or a slide between
  /// positions) leaves the air path partly obstructed, so the bore is lossy and
  /// stuffy and the pitch is unstable — the "half-valve" effect. Modelled as a
  /// gated extra in-loop loss plus a small detune. 0 = off -> render is
  /// bit-identical (the half-valve is skipped).
  float half_valve = 0.0f;

  /// Dynamic (2-DOF) lip in [0,1]: the single lip resonator is a swinging-door
  /// (outward) valve only; a real lip also vibrates TRANSVERSELY (a second mode),
  /// and the two couple (Adachi-Sato 1996). This adds a second, higher lip mode
  /// whose displacement biases the reflection coefficient, giving a livelier
  /// attack and a fuller buzz. 0 = off -> render is bit-identical (the second
  /// mode is skipped).
  float dynamic_lip = 0.0f;
};

/// Per-voice brass state, embedded in NativeSynthVoice. The voice's amplitude
/// envelope / filter / mod matrix and the shared BodyResonator wrap around this
/// core; render() returns the raw bore pressure.
class BrassVoiceCore {
 public:
  /// CONTROL-thread wiring (or audio-thread pointer assignment before start()):
  /// hands the core its bore delay span (@p capacity samples). The slab outlives
  /// the voice.
  void attach(float* bore, int capacity) noexcept {
    bore_ = bore;
    capacity_ = capacity;
  }

  /// Configures the bore / lip for @p note / @p velocity and zeroes / seeds the
  /// used span. @p seed drives the deterministic breath noise and onset chiff.
  void start(const BrassPatchParams& params, double sample_rate, uint8_t note, uint8_t velocity,
             uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (bend / vibrato / drift), 1 = on pitch.
  float render(float pitch_ratio) noexcept;
  /// Note-off: tongue off (ramp the breath to zero); the bore rings down.
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

  // --- live continuous control (brass is a continuous-control instrument; the
  // host drives these from MIDI CCs while the note sounds). Each sets a smoothing
  // TARGET the render ramps toward (no zipper); call snap_brass_control() to jump
  // to the targets without a glide. Loudness/dynamics are NOT driven here — they
  // come from the voice's velocity / expression VCA, because pushing the breath
  // toward the buzzing threshold would silence the lips. ---

  /// Mouth pressure in [0,1] (CC2 breath): bounded to the lip's stable buzzing
  /// band, so more breath colours the tone (brighter, more brilliant, closer to
  /// the buzzing edge) without ever crossing into the slammed-shut silent regime.
  void set_breath(float breath01) noexcept;
  /// Bell brightness in [0,1] (CC74): opens the bell reflection filter, the clean
  /// timbral brightness control.
  void set_brightness(float bright01) noexcept;
  /// Jump the smoothed controls to their targets (seed a fresh note at the
  /// host's current CC positions without an audible glide).
  void snap_brass_control() noexcept;

 private:
  // Bore delay line (host-owned): the travelling-wave air column.
  float* bore_ = nullptr;
  int capacity_ = 0;
  int bore_size_ = 0;
  size_t bore_write_ = 0;
  // Last delay-line output (the pressure returning to the lips next sample).
  float bore_out_ = 0.0f;

  // Tuning: the loop period (samples) and the delay not carried in the line
  // (feedback register + loop-filter phase). Brass uses the full period (a
  // positive-feedback comb, full harmonics).
  float bore_period_ = 0.0f;
  float comp_ = 1.0f;
  // Feedback sign: +1 (full harmonics), the brass bore reinforced by the bell /
  // mouthpiece correction. Kept as a field for symmetry with the reed core.
  float sign_ = 1.0f;

  // Bell reflection: one-pole loop lowpass y += alpha*(x - y), a loss gain, and
  // the sign (folded in render).
  float lp_alpha_ = 1.0f;
  float lp_state_ = 0.0f;
  float loss_gain_ = 0.95f;
  // Retained from start() so live brightness updates apply the same conical
  // darkening bias as the note-on seed (a conical bore reflects darker).
  bool conical_ = false;
  // In-loop DC blocker (the positive-feedback comb has a DC mode that does not
  // radiate, and the breath DC drives the lips, so the loop must shed the offset).
  float dc_x1_ = 0.0f;
  float dc_y1_ = 0.0f;
  float dc_r_ = 0.0f;

  // Lip valve (resonant): a two-pole lip resonator (mass-spring) tuned to the
  // note, driven by the pressure difference dp = mouth - bore. Its displacement,
  // biased by the rest opening and clamped to [0,1], is the lip opening. The
  // resonator is a BANDPASS (a (1 - z^-2) zero pair at DC / Nyquist over the
  // two-pole resonance) so the steady breath DC never drives it — an all-pole
  // resonator tuned to a low fundamental would have a huge DC gain and integrate
  // the breath into a runaway, so the DC zero is essential here (unlike the reed
  // resonator, which sits at a high, DC-safe reed frequency).
  float lip_b0_ = 0.0f;
  float lip_a1_ = 0.0f;
  float lip_a2_ = 0.0f;
  float lip_x1_ = 0.0f;
  float lip_x2_ = 0.0f;
  float lip_z1_ = 0.0f;
  float lip_z2_ = 0.0f;
  // Rest reflection coefficient (the lip termination at equilibrium) and how
  // strongly the resonator's displacement modulates it (the loop gain).
  float lip_offset_ = -0.1f;
  float lip_couple_ = 4.5f;
  // Mouth-pressure scale (mouthpiece coefficient).
  float mouth_scale_ = 1.0f;

  // Breath contour: a one-pole ramp of the mouth pressure toward the target
  // level (1 while blowing, 0 once tongued off). breath_target_ is the steady
  // mouth pressure (live-smoothed toward breath_ctrl_target_).
  float breath_target_ = 0.7f;
  float breath_level_ = 0.0f;
  float attack_coeff_ = 0.0f;
  float release_coeff_ = 0.0f;
  bool releasing_ = false;

  // Live-control smoothing: the render ramps breath_target_ / lp_alpha_ toward
  // these CC targets so a moving controller never zippers. Initialised equal to
  // the note-on values, so an untouched note renders exactly as Phase 1.
  float ctrl_coeff_ = 1.0f;
  float breath_ctrl_target_ = 0.7f;
  float lp_alpha_target_ = 1.0f;

  // Breath turbulence (deterministic mouth-pressure noise; 0 = steady breath).
  float breath_noise_ = 0.0f;

  // Onset chiff (one-pole decaying noise burst, the tonguing "speak").
  float chiff_level_ = 0.0f;
  float chiff_coeff_ = 0.0f;

  // Output trim bringing the raw bore pressure up to a musical voice level,
  // calibrated so a forte note peaks near the other engines' output range.
  float output_scale_ = 1.0f;

  VoiceRandomSequence noise_;
  uint64_t drive_index_ = 0;

  // --- off-by-default advanced physics (Phase 4; skipped entirely when off, so
  // the render path is bit-identical to the linear model). ---

  // 4a: cuivré. brassiness_ == 0 -> skipped (bit-identical). A radiation-side
  // level-preserving waveshaper: the bore output is normalised by the note's raw
  // peak, pushed through a bounded asymmetric tanh (the shock front), and
  // rescaled — so the peak is kept while the upper harmonics bloom. cuivre_scale_
  // is the per-note raw peak used to normalise.
  float brassiness_ = 0.0f;
  float cuivre_scale_ = 1.0f;
  float cuivre_inv_scale_ = 1.0f;
  // Per-note shock drive: the nominal drive scaled by the low-register frequency
  // compensation, so the shaper saturates in the low range as it does up top.
  float cuivre_drive_ = 0.0f;
  // Reciprocal of tanh(cuivre_drive_): the level-preserving rescale that keeps the
  // shaped signal's peak (precomputed for the static, dynamics-off path).
  float cuivre_inv_tanh_ = 1.0f;
  // Low-register drive compensation squared, retained so the effective drive can
  // be recomputed per sample when the dynamics gate scales the brassiness.
  float cuivre_fc_sq_ = 1.0f;
  // Cuivré dynamics gate: 0 -> the drive/mix are static (bit-identical); > 0 ->
  // the played dynamic scales the effective brassiness so the shock blooms with
  // loudness. The dynamic source is the note-on velocity (the amp VCA carries the
  // loudness, so the self-limiting mouth pressure cannot; velocity is the played
  // dynamic), with a live CC2 breath swell above the seated level adding on top.
  float cuivre_dynamics_ = 0.0f;
  float cuivre_vel_ = 0.0f;
  float cuivre_seat_ = 0.0f;
  // First-order ADAA on the tanh shock front: antialiases the bloomed upper
  // harmonics so they do not fold back in the high register.
  rt::Adaa1<rt::TanhNonlinearity> cuivre_adaa_{};

  // 4b: mute. mute_ == 0 -> skipped (bit-identical). A radiation-side formant
  // (peak) + notch pair that reshapes the bell output into the nasal muted colour.
  float mute_ = 0.0f;
  float mute_peak_b0_ = 0.0f;
  float mute_peak_a1_ = 0.0f;
  float mute_peak_a2_ = 0.0f;
  float mute_x1_ = 0.0f;
  float mute_x2_ = 0.0f;
  float mute_y1_ = 0.0f;
  float mute_y2_ = 0.0f;

  // 4c: half-valve. half_valve_ == 0 -> skipped (bit-identical). Extra in-loop
  // loss (a stuffier, more damped bore) plus a small detune of the loop delay.
  float half_valve_ = 0.0f;
  float half_valve_loss_ = 1.0f;

  // 4d: dynamic (2-DOF) lip. dyn_lip_ == 0 -> skipped (bit-identical). A second,
  // higher lip resonance (a bandpass) whose displacement adds to the reflection
  // coefficient, coupling a transverse mode into the swinging-door valve.
  float dyn_lip_ = 0.0f;
  float lip2_b0_ = 0.0f;
  float lip2_a1_ = 0.0f;
  float lip2_a2_ = 0.0f;
  float lip2_x1_ = 0.0f;
  float lip2_x2_ = 0.0f;
  float lip2_z1_ = 0.0f;
  float lip2_z2_ = 0.0f;
  float lip2_couple_ = 0.0f;

  // Lip resonator step: a two-pole (mass-spring) resonator driven by the pressure
  // difference; returns the lip displacement.
  float lip_resonator(float dp) noexcept;
  // Second lip mode (4d): the transverse resonance, same bandpass form.
  float lip_resonator2(float dp) noexcept;
  // Bell-loop pole for a brightness in [0,1], including the conical darkening
  // bias, shared by the note-on seed and live CC74 updates.
  float bell_alpha_for_brightness(float bright01) const noexcept;
};

}  // namespace sonare::midi::synth
