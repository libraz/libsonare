#pragma once

/// @file pipe_organ_voice.h
/// @brief Flue (labial) organ-pipe core for the NativeSynth voice — the
///        data-free church-organ model (the air-driven resonant air column;
///        Fletcher & Rossing, Fabre & Hirschberg). One note-on sounds a whole
///        REGISTRATION: the selected ranks (footages) speak together, the way a
///        drawn stop couples 8'/4'/2' principals or a mixture of upperwork.
///        The reed (lingual) pipe family is layered on the same jet loop.
///
/// A flue pipe is a turbulent air jet blown across the mouth's labium, and the
/// pipe's standing wave phase-locks that jet — the jet is a nonlinear exciter,
/// the air column a linear resonator. This core SELF-OSCILLATES through that
/// jet (the same physics as the sibling flute core, flute_voice.h): once the
/// wind is on, the jet drives a periodic limit cycle that holds the tone rock
/// steady and endlessly (an organ note does not decay or waver while keyed) —
/// the "won't stop while the key is down" solidity a decaying, noise-driven
/// waveguide cannot voice. Each rank is an independent jet pipe: a bore delay
/// line (the travelling-wave air column) beside a short jet delay line (the
/// jet's convection time), driven by a cubic jet table (Fabre-Hirschberg lumped
/// model / STK JetTable) whose inverting small-signal slope, with the bore
/// reflection, forms the oscillator and whose clamp bounds the limit cycle.
/// What separates "organ pipe" from "string" and carries the realism:
///   1. OPEN vs STOPPED PIPE: an open pipe (principal/flute) is voiced bright
///      with the full harmonic series and the even-harmonic octave pump; a
///      stopped pipe (gedackt/bourdon) is voiced HOLLOW off the same jet loop —
///      the octave pump is nearly muted and the reflection darkened, so the tone
///      is fundamental-dominant with little upperwork (the covered gedackt
///      colour). Both use the positive-feedback open topology: it locks its
///      fundamental and tunes cleanly across the whole 16'-down compass, where a
///      literal negative-feedback half-length stopped comb would not hold pitch.
///   2. SELF-OSCILLATING JET: the jet convection ratio (jet delay / bore delay,
///      ~0.5) selects the register the jet drives, so the pipe locks its pitch
///      and speaks a solid periodic tone rather than a resonator merely coloured
///      by breath noise.
///   3. PROMPT SPEECH: the bore is pre-filled with a low-level seeded burst at
///      note-on so the jet has an f0 seed to lock onto immediately rather than
///      swelling in over the resonator's ring time.
///   4. CHIFF: the brief, brighter onset transient before the pitch settles
///      (the pipe "speaking"), a short decaying noise burst.
///   5. REGISTRATION: several ranks at different footages (16'/8'/4'/2-2/3'/2'/…)
///      sound on one key, each an independent jet pipe. A principal chorus
///      (8'+4'+2') and a mixture (several high ranks in one stop) fall out of
///      summing decorrelated pipes, the way a real organ builds its plenum.
///
/// The delay buffers are NOT owned by the core: the host instrument allocates
/// one slab per voice slot in prepare() (the only allocation site) sized for
/// kMaxPipeRanks pipes — a bore span AND a jet span per rank — and attach()es it
/// before start(). The jet cubic self-limits (clamped to [-1,1]) and the
/// reflection magnitudes are < 1, so every loop is BIBO-stable. The shared wind
/// supply (tremulant / wind sag) is a separate host-owned object
/// (OrganWindSupply) folded into the per-sample pitch/level.
///
/// RT contract: attach()/start()/render() are allocation-free (start zeroes /
/// fills the attached spans). Determinism: the breath and chiff noise are the
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

/// Returns the per-span delay-buffer capacity (in samples) for @p sample_rate.
/// Sized for a full open-pipe period at the lowest fundamental plus bend-down
/// headroom (the stopped pipe's bore is half this; the jet span reuses the same
/// capacity — its convection delay is always shorter than the bore).
inline int pipe_organ_buffer_capacity(double sample_rate) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  return static_cast<int>(sr / kPipeMinFundamentalHz) + 8;
}

