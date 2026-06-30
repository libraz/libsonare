#pragma once

/// @file pipe_organ_voice.h
/// @brief Flue (labial) organ-pipe core for the NativeSynth voice — the
///        data-free church-organ model (the air-driven resonant air column;
///        Fletcher & Rossing, Fabre & Hirschberg). One note-on sounds a whole
///        REGISTRATION: the selected ranks (footages) speak together, the way a
///        drawn stop couples 8'/4'/2' principals or a mixture of upperwork.
///        The reed (lingual) pipe family is layered on in a later phase.
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
///   5. REGISTRATION: several ranks at different footages (16'/8'/4'/2-2/3'/2'/…)
///      sound on one key, each an independent waveguide pipe. A principal chorus
///      (8'+4'+2') and a mixture (several high ranks in one stop) fall out of
///      summing decorrelated pipes, the way a real organ builds its plenum.
///
/// The delay buffers are NOT owned by the core: the host instrument allocates
/// one slab per voice slot in prepare() (the only allocation site) sized for
/// kMaxPipeRanks pipes and attach()es it before start(). Self-oscillation via a
/// nonlinear jet (true overblowing, register transitions) is intentionally out
/// of scope for this core; each loop is unconditionally stable (feedback
/// magnitude < 1). The shared wind supply (tremulant / wind sag) is a separate
/// host-owned object (OrganWindSupply) folded into the per-sample pitch/level.
///
/// RT contract: attach()/start()/render() are allocation-free (start zeroes /
/// fills the attached span). Determinism: the breath and chiff noise are the
/// counter-based (voice_index, note, age) stream with a per-rank offset, so
/// identical event streams render bit-identically.

#include <array>
#include <cstddef>
#include <cstdint>

#include "midi/synth/voice_random.h"

namespace sonare::midi::synth {

/// Maximum ranks (pipes) one key sounds at once — covers a principal chorus
/// plus a several-rank mixture.
inline constexpr int kMaxPipeRanks = 8;

/// Lowest fundamental the pipe delay line is sized for; covers the 16' octave
/// (CCC ~16 Hz). Notes below clamp to the buffer (their pitch lands sharp
/// instead of overflowing).
inline constexpr float kPipeMinFundamentalHz = 16.0f;

/// Returns the per-rank delay-buffer capacity (in samples) for @p sample_rate.
/// Sized for a full open-pipe period at the lowest fundamental (the stopped
/// pipe uses half this; a 16' rank sounds an octave down and clamps below CCC).
inline int pipe_organ_buffer_capacity(double sample_rate) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  return static_cast<int>(sr / kPipeMinFundamentalHz) + 8;
}

/// Whole-voice slab capacity (samples) the host must allocate: one per-rank
/// span for every rank slot.
inline int pipe_organ_slab_capacity(double sample_rate) noexcept {
  return kMaxPipeRanks * pipe_organ_buffer_capacity(sample_rate);
}

/// One rank in a registration: an independent flue pipe at a footage (pitch)
/// multiple of the played note. 16'=0.5, 8'=1, 5-1/3'=1.5, 4'=2, 2-2/3'=3,
/// 2'=4, 1-3/5'=5, 1-1/3'=6, 1'=8 (the drawbar/Hammond footage ratios).
struct PipeOrganRank {
  /// Pitch multiplier (footage): the rank sounds note_f0 * footage_mult.
  float footage_mult = 1.0f;
  /// Stopped (gedackt) pipe: odd harmonics only, physically half length.
  bool stopped = false;
  /// Loop-lowpass openness in [0,1] (this rank's voicing brightness).
  float brightness = 0.5f;
  /// Rank mix level in [0,1] (balance within the stop).
  float level = 1.0f;
  /// Reed (lingual) character in [0,1]: 0 = a flue pipe (smooth labial tone);
  /// >0 folds a saturating reed valve into the loop so the pipe buzzes with a
  /// harmonic-rich, brassy spectrum (trumpet / oboe / trompette stops). The
  /// nonlinearity self-limits, so the loop stays bounded.
  float reed = 0.0f;
};

