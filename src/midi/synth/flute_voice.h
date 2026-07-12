#pragma once

/// @file flute_voice.h
/// @brief Air-jet (flue / edge-tone) flute core for the NativeSynth voice — the
///        data-free flute model (concert flute / piccolo / recorder / pan flute /
///        shakuhachi / whistle, and jet-driven flue instruments in general;
///        McIntyre, Schumacher & Woodhouse 1983, Verge/Fabre/Hirschberg 1994-2000
///        lumped jet model, Cook/Scavone STK Flute). It completes the four
///        classic physical-modelling exciters — the bow (kBowedString), the reed
///        (kReed), the lip (kBrass) and, here, the AIR JET.
///
/// A flute is a turbulent air jet blown across a sharp edge (the labium) at the
/// mouth of an open pipe. The jet oscillates from one side of the edge to the
/// other, and the pipe's standing wave phase-locks that oscillation — the jet is
/// the nonlinear exciter, the bore is the linear resonator. Unlike the reed's
/// or the lip's memoryless pressure valve, the jet drive carries a DELAY (the
/// air's convection time from the flue slit to the edge), so the model needs a
/// SECOND delay line beside the bore, and it is the RATIO of the two (jet delay
/// vs bore delay) that selects which register the jet drives — the physical seat
/// of OVERBLOWING (blow harder and the tone jumps the octave). This is exactly
/// what the sibling flue pipe-organ core (pipe_organ_voice.h) leaves out: that
/// core is a linear resonator kept alive by a steady breath and is
/// unconditionally stable, whereas the flute SELF-OSCILLATES through the
/// nonlinear jet. The three parts:
///   1. AIR JET (nonlinear, delayed): a short jet delay line (the convection
///      time, jet_ratio * bore period) feeds a cubic jet function
///      jet(x) = clamp(x*(x^2 - 1), -1, 1) — the S-shaped saturating transfer of
///      the jet deflecting across the labium (the Fabre-Hirschberg lumped model,
///      STK JetTable). Its small-signal slope is inverting, so with the bore
///      reflection it forms the oscillator; the clamp bounds the limit cycle.
///   2. BORE: an open-open pipe as a travelling-wave delay line with a one-pole
///      loss lowpass (the frequency-dependent radiation loss at the two open
///      ends) and an in-loop DC blocker. Each open end reflects with a PRESSURE
///      INVERSION (a pressure node); the two inversions over a round trip cancel,
///      so the bore is a POSITIVE-feedback comb of one full period — the full
///      harmonic series an open flue pipe radiates, on which the jet locks the
///      fundamental.
///   3. BREATH + JET NOISE: a steady mouth pressure (the blowing) with a
///      multiplicative turbulence noise (the breathy air texture a flute has and
///      an additive/FM flute lacks) and a bright onset chiff (the pipe
///      "speaking"). Breath also selects the register: below a threshold the jet
///      never oscillates (the note is silent), and pushing the breath toward the
///      top of its band drives the tone brighter and, with the overblow gate,
///      up to the next register.
///
/// The delay buffers are NOT owned by the core: the host allocates one slab per
/// voice slot in prepare() (a bore span plus a jet span) and attach()es it before
/// start(). Radiation voicing is the shared BodyResonator on the voice, enabled
/// per preset; the core emits the raw bore pressure. Self-oscillating but
/// bounded: the jet cubic self-limits (clamped to [-1,1]) and the reflection
/// magnitudes are < 1.
///
/// RT contract: attach()/start()/render() are allocation-free (start zeroes /
/// seeds the attached spans). Determinism: the jet turbulence and the onset chiff
/// are the counter-based (voice_index, note, age) stream, so identical event
/// streams render bit-identically.

#include <cstddef>
#include <cstdint>

#include "midi/synth/voice_random.h"

