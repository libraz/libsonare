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

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
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
#include "midi/synth/free_reed_voice.h"
#include "midi/synth/harpsichord_voice.h"
#include "midi/synth/ks_voice.h"
#include "midi/synth/mod_matrix.h"
#include "midi/synth/modal_voice.h"
#include "midi/synth/oscillator.h"
#include "midi/synth/percussion_voice.h"
#include "midi/synth/piano_voice.h"
#include "midi/synth/pipe_organ_voice.h"
#include "midi/synth/plucked_string_voice.h"
#include "midi/synth/reed_voice.h"
#include "midi/synth/sf2_voice.h"
#include "midi/synth/vocal_voice.h"
#include "midi/synth/voice_pool.h"
#include "util/constants.h"

namespace sonare::midi::synth {

/// Synthesis method tag. Every mode is implemented.
enum class SynthEngineMode : int {
  kSubtractive = 0,
  kFm = 1,              // operator-stack FM (fm_voice.h)
  kKarplusStrong = 2,   // plucked-string waveguide (ks_voice.h)
  kModal = 3,           // resonator-bank mallets/bells (modal_voice.h)
  kAdditive = 4,        // drawbar organ (additive_voice.h)
  kPercussion = 5,      // membrane modal + filtered noise (percussion_voice.h)
  kPiano = 6,           // extended waveguide piano (piano_voice.h)
  kPipeOrgan = 7,       // sustained waveguide flue pipe (pipe_organ_voice.h)
  kBowedString = 8,     // sustained waveguide bowed string (bowed_string_voice.h)
  kReed = 9,            // sustained waveguide reed woodwind (reed_voice.h)
  kBrass = 10,          // sustained waveguide brass / lip reed (brass_voice.h)
  kFlute = 11,          // sustained waveguide air-jet flute (flute_voice.h)
  kPluckedString = 12,  // buzzing-bridge plucked string (plucked_string_voice.h)
  kVocal = 13,          // source-filter glottal + formant voice (vocal_voice.h)
  kFreeReed = 14,       // driven free-reed accordion / harmonica (free_reed_voice.h)
  kHarpsichord = 15,    // jack-and-plectrum string choirs (harpsichord_voice.h)
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

  /// Buzzing-bridge plucked string (used when mode == kPluckedString).
  PluckedStringPatchParams plucked_string;

  /// Source-filter glottal + formant voice (used when mode == kVocal).
  VocalPatchParams vocal;

  /// Driven free-reed accordion / harmonica (used when mode == kFreeReed).
  FreeReedPatchParams free_reed;

