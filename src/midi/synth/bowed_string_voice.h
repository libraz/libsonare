#pragma once

/// @file bowed_string_voice.h
/// @brief Bowed-string core for the NativeSynth voice — violin, viola, cello,
///        contrabass. A Smith single-junction waveguide: two delay lines (bow to
///        nut, bow to bridge) spanning one period, coupled at the bow by a
///        memoryless stick-slip friction table, out of which Helmholtz motion
///        falls rather than being hand-drawn (McIntyre, Schumacher & Woodhouse
///        1983).
///
/// Bow position is the delay-line split, not an EQ: near the bridge is sul
/// ponticello, over the fingerboard sul tasto. Bow force sets the table slope,
/// so a harder force is a rougher, brighter slip. Both terminations invert, and
/// the two inversions multiply to positive feedback — the full harmonic series,
/// unlike the stopped organ pipe's odd-only comb.
///
/// The delay buffers are not owned here — the host attaches two spans per voice
/// slot before start(), and the corpus is the shared BodyResonator
/// (BodyType::kViolin), so the core emits the raw signal at the bridge.
/// Unconditionally stable: the bow table is bounded to [0,1] and the bridge loss
/// gain is < 1.
///
/// RT contract: attach()/start()/render() are allocation-free. Determinism: the
/// optional rosin texture comes from the counter-based (voice_index, note, age)
/// stream, so identical events render bit-identically.

#include <cstddef>
#include <cstdint>

#include "midi/synth/voice_random.h"

namespace sonare::midi::synth {

/// Lowest fundamental the bowed-string delay lines are sized for; covers the
/// contrabass low range with pitch-bend headroom (well below CCC ~16 Hz is
/// unnecessary — a bowed string never sounds that low, but the margin keeps a
/// bent-down cello note inside the buffer).
inline constexpr float kBowedMinFundamentalHz = 20.0f;

/// Per-delay-line buffer capacity (samples) for @p sample_rate: a full period at
/// the lowest fundamental (each of the two lines is sized for the whole period
/// so any bow position — which splits the period between them — fits).
inline int bowed_string_buffer_capacity(double sample_rate) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  return static_cast<int>(sr / kBowedMinFundamentalHz) + 8;
}

/// Whole-voice slab capacity (samples) the host must allocate: three delay-line
/// spans — the neck and bridge of the bowed (vertical) polarization, plus the
/// second (horizontal) polarization line used only when it is gated on. The
/// third span is always reserved so attach() stays allocation-free regardless of
/// whether a given note enables the second polarization.
inline int bowed_string_slab_capacity(double sample_rate) noexcept {
  return 3 * bowed_string_buffer_capacity(sample_rate);
}

/// Bowed-string section of a NativeSynthPatch (used when mode == kBowedString).
struct BowedStringPatchParams {
  /// Bow contact point as a fraction of the string length from the bridge
  /// (0..0.5): the bow splits the period into a bridge side (this) and a nut
  /// side (1 - this). Small = near the bridge (bright, sul ponticello); larger =
  /// over the fingerboard (soft, sul tasto). ~0.13 is the natural playing point.
  float bow_position = 0.13f;
  /// Bow force / downward pressure in [0,1]: sets the width of the bow table's
  /// sticking region (the friction-curve slope). Low = a light, whistly bow that
  /// barely captures the string; high = a firm, rich, slightly rougher tone.
  float bow_force = 0.5f;
  /// Bow speed in [0,1]: how fast the bow is drawn, i.e. the dynamic level. It
  /// scales the bow velocity that drives the differential-velocity input.
  float bow_speed = 0.5f;
  /// Note velocity -> bow speed in [0,1]: how much the struck velocity opens the
  /// bow speed (0 = velocity-independent dynamics, 1 = velocity is the dynamic).
  float vel_to_speed = 0.6f;
  /// Bridge reflection-filter openness in [0,1]: how brightly the string
  /// reflects at the bridge (1 = bright/edgy, 0 = dark/muted). The loop lowpass.
  float brightness = 0.5f;
  /// String damping in [0,1]: the bridge loop loss. Low = a purer, more
  /// sustained, sharply pitched string; high = a more damped, quicker-speaking
  /// tone. The bow replenishes the loss either way (the note is sustained).
  float damping = 0.4f;
  /// Bow acceleration on note-on (ms): the string takes a few periods to lock
  /// into Helmholtz motion, so the bow speed ramps in rather than stepping.
  float attack_ms = 60.0f;
  /// Bow deceleration on note-off (ms): the bow lifts and the string rings down.
  float release_ms = 120.0f;
  /// Rosin texture in [0,1]: a subtle deterministic velocity noise on the bow
  /// (the grip of rosined hair), 0 = a perfectly smooth bow. Kept small; the
  /// noise is the seeded per-voice stream so bounces stay bit-identical.
  float rosin = 0.0f;

