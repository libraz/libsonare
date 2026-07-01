#pragma once

/// @file reed_voice.h
/// @brief Reed-woodwind (breath-excited) core for the NativeSynth voice — the
///        data-free woodwind model (clarinet / saxophone / oboe / bassoon, and
///        single/double-reed winds in general; McIntyre, Schumacher & Woodhouse
///        1983, Smith's digital-waveguide single-reed). Where the bowed string
///        is a friction-excited sustained tone, the reed is a BREATH-excited
///        sustained tone: the player's mouth pressure blows a nonlinear reed
///        valve that drives a resonant air column, and the note holds until the
///        breath stops.
///
/// The model is the standard Smith single-reed digital waveguide, the same one
/// STK's Clarinet implements. The air column is a travelling-wave delay line
/// (the bore); at the mouthpiece a NONLINEAR REED VALVE couples the player's
/// breath to the bore. The reed is a memoryless "reed table" mapping the
/// pressure difference across the reed (mouth pressure minus the pressure
/// reflected back up the bore) to a reflection coefficient — the reed opens and
/// closes with the pressure, and this one nonlinearity, with the bore's
/// resonance, is the whole instrument:
///   1. REED VALVE (reed table): coeff = clamp(offset + slope*dp, -1, 1). The
///      offset is the reed's rest opening (bigger offset = a smaller tip gap,
///      the reed closes more easily); the slope is the reed stiffness (steeper =
///      a harder, brighter reed). When the bore pressure rises the reed pinches
///      shut, gating the breath into pressure pulses — the self-sustained
///      oscillation that a real reed's stick/beat against the mouthpiece gives.
///   2. BORE TOPOLOGY — CYLINDER vs CONE: a clarinet's CYLINDRICAL bore is
///      closed at the reed and open at the bell, a quarter-wave resonator that
///      sounds an octave lower for its length and radiates ODD HARMONICS ONLY
///      (it overblows at the twelfth). A saxophone/oboe/bassoon's CONICAL bore
///      behaves like an open pipe — the FULL harmonic series, overblowing at the
///      octave. This is the loop topology, not an EQ: the cylinder is a
///      NEGATIVE-feedback comb of half the period (sign flips every M samples ->
///      f0, 3f0, 5f0 …), the cone a POSITIVE-feedback comb of the full period
///      (f0, 2f0, 3f0 …). The odd-only clarinet spectrum falls out of the
///      physics, the same way the stopped organ pipe's does. The cone is the
///      cylindrical approximation (STK Saxofony style); the true conical
///      waveguide (apex junction + growth term) is a later phase.
///   3. BELL TERMINATION: the open bell reflects pressure with a sign inversion
///      through a one-pole loss filter (the bore's round-trip damping and the
///      energy the bell radiates). Brightness voices that filter; damping sets
///      the loss.
///   4. BREATH CONTOUR: the breath rises into the note (the reed takes a few
///      periods to lock into oscillation) and falls when the player tongues off.
///      A small internal envelope drives the mouth pressure; the reed table's
///      saturation self-regulates the amplitude, so the loop needs no explicit
///      level normalisation to stay bounded. A reed only speaks ABOVE a breath /
///      stiffness threshold — below it the valve never beats and the note is
///      silent — so the contour is calibrated to cross into the oscillating
///      region promptly.
///
/// The delay buffer is NOT owned by the core: the host allocates one slab per
/// voice slot in prepare() (one bore span) and attach()es it before start().
/// Radiation/formant voicing (the bell and the woodwind's formants) is the
/// shared BodyResonator on the voice, enabled per preset; the core emits the raw
/// bore pressure. Self-sustained but unconditionally stable: the reed table
/// output is bounded to [-1,1] and the bell loss gain is < 1.
///
/// RT contract: attach()/start()/render() are allocation-free (start zeroes /
/// seeds the attached span). Determinism: the breath turbulence and the onset
/// chiff are drawn from the counter-based (voice_index, note, age) stream, so
/// identical event streams render bit-identically.

