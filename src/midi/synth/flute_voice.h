#pragma once

/// @file flute_voice.h
/// @brief Air-jet (flue / edge-tone) flute core for the NativeSynth voice —
///        concert flute, piccolo, recorder, pan flute, shakuhachi, whistle. A
///        cubic Fabre-Hirschberg jet table drives an open-open bore
///        (positive-feedback comb of one full period), completing the four
///        classic exciters beside the bow, the reed and the lip
///        (Verge/Fabre/Hirschberg 1994-2000).
///
/// The jet drive carries a DELAY the reed's and the lip's pressure valves do
/// not — the air's convection time from slit to labium — so there is a second
/// delay line beside the bore, and the RATIO of the two is what selects the
/// register. That ratio is the physical seat of overblowing, and it is what the
/// sibling pipe-organ core deliberately leaves out. Breath level also gates the
/// register: below a threshold the jet does not oscillate and the note is
/// silent.
///
/// The delay buffers are not owned here — the host attaches a bore span plus a
/// jet span per voice slot before start(), and radiation voicing is the shared
/// BodyResonator, so the core emits raw bore pressure. Self-oscillating but
/// bounded: the jet cubic clamps to [-1,1] and the reflection magnitudes are < 1.
///
/// RT contract: attach()/start()/render() are allocation-free. Determinism: jet
/// turbulence and onset chiff come from the counter-based
/// (voice_index, note, age) stream, so identical events render bit-identically.

#include <cstddef>
#include <cstdint>