  // --- off-by-default advanced friction (Phase 4; C-ABI non-exposed, gated) ---
  /// Elasto-plastic bow friction (Dupont 2002; Avanzini, Serafin & Rocchesso
  /// 2003): replaces the memoryless bow table with a single-bristle friction
  /// STATE, so the rosin's stick->slip transition carries HYSTERESIS (the string
  /// re-grips along a different curve than it released along). A memoryless table
  /// is famously "dry"; the bristle memory is what warms it. OFF by default — the
  /// render path is then bit-identical to the static table (like the gated
  /// percussion physics layers), and neither goldens nor parity move.
  bool elasto_plastic = false;
  /// Stribeck velocity scale in [0,1] (only when elasto_plastic): how wide the
  /// sticking hump is before the bristles break away. Higher = a rounder, grippier
  /// stick with a slower, warmer slip; lower = a sharper, edgier release.
  float stribeck = 0.5f;

  /// Sympathetic resonance in [0,1]: the halo of the instrument's OTHER (undamped
  /// open) strings ringing along with the bowed note. A small fixed bank of
  /// open-string resonators driven one-way by the bridge output (no feedback into
  /// the waveguide loop -> unconditionally stable), unity-peak normalized like the
  /// piano's sympathetic bank so it colours rather than rings away. 0 = off ->
  /// render is bit-identical (the bank is skipped entirely).
  float sympathetic = 0.0f;

  /// Second-polarization coupling in [0,1]: a real string vibrates in two
  /// transverse planes at once (the bowed vertical plane and a weakly-coupled
  /// horizontal one at a slightly different pitch). A second delay line, detuned
  /// a few cents and sharing the bow, beats against the primary — the "thickness"
  /// and the restless attack of a real bowed string. Weakly coupled so the added
  /// feedback stays bounded. 0 = off -> the second line is skipped entirely
  /// (bit-identical render).
  float polarization = 0.0f;
};

/// Per-voice bowed-string state, embedded in NativeSynthVoice. The voice's
/// amplitude envelope / filter / mod matrix and the shared BodyResonator wrap
/// around this core; render() returns the raw string signal at the bridge.
class BowedStringVoiceCore {
 public:
  /// CONTROL-thread wiring (or audio-thread pointer assignment before start()):
  /// hands the core its delay slab (two spans of @p per_line_capacity — the neck
  /// and bridge delay lines). The slab outlives the voice.
  void attach(float* slab, int per_line_capacity) noexcept {
    neck_ = slab;
    bridge_ = slab != nullptr ? slab + per_line_capacity : nullptr;
    pol_ = slab != nullptr ? slab + 2 * per_line_capacity : nullptr;
    capacity_ = per_line_capacity;
  }

