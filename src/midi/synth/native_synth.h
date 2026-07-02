#pragma once

/// @file native_synth.h
/// @brief NativeSynth — the patch-driven synthesis engine: a mode-tagged
///        patch POD (subtractive / FM / Karplus-Strong / modal / additive /
///        percussion / waveguide piano), the unison subtractive voice
///        (PolyBLEP oscillators -> TPT SVF -> exponential DAHDSR VCA) with
///        the per-mode cores embedded beside it, and a 16-channel
///        MidiInstrument host around the shared voice pool.
///
/// Two consumers share the voice:
///   - NativeSynth (this file): a standalone patch-driven instrument.
///   - Sf2Player: per-note synth fallback when no SoundFont covers a program
///     (the data-free floor — every GM program stays audible with zero data).
///
/// RT contract (MidiInstrument): prepare() runs on the control thread and is
/// the only allocating call; on_event()/process() are allocation-free,
/// lock-free and IO-free. Voices reference their patch by pointer — patches
/// must outlive the voice (config member / static fallback tables).
///
/// Determinism: no RNG, no wall clock. Unison detune jitter, oscillator
/// start phases and drift-LFO variation derive from the counter-based
/// (voice_index, note, age) hash, so identical event streams bounce
/// bit-identically within one build.

#include <array>
#include <cstdint>
#include <vector>

#include "midi/instrument.h"
#include "midi/synth/additive_voice.h"
#include "midi/synth/body_resonator.h"
#include "midi/synth/bowed_string_voice.h"
#include "midi/synth/brass_voice.h"
#include "midi/synth/channel_param_state.h"
#include "midi/synth/envelope.h"
#include "midi/synth/filter_models.h"
#include "midi/synth/flute_voice.h"
#include "midi/synth/fm_voice.h"
#include "midi/synth/ks_voice.h"
#include "midi/synth/mod_matrix.h"
#include "midi/synth/modal_voice.h"
#include "midi/synth/oscillator.h"
#include "midi/synth/percussion_voice.h"
#include "midi/synth/piano_voice.h"
#include "midi/synth/pipe_organ_voice.h"
#include "midi/synth/reed_voice.h"
#include "midi/synth/sf2_voice.h"
#include "midi/synth/voice_pool.h"

namespace sonare::midi::synth {

/// Synthesis method tag. Every mode is implemented.
enum class SynthEngineMode : int {
  kSubtractive = 0,
  kFm = 1,             // operator-stack FM (fm_voice.h)
  kKarplusStrong = 2,  // plucked-string waveguide (ks_voice.h)
  kModal = 3,          // resonator-bank mallets/bells (modal_voice.h)
  kAdditive = 4,       // drawbar organ (additive_voice.h)
  kPercussion = 5,     // membrane modal + filtered noise (percussion_voice.h)
  kPiano = 6,          // extended waveguide piano (piano_voice.h)
  kPipeOrgan = 7,      // sustained waveguide flue pipe (pipe_organ_voice.h)
  kBowedString = 8,    // sustained waveguide bowed string (bowed_string_voice.h)
  kReed = 9,           // sustained waveguide reed woodwind (reed_voice.h)
  kBrass = 10,         // sustained waveguide brass / lip reed (brass_voice.h)
  kFlute = 11,         // sustained waveguide air-jet flute (flute_voice.h)
};

/// Maximum unison oscillators per voice (supersaw width).
inline constexpr int kMaxUnisonOscs = 7;

/// One playable patch: oscillator section, filter section, envelopes and the
/// single vibrato LFO. A POD by design — fallback tables are static const
/// data and the later ABI struct mirrors these fields.
struct NativeSynthPatch {
  SynthEngineMode mode = SynthEngineMode::kSubtractive;

