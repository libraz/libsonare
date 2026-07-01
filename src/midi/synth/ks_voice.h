#pragma once

/// @file ks_voice.h
/// @brief Extended Karplus-Strong plucked-string core for the NativeSynth
///        voice — the guitar / harp / banjo family (synthesis method (3) of
///        the instrument build plan; Karplus & Strong 1983, Jaffe & Smith
///        1983).
///
/// The textbook core is a fractional-delay loop closed through a one-pole
/// loop lowpass. What carries the realism beyond it (the Jaffe-Smith
/// extensions):
///   - FRACTIONAL-DELAY TUNING: the loop length is interpolated with the
///     shared 3rd-order Lagrange kernel and compensated for the loop filter's
///     exact phase delay at the fundamental, so the pitch is accurate to a
///     few cents across the keyboard.
///   - Decay stretching: the string's t60 lengthens going down the keyboard
///     (low strings ring longer), scaled per octave below A4.
///   - Pick-position comb: the excitation burst is combed by a delay of
///     pick_position * period, notching the harmonics with a node at the
///     plucking point.
///   - Dynamic-level lowpass: velocity opens the excitation lowpass
///     (hard pluck = bright attack), the velocity -> brightness cue.
///   - Note-off damping: release re-targets the loop gain to a short damped
///     t60 (the finger/palm mute) instead of cutting the string.
///
/// The delay buffer is NOT owned by the core: the host instrument allocates
/// one slab per voice slot in prepare() (the only allocation site) and
/// attach()es a span before start(). Sympathetic-string coupling (harp/koto)
/// is intentionally out of scope for this core.
///
/// RT contract: attach()/start()/render() are allocation-free (start zeroes
/// the attached span). Determinism: the excitation noise is the counter-based
/// (voice_index, note, age) stream, so identical event streams render
/// bit-identically.

#include <cstddef>
#include <cstdint>

#include "midi/synth/voice_random.h"

namespace sonare::midi::synth {

/// Lowest fundamental the KS delay line is sized for; notes below clamp to
/// the buffer (their pitch lands sharp instead of overflowing).
inline constexpr float kKsMinFundamentalHz = 20.0f;

/// In-loop allpass stages for steel-string dispersion. Fewer than the piano
/// (kPianoDispersionStages) — steel-guitar inharmonicity is much weaker.
inline constexpr int kKsDispersionStages = 2;

/// Returns the per-LINE delay-buffer capacity (in samples): one string
/// polarization span. attach() carves the slab into spans of this size.
inline int ks_buffer_capacity(double sample_rate) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  return static_cast<int>(sr / kKsMinFundamentalHz) + 8;
}

/// Returns the per-voice delay-SLAB capacity (in samples) the host must
/// allocate: two spans (the primary string plus the second-polarization line,
/// always reserved so attach() stays allocation-free whether or not a note
/// engages the polarization gate).
inline int ks_slab_capacity(double sample_rate) noexcept {
  return 2 * ks_buffer_capacity(sample_rate);
}