  /// Configures the string for @p note / @p velocity and zeroes the used spans.
  /// @p seed drives the deterministic rosin texture (unused when rosin == 0).
  void start(const BowedStringPatchParams& params, double sample_rate, uint8_t note,
             uint8_t velocity, uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (bend / vibrato / drift), 1 = on pitch.
  float render(float pitch_ratio) noexcept;
  /// Note-off: lift the bow (ramp the bow speed to zero); the string rings down.
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

  // --- live continuous control (bowed strings are a continuous-control
  // instrument; the host drives these from MIDI CCs while the note sounds).
  // Each sets a smoothing TARGET the render ramps toward (no zipper); call
  // snap_bow_control() to jump to the targets without a glide. ---

  /// Bow speed / dynamic level as a scale in [0, ~2] of the note-on bow speed
  /// (1 = the struck level). The expression pedal's crescendo/swell.
  void set_bow_speed_scale(float scale) noexcept {
    const float s = scale < 0.0f ? 0.0f : (scale > 2.0f ? 2.0f : scale);
    bow_speed_target_ = base_bow_velocity_ * s;
  }
  /// Bow force / downward pressure in [0,1] (friction-curve slope): light,
  /// whistly bow through firm, rich, rougher tone.
  void set_bow_force(float force01) noexcept {
    const float f = force01 < 0.0f ? 0.0f : (force01 > 1.0f ? 1.0f : force01);
    slope_target_ = kBowSlopeMax_ - kBowSlopeSpan_ * f;
  }
  /// Bow contact point in [0,1]: 0 = at the bridge (bright, sul ponticello),
  /// 1 = over the fingerboard (soft, sul tasto). Maps to the delay-line split.
  void set_bow_position(float pos01) noexcept {
    const float p = pos01 < 0.0f ? 0.0f : (pos01 > 1.0f ? 1.0f : pos01);
    // Map across the natural playing range (bridge .. fingerboard); beta at the
    // string's centre is an unusual, non-monotone extreme, so the CC stops short.
    beta_target_ = 0.02f + 0.23f * p;
  }
  /// Jump the smoothed controls to their targets (seed a fresh note at the
  /// host's current CC positions without an audible glide).
  void snap_bow_control() noexcept {
    max_bow_velocity_ = bow_speed_target_;
    bow_slope_ = slope_target_;
    beta_ = beta_target_;
  }

 private:
  // Bow-table friction-curve slope from bow force (Smith / STK: slope in [1,5],
  // harder force -> lower slope -> wider sticking region).
  static constexpr float kBowSlopeMax_ = 5.0f;
  static constexpr float kBowSlopeSpan_ = 4.0f;

  // Elasto-plastic bow injection for the current relative velocity @p dv: evolves
  // the single bristle state through the Stribeck adhesion map and returns the
  // injected velocity wave, bounded to |dv| so the junction stays passive. Only
  // called on the gated path (elasto_plastic_).
  float elasto_plastic_injection(float dv) noexcept;

  // One sympathetic-bank sample: drives the open-string resonator bank with the
  // bridge output @p x and returns their summed ring. Only called on the gated
  // path (sympathetic_mix_ > 0).
  float sympathetic_process(float x) noexcept;

  // Sympathetic open-string resonance: a fixed bank of unity-peak biquads, driven
  // one-way by the bridge output (no loop feedback -> stable). Off unless
  // params.sympathetic > 0, in which case sympathetic_mix_ is the return level.
  static constexpr int kSympatheticModes_ = 8;
  struct SympatheticMode {
    float a1 = 0.0f;
    float a2 = 0.0f;
    float gain = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
  };
  SympatheticMode sympathetic_[kSympatheticModes_];
  float sympathetic_mix_ = 0.0f;

  // Delay slab (host-owned): neck = bow->nut, bridge = bow->bridge.
  float* neck_ = nullptr;
  float* bridge_ = nullptr;
  int capacity_ = 0;
  int neck_size_ = 0;
  int bridge_size_ = 0;
  size_t neck_write_ = 0;
  size_t bridge_write_ = 0;
  // Last delay-line outputs (feed the scattering junction next sample).
  float neck_out_ = 0.0f;
  float bridge_out_ = 0.0f;

  // Tuning: full fundamental period (samples) and the bow split; base_period is
  // divided between the two lines after subtracting comp (filter + structural
  // feedback-register phase delay) so the sounding pitch matches the note.
  float base_period_ = 0.0f;
  float beta_ = 0.13f;
  float comp_ = 2.0f;

  // Bridge reflection: one-pole loop lowpass y += alpha*(x - y), a loss gain,
  // and a sign inversion (folded in render). The nut is an ideal -1.
  float lp_alpha_ = 1.0f;
  float lp_state_ = 0.0f;
  float loss_gain_ = 0.95f;

  // Bow table (memoryless friction curve): coeff = 1/(|slope*dv + offset|+0.75)^4
  // clamped to 1. slope is set by bow force; offset stays 0 (symmetric bow).
  float bow_slope_ = 3.0f;
  float bow_offset_ = 0.0f;

  // Bow velocity contour: a one-pole ramp of the bow speed in [0,1].
  float max_bow_velocity_ = 0.1f;
  float bow_level_ = 0.0f;
  float attack_coeff_ = 0.0f;
  float release_coeff_ = 0.0f;
  bool releasing_ = false;

  // Live-control smoothing: the render ramps the current bow speed / slope /
  // position toward these targets, so a moving CC never zippers. base_bow_-
  // velocity_ is the note-on (velocity-blended) speed the expression scales.
  float base_bow_velocity_ = 0.1f;
  float bow_speed_target_ = 0.1f;
  float slope_target_ = 3.0f;
  float beta_target_ = 0.13f;
  float ctrl_coeff_ = 1.0f;

  // Output trim bringing the raw waveguide velocity-wave (a small fraction of
  // the bow velocity) up to a musical voice level, calibrated so a forte note
  // peaks near the other engines' output range.
  float output_scale_ = 6.0f;

  // Rosin texture (deterministic bow-velocity noise; 0 = smooth bow).
  float rosin_level_ = 0.0f;
  VoiceRandomSequence noise_;
  uint64_t drive_index_ = 0;

  // Elasto-plastic friction (off unless params.elasto_plastic): a single bristle
  // deflection z evolved through the Stribeck adhesion map. When off, render()
  // takes the memoryless static-table branch untouched (bit-identical). All
  // knobs are dimensionless in the model's velocity-wave units, calibrated so the
  // loop stays bounded and still locks into Helmholtz motion.
  bool elasto_plastic_ = false;
  float bristle_z_ = 0.0f;      // bristle deflection state (the friction memory)
  float ep_stribeck_v_ = 0.1f;  // Stribeck velocity (stick-hump half-width)
  float ep_load_rate_ = 0.0f;   // per-sample bristle load rate (dt folded in)
  float ep_z_ba_ = 0.0f;        // breakaway deflection (stick below this)
  float ep_z_max_ = 0.0f;       // bristle clamp (divergence guard rail)

  // Second (horizontal) polarization: a detuned string loop sharing the bow.
  // Off unless params.polarization > 0 (pol_couple_ == 0 -> render skips it,
  // bit-identical). Its own lossy loop with a weak cross-coupling to the primary.
  float* pol_ = nullptr;  // 2nd-polarization delay line (host slab, 3rd span)
  int pol_size_ = 0;
  size_t pol_write_ = 0;
  float pol_out_ = 0.0f;
  float pol_period_ = 0.0f;  // detuned from base_period_
  float pol_lp_state_ = 0.0f;
  float pol_lp_alpha_ = 1.0f;
  float pol_loss_ = 0.95f;
  float pol_couple_ = 0.0f;  // 0 = off; horizontal -> vertical feed
  float pol_drive_ = 0.0f;   // bow injection into the 2nd polarization
};

}  // namespace sonare::midi::synth