  // --- oscillator section ---
  VaWaveform waveform = VaWaveform::kSaw;
  /// Unison oscillator count (clamped to [1, kMaxUnisonOscs]).
  int unison = 1;
  /// Full detune spread between the outermost unison oscillators (cents).
  float detune_cents = 0.0f;
  /// Per-voice slow seeded pitch drift depth (cents) and rate (Hz) — the
  /// "analog beat" that keeps stacked notes from sounding static.
  float drift_cents = 0.0f;
  float drift_rate_hz = 0.3f;
  /// Coarse tune applied to the played note (cents; e.g. -1200 = sub octave).
  float pitch_offset_cents = 0.0f;

  // --- amplitude ---
  /// Per-voice linear gain (the instrument master gain is separate).
  float gain = 0.5f;
  DahdsrConfig amp_env;
  /// One-shot (drum) voices ignore note-off and end at the envelope's
  /// zero-sustain decay floor.
  bool one_shot = false;

  // --- filter section ---
  /// Filter model (the "character" core; see filter_models.h).
  SynthFilterModel filter_model = SynthFilterModel::kSvf;
  SynthFilterOutput filter_output = SynthFilterOutput::kLowpass;
  float cutoff_hz = 12000.0f;
  /// Resonance Q. The ladder / Sallen-Key models map it to their normalized
  /// feedback; Q >= kSelfOscQ reaches self-oscillation on those models.
  float resonance_q = 0.707f;
  /// Pre-filter drive in [0,1]: gain-compensated tanh saturation on the
  /// oscillator mix (0 = clean).
  float drive = 0.0f;
  DahdsrConfig filter_env;
  /// Filter envelope -> cutoff offset at full envelope (cents).
  float env_to_cutoff_cents = 0.0f;
  /// Keyboard tracking: cutoff follows the note by this fraction (0..1).
  float key_track = 0.0f;
  /// Velocity -> brightness: soft notes lower the cutoff by up to this many
  /// cents (full velocity = no offset), like the SF2 default modulator.
  float vel_to_cutoff_cents = 0.0f;

  // --- LFOs ---
  /// LFO1 with the hardwired vibrato routing (also ModSource::kLfo1).
  float lfo_rate_hz = 5.0f;
  float lfo_to_pitch_cents = 0.0f;
  /// LFO2 is matrix-routed only (ModSource::kLfo2).
  float lfo2_rate_hz = 1.0f;

  // --- glide / portamento ---
  /// One-pole pitch glide from the channel's previous note (ms to reach the
  /// target within ~5%; 0 = off).
  float glide_ms = 0.0f;

  // --- realism polish ---
  /// Body/formant resonance voicing applied to the voice output (commuted
  /// synthesis, body_resonator.h) and its mix in [0,1].
  BodyType body = BodyType::kNone;
  float body_mix = 0.0f;
  /// Per-voice seeded stereo pan scatter in [0,1] (0 keeps every voice
  /// centre-relative, preserving bit-stable mono bounces).
  float stereo_spread = 0.0f;

  /// Free-form modulation routings on top of the hardwired patch modulations.
  ModMatrix mod_matrix;

  /// FM operator stack (used when mode == kFm; the subtractive oscillator
  /// section is ignored in that mode, while amp envelope / filter / matrix /
  /// glide still apply around the FM core).
  FmPatchParams fm;

  /// Karplus-Strong string (used when mode == kKarplusStrong; like FM, the
  /// oscillator section is ignored while the wrapper sections still apply).
  KsPatchParams ks;

  /// Modal resonator bank (used when mode == kModal).
  ModalPatchParams modal;

  /// Drawbar-organ partials (used when mode == kAdditive).
  AdditivePatchParams additive;

  /// Membrane + noise kit piece (used when mode == kPercussion).
  PercussionPatchParams percussion;

  /// Extended waveguide piano (used when mode == kPiano).
  PianoPatchParams piano;

  /// Sustained waveguide flue pipe (used when mode == kPipeOrgan).
  PipeOrganPatchParams pipe_organ;