#include "midi/control_value.h"
#include "midi/synth/excitation_axes.h"
#include "midi/synth/voice_random.h"
#include "midi/synth/wind_bore.h"
#include "midi/synth/wind_breath.h"
#include "rt/biquad_design.h"
#include "util/constants.h"

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
  const double sr = sample_rate > 0.0 ? sample_rate : ::sonare::constants::kDefaultDawSampleRate;
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
  /// Vortex (vena-contracta) loss at the mouth, K in u + K u|u| = r: the flow the
  /// returning bore pressure drives through the embouchure saturates as K|u| passes
  /// one, so this is what bounds the oscillation when the jet does not. Dimensionless
  /// in bore-pressure units. 0 = off -> render is bit-identical.
  float vortex_loss = 0.0f;

  // --- jet-drive source (Verge et al. 1994; gated, off by default) ---
  /// Replaces the cubic jet table and the even-harmonic pump with the jet-drive
  /// source: the time difference of the flow split tanh((eta - y0)/b) at the
  /// labium, scaled by the jet speed Uj ~ sqrt(P), with the jet's convection delay
  /// going as 1/Uj so it shortens as the breath rises. false = off -> bit-identical.
  bool jet_drive = false;
  /// Jet centreline offset from the labium at rest, y0/b in jet half-widths
  /// ([0, 2]): the flow-split asymmetry that voices the even harmonics on the
  /// jet-drive path. Read only when jet_drive is on.
  float jet_offset = 0.0f;

  /// Tone-hole lattice cutoff (Hz), the same at every note: above it the wave runs
  /// out through the open holes and the bore stops supporting partials (Benade).
  /// A Butterworth section on the returning pressure; 0 = off.
  float lattice_cutoff_hz = 0.0f;

  /// Share of the jet's rest offset taken off per octave above the first register,
  /// floored at centred: overblown registers are blown on the labium's line, so their
  /// even harmonics fall away (Fletcher 1975). 0 = off.
  float jet_centring_per_octave = 0.0f;

  /// Receptivity window (jet-drive only): the jet reads the returning flow as a triangular
  /// average of this half-width in shares of the jet delay, centred on it, so every harmonic
  /// sees the delay alone (-3 dB at a Strouhal number of 1/(pi * share)). [0, 0.7], 0 = off.
  float jet_receptivity_window = 0.0f;
  /// Zero-mean deflection (jet-drive only): a pure integrator giving up its cycle mean once a
  /// period in place of the leak, with no DC blocker on the source (the split's time difference
  /// carries none), so the jet branch holds no one-pole at all. false = off.
  bool jet_cycle_mean = false;

  /// Tongued-onset breath overshoot (dB of mouth pressure) at velocity 127: how
  /// far the mouth pressure stands above its held level as the tongue releases
  /// it, before it settles. The radiated overshoot is the jet's response to
  /// this, not a copy of it. Applies only while no input streams the breath.
  /// 0 = off -> bit-identical.
  float onset_breath_overshoot_db = 0.0f;
  /// Time the breath overshoot takes to settle back onto the held level (ms, ~95%).
  float onset_breath_settle_ms = 40.0f;
  /// Change of the breath overshoot per doubling of velocity (dB), read down from
  /// velocity 127: positive gives a soft note less of it.
  float onset_breath_overshoot_vel_db = 0.0f;
  /// Exponent of the voice-amplifier velocity curve, (velocity/127)^e; 2 is the
  /// SoundFont curve, bit-identical to it.
  float velocity_exponent = 2.0f;
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
    bore_.buffer = slab;
    // Both spans are the same length, and bore_.capacity has to hold it from
    // here rather than from start(): the bore's own delay clamp reads it, while
    // the jet's reads capacity_, and the two must never disagree.
    bore_.capacity = per_span_capacity;
    jet_ = slab != nullptr ? slab + per_span_capacity : nullptr;
    capacity_ = per_span_capacity;
  }

  /// Configures the bore / jet for @p note / @p velocity and zeroes / seeds the
  /// used spans. @p seed drives the deterministic jet turbulence and onset chiff.
  void start(const FlutePatchParams& params, double sample_rate, uint8_t note, Velocity16 velocity,
             uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (bend / drift), 1 = on pitch.
  float render(float pitch_ratio) noexcept;
  /// Note-off: stop blowing (ramp the breath to zero); the bore rings down.
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;
  /// The factor the loop runs at per host sample, settled at start(); 1 when no line is floored.
  int oversample() const noexcept { return decimator_.factor(); }
  /// The jet line's delay at the last rendered sample, in samples at the loop's rate.
  float jet_delay_samples() const noexcept { return jet_delay_; }

  // --- live continuous control (a flute is a continuous-control instrument; the
  // host drives these from MIDI CCs while the note sounds). Each sets a smoothing
  // TARGET the render ramps toward (no zipper); call snap_excitation() to jump
  // to the targets without a glide. Loudness/dynamics ride the voice's velocity /
  // expression VCA, not the breath (pushing the breath toward the jet threshold
  // would silence the tone). ---

  /// Base axis positions in [0,1] from the patch or a CC. @p present names the
  /// fields the caller filled (ExcitationAxisMask); an axis it does not name
  /// keeps the value the note started with. Force is mouth pressure (CC2),
  /// bounded to the jet's stable band so more breath colours the tone toward the
  /// overblow edge without crossing into the silent under-blown regime;
  /// brightness (CC74) opens the open-end reflection filter.
  void set_excitation_base(const ExcitationAxes& base, uint32_t present) noexcept;
  void restore_excitation_base() noexcept;
  /// Drops the onset overshoot: the breath is driven by an input, or velocity
  /// means nothing, so the synthesized tonguing would play over the player.
  void hold_onset() noexcept { breath_.hold_overshoot(); }
  /// Vibrato depth in [0,1] (CC1 modulation wheel): the voice-local pitch/level
  /// vibrato. 0 = off (the LFO is skipped). Not an excitation axis — it is the
  /// preset's own vibrato rather than anything the exciter does.
  void set_vibrato(float depth01) noexcept;
  /// Mod-matrix offsets on the same axes, in the same normalized units. Held
  /// apart from the base so the two compose rather than overwrite, and applied
  /// through the same smoothing ramp: this is a control-rate destination, not an
  /// audio-rate path into the bore.
  void set_excitation_mod(const ExcitationAxes& offsets) noexcept;
  /// Jump the smoothed controls to their targets (seed a fresh note at the host's
  /// current CC positions without an audible glide).
  void snap_excitation() noexcept;

 private:
  // Builds the whole voice for a loop running at @p sr (the host rate times the oversampling
  // factor) and returns the loop's budget at that rate.
  LoopBudget configure(const FlutePatchParams& params, double sr, uint8_t note, Velocity16 velocity,
                       uint64_t seed) noexcept;
  // One sample of the loop at the internal rate.
  float render_internal(float pitch_ratio) noexcept;
  // Recomposes the two smoothing targets from their bases and the offsets.
  void refresh_excitation_targets() noexcept;
  // Re-solves bore_.comp from the pole lp_alpha_ now holds.
  void retune_loop_comp() noexcept;

  // Bore + jet delay lines (host-owned): the travelling-wave air column and the
  // air-jet convection line.
  WindBore bore_{nullptr, 0, 0, 0, 0.0f, 1.0f, 0.0f};
  float* jet_ = nullptr;
  int capacity_ = 0;
  size_t jet_write_ = 0;

  // Tuning: bore_'s period is the bore loop period (samples, ~one fundamental
  // period) and comp the delay not carried in the line (feedback register +
  // loop-filter phase); jet_ratio_ is the fraction of the PERIOD the jet
  // convection spans (jet_delay = jet_ratio * period), a duration at every
  // note and rate.
  // The jet line is read at no less than one sample (the smallest delay the
  // passive Lagrange stencil supports); a period too short for that runs the
  // loop at an integer multiple of the host rate, so the ratio holds at every
  // note below Nyquist.
  float jet_ratio_ = 0.4f;
  float jet_delay_ = 0.0f;
  /// The played fundamental (Hz) and the sample rate, held for the pole
  /// mapping and for retune_loop_comp(), which follows the smoothed pole on
  /// every live brightness move (a CC74-live axis), not only at note-on.
  float f0_ = 0.0f;
  /// The rate the loop runs at (host rate times the oversampling factor).
  float srf_ = 48000.0f;
  /// Brings the oversampled loop back to the host rate; the identity at factor 1.
  LoopDecimator decimator_;
  /// The lp_alpha_ bore_.comp was last solved for.
  float comp_alpha_ = -1.0f;

  // Open-end reflection: one-pole loss lowpass y += alpha*(x - y) (the frequency-
  // dependent radiation loss), an overall loss gain, and the inverting reflection
  // (folded in render as -loss_gain*lp_state). Both are solved together, from
  // two per-traversal gains quoted in Hz, whenever the pole is refreshed
  // (refresh_excitation_targets()); damping_gain_ is the fixed, note/rate-
  // invariant gain damping alone sets and the solve reproduces at the
  // fundamental.
  float lp_alpha_ = 1.0f;
  float lp_state_ = 0.0f;
  float loss_gain_ = 1.0f;
  float damping_gain_ = 1.0f;
  // Tone-hole lattice cutoff (Hz) on the returning pressure, a Butterworth section; 0 -> skipped.
  // Its phase delay at f0 is held so retune_loop_comp() can take it off the bore.
  float lattice_hz_ = 0.0f;
  rt::BiquadState lattice_{};
  float lattice_comp_ = 0.0f;
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
  // Register fade of the jet's offset in [0,1], 1 when off; the pump reads it live.
  float even_fade_ = 1.0f;
  float jet_asym_ = 0.0f;

  // Breath contour: a one-pole ramp of the mouth pressure toward the target level
  // (1 while blowing, 0 once released). breath_target_ is the steady mouth
  // pressure (live-smoothed toward breath_ctrl_target_).
  float breath_target_ = 0.55f;
  OnsetContour breath_{};
  bool killed_ = true;

  // Live-control smoothing: the render ramps breath_target_ / lp_alpha_ toward
  // these CC targets so a moving controller never zippers. Initialised equal to
  // the note-on values, so an untouched note renders exactly as Phase 1.
  float ctrl_coeff_ = 1.0f;
  float breath_ctrl_target_ = 0.55f;
  float lp_alpha_target_ = 1.0f;
  float loss_gain_target_ = 1.0f;
  // The normalized bases behind those two targets, and the matrix offsets on
  // them; the targets are always the composed pair. force01_base/bright01_base
  // start at flute's own 0.55/0.5 (ExcitationBases's in-class defaults are
  // placeholders shared with other engines, not this one's).
  ExcitationBases excite_{0.55f, 0.5f, 0.0f, 0.0f};

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
  // one-pole shaping of the breath noise. Its coefficient is a pole and so is a
  // count of samples; both followers here are voiced at kLossVoicedSr and mapped
  // onto the running rate in start(), the same way the bore's own pole is.
  float jet_turb_ = 0.0f;
  float jet_turb_state_ = 0.0f;
  float jet_turb_alpha_ = 0.0f;
  // 4c: edge hysteresis. edge_hyst_ == 0 -> skipped. A slow follower of the
  // breath direction that biases the effective jet gain.
  float edge_hyst_ = 0.0f;
  float edge_hyst_state_ = 0.0f;
  float edge_hyst_alpha_ = 0.0f;
  // 4d: discrete-vortex source. vortex_ == 0 -> skipped. A rougher, amplitude-
  // gated noise burst added to the jet drive.
  float vortex_ = 0.0f;
  // Mouth vortex loss K on the returning flow. 0 -> skipped.
  float vortex_loss_ = 0.0f;

  // Jet-drive source (jet_drive; skipped entirely when off).
  bool jet_drive_ = false;
  // y0/b, the flow split's offset in jet half-widths, and the source gain holding its rest slope.
  float jet_offset_ = 0.0f;
  float jet_drive_gain_ = 1.0f;
  // Jet speed at the patch's own breath, over its full-breath value: the speed the
  // jet delay is normalised to.
  float jet_speed_ref_ = 1.0f;
  // 1 / |first difference| at the fundamental, so the source's gain at f0 is the
  // same at every note and rate.
  float jet_drive_norm_ = 1.0f;
  // The flow split one sample back, for its time difference.
  float jet_flow_z1_ = 0.0f;
  // The jet's displacement at the labium, eta: a leaky integral of the acoustic flow.
  float jet_eta_ = 0.0f;
  float jet_eta_leak_ = 0.0f;
  // Receptivity lowpass on the flow the jet reads: its coefficient, state, and the phase delay
  // (samples) at f0 taken off the jet line. All zero when off.
  float jet_lp_alpha_ = 0.0f;
  float jet_lp_state_ = 0.0f;
  float jet_lp_comp_ = 0.0f;
  // Receptivity window: half-width (samples) of the triangular average read off the jet line in
  // place of the point read, centred on the jet delay. 0 = the point read.
  int jet_window_half_ = 0;
  // Cycle mean of the displacement: its length (samples, one period), the running count and
  // sum; at each boundary the mean is taken off the integrator's state. 0 = off.
  int jet_cycle_ = 0;
  int jet_cycle_count_ = 0;
  float jet_cycle_acc_ = 0.0f;

  // Reads the jet line as a triangular average centred on @p delay after writing @p input.
  float jet_window_read(float delay, float input) noexcept;
};

}  // namespace sonare::midi::synth