  /// Jack-and-plectrum string choirs (used when mode == kHarpsichord).
  HarpsichordPatchParams harpsichord;
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
  /// Buzzing-bridge plucked-string core; like KS, the host attach()es its delay
  /// span before start().
  PluckedStringVoiceCore plucked_string;
  /// Source-filter vocal core (no host slab; the formant bank is feed-forward).
  VocalVoiceCore vocal;
  /// Free-reed core (no host slab; the driven tongue oscillator is feed-forward).
  FreeReedVoiceCore free_reed;
  /// Harpsichord core; the host attach()es its registration slab before start().
  HarpsichordVoiceCore harpsichord;
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
  // Cached stereo gains for the channel pan; recomputed on change. Seeded
  // centred, which under the constant-power law is 1/sqrt(2) a side.
  float cached_pan_units = 1.0e9f;
  float gain_left = ::sonare::constants::kInvSqrt2;
  float gain_right = ::sonare::constants::kInvSqrt2;
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
  /// True when the patch's filter stage cannot colour this voice: a wide-open
  /// static SVF lowpass, no resonance, no envelope depth and no negative
  /// static offset. Read from the LIVE patch on every sample rather than
  /// latched at start(): `cutoff_hz` and `resonance_q` are automation targets
  /// that apply to already-sounding voices, so a sweep on a held note has to
  /// engage the stage on the block it starts, not on the next note-on.
  bool filter_inaudible() const noexcept {
    return patch->filter_model == SynthFilterModel::kSvf &&
           patch->filter_output == SynthFilterOutput::kLowpass && patch->cutoff_hz >= 18000.0f &&
           patch->env_to_cutoff_cents == 0.0f && static_cutoff_cents >= 0.0f &&
           patch->resonance_q <= 0.71f;
  }
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
  /// Resolve each melodic channel from its GM bank/program state and route
  /// channel 10 through the GM drum-kit map. This is intended for generic MIDI
  /// file playback; false keeps a deliberately selected patch fixed.
  bool use_gm_programs = false;
};

/// Continuously automatable NativeSynth parameters, addressed through
/// MidiInstrument::parameter_id_for_key / apply_parameter. The ordinals are the
/// persisted automation-target ids, so they are append-only: never renumber an
/// existing entry.
///
/// Two timing classes, because a voice reads some patch fields every sample and
/// caches others at note-on:
///  - applied to SOUNDING voices from the next block: kGain, kBusDrive,
///    kCutoffHz, kResonanceQ, kEnvToCutoffCents, kLfoToPitchCents,
///    kPitchOffsetCents
///  - applied from the NEXT NOTE-ON: kDrive, kKeyTrack, kVelToCutoffCents, the
///    envelope segments, kLfoRateHz, kLfo2RateHz, kGlideMs, kBodyMix,
///    kStereoSpread, kDetuneCents, kDriftCents (each is precomputed into
///    per-voice state when the voice starts)
enum class NativeSynthParamId : unsigned int {
  kGain = 0,
  kBusDrive = 1,
  kCutoffHz = 2,
  kResonanceQ = 3,
  kDrive = 4,
  kKeyTrack = 5,
  kEnvToCutoffCents = 6,
  kVelToCutoffCents = 7,
  kAmpAttackMs = 8,
  kAmpDecayMs = 9,
  kAmpSustain = 10,
  kAmpReleaseMs = 11,
  kFilterAttackMs = 12,
  kFilterDecayMs = 13,
  kFilterSustain = 14,
  kFilterReleaseMs = 15,
  kLfoRateHz = 16,
  kLfoToPitchCents = 17,
  kLfo2RateHz = 18,
  kGlideMs = 19,
  kBodyMix = 20,
  kStereoSpread = 21,
  kDetuneCents = 22,
  kDriftCents = 23,
  kPitchOffsetCents = 24,
};

/// JSON-key name for @p id (the same string a SynthPatch object would use), or
/// nullptr for an unknown id.
const char* native_synth_param_name(NativeSynthParamId id) noexcept;
/// Number of automatable parameters, for enumeration by a host UI.
size_t native_synth_param_count() noexcept;
/// JSON-key name at @p index in [0, native_synth_param_count()), or nullptr.
const char* native_synth_param_name_at(size_t index) noexcept;

/// Standalone patch-driven MidiInstrument: all 16 channels play the same
/// patch with BuiltinSynth-compatible channel semantics plus the default-
/// modulator CCs (CC1 vibrato, CC7/CC11 gain, CC10 pan, pitch bend, CC64
/// sustain, CC120/121/123 channel modes).
class NativeSynth final : public MidiInstrument {
 public:
  explicit NativeSynth(const NativeSynthConfig& config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  bool process_source_tracks(const MidiInstrumentSourceOutput* outputs, size_t output_count,
                             int num_channels, int num_samples) noexcept override;
  bool supports_source_track_rendering() const noexcept override { return true; }
  void reset() override;
  int tail_samples() const noexcept override { return static_cast<int>(tail_samples_); }
  void on_event(uint32_t destination_id, const MidiEvent& event) noexcept override;
  int parameter_id_for_key(const std::string& key) const noexcept override;
  bool apply_parameter(unsigned int param_id, float value) noexcept override;

  const NativeSynthPatch& patch() const noexcept { return config_.patch; }
  /// Instrument master gain (test/diagnostic; the automated kGain target).
  float gain() const noexcept { return config_.gain; }

  /// Currently sounding voices (test/diagnostic).
  int active_voice_count() const noexcept { return pool_.active_count(); }

 private:
  struct ChannelState {
    bool sustain = false;       // CC64 >= 64 (dampers lifted)
    uint8_t sustain_level = 0;  // raw CC64 (half-pedal damper position)
    /// CC66 pedal position. Sostenuto captures exactly the keys held at the
    /// DOWN EDGE, so the capture sweep must be edge-triggered: a pedal that
    /// keeps sending values >= 64 would otherwise capture notes struck after
    /// the press, which is the opposite of what the pedal means.
    bool sostenuto_down = false;
    bool una_corda = false;  // CC67 soft pedal
    /// Rhythm part: resolves through the drum map instead of the melodic
    /// programs. Channel 10 by default (GM), matching Sf2Player.
    bool drums = false;
    uint8_t volume = 100;      // CC7
    uint8_t expression = 127;  // CC11
    uint8_t pan = 64;          // CC10
    uint8_t mod_wheel = 0;     // CC1
    uint8_t program = 0;       // last program change (or GM melodic program)
    uint8_t bank_msb = 0;
    uint8_t bank_lsb = 0;
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

  void note_on(uint8_t channel, uint8_t note, uint8_t velocity, uint32_t source_track_id) noexcept;
  void note_off(uint8_t channel, uint8_t note, uint32_t source_track_id) noexcept;
  void process_impl(float* const* channels, const MidiInstrumentSourceOutput* source_outputs,
                    size_t source_output_count, int num_channels, int num_samples) noexcept;
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
  /// The configured patch is a piano, so the bus body is tuned in prepare().
  bool piano_mode_ = false;
  /// A piano patch is currently voiced, so the render loop routes its voices
  /// through the bus body (direct-share attenuation, modal soundboard,
  /// pedal-gated sympathetic bank). Decided per note-on rather than from the
  /// construction-time patch, because GM mode resolves the engine per program:
  /// program 0 reaches the piano patch there too, and gating on the configured
  /// mode would render it as a bare string. Mirrors Sf2Player's per-part
  /// fallback_body_, bus-scoped because NativeSynth has one mix bus.
  bool piano_body_active_ = false;
  /// Soundboard mix the bus body is currently tuned for (-1 = never tuned), so
  /// a note-on re-prepares only when the resolved patch asks for a different
  /// board — the bank keeps its state across notes otherwise.
  float piano_body_soundboard_ = -1.0f;
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
  /// Plucked-string delay slab: one string span per voice slot, allocated in
  /// prepare() only when the patch is a plucked string.
  std::vector<float> plucked_string_buffers_;
  int plucked_string_capacity_ = 0;  // per-span capacity
  bool plucked_string_mode_ = false;
  /// Harpsichord registration slab: the two 8' choirs, the 4' choir and the
  /// behind-the-bridge segment per voice slot, allocated in prepare() only when
  /// the patch is a harpsichord. The spans are not all the same length, so the
  /// per-voice stride is carried separately from the speaking-string span.
  std::vector<float> harpsichord_buffers_;
  int harpsichord_capacity_ = 0;  // speaking-string span
  int harpsichord_stride_ = 0;    // whole registration slab, per voice slot
  bool harpsichord_mode_ = false;
  /// Shared organ wind chest (tremulant / wind sag); pipe-organ patches only.
  OrganWindSupply wind_;
  /// Swell box: a bus-level shutter lowpass driven by the expression pedal
  /// (CC11). swell_depth_ == 0 disables it; the one-pole state is per leg.
  float swell_depth_ = 0.0f;
  float swell_coeff_ = 1.0f;  // recomputed per block from the shutter position
  float swell_lp_l_ = 0.0f;
  float swell_lp_r_ = 0.0f;
};

/// Patch-clamp helpers. Named rather than local because `clamp_synth_patch`
/// is `constexpr`: the fallback tables it feeds are constant-initialised, so
/// its whole call tree has to be visible here rather than in a translation
/// unit of its own.
namespace patch_clamp_detail {

constexpr DahdsrConfig clamp_env(const DahdsrConfig& env) noexcept {
  DahdsrConfig out{};
  out.delay_ms = std::clamp(env.delay_ms, 0.0f, 5000.0f);
  out.attack_ms = std::clamp(env.attack_ms, 0.0f, 20000.0f);
  out.hold_ms = std::clamp(env.hold_ms, 0.0f, 5000.0f);
  out.decay_ms = std::clamp(env.decay_ms, 0.0f, 20000.0f);
  out.sustain = std::clamp(env.sustain, 0.0f, 1.0f);
  out.release_ms = std::clamp(env.release_ms, 1.0f, 20000.0f);
  return out;
}

/// Finite test usable during constant evaluation. `std::isfinite` is not
/// `constexpr` in C++17; `value == value` rejects NaN and the magnitude
/// bounds reject both infinities, which is the same predicate for `float`.
constexpr float sanitize(float value, float fallback) noexcept {
  return (value == value && value <= std::numeric_limits<float>::max() &&
          value >= std::numeric_limits<float>::lowest())
             ? value
             : fallback;
}

}  // namespace patch_clamp_detail

/// Returns a copy of @p patch with every field clamped to a safe range.
///
/// `constexpr` so the GM fallback tables (`program_overrides()`,
/// `drum_note_table()`, `family_patches()`) are constant-initialised into the
/// data section rather than assembled field by field by start-up code, which
/// the WebAssembly size budget cannot afford. Caller-supplied patches still
/// clamp at run time through the same definition.
constexpr NativeSynthPatch clamp_synth_patch(const NativeSynthPatch& patch) noexcept {
  NativeSynthPatch p = patch;
  p.unison = std::clamp(p.unison, 1, kMaxUnisonOscs);
  p.detune_cents = std::clamp(patch_clamp_detail::sanitize(p.detune_cents, 0.0f), 0.0f, 200.0f);
  p.drift_cents = std::clamp(patch_clamp_detail::sanitize(p.drift_cents, 0.0f), 0.0f, 100.0f);
  p.drift_rate_hz = std::clamp(patch_clamp_detail::sanitize(p.drift_rate_hz, 0.3f), 0.01f, 20.0f);
  p.pitch_offset_cents =
      std::clamp(patch_clamp_detail::sanitize(p.pitch_offset_cents, 0.0f), -4800.0f, 4800.0f);
  p.gain = std::clamp(patch_clamp_detail::sanitize(p.gain, 0.5f), 0.0f, 4.0f);
  p.amp_env = patch_clamp_detail::clamp_env(p.amp_env);
  p.cutoff_hz = std::clamp(patch_clamp_detail::sanitize(p.cutoff_hz, 12000.0f), 10.0f, 22000.0f);
  p.resonance_q = std::clamp(patch_clamp_detail::sanitize(p.resonance_q, constants::kButterworthQ),
                             0.5f, 30.0f);
  p.drive = std::clamp(patch_clamp_detail::sanitize(p.drive, 0.0f), 0.0f, 1.0f);
  p.filter_env = patch_clamp_detail::clamp_env(p.filter_env);
  p.env_to_cutoff_cents =
      std::clamp(patch_clamp_detail::sanitize(p.env_to_cutoff_cents, 0.0f), -9600.0f, 9600.0f);
  p.key_track = std::clamp(patch_clamp_detail::sanitize(p.key_track, 0.0f), 0.0f, 1.0f);
  p.vel_to_cutoff_cents =
      std::clamp(patch_clamp_detail::sanitize(p.vel_to_cutoff_cents, 0.0f), -9600.0f, 9600.0f);
  p.lfo_rate_hz = std::clamp(patch_clamp_detail::sanitize(p.lfo_rate_hz, 5.0f), 0.0f, 40.0f);
  p.lfo_to_pitch_cents =
      std::clamp(patch_clamp_detail::sanitize(p.lfo_to_pitch_cents, 0.0f), 0.0f, 1200.0f);
  p.lfo2_rate_hz = std::clamp(patch_clamp_detail::sanitize(p.lfo2_rate_hz, 1.0f), 0.0f, 40.0f);
  p.glide_ms = std::clamp(patch_clamp_detail::sanitize(p.glide_ms, 0.0f), 0.0f, 5000.0f);
  for (ModRoute& route : p.mod_matrix.routes) {
    route.depth = std::clamp(patch_clamp_detail::sanitize(route.depth, 0.0f), -9600.0f, 9600.0f);
  }
  for (FmOperatorParams& op : p.fm.ops) {
    op.ratio = std::clamp(patch_clamp_detail::sanitize(op.ratio, 1.0f), 0.0f, 64.0f);
    op.detune_cents =
        std::clamp(patch_clamp_detail::sanitize(op.detune_cents, 0.0f), -1200.0f, 1200.0f);
    op.level = std::clamp(patch_clamp_detail::sanitize(op.level, 0.0f), 0.0f, 16.0f);
    op.env = patch_clamp_detail::clamp_env(op.env);
    op.vel_to_level = std::clamp(patch_clamp_detail::sanitize(op.vel_to_level, 0.0f), 0.0f, 1.0f);
    op.key_rate_scale =
        std::clamp(patch_clamp_detail::sanitize(op.key_rate_scale, 0.0f), 0.0f, 1.0f);
    op.feedback = std::clamp(patch_clamp_detail::sanitize(op.feedback, 0.0f), 0.0f, 4.0f);
  }
  p.ks.brightness = std::clamp(patch_clamp_detail::sanitize(p.ks.brightness, 0.6f), 0.0f, 1.0f);
  p.ks.decay_s = std::clamp(patch_clamp_detail::sanitize(p.ks.decay_s, 3.0f), 0.05f, 60.0f);
  p.ks.decay_stretch =
      std::clamp(patch_clamp_detail::sanitize(p.ks.decay_stretch, 0.5f), 0.0f, 1.0f);
  p.ks.pick_position =
      std::clamp(patch_clamp_detail::sanitize(p.ks.pick_position, 0.18f), 0.0f, 0.5f);
  p.ks.exc_brightness =
      std::clamp(patch_clamp_detail::sanitize(p.ks.exc_brightness, 0.85f), 0.0f, 1.0f);
  p.ks.vel_to_brightness =
      std::clamp(patch_clamp_detail::sanitize(p.ks.vel_to_brightness, 0.6f), 0.0f, 1.0f);
  p.ks.release_damp_s =
      std::clamp(patch_clamp_detail::sanitize(p.ks.release_damp_s, 0.08f), 0.01f, 10.0f);
  p.ks.slap = std::clamp(patch_clamp_detail::sanitize(p.ks.slap, 0.0f), 0.0f, 1.0f);
  p.ks.polarization = std::clamp(patch_clamp_detail::sanitize(p.ks.polarization, 0.0f), 0.0f, 1.0f);
  p.modal.num_modes = std::clamp(p.modal.num_modes, 0, kMaxModalModes);
  for (ModalMode& mode : p.modal.modes) {
    mode.ratio = std::clamp(patch_clamp_detail::sanitize(mode.ratio, 1.0f), 0.01f, 64.0f);
    mode.gain = std::clamp(patch_clamp_detail::sanitize(mode.gain, 1.0f), 0.0f, 4.0f);
    mode.decay_scale =
        std::clamp(patch_clamp_detail::sanitize(mode.decay_scale, 1.0f), 0.01f, 4.0f);
  }
  p.modal.decay_s = std::clamp(patch_clamp_detail::sanitize(p.modal.decay_s, 2.0f), 0.01f, 60.0f);
  p.modal.decay_stretch =
      std::clamp(patch_clamp_detail::sanitize(p.modal.decay_stretch, 0.3f), 0.0f, 1.0f);
  p.modal.strike_brightness =
      std::clamp(patch_clamp_detail::sanitize(p.modal.strike_brightness, 0.7f), 0.0f, 1.0f);
  p.modal.vel_to_brightness =
      std::clamp(patch_clamp_detail::sanitize(p.modal.vel_to_brightness, 0.6f), 0.0f, 1.0f);
  p.modal.release_damp_s =
      std::clamp(patch_clamp_detail::sanitize(p.modal.release_damp_s, 0.15f), 0.01f, 10.0f);
  for (float& level : p.additive.drawbars)
    level = std::clamp(patch_clamp_detail::sanitize(level, 0.0f), 0.0f, 8.0f);
  p.additive.key_click =
      std::clamp(patch_clamp_detail::sanitize(p.additive.key_click, 0.4f), 0.0f, 1.0f);
  p.additive.click_decay_ms =
      std::clamp(patch_clamp_detail::sanitize(p.additive.click_decay_ms, 6.0f), 0.5f, 100.0f);
  p.percussion.num_modes = std::clamp(p.percussion.num_modes, 0, kMaxPercussionModes);
  for (float& ratio : p.percussion.mode_ratios) {
    ratio = std::clamp(patch_clamp_detail::sanitize(ratio, 0.0f), 0.0f, 64.0f);
  }
  p.percussion.mode_decay_s =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.mode_decay_s, 0.3f), 0.005f, 30.0f);
  p.percussion.tone_gain =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.tone_gain, 1.0f), 0.0f, 4.0f);
  p.percussion.base_freq_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.base_freq_hz, 0.0f), 0.0f, 20000.0f);
  p.percussion.pitch_drop =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.pitch_drop, 0.0f), 0.0f, 8.0f);
  p.percussion.pitch_drop_ms =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.pitch_drop_ms, 40.0f), 1.0f, 2000.0f);
  p.percussion.strike_r =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.strike_r, 0.0f), 0.0f, 1.0f);
  p.percussion.strike_theta = patch_clamp_detail::sanitize(p.percussion.strike_theta, 0.0f);
  for (float& alpha : p.percussion.mode_alpha) {
    alpha = std::clamp(patch_clamp_detail::sanitize(alpha, 0.0f), 0.0f, 64.0f);
  }
  p.percussion.noise_gain =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.noise_gain, 0.0f), 0.0f, 4.0f);
  p.percussion.noise_decay_ms =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.noise_decay_ms, 150.0f), 1.0f, 20000.0f);
  p.percussion.noise_cutoff_hz = std::clamp(
      patch_clamp_detail::sanitize(p.percussion.noise_cutoff_hz, 2500.0f), 20.0f, 20000.0f);
  p.percussion.noise_q =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.noise_q, 1.0f), 0.5f, 30.0f);
  p.percussion.shell_mix =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.shell_mix, 0.0f), 0.0f, 1.0f);
  p.percussion.shell_num_modes = std::clamp(p.percussion.shell_num_modes, 0, kMaxShellModes);
  for (float& freq : p.percussion.shell_freq_hz) {
    freq = std::clamp(patch_clamp_detail::sanitize(freq, 0.0f), 0.0f, 20000.0f);
  }
  for (float& t60 : p.percussion.shell_t60_s) {
    t60 = std::clamp(patch_clamp_detail::sanitize(t60, 0.05f), 0.005f, 5.0f);
  }
  for (float& weight : p.percussion.shell_weight) {
    weight = std::clamp(patch_clamp_detail::sanitize(weight, 0.0f), 0.0f, 4.0f);
  }
  // 0 stays 0 — the unbounded voicing, not a cutoff pinned to the low end.
  // That makes the accepted interval two regions rather than one: off, and a
  // real ceiling. A sweep that treats [0, 20000] as continuous spends its low
  // end bounding a cymbal below its own corner, which is not a darker cymbal
  // but a quiet one — search from a musical floor, not from zero.
  p.percussion.noise_air_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.noise_air_hz, 0.0f), 0.0f, 20000.0f);
  p.percussion.wire_buzz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.wire_buzz, 0.0f), 0.0f, 4.0f);
  p.percussion.wire_threshold =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.wire_threshold, 0.1f), 0.0f, 4.0f);
  p.percussion.wire_cutoff_hz = std::clamp(
      patch_clamp_detail::sanitize(p.percussion.wire_cutoff_hz, 4000.0f), 20.0f, 20000.0f);
  p.percussion.shimmer =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.shimmer, 0.0f), 0.0f, 16.0f);
  p.percussion.shimmer_attack_ms = std::clamp(
      patch_clamp_detail::sanitize(p.percussion.shimmer_attack_ms, 40.0f), 1.0f, 2000.0f);
  p.percussion.shimmer_cutoff_hz = std::clamp(
      patch_clamp_detail::sanitize(p.percussion.shimmer_cutoff_hz, 8000.0f), 20.0f, 20000.0f);
  p.percussion.plate_gain =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_gain, 0.0f), 0.0f, 4.0f);
  // A cymbal rings for tens of seconds and a gong for longer, so the upper
  // bound is the note length rather than a drum's.
  p.percussion.plate_t60_s =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_t60_s, 2.0f), 0.01f, 30.0f);
  p.percussion.plate_hf_ratio =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_hf_ratio, 0.6f), 0.01f, 1.0f);
  // The floor is the lowest partial the delay lines can still place at 96 kHz,
  // so a patch reads the same at every sample rate the library renders at. Ask
  // for less and the network scales itself to fit, which is a smaller plate
  // than the patch called for rather than a clamped number.
  p.percussion.plate_low_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_low_hz, 180.0f), 140.0f, 4000.0f);
  // 0 stays 0 — unbounded, not a ceiling pinned to the low end — for the same
  // reason `noise_air_hz` does: the accepted interval is off plus a real
  // ceiling, and a sweep that reads it as continuous spends its low end
  // squeezing a plate below its own band.
  p.percussion.plate_air_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_air_hz, 0.0f), 0.0f, 20000.0f);
  p.percussion.phisem_beans =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_beans, 0.0f), 0.0f, 256.0f);
  p.percussion.phisem_energy_ms = std::clamp(
      patch_clamp_detail::sanitize(p.percussion.phisem_energy_ms, 100.0f), 1.0f, 20000.0f);
  p.percussion.phisem_sound_ms =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_sound_ms, 3.0f), 0.2f, 200.0f);
  p.percussion.phisem_res_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_res_hz, 0.0f), 0.0f, 20000.0f);
  p.percussion.phisem_res_q =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_res_q, 1.0f), 0.5f, 30.0f);
  p.percussion.phisem_body_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_body_hz, 0.0f), 0.0f, 20000.0f);
  p.percussion.phisem_body_q =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_body_q, 4.0f), 0.5f, 30.0f);
  p.percussion.phisem_body_gain =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_body_gain, 0.0f), 0.0f, 4.0f);
  p.percussion.phisem_scrape_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_scrape_hz, 0.0f), 0.0f, 20000.0f);
  p.percussion.phisem_pitch_glide =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_pitch_glide, 0.0f), -0.95f, 8.0f);
  p.piano.strings = std::clamp(p.piano.strings, 1, kMaxPianoStrings);
  p.piano.detune_cents =
      std::clamp(patch_clamp_detail::sanitize(p.piano.detune_cents, 1.6f), 0.0f, 50.0f);
  p.piano.decay_fast_s =
      std::clamp(patch_clamp_detail::sanitize(p.piano.decay_fast_s, 3.0f), 0.05f, 60.0f);
  p.piano.decay_slow_s =
      std::clamp(patch_clamp_detail::sanitize(p.piano.decay_slow_s, 12.0f), 0.05f, 120.0f);
  p.piano.decay_stretch =
      std::clamp(patch_clamp_detail::sanitize(p.piano.decay_stretch, 0.7f), 0.0f, 1.0f);
  p.piano.brightness =
      std::clamp(patch_clamp_detail::sanitize(p.piano.brightness, 0.75f), 0.0f, 1.0f);
  p.piano.dispersion =
      std::clamp(patch_clamp_detail::sanitize(p.piano.dispersion, 1.0f), 0.0f, 1.0f);
  p.piano.strike_position =
      std::clamp(patch_clamp_detail::sanitize(p.piano.strike_position, 0.12f), 0.0f, 0.5f);
  p.piano.hammer_exponent =
      std::clamp(patch_clamp_detail::sanitize(p.piano.hammer_exponent, 2.5f), 1.5f, 4.0f);
  p.piano.hammer_contact_ms =
      std::clamp(patch_clamp_detail::sanitize(p.piano.hammer_contact_ms, 1.2f), 0.2f, 10.0f);
  p.piano.hammer_dynamics =
      std::clamp(patch_clamp_detail::sanitize(p.piano.hammer_dynamics, 0.0f), 0.0f, 1.0f);
  p.piano.soundboard =
      std::clamp(patch_clamp_detail::sanitize(p.piano.soundboard, 0.25f), 0.0f, 1.0f);
  p.piano.release_damp_s =
      std::clamp(patch_clamp_detail::sanitize(p.piano.release_damp_s, 0.1f), 0.01f, 10.0f);
  p.pipe_organ.brightness =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.brightness, 0.5f), 0.0f, 1.0f);
  p.pipe_organ.tone_decay_s =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.tone_decay_s, 4.0f), 0.05f, 60.0f);
  p.pipe_organ.breath =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.breath, 0.35f), 0.0f, 1.0f);
  p.pipe_organ.chiff =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.chiff, 0.5f), 0.0f, 1.0f);
  p.pipe_organ.chiff_ms =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.chiff_ms, 18.0f), 0.5f, 500.0f);
  p.pipe_organ.release_damp_s =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.release_damp_s, 0.08f), 0.01f, 10.0f);
  p.pipe_organ.reed = std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.reed, 0.0f), 0.0f, 1.0f);
  p.pipe_organ.radiation =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.radiation, 0.0f), 0.0f, 1.0f);
  p.pipe_organ.rank_count = std::clamp(p.pipe_organ.rank_count, 0, kMaxPipeRanks);
  for (auto& rank : p.pipe_organ.ranks) {
    rank.footage_mult =
        std::clamp(patch_clamp_detail::sanitize(rank.footage_mult, 1.0f), 0.25f, 16.0f);
    rank.brightness = std::clamp(patch_clamp_detail::sanitize(rank.brightness, 0.5f), 0.0f, 1.0f);
    rank.level = std::clamp(patch_clamp_detail::sanitize(rank.level, 1.0f), 0.0f, 1.0f);
    rank.reed = std::clamp(patch_clamp_detail::sanitize(rank.reed, 0.0f), 0.0f, 1.0f);
    rank.radiation = std::clamp(patch_clamp_detail::sanitize(rank.radiation, 0.0f), 0.0f, 1.0f);
  }
  p.pipe_organ.tremulant_rate_hz =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.tremulant_rate_hz, 0.0f), 0.0f, 12.0f);
  p.pipe_organ.tremulant_depth =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.tremulant_depth, 0.0f), 0.0f, 1.0f);
  p.pipe_organ.wind_sag =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.wind_sag, 0.0f), 0.0f, 1.0f);
  p.pipe_organ.swell =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.swell, 0.0f), 0.0f, 1.0f);
  p.bowed_string.bow_position =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.bow_position, 0.13f), 0.02f, 0.5f);
  p.bowed_string.bow_force =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.bow_force, 0.5f), 0.0f, 1.0f);
  p.bowed_string.bow_speed =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.bow_speed, 0.5f), 0.0f, 1.0f);
  p.bowed_string.vel_to_speed =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.vel_to_speed, 0.6f), 0.0f, 1.0f);
  p.bowed_string.brightness =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.brightness, 0.5f), 0.0f, 1.0f);
  p.bowed_string.damping =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.damping, 0.4f), 0.0f, 1.0f);
  p.bowed_string.attack_ms =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.attack_ms, 60.0f), 1.0f, 2000.0f);
  p.bowed_string.release_ms =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.release_ms, 120.0f), 1.0f, 5000.0f);
  p.bowed_string.rosin =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.rosin, 0.0f), 0.0f, 1.0f);
  p.reed.breath_pressure =
      std::clamp(patch_clamp_detail::sanitize(p.reed.breath_pressure, 0.6f), 0.0f, 1.0f);
  p.reed.vel_to_breath =
      std::clamp(patch_clamp_detail::sanitize(p.reed.vel_to_breath, 0.6f), 0.0f, 1.0f);
  p.reed.reed_stiffness =
      std::clamp(patch_clamp_detail::sanitize(p.reed.reed_stiffness, 0.5f), 0.0f, 1.0f);
  p.reed.reed_opening =
      std::clamp(patch_clamp_detail::sanitize(p.reed.reed_opening, 0.5f), 0.0f, 1.0f);
  p.reed.brightness = std::clamp(patch_clamp_detail::sanitize(p.reed.brightness, 0.5f), 0.0f, 1.0f);
  p.reed.damping = std::clamp(patch_clamp_detail::sanitize(p.reed.damping, 0.4f), 0.0f, 1.0f);
  p.reed.attack_ms =
      std::clamp(patch_clamp_detail::sanitize(p.reed.attack_ms, 40.0f), 1.0f, 2000.0f);
  p.reed.release_ms =
      std::clamp(patch_clamp_detail::sanitize(p.reed.release_ms, 80.0f), 1.0f, 5000.0f);
  p.reed.breath_noise =
      std::clamp(patch_clamp_detail::sanitize(p.reed.breath_noise, 0.12f), 0.0f, 1.0f);
  p.reed.chiff = std::clamp(patch_clamp_detail::sanitize(p.reed.chiff, 0.4f), 0.0f, 1.0f);
  p.reed.chiff_ms = std::clamp(patch_clamp_detail::sanitize(p.reed.chiff_ms, 12.0f), 1.0f, 500.0f);
  p.reed.reed_resonance =
      std::clamp(patch_clamp_detail::sanitize(p.reed.reed_resonance, 0.5f), 0.0f, 1.0f);
  p.reed.register_vent =
      std::clamp(patch_clamp_detail::sanitize(p.reed.register_vent, 0.0f), 0.0f, 1.0f);
  p.reed.growl = std::clamp(patch_clamp_detail::sanitize(p.reed.growl, 0.0f), 0.0f, 1.0f);
  p.reed.cone_growth =
      std::clamp(patch_clamp_detail::sanitize(p.reed.cone_growth, 0.0f), 0.0f, 1.0f);
  p.reed.tonehole = std::clamp(patch_clamp_detail::sanitize(p.reed.tonehole, 0.0f), 0.0f, 1.0f);
  p.brass.breath_pressure =
      std::clamp(patch_clamp_detail::sanitize(p.brass.breath_pressure, 0.7f), 0.0f, 1.0f);
  p.brass.vel_to_breath =
      std::clamp(patch_clamp_detail::sanitize(p.brass.vel_to_breath, 0.6f), 0.0f, 1.0f);
  p.brass.lip_tension =
      std::clamp(patch_clamp_detail::sanitize(p.brass.lip_tension, 0.5f), 0.0f, 1.0f);
  p.brass.lip_damping =
      std::clamp(patch_clamp_detail::sanitize(p.brass.lip_damping, 0.5f), 0.0f, 1.0f);
  p.brass.brightness =
      std::clamp(patch_clamp_detail::sanitize(p.brass.brightness, 0.5f), 0.0f, 1.0f);
  p.brass.damping = std::clamp(patch_clamp_detail::sanitize(p.brass.damping, 0.4f), 0.0f, 1.0f);
  p.brass.attack_ms =
      std::clamp(patch_clamp_detail::sanitize(p.brass.attack_ms, 25.0f), 1.0f, 2000.0f);
  p.brass.release_ms =
      std::clamp(patch_clamp_detail::sanitize(p.brass.release_ms, 90.0f), 1.0f, 5000.0f);
  p.brass.breath_noise =
      std::clamp(patch_clamp_detail::sanitize(p.brass.breath_noise, 0.1f), 0.0f, 1.0f);
  p.brass.chiff = std::clamp(patch_clamp_detail::sanitize(p.brass.chiff, 0.35f), 0.0f, 1.0f);
  p.brass.chiff_ms =
      std::clamp(patch_clamp_detail::sanitize(p.brass.chiff_ms, 10.0f), 1.0f, 500.0f);
  p.brass.brassiness =
      std::clamp(patch_clamp_detail::sanitize(p.brass.brassiness, 0.0f), 0.0f, 1.0f);
  p.brass.cuivre_dynamics =
      std::clamp(patch_clamp_detail::sanitize(p.brass.cuivre_dynamics, 0.0f), 0.0f, 1.0f);
  p.brass.mute = std::clamp(patch_clamp_detail::sanitize(p.brass.mute, 0.0f), 0.0f, 1.0f);
  p.brass.half_valve =
      std::clamp(patch_clamp_detail::sanitize(p.brass.half_valve, 0.0f), 0.0f, 1.0f);
  p.brass.dynamic_lip =
      std::clamp(patch_clamp_detail::sanitize(p.brass.dynamic_lip, 0.0f), 0.0f, 1.0f);
  p.flute.breath_pressure =
      std::clamp(patch_clamp_detail::sanitize(p.flute.breath_pressure, 0.55f), 0.0f, 1.0f);
  p.flute.vel_to_breath =
      std::clamp(patch_clamp_detail::sanitize(p.flute.vel_to_breath, 0.5f), 0.0f, 1.0f);
  p.flute.jet_ratio = std::clamp(patch_clamp_detail::sanitize(p.flute.jet_ratio, 0.5f), 0.1f, 0.9f);
  p.flute.jet_reflection =
      std::clamp(patch_clamp_detail::sanitize(p.flute.jet_reflection, 0.5f), 0.0f, 1.0f);
  p.flute.end_reflection =
      std::clamp(patch_clamp_detail::sanitize(p.flute.end_reflection, 0.5f), 0.0f, 1.0f);
  p.flute.brightness =
      std::clamp(patch_clamp_detail::sanitize(p.flute.brightness, 0.5f), 0.0f, 1.0f);
  p.flute.damping = std::clamp(patch_clamp_detail::sanitize(p.flute.damping, 0.35f), 0.0f, 1.0f);
  p.flute.attack_ms =
      std::clamp(patch_clamp_detail::sanitize(p.flute.attack_ms, 18.0f), 1.0f, 2000.0f);
  p.flute.release_ms =
      std::clamp(patch_clamp_detail::sanitize(p.flute.release_ms, 90.0f), 1.0f, 5000.0f);
  p.flute.breath_noise =
      std::clamp(patch_clamp_detail::sanitize(p.flute.breath_noise, 0.15f), 0.0f, 1.0f);
  p.flute.chiff = std::clamp(patch_clamp_detail::sanitize(p.flute.chiff, 0.4f), 0.0f, 1.0f);
  p.flute.chiff_ms =
      std::clamp(patch_clamp_detail::sanitize(p.flute.chiff_ms, 12.0f), 1.0f, 500.0f);
  p.flute.vibrato_rate_hz =
      std::clamp(patch_clamp_detail::sanitize(p.flute.vibrato_rate_hz, 5.0f), 0.1f, 12.0f);
  p.flute.vibrato_depth =
      std::clamp(patch_clamp_detail::sanitize(p.flute.vibrato_depth, 0.0f), 0.0f, 1.0f);
  p.flute.overblow = std::clamp(patch_clamp_detail::sanitize(p.flute.overblow, 0.0f), 0.0f, 1.0f);
  p.flute.jet_turbulence =
      std::clamp(patch_clamp_detail::sanitize(p.flute.jet_turbulence, 0.0f), 0.0f, 1.0f);
  p.flute.edge_hysteresis =
      std::clamp(patch_clamp_detail::sanitize(p.flute.edge_hysteresis, 0.0f), 0.0f, 1.0f);
  p.flute.vortex = std::clamp(patch_clamp_detail::sanitize(p.flute.vortex, 0.0f), 0.0f, 1.0f);
  p.plucked_string.brightness =
      std::clamp(patch_clamp_detail::sanitize(p.plucked_string.brightness, 0.7f), 0.0f, 1.0f);
  p.plucked_string.decay_s =
      std::clamp(patch_clamp_detail::sanitize(p.plucked_string.decay_s, 4.0f), 0.05f, 60.0f);
  p.plucked_string.decay_stretch =
      std::clamp(patch_clamp_detail::sanitize(p.plucked_string.decay_stretch, 0.5f), 0.0f, 1.0f);
  p.plucked_string.pick_position =
      std::clamp(patch_clamp_detail::sanitize(p.plucked_string.pick_position, 0.2f), 0.0f, 0.5f);
  p.plucked_string.exc_brightness =
      std::clamp(patch_clamp_detail::sanitize(p.plucked_string.exc_brightness, 0.85f), 0.0f, 1.0f);
  p.plucked_string.vel_to_brightness = std::clamp(
      patch_clamp_detail::sanitize(p.plucked_string.vel_to_brightness, 0.6f), 0.0f, 1.0f);
  p.plucked_string.release_damp_s = std::clamp(
      patch_clamp_detail::sanitize(p.plucked_string.release_damp_s, 0.12f), 0.01f, 10.0f);
  p.plucked_string.buzz =
      std::clamp(patch_clamp_detail::sanitize(p.plucked_string.buzz, 0.0f), 0.0f, 1.0f);
  p.vocal.vowel = std::clamp(p.vocal.vowel, 0, kVocalVowels - 1);
  p.vocal.brightness =
      std::clamp(patch_clamp_detail::sanitize(p.vocal.brightness, 0.5f), 0.0f, 1.0f);
  p.vocal.breath_noise =
      std::clamp(patch_clamp_detail::sanitize(p.vocal.breath_noise, 0.1f), 0.0f, 1.0f);
  p.vocal.vibrato_rate_hz =
      std::clamp(patch_clamp_detail::sanitize(p.vocal.vibrato_rate_hz, 5.5f), 0.1f, 12.0f);
  p.vocal.vibrato_depth =
      std::clamp(patch_clamp_detail::sanitize(p.vocal.vibrato_depth, 0.3f), 0.0f, 1.0f);
  p.vocal.attack_ms =
      std::clamp(patch_clamp_detail::sanitize(p.vocal.attack_ms, 30.0f), 1.0f, 2000.0f);
  p.vocal.release_ms =
      std::clamp(patch_clamp_detail::sanitize(p.vocal.release_ms, 120.0f), 1.0f, 5000.0f);
  p.free_reed.brightness =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.brightness, 0.6f), 0.0f, 1.0f);
  p.free_reed.reed_stiffness =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.reed_stiffness, 0.5f), 0.0f, 1.0f);
  p.free_reed.breath_pressure =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.breath_pressure, 0.7f), 0.0f, 1.0f);
  p.free_reed.vel_to_breath =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.vel_to_breath, 0.5f), 0.0f, 1.0f);
  p.free_reed.detune =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.detune, 0.3f), 0.0f, 1.0f);
  p.free_reed.attack_ms =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.attack_ms, 20.0f), 1.0f, 2000.0f);
  p.free_reed.release_ms =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.release_ms, 80.0f), 1.0f, 5000.0f);
  p.free_reed.breath_noise =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.breath_noise, 0.08f), 0.0f, 1.0f);
  p.harpsichord.pluck_8a =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.pluck_8a, 0.14f), 0.0f, 0.5f);
  p.harpsichord.pluck_8b =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.pluck_8b, 0.22f), 0.0f, 0.5f);
  p.harpsichord.pluck_4 =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.pluck_4, 0.11f), 0.0f, 0.5f);
  p.harpsichord.plectrum_edge =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.plectrum_edge, 0.8f), 0.0f, 1.0f);
  // The instrument's own range is 3 to 6 dB; the ceiling leaves room to voice a
  // stop that is deliberately more responsive without letting a patch turn the
  // harpsichord into a piano.
  p.harpsichord.velocity_range_db =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.velocity_range_db, 5.0f), 0.0f, 24.0f);
  p.harpsichord.velocity_droop_db =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.velocity_droop_db, 0.0f), 0.0f, 12.0f);
  p.harpsichord.decay_s =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.decay_s, 3.0f), 0.05f, 60.0f);
  p.harpsichord.decay_stretch =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.decay_stretch, 0.7f), 0.0f, 2.0f);
  p.harpsichord.hf_damping =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.hf_damping, 0.45f), 0.05f, 1.0f);
  p.harpsichord.damping_ref_hz = std::clamp(
      patch_clamp_detail::sanitize(p.harpsichord.damping_ref_hz, 2000.0f), 100.0f, 20000.0f);
  p.harpsichord.unison_detune_cents = std::clamp(
      patch_clamp_detail::sanitize(p.harpsichord.unison_detune_cents, 3.5f), -50.0f, 50.0f);
  p.harpsichord.octave_detune_cents = std::clamp(
      patch_clamp_detail::sanitize(p.harpsichord.octave_detune_cents, 2.0f), -50.0f, 50.0f);
  p.harpsichord.rear_segment_mm =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.rear_segment_mm, 0.0f), 0.0f, 400.0f);
  p.harpsichord.rear_coupling =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.rear_coupling, 0.35f), 0.0f, 1.0f);
  p.harpsichord.scale_c5_mm =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.scale_c5_mm, 280.0f), 80.0f, 800.0f);
  p.harpsichord.bass_foreshortening = std::clamp(
      patch_clamp_detail::sanitize(p.harpsichord.bass_foreshortening, 0.35f), 0.0f, 0.9f);
  p.harpsichord.pluck_noise =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.pluck_noise, 0.0f), 0.0f, 1.0f);
  p.harpsichord.jack_noise =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.jack_noise, 0.0f), 0.0f, 1.0f);
  p.harpsichord.damper_s =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.damper_s, 0.09f), 0.005f, 10.0f);
  if (static_cast<int>(p.body) < 0 || static_cast<int>(p.body) > 4) p.body = BodyType::kNone;
  p.body_mix = std::clamp(patch_clamp_detail::sanitize(p.body_mix, 0.0f), 0.0f, 1.0f);
  p.stereo_spread = std::clamp(patch_clamp_detail::sanitize(p.stereo_spread, 0.0f), 0.0f, 1.0f);
  return p;
}

/// MIDI note -> equal-tempered frequency (A4 = 440 Hz).
float synth_note_to_hz(float note) noexcept;

}  // namespace sonare::midi::synth