  /// Sustained waveguide bowed string (used when mode == kBowedString).
  BowedStringPatchParams bowed_string;

  /// Sustained waveguide reed woodwind (used when mode == kReed).
  ReedPatchParams reed;

  /// Sustained waveguide brass / lip reed (used when mode == kBrass).
  BrassPatchParams brass;

  /// Sustained waveguide air-jet flute (used when mode == kFlute).
  FlutePatchParams flute;
};

/// Per-note GS drum overrides applied to a fallback percussion voice at
/// note-on (NRPN pitch coarse / TVA level / absolute pan). Defaults are no-ops,
/// so a voice with no per-note edit renders exactly as before.
struct DrumVoiceMod {
  float pitch_ratio = 1.0f;  ///< 2^(pitch_coarse / 12)
  float level_gain = 1.0f;   ///< (level / 127)^2 (same square law as CC7)
  float pan_units = 1.0e9f;  ///< absolute pan units (1e9 = untouched: keep channel pan)
};

/// One playing subtractive voice (lives in a VoicePool inside NativeSynth and
/// in Sf2Player's fallback pool). Renders mono; the mixer applies
/// gain_left/right (refreshed from the channel pan like Sf2Voice).
struct NativeSynthVoice : VoiceState {
  const NativeSynthPatch* patch = nullptr;
  std::array<VaOscillator, kMaxUnisonOscs> oscs{};
  std::array<float, kMaxUnisonOscs> detune_ratio{};
  int unison = 1;
  float osc_norm = 1.0f;  // 1/sqrt(unison)
  float base_freq_hz = 440.0f;
  float velocity_gain = 1.0f;
  /// Static cutoff offset precomputed at start (velocity + key tracking).
  float static_cutoff_cents = 0.0f;
  bool filter_bypass = false;
  /// Pre-filter drive gain / makeup (precomputed from patch->drive; 0 = off).
  float drive_gain = 0.0f;
  float drive_makeup = 1.0f;
  DahdsrEnvelope amp_env;
  DahdsrEnvelope filter_env;
  SynthFilter filter;
  FmVoiceCore fm;
  /// KS string core; the host attach()es its delay span before start() (the
  /// slab is owned by the instrument and allocated in prepare()).
  KsVoiceCore ks;
  ModalVoiceCore modal;
  AdditiveVoiceCore additive;
  PercussionVoiceCore percussion;
  /// Piano string core; like KS, the host attach()es its delay slab before
  /// start().
  PianoVoiceCore piano;
  /// Flue-pipe core; like KS, the host attach()es its delay span before
  /// start().
  PipeOrganVoiceCore pipe_organ;
  /// Bowed-string core; like KS, the host attach()es its delay slab before
  /// start().
  BowedStringVoiceCore bowed_string;
  /// Reed-woodwind core; like KS, the host attach()es its bore span before
  /// start().
  ReedVoiceCore reed;
  /// Brass / lip-reed core; like KS, the host attach()es its bore span before
  /// start().
  BrassVoiceCore brass;
  /// Air-jet flute core; like KS, the host attach()es its delay slab (bore +
  /// jet spans) before start().
  FluteVoiceCore flute;
  BodyResonator body;
  Sf2Lfo vibrato_lfo;
  Sf2Lfo lfo2;
  Sf2Lfo drift_lfo;
  float drift_depth_cents = 0.0f;
  // Mod-matrix source constants (precomputed at start).
  bool has_matrix = false;
  float velocity01 = 0.0f;
  float key_track_octaves = 0.0f;
  float random_value = 0.0f;
  // Glide: pitch offset in cents decaying to zero through a one-pole.
  float glide_cents = 0.0f;
  float glide_coeff = 0.0f;
  /// Seeded per-voice pan scatter (patch stereo_spread; pan units).
  float pan_spread_units = 0.0f;
  bool key_down = false;
  /// Captured by the sostenuto pedal (CC66): held past key-up until the pedal
  /// lifts, regardless of the sustain pedal.
  bool sostenuto = false;
  // Cached stereo gains for the channel pan; recomputed on change.
  float cached_pan_units = 1.0e9f;
  float gain_left = 0.70710678f;
  float gain_right = 0.70710678f;
  /// GS per-note drum overrides (pitch coarse / absolute pan; level is folded
  /// into velocity_gain at start). Defaults are no-ops.
  float drum_pitch_ratio = 1.0f;
  float drum_pan_units = 1.0e9f;