/// KS section of a NativeSynthPatch (used when mode == kKarplusStrong).
struct KsPatchParams {
  /// Loop-lowpass openness in [0,1]: how slowly the upper harmonics decay
  /// relative to the fundamental (1 = bright/metallic, 0 = dull/nylon).
  float brightness = 0.6f;
  /// String t60 at A4 in seconds (fundamental decay to -60 dB).
  float decay_s = 3.0f;
  /// Decay stretching in [0,1]: t60 scales by 2^(stretch * octaves below A4),
  /// so low strings ring longer (Jaffe-Smith).
  float decay_stretch = 0.5f;
  /// Plucking point as a fraction of the string period in [0, 0.5]; the
  /// excitation comb notches harmonics with a node there (0 = no comb).
  float pick_position = 0.18f;
  /// Excitation lowpass openness at full velocity in [0,1].
  float exc_brightness = 0.85f;
  /// Velocity -> excitation brightness amount in [0,1] (0 = velocity only
  /// scales level; 1 = soft notes fully close the pick lowpass).
  float vel_to_brightness = 0.6f;
  /// Damped t60 in seconds applied at note-off (finger/palm mute).
  float release_damp_s = 0.08f;
  /// Fret-slap intensity in [0,1] (off-by-default; 0 = no fret contact, render
  /// bit-identical to the plain string). A hard-driven bass string slaps
  /// against the frets/fingerboard: displacement past the fret gap is limited
  /// by a nonlinear reflection, adding the buzzy slap/pop attack (the electric
  /// bass slap technique — Rank & Kubin 1997). Higher values narrow the gap
  /// (more contact, more buzz).
  float slap = 0.0f;
  /// Second-polarization coupling in [0,1] (off-by-default; 0 = the second line
  /// is skipped entirely and render is bit-identical to the plain string). A
  /// real string vibrates in two planes (the plucked/vertical one plus a
  /// horizontal one at a slightly different pitch that decays faster). A second
  /// delay line, detuned a few cents and more damped, shares the pluck: it
  /// beats against the primary and adds a two-stage decay — the "thickness" and
  /// slow shimmer of a big low string (Karjalainen, Välimäki & Tolonen 1998).
  float polarization = 0.0f;
  /// Bridge coupling between the two polarizations in [0,1] (off-by-default;
  /// 0 = the planes stay independent and their outputs are merely summed, so
  /// render is bit-identical to plain @ref polarization). With @ref polarization
  /// active a real bridge is a shared, lossy 2x2 admittance: the vertical and
  /// horizontal planes trade energy through it, so each plane's envelope becomes
  /// biexponential (a true double decay) instead of two independent exponentials
  /// merely added at the output (Woodhouse 2004/2021; Bank & Karjalainen 2010).
  /// The coupling strength is scaled at start() so the coupled 2x2 loop stays
  /// inside the unit circle even at the near-degenerate detune (eigenvalue
  /// bound, not a scalar per-loop bound). No effect unless @ref polarization > 0.
  float body_coupling = 0.0f;
  /// Physical-pluck excitation blend in [0,1] (off-by-default; 0 = the textbook
  /// combed-noise burst, render bit-identical). As it rises the burst crossfades
  /// toward a deterministic pluck doublet — a raised-cosine up/down lobe standing
  /// in for the finger's release-velocity pulse (a feedforward reduction of the
  /// Cuzzucoli-Lombardo player-touch model; the full model is two-way, this
  /// shapes and injects the burst once). The doublet is zero-mean, so the
  /// pick-position comb and loop stay well behaved.
  float pluck_style = 0.0f;
  /// Pluck edge in [0,1]: fingertip flesh (0, a wide/round lobe, fewer highs) to
  /// nail or pick (1, a narrow/sharp lobe, more highs — the bright release edge).
  /// Only affects the pluck doublet, so it is inert unless @ref pluck_style > 0.
  float nail = 0.0f;
  /// Enables the instrument-wide sympathetic-string bank (off-by-default; false
  /// leaves the voice render bit-identical). The played note excites a shared
  /// bank tuned to the open strings, which rings behind it — the "sound halo" of
  /// a classical guitar or harp (Karjalainen, Välimäki & Jánosy 1998; Lehtonen
  /// et al. 2007). This is a host-level (per-instrument) effect, not per-voice:
  /// NativeSynth reuses the piano sympathetic bank, held open (plucked strings
  /// have no dampers). Not carried in the per-voice core state.
  bool sympathetic = false;
  /// Magnetic-pickup position in [0, 0.5] (off-by-default; 0 = no pickup, render
  /// bit-identical — the electric-guitar model). A magnetic pickup senses the
  /// string at a fixed point, not the whole motion: it combs the output with a
  /// node at that point (a second, output-side comb distinct from the pluck-
  /// position comb) and its field gradient makes the string-to-voltage transfer
  /// mildly nonlinear, adding even harmonics (Lindroos, Penttinen & Välimäki
  /// 2011). Higher = nearer the neck (rounder comb); low = near the bridge
  /// (brighter). Amp/overdrive is a separate downstream stage, not modeled here.
  float pickup_pos = 0.0f;
  /// Stiff-string dispersion in [0,1] (off-by-default; 0 = a harmonic string,
  /// render bit-identical). Steel strings (acoustic-steel and electric) are
  /// stiffer than nylon, so their partials stretch sharp to f_n =
  /// n*f0*sqrt(1 + B*n^2). This scales a steel-string inharmonicity coefficient
  /// (weaker than a piano) into an in-loop allpass cascade — the same dispersion
  /// method the piano core uses. Leave 0 for nylon (its plain strings are not
  /// audibly inharmonic); the "no dispersion" decision must NOT be inherited by
  /// the steel members, which share this Karplus-Strong core.
  float dispersion = 0.0f;
  /// Tension-modulation amount in [0,1] (off-by-default; 0 = a linear string,
  /// render bit-identical). A hard pluck stretches the string, raising its
  /// tension and so its pitch, which then relaxes back over the first tens of
  /// milliseconds — the slight downward glide at the attack of a hard-plucked
  /// string (a reduced Tolonen/Välimäki nonlinear string). Scaled by velocity;
  /// the pitch rise is clamped explicitly in cents so a hard attack stacked on
  /// top of bend/vibrato cannot glitch the delay line.
  float tension_mod = 0.0f;
};