/// Flue-pipe section of a NativeSynthPatch (used when mode == kPipeOrgan).
struct PipeOrganPatchParams {
  /// Stopped pipe (gedackt/bourdon): one end closed, so the column is a
  /// quarter-wave resonator radiating odd harmonics only and the pipe is
  /// physically half the length for the same pitch. false = open pipe
  /// (principal/flute) with the full harmonic series. Used only for the single
  /// implicit rank when rank_count == 0.
  bool stopped = false;
  /// Loop-lowpass openness in [0,1]: how slowly the upper harmonics decay
  /// relative to the fundamental (1 = bright/principal, 0 = dull/stopped flute).
  /// The single-rank voicing when rank_count == 0.
  float brightness = 0.5f;
  /// Resonator ring t60 at A4 in seconds (the undriven decay), shared by every
  /// rank. Higher = purer, more sharply pitched tone; the note is held by the
  /// jet drive regardless, and note-off shortens the decay to release_damp_s.
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
  /// Reed (lingual) character in [0,1] for the single implicit rank (used only
  /// when rank_count == 0). See PipeOrganRank::reed.
  float reed = 0.0f;

  /// Registration: number of ranks that sound together (0 = a single implicit
  /// 8' rank built from {stopped, brightness}; >0 uses ranks[0..rank_count)).
  int rank_count = 0;
  /// The drawn ranks (footage / pipe type / voicing / balance per pipe).
  std::array<PipeOrganRank, kMaxPipeRanks> ranks{};

  // --- shared wind supply (read by the host into OrganWindSupply) ---
  /// Tremulant rate in Hz (0 = tremulant off). A slow undulation of the wind
  /// pressure heard as a combined pitch + amplitude wobble.
  float tremulant_rate_hz = 0.0f;
  /// Tremulant depth in [0,1] (pressure-undulation amount).
  float tremulant_depth = 0.0f;
  /// Wind sag in [0,1]: how far the shared wind pressure drops as more pipes
  /// draw on it, so a full chord sinks slightly in pitch and level then the
  /// regulator recovers — the breathing of a real wind chest.
  float wind_sag = 0.0f;
  /// Swell-box depth in [0,1] (0 = no swell box). A division behind a swell
  /// shutter: as the expression pedal (CC11) closes, a bus-level shutter darkens
  /// the organ (a lowpass), the way the closing louvres muffle the pipes. The
  /// level side of the swell is the expression's existing gain; this is the
  /// timbral shutter on top. Read by the host into a bus filter.
  float swell = 0.0f;
};

/// Per-voice flue-pipe state, embedded in NativeSynthVoice. The voice's
/// amplitude envelope / filter / mod matrix wrap around this core; render()
/// returns the summed raw pipe sample (all drawn ranks).
class PipeOrganVoiceCore {
 public:
  /// CONTROL-thread wiring (or audio-thread pointer assignment before
  /// start()): hands the core its delay slab (kMaxPipeRanks spans of
  /// @p per_rank_capacity). The slab outlives the voice.
  void attach(float* slab, int per_rank_capacity) noexcept {
    slab_ = slab;
    rank_capacity_ = per_rank_capacity;
  }