  /// Starts the voice for @p p. note/channel/age must already be set (the
  /// pool fills them in allocate()); @p voice_index seeds the deterministic
  /// per-voice variation. @p p must outlive the voice. @p glide_from_hz != 0
  /// glides the pitch from that frequency (portamento; needs p.glide_ms > 0).
  /// @p una_corda engages the soft-pedal voicing (piano mode only).
  /// @p drum_kit != 0 applies a GS kit variation (Room/Power/808/...) to the
  /// resolved drum patch at note-on (percussion mode only; see apply_gs_drum_kit).
  /// @p drum_mod carries GS per-note drum NRPN edits (pitch coarse / level /
  /// pan); default is a no-op.
  void start(const NativeSynthPatch& p, double sample_rate, uint8_t velocity, uint32_t voice_index,
             float glide_from_hz = 0.0f, bool una_corda = false, uint8_t drum_kit = 0,
             DrumVoiceMod drum_mod = {}) noexcept;
  /// Renders one mono sample. Deactivates when the amp envelope ends.
  /// @p wind_pitch / @p wind_gain carry the shared organ wind modulation
  /// (tremulant / wind sag); 1.0 leaves the voice unmodulated.
  float render(const Sf2ChannelMod& mod, float wind_pitch = 1.0f, float wind_gain = 1.0f) noexcept;
  /// Note-off: enter release (ignored by one-shot patches).
  void release() noexcept;
  /// Immediate silence (All Sound Off / steal-kill).
  void kill() noexcept;
  /// Exclusive-group choke: force the amp envelope into release even for
  /// one-shot (drum) voices, so a same-group strike (hi-hat / triangle / ...)
  /// cuts this ringing voice with a short fade instead of an abrupt kill. The
  /// fade length is the patch's amp release_ms.
  void choke() noexcept;
};

struct NativeSynthConfig {
  NativeSynthPatch patch;
  /// Master output gain applied to the summed voices (linear).
  float gain = 0.5f;
  /// Voice pool size (clamped to [1, kMaxSynthVoices]).
  int polyphony = 16;
  /// Gentle gain-neutral tanh on the mix bus in [0,1] (0 = clean) — glues a
  /// stack of voices together.
  float bus_drive = 0.0f;
  /// DC blocker on the mix bus (the physical-model voices can carry a small
  /// DC component).
  bool dc_block = true;
};

/// Standalone patch-driven MidiInstrument: all 16 channels play the same
/// patch with BuiltinSynth-compatible channel semantics plus the default-
/// modulator CCs (CC1 vibrato, CC7/CC11 gain, CC10 pan, pitch bend, CC64
/// sustain, CC120/121/123 channel modes).
class NativeSynth final : public MidiInstrument {
 public:
  explicit NativeSynth(const NativeSynthConfig& config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int tail_samples() const noexcept override { return static_cast<int>(tail_samples_); }
  void on_event(uint32_t destination_id, const MidiEvent& event) noexcept override;

  const NativeSynthPatch& patch() const noexcept { return config_.patch; }

  /// Currently sounding voices (test/diagnostic).
  int active_voice_count() const noexcept { return pool_.active_count(); }

 private:
  struct ChannelState {
    bool sustain = false;       // CC64 >= 64 (dampers lifted)
    uint8_t sustain_level = 0;  // raw CC64 (half-pedal damper position)
    bool una_corda = false;     // CC67 soft pedal
    uint8_t volume = 100;       // CC7
    uint8_t expression = 127;   // CC11
    uint8_t pan = 64;           // CC10
    uint8_t mod_wheel = 0;      // CC1
    uint8_t program = 0;        // last program change (GS drum-kit select in gm_kit mode)
    uint16_t pitch_bend = 8192;
    /// Bowed-string continuous controllers (255 = untouched, so the preset's
    /// own bow force / position stands until the host sends the CC): CC2 breath
    /// -> bow force, CC74 -> bow position. CC11 expression scales bow speed.
    uint8_t bow_force = 255;
    uint8_t bow_position = 255;
    /// Reed-woodwind continuous controllers (255 = untouched, so the preset's
    /// own breath / brightness stands until the host sends the CC): CC2 breath
    /// -> mouth pressure, CC74 -> bell brightness. CC11 expression is the shared
    /// loudness VCA (no reed-specific breath push — that would silence the reed).
    uint8_t reed_breath = 255;
    uint8_t reed_bright = 255;
    /// Brass / lip-reed continuous controllers (255 = untouched, so the preset's
    /// own breath / brightness stands until the host sends the CC): CC2 breath
    /// -> mouth pressure, CC74 -> bell brightness. CC11 expression is the shared
    /// loudness VCA (no brass-specific breath push — that would silence the lips).
    uint8_t brass_breath = 255;
    uint8_t brass_bright = 255;
    /// Air-jet flute continuous controllers (255 = untouched, so the preset's
    /// own breath / brightness stands until the host sends the CC): CC2 breath ->
    /// mouth pressure, CC74 -> reflection brightness. CC11 expression is the
    /// shared loudness VCA (no flute-specific breath push — that would silence the
    /// jet); CC1 vibrato rides the shared mod-wheel LFO like every voice (the
    /// core's own vibrato is the preset's intrinsic, per-voice vibrato).
    uint8_t flute_breath = 255;
    uint8_t flute_bright = 255;
    ChannelParamState params;
    float bend_range_cents = 200.0f;
    /// Previous note's frequency (glide source; 0 = none yet).
    float last_freq_hz = 0.0f;
  };

  void note_on(uint8_t channel, uint8_t note, uint8_t velocity) noexcept;
  void note_off(uint8_t channel, uint8_t note) noexcept;
  void control_change(uint8_t channel, uint8_t controller, uint8_t value) noexcept;
  void sustain_cc(uint8_t channel, uint8_t value) noexcept;
  void sostenuto_pedal(uint8_t channel, bool down) noexcept;
  void all_notes_off(uint8_t channel) noexcept;
  void all_sound_off(uint8_t channel) noexcept;
  /// Pushes the channel's live bowed-string controllers (CC11 bow speed, CC2
  /// bow force, CC74 bow position) to its sounding bowed voices.
  void push_bow_control(uint8_t channel) noexcept;
  /// Pushes the channel's live reed controllers (CC2 breath, CC74 bell
  /// brightness) to its sounding reed voices.
  void push_reed_control(uint8_t channel) noexcept;
  /// Pushes the channel's live brass controllers (CC2 breath, CC74 bell
  /// brightness) to its sounding brass voices.
  void push_brass_control(uint8_t channel) noexcept;
  /// Pushes the channel's live flute controllers (CC2 breath, CC74 reflection
  /// brightness, CC1 vibrato depth) to its sounding flute voices.
  void push_flute_control(uint8_t channel) noexcept;
  void reset_controllers(uint8_t channel) noexcept;
  void refresh_channel_mod(uint8_t channel) noexcept;

  NativeSynthConfig config_{};
  double sample_rate_ = 0.0;
  bool prepared_ = false;
  int64_t tail_samples_ = 0;
  std::array<ChannelState, 16> channels_{};
  std::array<Sf2ChannelMod, 16> channel_mods_{};
  /// Mix-bus polish state (per stereo leg): DC blocker + drive constants.
  std::array<float, 2> dc_x1_{};
  std::array<float, 2> dc_y1_{};
  float dc_r_ = 0.999f;
  float bus_drive_gain_ = 0.0f;
  VoicePool<NativeSynthVoice> pool_;
  /// KS delay slab: one ks_slab_capacity() (three ks_buffer_capacity() spans —
  /// the primary string, the second-polarization line, and the octave-up 4'
  /// companion line) per voice slot, allocated in prepare() only when the patch
  /// is a Karplus-Strong instrument.
  std::vector<float> ks_buffers_;
  int ks_capacity_ = 0;
  /// Piano delay slab: kMaxPianoStrings string spans per voice slot,
  /// allocated in prepare() only when the patch is a piano.
  std::vector<float> piano_buffers_;
  int piano_string_capacity_ = 0;
  /// Shared sympathetic resonance bank. Piano patches drive it pedal-gated (the
  /// sustain-pedal sound halo); Karplus-Strong patches that opt in (patch.ks.
  /// sympathetic) reuse the same bank tuned to the open-string set, held open
  /// (plucked strings have no dampers). kPiano and kKarplusStrong are mutually
  /// exclusive modes, so one bank serves both without a second allocation.
  PianoResonanceBank resonance_;
  /// Shared modal soundboard body (piano patches only).
  PianoSoundboard soundboard_;
  bool piano_mode_ = false;
  /// A Karplus-Strong patch has opted into the shared sympathetic bank
  /// (patch.ks.sympathetic). false leaves every existing KS voicing on its
  /// original render path (the resonance branch is skipped entirely).
  bool sympathetic_active_ = false;
  /// Pipe-organ delay slab: one kMaxPipeRanks-pipe slab per voice slot,
  /// allocated in prepare() only when the patch is a pipe organ.
  std::vector<float> pipe_organ_buffers_;
  int pipe_organ_capacity_ = 0;  // per-rank span
  bool pipe_organ_mode_ = false;
  /// Bowed-string delay slab: two delay-line spans (neck + bridge) per voice
  /// slot, allocated in prepare() only when the patch is a bowed string.
  std::vector<float> bowed_string_buffers_;
  int bowed_string_capacity_ = 0;  // per-line span
  bool bowed_string_mode_ = false;
  /// Reed-woodwind bore slab: one bore span per voice slot, allocated in
  /// prepare() only when the patch is a reed woodwind.
  std::vector<float> reed_buffers_;
  int reed_capacity_ = 0;  // bore span
  bool reed_mode_ = false;
  /// Brass bore slab: one bore span per voice slot, allocated in prepare() only
  /// when the patch is a brass / lip reed.
  std::vector<float> brass_buffers_;
  int brass_capacity_ = 0;  // bore span
  bool brass_mode_ = false;
  /// Flute delay slab: a bore span plus a jet span per voice slot, allocated in
  /// prepare() only when the patch is an air-jet flute.
  std::vector<float> flute_buffers_;
  int flute_capacity_ = 0;  // per-span (bore / jet) capacity
  bool flute_mode_ = false;
  /// Shared organ wind chest (tremulant / wind sag); pipe-organ patches only.
  OrganWindSupply wind_;
  /// Swell box: a bus-level shutter lowpass driven by the expression pedal
  /// (CC11). swell_depth_ == 0 disables it; the one-pole state is per leg.
  float swell_depth_ = 0.0f;
  float swell_coeff_ = 1.0f;  // recomputed per block from the shutter position
  float swell_lp_l_ = 0.0f;
  float swell_lp_r_ = 0.0f;
};

/// Returns a copy of @p patch with every field clamped to a safe range.
NativeSynthPatch clamp_synth_patch(const NativeSynthPatch& patch) noexcept;

/// MIDI note -> equal-tempered frequency (A4 = 440 Hz).
float synth_note_to_hz(float note) noexcept;

}  // namespace sonare::midi::synth