namespace sonare::midi::synth {

/// Lowest fundamental the bore delay line is sized for; covers the bass-flute /
/// low pan-flute range with pitch-bend headroom (a flute never sounds below this,
/// but the margin keeps a bent-down low note inside the buffer).
inline constexpr float kFluteMinFundamentalHz = 40.0f;

/// Open-pipe loop length: the flute is open at both ends, so the bore is a
/// POSITIVE-feedback comb of one full fundamental period (the full harmonic
/// series, like the flue organ's open pipe) — the buffer must hold one period at
/// the lowest fundamental plus bend-down headroom.
inline constexpr float kFluteBoreLengthPeriods = 1.0f;

/// Per-span buffer capacity (samples) for @p sample_rate: one loop period at the
/// lowest fundamental (the 40 Hz floor folds in the bend-down headroom). The jet span reuses the
/// same size (its delay is jet_ratio * bore, always shorter).
inline int flute_buffer_capacity(double sample_rate) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  return static_cast<int>(kFluteBoreLengthPeriods * sr / kFluteMinFundamentalHz) + 8;
}

/// Whole-voice slab capacity (samples) the host must allocate: a bore span plus
/// a jet span.
inline int flute_slab_capacity(double sample_rate) noexcept {
  return 2 * flute_buffer_capacity(sample_rate);
}

/// Air-jet flute section of a NativeSynthPatch (used when mode == kFlute).
struct FlutePatchParams {
  /// Steady mouth pressure in [0,1]: how hard the player blows, i.e. the dynamic
  /// level and the jet drive. The jet oscillates only above a threshold, so very
  /// low values approach silence.
  float breath_pressure = 0.55f;
  /// Note velocity -> breath pressure in [0,1]: how much the struck velocity
  /// opens the breath (0 = velocity-independent dynamics, 1 = velocity is the
  /// dynamic).
  float vel_to_breath = 0.5f;
  /// Jet delay / bore delay ratio in (0,1): the convection time of the air jet
  /// relative to the bore period. Around 0.5 the jet drives the fundamental (the
  /// first register); this ratio, with the breath, is the physical seat of the
  /// register the pipe speaks in (the overblow point — smaller ratios drive the
  /// octave and above).
  float jet_ratio = 0.5f;
  /// Jet reflection coefficient in [0,1]: how strongly the bore's returning
  /// pressure deflects the jet at the flue. Higher = a stronger jet drive (more
  /// harmonics, closer to overblow); the STK-stable value is ~0.5.
  float jet_reflection = 0.5f;
  /// Bore end reflection coefficient in [0,1]: how strongly the open end reflects
  /// the wave back into the pipe. Higher = a purer, more sustained resonance; the
  /// STK-stable value is ~0.5.
  float end_reflection = 0.5f;
  /// Reflection-filter openness in [0,1]: how brightly the open end reflects the
  /// upper partials (1 = bright/brilliant piccolo, 0 = dark/covered). The loop
  /// lowpass.
  float brightness = 0.5f;
  /// Bore damping in [0,1]: extra loss on the reflection. Low = a purer, more
  /// sustained, sharply pitched tone (a flute); high = a damped, quick tone that
  /// does not overblow (an ocarina / blown bottle, a lumped Helmholtz resonator).
  float damping = 0.35f;
  /// Breath rise on note-on (ms): the jet takes a few periods to lock onto the
  /// bore, so the mouth pressure ramps in rather than stepping.
  float attack_ms = 18.0f;
  /// Breath fall on note-off (ms): the player stops blowing and the bore rings
  /// down.
  float release_ms = 90.0f;
  /// Jet turbulence in [0,1]: the breathy air noise on the mouth pressure (a
  /// flute's signature texture; shakuhachi = high, tin-whistle = low). The noise
  /// is the seeded per-voice stream so bounces stay bit-identical.
  float breath_noise = 0.15f;
  /// Onset speech transient (chiff) in [0,1]: the brief bright noise as the jet
  /// catches the edge (the recorder / flute "speak"), so the note articulates
  /// rather than swelling in.
  float chiff = 0.4f;
  /// Chiff decay time constant (ms).
  float chiff_ms = 12.0f;
  /// Vibrato rate (Hz) for the voice-local LFO (a solo flute's own vibrato, not a
  /// shared wind tremulant). Read at note-on.
  float vibrato_rate_hz = 5.0f;
  /// Vibrato depth in [0,1] (0 = no vibrato). Live-controllable (CC1). At 0 the
  /// LFO is skipped, so the render is bit-identical to a vibrato-free note.
  float vibrato_depth = 0.0f;