/// Whole-voice slab capacity (samples) the host must allocate: a bore span AND a
/// jet span for every rank slot.
inline int pipe_organ_slab_capacity(double sample_rate) noexcept {
  return 2 * kMaxPipeRanks * pipe_organ_buffer_capacity(sample_rate);
}

/// One rank in a registration: an independent flue pipe at a footage (pitch)
/// multiple of the played note. 16'=0.5, 8'=1, 5-1/3'=1.5, 4'=2, 2-2/3'=3,
/// 2'=4, 1-3/5'=5, 1-1/3'=6, 1'=8 (the drawbar/Hammond footage ratios).
struct PipeOrganRank {
  /// Pitch multiplier (footage): the rank sounds note_f0 * footage_mult.
  float footage_mult = 1.0f;
  /// Stopped (gedackt/bourdon) pipe: voiced hollow — fundamental-dominant with
  /// the octave pump muted and the reflection darkened (the covered colour).
  bool stopped = false;
  /// Reflection-filter openness in [0,1] (this rank's voicing brightness): how
  /// brightly the open end reflects the upper partials.
  float brightness = 0.5f;
  /// Rank mix level in [0,1] (balance within the stop).
  float level = 1.0f;
  /// Reed (lingual) character in [0,1]: 0 = a flue pipe (smooth labial tone);
  /// >0 drives the jet harder and more asymmetrically so the pipe buzzes with a
  /// harmonic-rich, brassy spectrum (trumpet / oboe / trompette stops). The jet
  /// cubic self-limits, so the loop stays bounded.
  float reed = 0.0f;
  /// Mouth/radiation correction in [0,1]: how brightly the pipe radiates into
  /// the room. A pipe's open mouth and end are inefficient bass radiators (the
  /// radiation impedance rises with frequency), so the sound in the room is
  /// brighter than the pressure inside the column. >0 adds a gentle post-loop
  /// high-shelf that lifts the upper partials, voicing the pipe's "speak" into
  /// the air. It is outside the feedback loop, so it never affects pitch or
  /// stability.
  float radiation = 0.0f;
};

/// Flue-pipe section of a NativeSynthPatch (used when mode == kPipeOrgan).
struct PipeOrganPatchParams {
  /// Stopped pipe (gedackt/bourdon): voiced hollow off the open jet loop —
  /// fundamental-dominant, octave pump muted, reflection darkened (the covered
  /// colour). false = open pipe (principal/flute) with the full harmonic series
  /// and octave pump. Used only for the single implicit rank when
  /// rank_count == 0.
  bool stopped = false;
  /// Reflection-filter openness in [0,1]: how brightly the open end reflects the
  /// upper partials (1 = bright/principal, 0 = dull/stopped flute). The
  /// single-rank voicing when rank_count == 0.
  float brightness = 0.5f;
  /// Bore purity: the driven jet's resonance sharpness (maps to the bore loss /
  /// end reflection). Higher = a purer, more sharply pitched, more sustained
  /// tone; the pipe is held by the jet regardless. Kept as tone_decay_s (seconds
  /// at A4) for wire compatibility — a longer nominal ring is a purer bore.
  float tone_decay_s = 4.0f;
  /// Mouth pressure / jet drive in [0,1]: how hard the wind blows the pipe (the
  /// dynamic level and jet drive). The jet self-oscillates across the exposed
  /// band, so this colours the tone; loudness rides the voice's amp VCA.
  float breath = 0.35f;
  /// Onset speech transient (chiff) amount in [0,1] — the brief bright noise as
  /// the pipe starts to speak.
  float chiff = 0.5f;
  /// Chiff decay time constant (ms).
  float chiff_ms = 18.0f;
  /// Breath fall at note-off (seconds): the wind stops and the bore rings down.
  float release_damp_s = 0.08f;
  /// Reed (lingual) character in [0,1] for the single implicit rank (used only
  /// when rank_count == 0). See PipeOrganRank::reed.
  float reed = 0.0f;
  /// Mouth/radiation correction in [0,1] for the single implicit rank (used
  /// only when rank_count == 0). See PipeOrganRank::radiation.
  float radiation = 0.0f;

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
  /// start()): hands the core its delay slab (kMaxPipeRanks pairs of
  /// @p per_span_capacity spans — a bore span then a jet span per rank). The
  /// slab outlives the voice.
  void attach(float* slab, int per_span_capacity) noexcept {
    slab_ = slab;
    span_capacity_ = per_span_capacity;
  }

