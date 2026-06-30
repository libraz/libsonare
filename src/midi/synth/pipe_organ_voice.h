#pragma once

/// @file pipe_organ_voice.h
/// @brief Flue (labial) organ-pipe core for the NativeSynth voice — the
///        data-free church-organ sketch (the air-driven resonant air column;
///        Fletcher & Rossing, Fabre & Hirschberg). The reed (lingual) pipe
///        family and multi-rank registration are layered on in later phases.
///
/// A flue pipe is a turbulent air jet across the mouth exciting a resonant air
/// column. The data-free model here is a SUSTAINED digital waveguide: the
/// Karplus-Strong feedback loop (fractional-delay line + one-pole loss filter)
/// reused from the plucked string, but kept alive by a continuous jet drive
/// instead of decaying after a single excitation. What separates "organ pipe"
/// from "string" and carries the realism:
///   1. OPEN vs STOPPED PIPE: an open pipe (principal/flute) is a half-wave
///      resonator with the FULL harmonic series; a stopped pipe (gedackt/
///      bourdon) is closed at one end — a quarter-wave resonator that sounds an
///      octave lower for the same length and radiates ODD HARMONICS ONLY. This
///      is the loop topology, not a filter: the open pipe is a positive-feedback
///      comb of one full period; the stopped pipe is a NEGATIVE-feedback comb of
///      half the length (period 2M with M = N/2), whose impulse response flips
///      sign every M samples and so resonates at f0, 3f0, 5f0 … The odd-only
///      spectrum falls out of the physics, not a hand-tuned EQ.
///   2. PROMPT SPEECH: the delay line is pre-filled with a seeded noise burst at
///      note-on (the Karplus-Strong trick), so the pipe speaks at full
///      amplitude immediately rather than swelling in over the resonator's ring
///      time — the way a pipe's jet locks quickly onto the air column.
///   3. SUSTAINING JET DRIVE: a low-level seeded breath turbulence injected
///      every sample, scaled by the loop loss so it replaces the energy the loop
///      bleeds without changing the steady level. This is what makes the tone
///      hold (an organ note does not decay) and gives the airy texture that a
///      drawbar/additive organ lacks.
///   4. CHIFF: the brief, brighter onset transient before the pitch settles
///      (the pipe "speaking"), a short decaying noise burst — the signature
///      that reads as a real pipe rather than a sine drone.
///
/// The delay buffer is NOT owned by the core: the host instrument allocates one
/// slab per voice slot in prepare() (the only allocation site) and attach()es a
/// span before start(). Self-oscillation via a nonlinear jet (true overblowing,
/// register transitions) is intentionally out of scope for this core; the loop
/// is unconditionally stable (feedback magnitude < 1).
///
/// RT contract: attach()/start()/render() are allocation-free (start zeroes /
/// fills the attached span). Determinism: the breath and chiff noise are the
/// counter-based (voice_index, note, age) stream, so identical event streams
/// render bit-identically.

#include <cstddef>
#include <cstdint>

#include "midi/synth/voice_random.h"

namespace sonare::midi::synth {

/// Lowest fundamental the pipe delay line is sized for; covers the 16' octave
/// (CCC ~16 Hz). Notes below clamp to the buffer (their pitch lands sharp
/// instead of overflowing).
inline constexpr float kPipeMinFundamentalHz = 16.0f;

/// Returns the per-voice delay-buffer capacity (in samples) the host must
/// allocate for @p sample_rate. Sized for a full open-pipe period at the
/// lowest fundamental (the stopped pipe uses half this).
inline int pipe_organ_buffer_capacity(double sample_rate) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  return static_cast<int>(sr / kPipeMinFundamentalHz) + 8;
}