  // --- off-by-default advanced physics (Phase 4; C-ABI non-exposed, gated) ---
  /// Overblow in [0,1]: actively allow the breath to drive the jet into the next
  /// register (the octave jump of a hard-blown flute / recorder / shakuhachi).
  /// The Phase-1 core stays locked to the first register; this gate raises the
  /// jet gain toward the top of the breath band so the upper mode wins. 0 = off
  /// -> render is bit-identical (the register push is skipped).
  float overblow = 0.0f;
  /// Jet turbulence shaping in [0,1]: an amplitude-dependent, frequency-shaped
  /// refinement of the breath noise (the jet's edge noise grows and brightens
  /// with the flow, not a flat hiss). 0 = off -> render is bit-identical.
  float jet_turbulence = 0.0f;
  /// Edge hysteresis in [0,1]: the regime-change hysteresis of the jet as it
  /// swaps registers (Auvray-Ernoult-Fabre-Lagree 2014) — the register the pipe
  /// settles in depends on whether the breath is rising or falling. 0 = off ->
  /// render is bit-identical.
  float edge_hysteresis = 0.0f;
  /// Discrete-vortex source in [0,1]: for a narrow flue (W/h < 2) the jet sheds
  /// discrete vortices rather than the smooth jet-drive regime (Auvray 2014), a
  /// rougher, breathier source (some shakuhachi / ocarina voicings). 0 = off ->
  /// render is bit-identical.
  float vortex = 0.0f;
};

/// Per-voice flute state, embedded in NativeSynthVoice. The voice's amplitude
/// envelope / filter / mod matrix and the shared BodyResonator wrap around this
/// core; render() returns the raw bore pressure.
class FluteVoiceCore {
 public:
  /// CONTROL-thread wiring (or audio-thread pointer assignment before start()):
  /// hands the core its delay slab (two spans of @p per_span_capacity: a bore
  /// span then a jet span). The slab outlives the voice.
  void attach(float* slab, int per_span_capacity) noexcept {
    bore_ = slab;
    jet_ = slab != nullptr ? slab + per_span_capacity : nullptr;
    capacity_ = per_span_capacity;
  }