  /// Configures the registration for @p note / @p velocity and pre-fills each
  /// rank's bore with its seeded onset seed. Zeroes the used spans first.
  void start(const PipeOrganPatchParams& params, double sample_rate, uint8_t note, uint8_t velocity,
             uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (bend / vibrato / drift / tremulant / wind sag), 1 = on pitch.
  float render(float pitch_ratio) noexcept;
  /// Note-off: stop blowing (ramp the breath to zero); the bores ring down.
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

 private:
  /// One flue pipe (one rank): a self-oscillating jet + bore waveguide. All
  /// per-pipe state lives here so the core can sum kMaxPipeRanks of them.
  struct Rank {
    // Bore + jet delay lines (host-owned spans of span_capacity_).
    float* bore = nullptr;
    float* jet = nullptr;
    int bore_size = 0;
    int jet_size = 0;
    size_t bore_write = 0;
    size_t jet_write = 0;
    /// Last bore delay-line output (returns to the mouth next sample).
    float bore_out = 0.0f;
    /// Ideal loop period (samples) at pitch_ratio == 1: one full period at the
    /// rank's sounding fundamental (footage-scaled), times the pitch correction.
    float bore_period = 0.0f;
    /// Loop delay NOT in the line (feedback register + loop-filter phase delay).
    float comp = 1.0f;
    /// Jet convection delay as a fraction of the bore LINE delay.
    float jet_ratio = 0.5f;
    /// Open-end reflection: one-pole loss lowpass y += alpha*(x - y), its state,
    /// the loss gain, and the bore feedback sign (always +1 — the open topology).
    float lp_alpha = 1.0f;
    float lp_state = 0.0f;
    float loss_gain = 1.0f;
    /// Bore loss applied at note-off so the pipe stops speaking promptly (a real
    /// pipe's wind is cut, not left ringing) rather than ringing at the sustain's
    /// near-lossless rate.
    float release_loss = 0.0f;
    float sign = 1.0f;
    /// Jet / end reflection coefficients (the two feedback taps of the jet loop).
    float jet_reflection = 0.5f;
    float end_reflection = 0.5f;
    /// Jet-table asymmetry (reed pipes drive it harder / more asymmetric).
    float jet_asym = 0.5f;
    float jet_drive = 1.0f;
    /// In-loop DC blocker on the jet output.
    float dc_x1 = 0.0f;
    float dc_y1 = 0.0f;
    float dc_r = 0.0f;
    /// Even-harmonic pump (open pipe only; 0 for stopped).
    float even_gain = 0.0f;
    float even_state = 0.0f;
    float even_hp_alpha = 0.0f;
    /// Steady mouth pressure and its note-on/off contour.
    float breath = 0.0f;
    /// Chiff onset burst (one-pole decay).
    float chiff_level = 0.0f;
    float chiff_coeff = 0.0f;
    /// Mouth/radiation high-shelf (post-loop, outside the feedback path).
    float rad_gain = 0.0f;
    float rad_alpha = 0.0f;
    float rad_state = 0.0f;
    /// Rank mix gain (level * chorus norm * output trim) and noise offset.
    float mix = 0.0f;
    float output_scale = 1.0f;
    uint64_t noise_offset = 0;
  };

  float* slab_ = nullptr;
  int span_capacity_ = 0;

  std::array<Rank, kMaxPipeRanks> ranks_{};
  int rank_count_ = 0;

  // Breath contour shared by every rank (the wind gate): ramps to 1 on note-on,
  // to 0 on release; each rank's steady breath is scaled by this.
  float breath_level_ = 0.0f;
  float attack_coeff_ = 0.0f;
  float release_coeff_ = 0.0f;
  bool releasing_ = false;

  // Determinism: one seeded stream, drawn per rank at a high-bit offset so the
  // ranks never reuse each other's chiff draws.
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