  /// Configures the registration for @p note / @p velocity and pre-fills each
  /// rank's loop with its seeded onset burst. Zeroes the used spans first.
  void start(const PipeOrganPatchParams& params, double sample_rate, uint8_t note, uint8_t velocity,
             uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (bend / vibrato / drift / tremulant / wind sag), 1 = on pitch.
  float render(float pitch_ratio) noexcept;
  /// Note-off: cut the jet drive and damp every loop towards release_damp_s
  /// (the pipes keep sounding through the host's release envelope, dying away).
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

 private:
  /// One flue pipe (one rank). All per-pipe waveguide state lives here so the
  /// core can sum kMaxPipeRanks of them.
  struct Rank {
    float* buffer = nullptr;
    /// Circular span actually used for this note (covers bend-down headroom).
    int size = 0;
    size_t write_index = 0;
    /// Ideal loop period (samples) at pitch_ratio == 1: the full period for an
    /// open pipe, half for a stopped pipe (negative-feedback comb), and divided
    /// by the rank footage.
    float base_period = 0.0f;
    /// Loop delay NOT in the line (feedback path + loop-filter + DC-blocker
    /// phase delay at the fundamental).
    float comp = 1.0f;
    /// Feedback sign: +1 open (full harmonics), -1 stopped (odd harmonics).
    float sign = 1.0f;
    /// One-pole loop lowpass y += alpha*(x - y) and its state.
    float alpha = 1.0f;
    float lp_state = 0.0f;
    /// In-loop DC blocker (open comb's DC pressure mode has no radiation).
    float dc_x1 = 0.0f;
    float dc_y1 = 0.0f;
    float dc_r = 0.0f;
    /// Per-loop amplitude factor for the current / release t60.
    float loop_gain = 0.0f;
    float release_gain = 0.0f;
    /// Sustaining jet drive (steady seeded breath, scaled by the loop loss).
    float breath_level = 0.0f;
    float breath_hp_state = 0.0f;
    float breath_hp_alpha = 0.0f;
    /// Chiff onset burst (one-pole decay).
    float chiff_level = 0.0f;
    float chiff_coeff = 0.0f;
    /// Reed valve: blend in [0,1] of the saturating nonlinearity, and the
    /// output trim that holds a buzzing reed at a flue pipe's loudness.
    float reed = 0.0f;
    float tone_scale = 1.0f;
    /// Rank mix gain (level * the chorus normalisation) and noise stream offset.
    float mix = 0.0f;
    uint64_t noise_offset = 0;
  };

  float* slab_ = nullptr;
  int rank_capacity_ = 0;

  std::array<Rank, kMaxPipeRanks> ranks_{};
  int rank_count_ = 0;

  // Determinism: one seeded stream, drawn per rank at a high-bit offset so the
  // ranks never reuse each other's breath/chiff draws.
  VoiceRandomSequence noise_;
  uint64_t drive_index_ = 0;
};

/// Shared organ wind supply: the wind chest the whole instrument draws on, a
/// host-owned object (one per NativeSynth, like the piano's shared soundboard).
/// Two effects ride on the common wind pressure:
///   - TREMULANT: a slow LFO undulation of the pressure, heard as a combined
///     pitch vibrato + amplitude tremolo (the classic organ tremulant — both at
///     once, because pressure sets both the pipe speed and its level).
///   - WIND SAG: as more pipes draw on the chest the pressure drops, so a full
///     chord sinks slightly in pitch and loudness, then the regulator recovers.
///     The "breathing" a fixed electronic organ lacks. The drop follows the
///     instantaneous demand (a count of sounding pipes) through a one-pole
///     follower, so it is order-independent and deterministic.
/// process() returns the per-sample pitch factor (fold into each pipe voice's
/// pitch_ratio) and level factor (scale the pipe voice's output).
///
/// RT contract: prepare() is the only configuration site (owns no heap);
/// process()/reset() are allocation-free and deterministic (no RNG / clock).
class OrganWindSupply {
 public:
  struct State {
    float pitch_ratio = 1.0f;
    float gain = 1.0f;
  };

  /// Tunes the wind chest for @p sample_rate. @p tremulant_rate_hz <= 0 turns
  /// the tremulant off; @p wind_sag is the pressure-drop depth in [0,1].
  void prepare(double sample_rate, float tremulant_rate_hz, float tremulant_depth,
               float wind_sag) noexcept;
  /// Clears the LFO phase and the pressure follower (full pressure).
  void reset() noexcept;
  /// Advances the wind one sample under @p demand sounding pipes; returns the
  /// shared pitch/level modulation. Inactive (no tremulant, no sag) returns a
  /// unity state cheaply.
  State process(int demand) noexcept;
  /// Whether any wind modulation is configured (host can skip the per-sample
  /// demand count and the State plumbing when false).
  bool active() const noexcept { return active_; }

 private:
  bool active_ = false;
  double sr_ = 48000.0;
  // Tremulant LFO (deterministic phase accumulator).
  double trem_phase_ = 0.0;
  double trem_inc_ = 0.0;
  float trem_pitch_cents_ = 0.0f;
  float trem_amp_ = 0.0f;
  // Wind-sag pressure follower: pressure relaxes toward 1 - sag*load.
  float sag_depth_ = 0.0f;
  float pressure_ = 1.0f;
  float follow_coeff_ = 0.0f;
};

}  // namespace sonare::midi::synth