  /// Configures the bore / jet for @p note / @p velocity and zeroes / seeds the
  /// used spans. @p seed drives the deterministic jet turbulence and onset chiff.
  void start(const FlutePatchParams& params, double sample_rate, uint8_t note, uint8_t velocity,
             uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (bend / drift), 1 = on pitch.
  float render(float pitch_ratio) noexcept;
  /// Note-off: stop blowing (ramp the breath to zero); the bore rings down.
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

  // --- live continuous control (a flute is a continuous-control instrument; the
  // host drives these from MIDI CCs while the note sounds). Each sets a smoothing
  // TARGET the render ramps toward (no zipper); call snap_flute_control() to jump
  // to the targets without a glide. Loudness/dynamics ride the voice's velocity /
  // expression VCA, not the breath (pushing the breath toward the jet threshold
  // would silence the tone). ---

  /// Mouth pressure in [0,1] (CC2 breath): bounded to the jet's stable band, so
  /// more breath colours the tone (brighter, breathier, closer to the overblow
  /// edge) without crossing into the silent under-blown regime.
  void set_breath(float breath01) noexcept;
  /// Reflection-filter brightness in [0,1] (CC74): opens the open-end reflection
  /// filter, the clean timbral brightness control.
  void set_brightness(float bright01) noexcept;
  /// Vibrato depth in [0,1] (CC1 modulation wheel): the voice-local pitch/level
  /// vibrato. 0 = off (the LFO is skipped).
  void set_vibrato(float depth01) noexcept;
  /// Jump the smoothed controls to their targets (seed a fresh note at the host's
  /// current CC positions without an audible glide).
  void snap_flute_control() noexcept;

 private:
  // Bore + jet delay lines (host-owned): the travelling-wave air column and the
  // air-jet convection line.
  float* bore_ = nullptr;
  float* jet_ = nullptr;
  int capacity_ = 0;
  int bore_size_ = 0;
  int jet_size_ = 0;
  size_t bore_write_ = 0;
  size_t jet_write_ = 0;
  // Last bore delay-line output (the pressure returning to the mouth next sample).
  float bore_out_ = 0.0f;

  // Tuning: the bore loop period (samples, ~one fundamental period), the delay
  // not carried in the line (feedback register + loop-filter phase), and the jet
  // delay as a fraction of the bore LINE delay (jet_delay = jet_ratio * (period -
  // comp), the STK jet-convection length).
  float bore_period_ = 0.0f;
  float comp_ = 1.0f;
  float jet_ratio_ = 0.4f;

  // Open-end reflection: one-pole loss lowpass y += alpha*(x - y) (the frequency-
  // dependent radiation loss), an overall loss gain, and the inverting reflection
  // (folded in render as -loss_gain*lp_state).
  float lp_alpha_ = 1.0f;
  float lp_state_ = 0.0f;
  float loss_gain_ = 1.0f;
  // Jet / end reflection coefficients (the two feedback taps of the jet-drive
  // model).
  float jet_reflection_ = 0.5f;
  float end_reflection_ = 0.5f;
  // In-loop DC blocker on the jet output (the jet's rectified DC does not
  // radiate and would charge the bore).
  float dc_x1_ = 0.0f;
  float dc_y1_ = 0.0f;
  float dc_r_ = 0.0f;

  // Even-harmonic pump: a real jet is offset from the labium, so its drive is
  // asymmetric and the OCTAVE (2f0) dominates a flue instrument's spectrum — the
  // "open flute pipe" colour a symmetric odd jet loop lacks. The squared bore
  // feedback carries a 2f0 component; high-passing off its DC and injecting it
  // into the bore (which resonates at 2f0) pumps the octave. even_gain_ scales it
  // (0 -> no pump); even_state_ is the DC follower subtracted to keep only the AC.
  float even_gain_ = 0.0f;
  float even_state_ = 0.0f;
  float even_hp_alpha_ = 0.0f;

  // Breath contour: a one-pole ramp of the mouth pressure toward the target level
  // (1 while blowing, 0 once released). breath_target_ is the steady mouth
  // pressure (live-smoothed toward breath_ctrl_target_).
  float breath_target_ = 0.55f;
  float breath_level_ = 0.0f;
  float attack_coeff_ = 0.0f;
  float release_coeff_ = 0.0f;
  bool releasing_ = false;

  // Live-control smoothing: the render ramps breath_target_ / lp_alpha_ toward
  // these CC targets so a moving controller never zippers. Initialised equal to
  // the note-on values, so an untouched note renders exactly as Phase 1.
  float ctrl_coeff_ = 1.0f;
  float breath_ctrl_target_ = 0.55f;
  float lp_alpha_target_ = 1.0f;

  // Jet turbulence (deterministic multiplicative mouth-pressure noise).
  float breath_noise_ = 0.0f;

  // Onset chiff (one-pole decaying noise burst, the pipe "speak").
  float chiff_level_ = 0.0f;
  float chiff_coeff_ = 0.0f;

  // Voice-local vibrato LFO (a solo flute's own vibrato). depth_ == 0 -> skipped
  // (bit-identical). Modulates the loop delay (pitch) and a touch of level.
  float vib_depth_ = 0.0f;
  float vib_depth_target_ = 0.0f;
  float vib_phase_ = 0.0f;
  float vib_inc_ = 0.0f;

  // Output trim bringing the raw bore pressure up to a musical voice level,
  // calibrated so a forte note peaks near the other engines' output range.
  float output_scale_ = 1.0f;

  VoiceRandomSequence noise_;
  uint64_t drive_index_ = 0;

  // --- off-by-default advanced physics (Phase 4; skipped entirely when off, so
  // the render path is bit-identical to the linear jet model). ---

  // 4a: overblow. overblow_ == 0 -> skipped. Raises the jet gain toward the top
  // of the breath band so the upper register wins (the octave jump).
  float overblow_ = 0.0f;
  // 4b: jet turbulence shaping. jet_turb_ == 0 -> skipped. An amplitude-dependent
  // one-pole shaping of the breath noise.
  float jet_turb_ = 0.0f;
  float jet_turb_state_ = 0.0f;
  // 4c: edge hysteresis. edge_hyst_ == 0 -> skipped. A slow follower of the
  // breath direction that biases the effective jet gain.
  float edge_hyst_ = 0.0f;
  float edge_hyst_state_ = 0.0f;
  // 4d: discrete-vortex source. vortex_ == 0 -> skipped. A rougher, amplitude-
  // gated noise burst added to the jet drive.
  float vortex_ = 0.0f;
};

}  // namespace sonare::midi::synth