#include <cstddef>
#include <cstdint>

#include "midi/synth/voice_random.h"

namespace sonare::midi::synth {

/// Lowest fundamental the bore delay line is sized for; covers the contrabassoon
/// low range with pitch-bend headroom (a reed wind never sounds below this, but
/// the margin keeps a bent-down low note inside the buffer).
inline constexpr float kReedMinFundamentalHz = 20.0f;

/// Per-bore buffer capacity (samples) for @p sample_rate: a full period at the
/// lowest fundamental (the cylinder uses half this; sizing for the whole period
/// leaves room for the conical (full-period) bore and bend-down headroom).
inline int reed_buffer_capacity(double sample_rate) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  return static_cast<int>(sr / kReedMinFundamentalHz) + 8;
}

/// Whole-voice slab capacity (samples) the host must allocate: one bore span.
inline int reed_slab_capacity(double sample_rate) noexcept {
  return reed_buffer_capacity(sample_rate);
}

/// Reed-woodwind section of a NativeSynthPatch (used when mode == kReed).
struct ReedPatchParams {
  /// Steady mouth pressure in [0,1]: how hard the player blows, i.e. the dynamic
  /// level. It sets the breath that drives the reed valve; the reed only speaks
  /// above a threshold, so very low values approach silence.
  float breath_pressure = 0.6f;
  /// Note velocity -> breath pressure in [0,1]: how much the struck velocity
  /// opens the breath (0 = velocity-independent dynamics, 1 = velocity is the
  /// dynamic).
  float vel_to_breath = 0.6f;
  /// Reed stiffness in [0,1] (the reed-table slope): soft, dark reed through a
  /// hard, bright, buzzier reed. Higher = a steeper table = a stiffer reed.
  float reed_stiffness = 0.5f;
  /// Reed rest opening in [0,1] (the reed-table offset): how far the reed tip
  /// sits open at rest. 1 = a wide-open, freely-blowing reed; 0 = a tightly
  /// closed reed that pinches shut sooner (a more pinched, nasal tone).
  float reed_opening = 0.5f;
  /// Conical bore: false = a cylindrical bore closed at the reed (clarinet) with
  /// odd harmonics only; true = a conical bore (saxophone / oboe / bassoon)
  /// approximated as an open pipe with the full harmonic series.
  bool conical = false;
  /// Bell reflection-filter openness in [0,1]: how brightly the bore reflects at
  /// the bell (1 = bright/edgy, 0 = dark/covered). The loop lowpass.
  float brightness = 0.5f;
  /// Bore damping in [0,1]: the bell loop loss. Low = a purer, more sustained,
  /// sharply pitched tone; high = a more damped, quicker-speaking tone. The
  /// breath replenishes the loss either way (the note is sustained).
  float damping = 0.4f;
  /// Breath rise on note-on (ms): the reed takes a few periods to lock into
  /// oscillation, so the mouth pressure ramps in rather than stepping.
  float attack_ms = 40.0f;
  /// Breath fall on note-off (ms): the player tongues off and the bore rings
  /// down.
  float release_ms = 80.0f;
  /// Breath turbulence in [0,1]: a subtle deterministic noise on the mouth
  /// pressure (the air rushing past the reed), 0 = a perfectly steady breath.
  /// Kept small; the noise is the seeded per-voice stream so bounces stay
  /// bit-identical.
  float breath_noise = 0.12f;
  /// Onset speech transient (chiff) in [0,1]: the brief bright noise as the reed
  /// starts to speak, so the note articulates rather than swelling in.
  float chiff = 0.4f;
  /// Chiff decay time constant (ms).
  float chiff_ms = 12.0f;