/// Per-voice plucked-string state, embedded in NativeSynthVoice. The voice's
/// amplitude envelope / filter / mod matrix wrap around this core; render()
/// returns the raw string sample.
class KsVoiceCore {
 public:
  /// CONTROL-thread wiring (or audio-thread pointer assignment before start()):
  /// hands the core its delay slab (two spans of @p per_line_capacity — the
  /// primary string plus the second-polarization line). The slab outlives the
  /// voice.
  void attach(float* slab, int per_line_capacity) noexcept {
    buffer_ = slab;
    capacity_ = per_line_capacity;
    pol_buffer_ = slab != nullptr ? slab + per_line_capacity : nullptr;
  }

  /// Configures the string for @p note / @p velocity and injects the seeded
  /// excitation burst state. Zeroes the used part of the attached span.
  void start(const KsPatchParams& params, double sample_rate, uint8_t note, uint8_t velocity,
             uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (bend / vibrato / drift / glide), 1 = on pitch.
  float render(float pitch_ratio) noexcept;
  /// Note-off: damp the loop towards release_damp_s (the string keeps
  /// sounding through the host's release envelope, muted).
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

 private:
  float* buffer_ = nullptr;
  int capacity_ = 0;
  /// Circular span actually used for this note (covers bend-down headroom).
  int size_ = 0;
  size_t write_index_ = 0;

  /// Ideal loop period (samples) at pitch_ratio == 1.
  float base_period_ = 0.0f;
  /// Samples of loop delay NOT in the delay line: the one-sample feedback
  /// path plus the loop filter's phase delay at the fundamental.
  float loop_comp_ = 1.0f;
  /// One-pole loop lowpass y += alpha * (x - y) and its state.
  float loop_alpha_ = 1.0f;
  float lp_state_ = 0.0f;
  /// In-loop dispersion allpass cascade (steel-string inharmonicity). disp_a_
  /// == 0 -> the cascade is skipped, render bit-identical.
  float disp_a_ = 0.0f;
  float disp_state_[kKsDispersionStages] = {};
  /// Per-loop amplitude factor for the current t60 target.
  float loop_gain_ = 0.0f;
  /// Per-loop gain for the note-off damped t60 (precomputed at start).
  float release_gain_ = 0.0f;
  /// Fret-slap displacement limit (0 = off -> render path bit-identical).
  /// When > 0, loop content past +/- this bound is softly reflected.
  float slap_threshold_ = 0.0f;

  // Second (horizontal) polarization: a detuned string loop sharing the pluck.
  // Off unless params.polarization > 0 (pol_couple_ == 0 -> render skips it, so
  // the primary path is bit-identical). The slab's second span (attach()).
  float* pol_buffer_ = nullptr;
  int pol_size_ = 0;
  size_t pol_write_ = 0;
  float pol_period_ = 0.0f;  // detuned from base_period_
  float pol_loop_comp_ = 1.0f;
  float pol_loop_alpha_ = 1.0f;
  float pol_lp_state_ = 0.0f;
  float pol_loop_gain_ = 0.0f;
  float pol_release_gain_ = 0.0f;
  float pol_couple_ = 0.0f;  // 0 = off; horizontal-plane mix into the output
  float pol_exc_ = 0.0f;     // pluck injection into the 2nd polarization
  // Symmetric bridge admittance between the two planes (0 = off -> the loops
  // stay independent, bit-identical to plain polarization). Scaled at start()
  // so the coupled 2x2 loop's eigenvalues stay inside the unit circle.
  float couple_gain_ = 0.0f;

  // Excitation burst (one period of combed, lowpassed seeded noise; the
  // dynamic-level lowpass is two cascaded one-poles).
  VoiceRandomSequence noise_;
  int exc_total_ = 0;
  int exc_pos_ = 0;
  int pick_delay_ = 0;
  float exc_alpha_ = 1.0f;
  float exc_lp1_ = 0.0f;
  float exc_lp2_ = 0.0f;
  // Physical-pluck doublet. pluck_style_ == 0 -> the burst is the plain seeded
  // noise, bit-identical. pluck_contact_ is the doublet width in samples
  // (narrow = nail/pick/bright, wide = fingertip/round).
  float pluck_style_ = 0.0f;
  int pluck_contact_ = 0;

  // Magnetic pickup (electric). pickup_depth_ == 0 -> no pickup, output
  // bit-identical. pickup_delay_q8_ is the position tap into the loop line (a
  // second output-side comb); pickup_mag_ is the field-gradient nonlinearity
  // amount (even harmonics).
  float pickup_depth_ = 0.0f;
  int pickup_delay_q8_ = 0;
  float pickup_mag_ = 0.0f;

  // Tension modulation. tension_ratio_peak_ == 0 -> no tension, pitch path
  // bit-identical. It holds the attack-peak pitch factor minus one (a small
  // positive, cents-clamped); tension_env_ decays it toward the nominal pitch.
  float tension_ratio_peak_ = 0.0f;
  float tension_env_ = 0.0f;
  float tension_decay_coeff_ = 0.0f;
};

}  // namespace sonare::midi::synth