/// Flue-pipe section of a NativeSynthPatch (used when mode == kPipeOrgan).
struct PipeOrganPatchParams {
  /// Stopped pipe (gedackt/bourdon): one end closed, so the column is a
  /// quarter-wave resonator radiating odd harmonics only and the pipe is
  /// physically half the length for the same pitch. false = open pipe
  /// (principal/flute) with the full harmonic series.
  bool stopped = false;
  /// Loop-lowpass openness in [0,1]: how slowly the upper harmonics decay
  /// relative to the fundamental (1 = bright/principal, 0 = dull/stopped flute).
  float brightness = 0.5f;
  /// Resonator ring t60 at A4 in seconds (the undriven decay). Higher = purer,
  /// more sharply pitched tone; the note is held by the jet drive regardless,
  /// and note-off shortens the decay to release_damp_s.
  float tone_decay_s = 4.0f;
  /// Steady jet-turbulence drive in [0,1]: the breath that sustains the tone
  /// and voices the pipe's airiness (0 = the loop decays like a plucked note).
  float breath = 0.35f;
  /// Onset speech transient (chiff) amount in [0,1] — the brief bright noise as
  /// the pipe starts to speak.
  float chiff = 0.5f;
  /// Chiff decay time constant (ms).
  float chiff_ms = 18.0f;
  /// Damped t60 in seconds applied at note-off (the wind stops, the pipe stops
  /// speaking).
  float release_damp_s = 0.08f;
};

/// Per-voice flue-pipe state, embedded in NativeSynthVoice. The voice's
/// amplitude envelope / filter / mod matrix wrap around this core; render()
/// returns the raw pipe sample.
class PipeOrganVoiceCore {
 public:
  /// CONTROL-thread wiring (or audio-thread pointer assignment before
  /// start()): hands the core its delay span. The slab outlives the voice.
  void attach(float* buffer, int capacity) noexcept {
    buffer_ = buffer;
    capacity_ = capacity;
  }

  /// Configures the pipe for @p note / @p velocity and pre-fills the loop with
  /// the seeded onset burst. Zeroes the used part of the attached span first.
  void start(const PipeOrganPatchParams& params, double sample_rate, uint8_t note, uint8_t velocity,
             uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (bend / vibrato / drift / tremulant), 1 = on pitch.
  float render(float pitch_ratio) noexcept;
  /// Note-off: cut the jet drive and damp the loop towards release_damp_s (the
  /// pipe keeps sounding through the host's release envelope, dying away).
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

 private:
  float* buffer_ = nullptr;
  int capacity_ = 0;
  /// Circular span actually used for this note (covers bend-down headroom).
  int size_ = 0;
  size_t write_index_ = 0;

  /// Ideal loop period (samples) at pitch_ratio == 1: the full period for an
  /// open pipe, half the period for a stopped pipe (negative-feedback comb).
  float base_period_ = 0.0f;
  /// Samples of loop delay NOT in the delay line (one-sample feedback path +
  /// the loop filter's phase delay at the fundamental).
  float loop_comp_ = 1.0f;
  /// Feedback sign: +1 for the open pipe (full harmonics), -1 for the stopped
  /// pipe (a negative comb resonating on odd harmonics only).
  float loop_sign_ = 1.0f;
  /// One-pole loop lowpass y += alpha * (x - y) and its state.
  float loop_alpha_ = 1.0f;
  float lp_state_ = 0.0f;
  /// In-loop DC blocker: the open pipe's positive comb has a DC pressure mode
  /// (radiation cannot sustain it physically); removing DC from the circulating
  /// signal stops that mode from charging up. Harmless to the stopped comb,
  /// which has no DC resonance.
  float dc_x1_ = 0.0f;
  float dc_y1_ = 0.0f;
  float dc_r_ = 0.0f;
  /// Per-loop amplitude factor for the current t60 target.
  float loop_gain_ = 0.0f;
  /// Per-loop gain for the note-off damped t60 (precomputed at start).
  float release_gain_ = 0.0f;

  // Sustaining jet drive (steady seeded breath turbulence, scaled by the loop
  // loss so the steady level is independent of the ring time).
  VoiceRandomSequence noise_;
  uint64_t drive_index_ = 0;
  float breath_level_ = 0.0f;
  // The open pipe's positive-feedback comb resonates at DC as well as the
  // harmonics; a one-pole high-pass on the breath keeps the broadband jet from
  // pumping that sub-audio mode into a slow wander.
  float breath_hp_state_ = 0.0f;
  float breath_hp_alpha_ = 0.0f;
  // Chiff: a brighter onset burst added on top, decaying through a one-pole.
  float chiff_level_ = 0.0f;
  float chiff_coeff_ = 0.0f;
};

}  // namespace sonare::midi::synth