  // --- off-by-default advanced physics (Phase 4; C-ABI non-exposed, gated) ---
  /// Dynamic (mass-spring) reed: the memoryless reed table assumes a massless
  /// reed, so it lacks the reed's own resonance and inertia. This adds a damped
  /// mass-spring reed whose displacement biases the table's operating point — the
  /// reed rings at its natural frequency, boosting the partials near it (the
  /// "reed formant" edge) and adding a live beating to the attack, the way a real
  /// cane reed does. Like the bowed string's elasto-plastic friction, the sharp
  /// table is KEPT (so the harmonic structure survives) and only biased. OFF by
  /// default -> the render path is bit-identical to the memoryless table, and
  /// neither goldens nor parity move.
  bool dynamic_reed = false;
  /// Reed natural frequency in [0,1] (only when dynamic_reed): low = a soft, low
  /// reed resonance (a darker, rounder cane reed); high = a stiff, high reed
  /// resonance (a brighter, edgier reed formant).
  float reed_resonance = 0.5f;

  /// Register vent in [0,1]: opening the register key vents the bore near the
  /// mouthpiece, damping the fundamental so the tube speaks its upper register
  /// (a clarinet's twelfth, a cone's octave). Modelled as a gated in-loop
  /// damping of the low band that pushes the dominant mode upward toward the
  /// register break. 0 = off -> render is bit-identical (the vent is skipped).
  float register_vent = 0.0f;

  /// Growl in [0,1]: the sax/reed "growl" — a low-frequency modulation of the
  /// breath (from humming or fluttering while blowing) that sidebands the tone
  /// with a rough, vocal edge. A deterministic sub-audio LFO on the mouth
  /// pressure. 0 = off -> render is bit-identical (no modulation).
  float growl = 0.0f;
};

/// Per-voice reed-woodwind state, embedded in NativeSynthVoice. The voice's
/// amplitude envelope / filter / mod matrix and the shared BodyResonator wrap
/// around this core; render() returns the raw bore pressure.
class ReedVoiceCore {
 public:
  /// CONTROL-thread wiring (or audio-thread pointer assignment before start()):
  /// hands the core its bore delay span (@p capacity samples). The slab outlives
  /// the voice.
  void attach(float* bore, int capacity) noexcept {
    bore_ = bore;
    capacity_ = capacity;
  }

  /// Configures the bore for @p note / @p velocity and zeroes / seeds the used
  /// span. @p seed drives the deterministic breath noise and onset chiff.
  void start(const ReedPatchParams& params, double sample_rate, uint8_t note, uint8_t velocity,
             uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (bend / vibrato / drift), 1 = on pitch.
  float render(float pitch_ratio) noexcept;
  /// Note-off: tongue off (ramp the breath to zero); the bore rings down.
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

  // --- live continuous control (reeds are a continuous-control instrument; the
  // host drives these from MIDI CCs while the note sounds). Each sets a smoothing
  // TARGET the render ramps toward (no zipper); call snap_reed_control() to jump
  // to the targets without a glide. Loudness/dynamics are NOT driven here — they
  // come from the voice's velocity / expression VCA, because pushing the breath
  // toward the beating threshold would silence the reed. ---

  /// Mouth pressure in [0,1] (CC2 breath): bounded to the reed's stable
  /// oscillating band, so more breath colours the tone (brighter, closer to the
  /// beating edge) without ever crossing into the slammed-shut silent regime.
  void set_breath(float breath01) noexcept;
  /// Bell brightness in [0,1] (CC74): opens the bell reflection filter, the clean
  /// timbral brightness control.
  void set_brightness(float bright01) noexcept;
  /// Jump the smoothed controls to their targets (seed a fresh note at the
  /// host's current CC positions without an audible glide).
  void snap_reed_control() noexcept;

 private:
  // Bore delay line (host-owned): the travelling-wave air column.
  float* bore_ = nullptr;
  int capacity_ = 0;
  int bore_size_ = 0;
  size_t bore_write_ = 0;
  // Last delay-line output (the pressure returning to the reed next sample).
  float bore_out_ = 0.0f;

  // Tuning: the loop period (samples) and the delay not carried in the line
  // (feedback register + loop-filter phase). The cylinder uses half the period
  // (negative-feedback comb, odd harmonics); the cone the full period.
  float bore_period_ = 0.0f;
  float comp_ = 1.0f;
  // Feedback sign: -1 = cylinder (odd harmonics), +1 = cone (full harmonics).
  float sign_ = -1.0f;

  // Bell reflection: one-pole loop lowpass y += alpha*(x - y), a loss gain, and
  // the sign (folded in render).
  float lp_alpha_ = 1.0f;
  float lp_state_ = 0.0f;
  float loss_gain_ = 0.95f;
  // In-loop DC blocker (the cone's positive-feedback comb has a DC mode that
  // does not radiate; the cylinder needs it too once driven).
  float dc_x1_ = 0.0f;
  float dc_y1_ = 0.0f;
  float dc_r_ = 0.0f;

  // Reed table (memoryless valve): coeff = clamp(offset + slope*dp, -1, 1).
  // offset = the reed rest opening, slope < 0 = the reed stiffness.
  float reed_offset_ = 0.7f;
  float reed_slope_ = -0.3f;

  // Breath contour: a one-pole ramp of the mouth pressure toward the target
  // level (1 while blowing, 0 once tongued off). breath_target_ is the steady
  // mouth pressure (live-smoothed toward breath_ctrl_target_).
  float breath_target_ = 0.6f;
  float breath_level_ = 0.0f;
  float attack_coeff_ = 0.0f;
  float release_coeff_ = 0.0f;
  bool releasing_ = false;

  // Live-control smoothing: the render ramps breath_target_ / lp_alpha_ toward
  // these CC targets so a moving controller never zippers. Initialised equal to
  // the note-on values, so an untouched note renders exactly as Phase 1.
  float ctrl_coeff_ = 1.0f;
  float breath_ctrl_target_ = 0.6f;
  float lp_alpha_target_ = 1.0f;

  // Breath turbulence (deterministic mouth-pressure noise; 0 = steady breath).
  float breath_noise_ = 0.0f;

  // Onset chiff (one-pole decaying noise burst, the reed's "speak").
  float chiff_level_ = 0.0f;
  float chiff_coeff_ = 0.0f;

  // Output trim bringing the raw bore pressure up to a musical voice level,
  // calibrated so a forte note peaks near the other engines' output range.
  float output_scale_ = 1.0f;

  VoiceRandomSequence noise_;
  uint64_t drive_index_ = 0;

  // --- off-by-default advanced physics (Phase 4; skipped entirely when off, so
  // the render path is bit-identical to the memoryless model). ---

  // Dynamic (mass-spring) reed: a damped resonator driven by the pressure
  // difference, whose displacement biases the reed table. Returns the bias.
  float reed_resonator(float dp) noexcept;

  // 4a: dynamic reed. reed_dyn_ gates it; the resonator is a biquad bandpass
  // (b0, a1, a2) tuned to the reed's natural frequency, its output scaled by
  // reed_couple_ into the table's operating point.
  bool reed_dyn_ = false;
  float reed_b0_ = 0.0f;
  float reed_a1_ = 0.0f;
  float reed_a2_ = 0.0f;
  float reed_z1_ = 0.0f;
  float reed_z2_ = 0.0f;
  float reed_couple_ = 0.0f;

  // 4b: register vent. reg_vent_ == 0 -> skipped (bit-identical). A one-pole
  // low-band follower whose output is subtracted from the loop reflection,
  // damping the fundamental so the dominant mode rises toward the register break.
  float reg_vent_ = 0.0f;
  float reg_lp_alpha_ = 0.0f;
  float reg_lp_state_ = 0.0f;

  // 4c: growl. growl_depth_ == 0 -> skipped (bit-identical). A deterministic
  // sub-audio LFO amplitude-modulating the mouth pressure.
  float growl_depth_ = 0.0f;
  float growl_phase_ = 0.0f;
  float growl_inc_ = 0.0f;
};

}  // namespace sonare::midi::synth
