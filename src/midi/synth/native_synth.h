#pragma once

/// @file native_synth.h
/// @brief NativeSynth — the patch-driven synthesis engine: a patch POD tagged
///        with a SynthEngineMode (declared below, one enumerator per engine),
///        the unison subtractive voice
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
/// start phases, drift-LFO variation and every engine's noise stream derive
/// from the counter-based (voice_index, note, age) hash — or from the note
/// alone under SynthRetrigger::kNote — so identical event streams bounce
/// bit-identically within one build.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "midi/channel_voice_decode.h"
#include "midi/control_value.h"
#include "midi/controller_profile.h"
#include "midi/instrument.h"
#include "midi/mpe.h"
#include "midi/per_note_state.h"
#include "midi/source_residual.h"
#include "midi/synth/additive_voice.h"
#include "midi/synth/body_resonator.h"
#include "midi/synth/bowed_string_voice.h"
#include "midi/synth/brass_voice.h"
#include "midi/synth/channel_param_state.h"
#include "midi/synth/envelope.h"
#include "midi/synth/excitation_axes.h"
#include "midi/synth/filter_models.h"
#include "midi/synth/flute_voice.h"
#include "midi/synth/fm_voice.h"
#include "midi/synth/free_reed_voice.h"
#include "midi/synth/harpsichord_voice.h"
#include "midi/synth/ks_voice.h"
#include "midi/synth/mod_matrix.h"
#include "midi/synth/modal_voice.h"
#include "midi/synth/oscillator.h"
#include "midi/synth/part_fx_stage.h"
#include "midi/synth/percussion_voice.h"
#include "midi/synth/piano_voice.h"
#include "midi/synth/pipe_organ_voice.h"
#include "midi/synth/plucked_string_voice.h"
#include "midi/synth/reed_voice.h"
#include "midi/synth/render_path_record.h"
#include "midi/synth/sample_bank.h"
#include "midi/synth/sample_voice.h"
#include "midi/synth/sf2_voice.h"
#include "midi/synth/vocal_voice.h"
#include "midi/synth/voice_pool.h"
#include "util/constants.h"
#include "util/numeric_validation.h"

namespace sonare::midi::synth {

/// Maximum unison oscillators per voice (supersaw width).
inline constexpr int kMaxUnisonOscs = 7;

/// What a note-on restarts. kFree seeds each voice's start phases and random
/// streams from its pool slot and allocation count, so repeated notes differ;
/// kNote seeds them from the note alone, so a repeated note renders the same
/// samples wherever it lands (patch field; the enum is mode-tagged for the ABI).
enum class SynthRetrigger : int {
  kFree = 0,
  kNote = 1,
};

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
  /// Carried in the render's per-sample pitch sum, like every other constant
  /// pitch term here, so it reaches every engine and an automation lane moving
  /// it reaches voices that are already sounding.
  float pitch_offset_cents = 0.0f;

  // --- amplitude ---
  /// Per-voice linear gain (the instrument master gain is separate).
  float gain = 0.5f;
  DahdsrConfig amp_env;
  /// One-shot (drum) voices ignore note-off and end at the envelope's
  /// zero-sustain decay floor, or where it holds, by their engine
  /// (sustained_one_shot_tail_samples).
  bool one_shot = false;

  // --- filter section ---
  /// Filter model (the "character" core; see filter_models.h).
  SynthFilterModel filter_model = SynthFilterModel::kSvf;
  SynthFilterOutput filter_output = SynthFilterOutput::kLowpass;
  float cutoff_hz = 12000.0f;
  /// Resonance Q. The ladder / Sallen-Key models map it to their normalized
  /// feedback; Q >= kSelfOscQ reaches self-oscillation on those models.
  float resonance_q = 0.707f;
  /// Series 12 dB/oct highpass AFTER the main filter (Hz); 0 disables the
  /// stage. This is the second half of a lowpass-plus-highpass pair, which is
  /// how a sampled instrument is narrowed to a band the main filter alone
  /// cannot reach. It runs flat at Butterworth Q -- @c resonance_q belongs to
  /// the main filter, and a resonant peak at each end of a band is not what
  /// the pairing is for.
  float hp_cutoff_hz = 0.0f;
  /// Pre-filter drive in [0,1]: gain-compensated tanh saturation on the
  /// oscillator mix (0 = clean).
  float drive = 0.0f;

  // --- converter (the voice's own output stage) ---
  /// Rate the voice's output is held at (Hz); 0 = off, and the voice is then
  /// bit-identical to one without the stage. A drum machine plays its samples
  /// through a converter running far below the mix rate, and the aliased images
  /// that folds down are as much of its sound as the samples are. Per voice
  /// rather than per bus because a machine converts only the voices it stores
  /// and leaves its analogue ones alone — crushing the bus takes the kick with
  /// it.
  float sample_hold_hz = 0.0f;
  /// Word length the held value is quantized to (bits); 0 = off. Fractional
  /// values are meaningful — the quantizer is uniform over 2^bits steps and a
  /// converter's effective resolution is rarely a whole number of them.
  float bit_depth = 0.0f;
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
  /// Body/formant resonance voicing applied to the voice output (parallel
  /// resonator bank, body_resonator.h) and its mix in [0,1].
  BodyType body = BodyType::kNone;
  float body_mix = 0.0f;
  /// Per-voice seeded stereo pan scatter in [0,1] (0 keeps every voice
  /// centre-relative, preserving bit-stable mono bounces).
  float stereo_spread = 0.0f;
  /// Seed source for every per-voice start state (see SynthRetrigger).
  SynthRetrigger retrigger = SynthRetrigger::kFree;

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

  /// Keymap into the host's sample bank (used when mode == kSample).
  SamplePatchParams sample;
};

namespace native_synth_detail {

/// The engine excitation slot @p axis fills, or kAxisNone for a channel-level axis.
inline uint32_t excitation_axis_mask(ControllerAxis axis) noexcept {
  switch (axis) {
    case ControllerAxis::kExcitation:
      return kAxisForce;
    case ControllerAxis::kPosition:
      return kAxisPosition;
    case ControllerAxis::kBrightness:
      return kAxisBrightness;
    case ControllerAxis::kMorph:
      return kAxisMorph;
    case ControllerAxis::kNone:
    case ControllerAxis::kLoudness:
    case ControllerAxis::kPitchCents:
    case ControllerAxis::kVibratoDepth:
      return kAxisNone;
  }
  return kAxisNone;
}

/// The value @p axes holds in @p axis's slot; 0 for a channel-level axis.
inline float excitation_value(const ExcitationAxes& axes, ControllerAxis axis) noexcept {
  switch (axis) {
    case ControllerAxis::kExcitation:
      return axes.force;
    case ControllerAxis::kPosition:
      return axes.position;
    case ControllerAxis::kBrightness:
      return axes.brightness;
    case ControllerAxis::kMorph:
      return axes.morph;
    case ControllerAxis::kNone:
    case ControllerAxis::kLoudness:
    case ControllerAxis::kPitchCents:
    case ControllerAxis::kVibratoDepth:
      return 0.0f;
  }
  return 0.0f;
}

}  // namespace native_synth_detail

/// How long a voice of @p patch can sound after its last event, in samples. A
/// zero-sustain envelope is ended by its decay, and a one-shot by its envelope;
/// a one-shot whose envelope holds is ended by its engine
/// (sustained_one_shot_tail_samples). INT64_MAX when nothing ends the voice.
int64_t native_patch_tail_samples(const NativeSynthPatch& patch, double sample_rate,
                                  const EnvelopeTimeScales& scales,
                                  const SampleBank* bank) noexcept;

/// What ends a one-shot whose amplitude envelope holds, which no note-off can:
/// FM carriers finishing, a percussion piece falling silent, an unlooped sample
/// running out. INT64_MAX for every other engine, which nothing ends.
int64_t sustained_one_shot_tail_samples(const NativeSynthPatch& patch, double sample_rate,
                                        const SampleBank* bank) noexcept;

/// Per-note GS drum overrides applied to a fallback percussion voice at
/// note-on (NRPN pitch coarse / TVA level / absolute pan). Defaults are no-ops,
/// so a voice with no per-note edit renders exactly as before.
struct DrumVoiceMod {
  float pitch_ratio = 1.0f;  ///< 2^(pitch_coarse / 12)
  float level_gain = 1.0f;   ///< (level / 127)^2 (same square law as CC7)
  float pan_units = 1.0e9f;  ///< absolute pan units (1e9 = untouched: keep channel pan)
  /// Send multiplicands over [0,1] (41 m5/m6/m9 rr): each scales this note's
  /// whole contribution to that unit's bus.
  float reverb_scale = 1.0f;
  float chorus_scale = 1.0f;
  float delay_scale = 1.0f;
  /// The note this strike SOUNDS, which PLAY NOTE NUMBER (41 m1 rr) and a user
  /// drum set's SOURCE NOTE NUMBER (21 dC rr) both move. -1 = the struck one.
  /// Carried here rather than substituted into the voice's own note, which stays
  /// the struck one because that is what a note-off matches.
  int16_t play_note = -1;
  /// ASSIGN GROUP (41 m3 rr): the exclusive group this strike joins, 0 = none.
  /// -1 = the kit piece's own, which is what a host with no GS layer means.
  int16_t exclusive_class = -1;
};

/// Crossfade length for a live change that crosses a stage's off point (drive,
/// body), where the law itself is discontinuous at 0.
inline constexpr float kLawFadeSeconds = 0.005f;

/// Progress of a crossfade from a retiring law to the current one; step 0 means
/// no fade is running and only the current law is computed.
struct LawFade {
  float fade01 = 1.0f;
  float step = 0.0f;
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
  /// The exclusive/mute group this voice was STARTED in, which a GS ASSIGN
  /// GROUP write moves away from the kit piece's own. Held per voice because
  /// one kit piece is shared by every note that resolves to it.
  uint8_t exclusive_class = 0;
  /// Static cutoff offset precomputed at start (velocity + key tracking).
  float static_cutoff_cents = 0.0f;
  /// Pre-filter drive gain / makeup (precomputed from patch->drive; 0 = off).
  float drive_gain = 0.0f;
  float drive_makeup = 1.0f;
  /// The drive law being faded out after a change across 0 (gain 0 = bypass).
  float drive_fade_gain = 0.0f;
  float drive_fade_makeup = 1.0f;
  LawFade drive_fade;
  DahdsrEnvelope amp_env;
  DahdsrEnvelope filter_env;
  SynthFilter filter;
  /// Series highpass stage (patch hp_cutoff_hz); idle when the patch asks for
  /// no highpass. A bare SVF rather than a second SynthFilter: this stage is
  /// always the SVF highpass tap, so the other three models would be dead
  /// state on every voice.
  TptSvf hp_stage;
  /// Whether this voice runs that stage. Decided at note-on and changed only by
  /// refresh_live(), which prepares the stage when it opens and retunes it in
  /// place while it runs; render() never reads the patch cutoff directly, which
  /// would open the stage on a voice that never prepared or set it.
  bool hp_active = false;
  // Converter stage. `hold_step` is how much of a held period one sample
  // advances, so the rate is compared against the mix rate once at note-on
  // rather than per sample.
  float hold_step = 0.0f;
  float hold_phase = 0.0f;
  float hold_value = 0.0f;
  float quant_step = 0.0f;
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
  /// A factory-backed NativeSynth routes piano bodies through a per-part bank.
  /// These are captured after start() and consumed by the host on the first
  /// render sample, once the published routing snapshot is known. Deferring
  /// the strike avoids exciting the shared no-EFX board for a note whose EFX
  /// assignment was still dirty at note-on.
  float piano_case_strike_pending = 0.0f;
  float piano_board_strike_pending = 0.0f;
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
  /// Host PCM source; like KS, the host attach()es the bank before start().
  SampleVoiceCore sampler;
  BodyResonator body;
  /// Body crossfade after a bodyMix change across 0; body_fade_in names the
  /// direction (true = dry to body). A finished fade-out stops the bank.
  LawFade body_fade;
  bool body_fade_in = false;
  Sf2Lfo vibrato_lfo;
  Sf2Lfo lfo2;
  Sf2Lfo drift_lfo;
  float drift_depth_cents = 0.0f;
  // Mod-matrix source constants (precomputed at start).
  bool has_matrix = false;
  /// At least one live route lands on an axis the engine owns rather than the
  /// wrapper, so its control setters are worth calling each sample.
  bool has_engine_control_routes = false;
  float velocity01 = 0.0f;
  /// Polyphonic aftertouch for this note, added to the channel's own before it
  /// reaches the matrix. Per voice because that is the granularity the message
  /// carries; cleared at note-on so a voice never inherits the last one's.
  float poly_pressure01 = 0.0f;
  float key_track_octaves = 0.0f;
  float random_value = 0.0f;
  /// Previous sample's ModDestination::kLfo1RateScale, applied to LFO1 on the
  /// next one. LFO1 feeds the matrix that sets this, so the delay is what keeps
  /// the routing acyclic.
  float matrix_lfo1_rate_scale = 1.0f;
  // Glide: pitch offset in cents decaying to zero through a one-pole.
  float glide_cents = 0.0f;
  float glide_coeff = 0.0f;
  /// Interval in cents from the note this voice was STARTED on to the note it
  /// is sounding now, which a legato continuation moves and nothing else does.
  /// It joins the per-sample pitch sum rather than base_freq_hz because that is
  /// the one argument every engine's render() already takes: a waveguide reads
  /// its line at the new period, an oscillator retunes, and no engine needs a
  /// second entry point to be carried into a new note. Permanent, unlike
  /// glide_cents, which only carries the transition into it.
  float retune_cents = 0.0f;
  /// Seeded per-voice pan scatter (patch stereo_spread; pan units).
  float pan_spread_units = 0.0f;
  /// The channel mod this voice reads instead of its channel's, and whether it
  /// has one. Set only on a member channel carrying more than one sounding
  /// note, where the channel's bend and pressure belong to the note the
  /// tracking rule names and the others keep the manager's contribution alone.
  /// MPE's normal form is one note per member channel, so this is inert
  /// wherever a sender is doing what the specification expects.
  bool mpe_mod_active = false;
  Sf2ChannelMod mpe_mod{};
  /// Once a member key has gone up, its own MPE pressure/timbre/bend stay at
  /// their NoteOff values while ordinary channel controls and manager bend can
  /// continue to update the sounding tail.
  bool mpe_release_frozen = false;
  uint32_t mpe_frozen_axis_mask = kAxisNone;
  uint32_t mpe_frozen_axis_present = kAxisNone;
  uint32_t mpe_frozen_source_mask = kAxisNone;
  std::array<float, kControllerAxisCount> mpe_frozen_axis_values{};
  float mpe_frozen_member_bend_cents = 0.0f;
  float mpe_frozen_aftertouch01 = 0.0f;
  /// Last engine-axis values written to this voice, including per-note
  /// profile bindings. The source masks are mutually exclusive per axis;
  /// they let a manager reset remove only its own folded contribution while
  /// leaving a member or ordinary per-note writer in place.
  ControllerAxisState live_excitation_axes;
  uint32_t live_mpe_pressure_axes = kAxisNone;
  uint32_t live_mpe_timbre_axes = kAxisNone;
  uint32_t live_mpe_bend_axes = kAxisNone;
  /// The member's own MPE values at the time this voice last received them.
  /// Manager pressure/timbre is combined with these raw values for each voice
  /// separately, so a manager update cannot collapse several tracked notes
  /// into the current channel attribution.
  Control32 mpe_member_pressure = Control32::from_raw(0);
  Control32 mpe_member_timbre = Control32::from_raw(0);
  bool mpe_member_pressure_present = false;
  bool mpe_member_timbre_present = false;
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
  /// GS per-note drum send multiplicands; the mixer scales the channel's send
  /// into each unit by them.
  float drum_reverb_scale = 1.0f;
  float drum_chorus_scale = 1.0f;
  float drum_delay_scale = 1.0f;
  /// GS melodic part edits that the render reads rather than start() folding
  /// away: the patch fields they modify are automation targets, so latching a
  /// finished value here would freeze a sweep on a held note.
  float gs_resonance_gain = 1.0f;
  float gs_vib_depth_cents = 0.0f;
  /// GS SCALE TUNING for the key that started this voice, in cents (0 = in
  /// tune). Constant for the voice's life, and in the render's pitch sum rather
  /// than in base_freq_hz so it reaches every engine.
  float gs_scale_cents = 0.0f;
  /// Set by a cutoff or resonance edit; engages the filter stage the way the
  /// SoundFont bank's filter_bypass = false does.
  bool gs_filter_edited = false;

  // Note-on inputs refresh_live() re-derives patch-dependent state from, so a
  // refreshed voice matches one started at the new value.
  float note_offset_semitones = 0.0f;  ///< voiced note - 60
  uint8_t base_voiced_note = 60;       ///< voiced note base_freq_hz was computed for
  float part_cutoff_cents = 0.0f;
  float part_attack_scale = 1.0f;
  float part_decay_scale = 1.0f;
  float part_release_scale = 1.0f;
  float part_vib_rate_scale = 1.0f;
  std::array<float, kMaxUnisonOscs> unison_spread{};
  float drift_seed = 0.0f;
  float pan_scatter = 0.0f;
  float sampler_pan_units = 0.0f;
  /// Set by choke(): the voice is being cut and takes no live refresh.
  bool choked = false;
  /// Amp level latched at note-off while the undamped 4' tail sounds; the tail
  /// holds it past the shared envelope's idle. Choke paths fade it with the
  /// envelope's release law.
  bool harpsichord_tail_latched = false;
  float harpsichord_tail_level = 0.0f;
  float harpsichord_tail_choke_multiplier = 0.0f;

  /// Starts the voice for @p p. note/channel/age must already be set (the
  /// pool fills them in allocate()); @p voice_index seeds the deterministic
  /// per-voice variation unless the patch retriggers from the note. @p p must outlive the voice. @p
  /// glide_from_hz != 0 glides the pitch from that frequency (portamento; needs p.glide_ms > 0).
  /// @p una_corda engages the soft-pedal voicing (piano mode only).
  /// @p drum_kit != 0 applies a GS kit variation (Room/Power/808/...) to the
  /// resolved drum patch at note-on (percussion mode only; see apply_gs_drum_kit).
  /// @p drum_mod carries GS per-note drum NRPN edits (pitch coarse / level /
  /// pan / the three send multiplicands); default is a no-op. @p organ_percussion fires the drawbar
  /// organ's single-shot on this note (additive mode only; the channel decides which note gets it).
  /// @p part_mod carries the GS melodic part edits (TONE MODIFY / the part NRPNs); default is a
  /// no-op.
  void start(const NativeSynthPatch& p, double sample_rate, Velocity16 velocity,
             uint32_t voice_index, float glide_from_hz = 0.0f, bool una_corda = false,
             uint8_t drum_kit = 0, DrumVoiceMod drum_mod = {}, bool organ_percussion = false,
             GsPartMod part_mod = {}) noexcept;
  /// True when the patch's filter stage cannot colour this voice: a wide-open
  /// static SVF lowpass, no resonance, no envelope depth and no negative
  /// static offset. Read from the LIVE patch on every sample rather than
  /// latched at start(): `cutoff_hz` and `resonance_q` are automation targets
  /// that apply to already-sounding voices, so a sweep on a held note has to
  /// engage the stage on the block it starts, not on the next note-on.
  bool filter_inaudible() const noexcept {
    return !gs_filter_edited && patch->filter_model == SynthFilterModel::kSvf &&
           patch->filter_output == SynthFilterOutput::kLowpass && patch->cutoff_hz >= 18000.0f &&
           patch->env_to_cutoff_cents == 0.0f && static_cutoff_cents >= 0.0f &&
           patch->resonance_q <= 0.71f;
  }
  /// Hands the engine its excitation-axis bases and jumps the smoothing to
  /// them, so a note struck mid-phrase starts at the host's live controller
  /// positions rather than gliding in from the preset. @p present names the
  /// axes the caller filled; an axis it omits keeps the patch's own value.
  /// A mode that owns no axis is a no-op.
  void seed_excitation(const ExcitationAxes& base, uint32_t present) noexcept;
  /// Pushes live excitation-axis bases to a sounding voice (a CC arriving
  /// mid-note), without the snap: the engine's own ramp owns the approach.
  void push_excitation(const ExcitationAxes& base, uint32_t present) noexcept;
  /// Restores each engine's start() excitation base (before any controller seeded it) without
  /// restarting its phase, envelope, or live matrix offset.
  void restore_excitation_base() noexcept;
  /// Renders one mono sample. Deactivates when the amp envelope ends.
  /// @p wind_pitch / @p wind_gain carry the shared organ wind modulation
  /// (tremulant / wind sag); 1.0 leaves the voice unmodulated.
  float render(const Sf2ChannelMod& mod, float wind_pitch = 1.0f, float wind_gain = 1.0f) noexcept;
  /// Legato continuation: carry this sounding voice to @p new_note, whose
  /// tracking inputs (filter and body) are those of a fresh voice on
  /// @p voiced_note.
  ///
  /// The exciter, the delay line and both envelopes are left exactly as they
  /// are; only the pitch moves, through retune_cents, with glide_cents taking
  /// up the difference so the transition is continuous rather than a step. A
  /// patch with no portamento lands on the new pitch immediately, which is what
  /// a slur with no glide is. `note` becomes the new key, because that is what
  /// the late note-off of the old key must NOT match (holding the old key here
  /// is what would cut the slur). @p per_note_shift_cents is the change of the
  /// per-note pitch correction applied beside retune_cents, which the glide
  /// origin has to account for to stay on the sounding pitch.
  void retune(uint8_t new_note, uint8_t voiced_note, float per_note_shift_cents,
              double sample_rate) noexcept;
  /// Frequency of the voiced key, which is what a note-tracked body follows.
  float voiced_freq_hz() const noexcept {
    return base_freq_hz * std::exp2(voiced_shift_cents() * (1.0f / 1200.0f));
  }
  /// Cents between the key base_freq_hz was computed for and the voiced key now.
  float voiced_shift_cents() const noexcept {
    return 100.0f * (note_offset_semitones + 60.0f - static_cast<float>(base_voiced_note));
  }
  /// Re-derives the state start() latched from @p p for the parameters in
  /// @p mask (bit = NativeSynthParamId), from the same derivations and the
  /// note-on inputs saved above. Running state (phases, levels) is kept.
  /// Audio thread; no allocation.
  void refresh_live(const NativeSynthPatch& p, double sample_rate, uint32_t mask) noexcept;
  /// Note-off: enter release (ignored by one-shot patches).
  void release() noexcept;
  /// Immediate silence (All Sound Off / steal-kill).
  void kill() noexcept;
  /// Exclusive-group choke: force the amp envelope into release even for
  /// one-shot (drum) voices, so a same-group strike (hi-hat / triangle / ...)
  /// cuts this ringing voice with a short fade instead of an abrupt kill. The
  /// fade length is the patch's amp release_ms.
  void choke() noexcept;
  /// The same, over kChokeReleaseMs instead of the patch's own release. GS
  /// ASSIGN MODE SINGLE needs this: on a sustaining patch the patch release runs
  /// past a second, which would leave audible the note it was told to replace.
  void choke_fast(double sample_rate) noexcept;
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
  /// Builds a streaming processor from a name and JSON params, for part rigs
  /// and GS insertion units. Null (the default) leaves the synth with no part
  /// buses, so every part renders dry; the host that links the effects wires it.
  GsEfxStageFactory insert_factory;
  /// Bind the bank's default rig to a part with no entry of its own, for the
  /// program it plays (docs/voicing.md). Reached only under use_gm_programs.
  bool bank_rig_binding = true;
  /// Offline hosts only: realise a pending rig or GS EFX change at the top of
  /// the block, and read GS EFX SysEx from the event stream. Realising
  /// allocates, so a live host leaves this false and pushes EFX SysEx through
  /// on_control_sysex instead.
  bool realize_efx_inline = false;
};

/// Continuously automatable NativeSynth parameters, addressed through
/// MidiInstrument::parameter_id_for_key / apply_parameter. The ordinals are the
/// persisted automation-target ids, so they are append-only: never renumber an
/// existing entry.
///
/// Every id reaches sounding voices from the next process() block. A voice
/// reads some patch fields every sample; the state it derives from the others
/// at note-on is re-derived by NativeSynthVoice::refresh_live() at the top of
/// the block after the write, so a held note lands where a note struck at the
/// new value would. Voices of a GM program or kit, and choked voices, are not
/// refreshed.
///
/// The set is the patch's own continuous fields, less the structural ones a
/// voice pool or a DSP topology depends on; anything continuous the patch grows
/// belongs here in the same change, or the two disagree in silence.
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
  kHpCutoffHz = 25,
  kSampleHoldHz = 26,
  kBitDepth = 27,
};

/// Bit for @p id in a refresh_live() mask.
inline constexpr uint32_t native_synth_param_bit(NativeSynthParamId id) noexcept {
  return 1u << static_cast<unsigned int>(id);
}

/// JSON-key name for @p id (the same string a SynthPatch object would use), or
/// nullptr for an unknown id.
const char* native_synth_param_name(NativeSynthParamId id) noexcept;
/// Number of automatable parameters, for enumeration by a host UI.
size_t native_synth_param_count() noexcept;
/// JSON-key name at @p index in [0, native_synth_param_count()), or nullptr.
const char* native_synth_param_name_at(size_t index) noexcept;

/// The "gm" controller preset: CC2 on the force axis, CC74 on the second one,
/// note-on velocity meaningful. What the synth answered before a profile layer
/// existed, so a host that sets none keeps its sound.
ControllerProfile default_controller_profile() noexcept;

/// Standalone patch-driven MidiInstrument: all 16 channels play the same
/// patch with BuiltinSynth-compatible channel semantics plus the default-
/// modulator CCs (CC1 vibrato, CC7/CC11 gain, CC10 pan, pitch bend, CC64
/// sustain, CC120/121/123 channel modes).
///
/// With an insert factory, a part with a rig or a GS insertion-effect route
/// renders through PartFxStage: its voices sum into the part's bus, which the
/// rig runs on in place. The GS EFX block and part assignment are read only
/// under use_gm_programs; the part edits (TONE MODIFY, Rx NRPN) and the resets
/// that clear them apply in both modes (docs/gs.md).
class NativeSynth final : public MidiInstrument, private PartFxHost {
 public:
  explicit NativeSynth(const NativeSynthConfig& config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  bool process_source_tracks(const MidiInstrumentSourceOutput* outputs, size_t output_count,
                             int num_channels, int num_samples) noexcept override;
  bool supports_source_track_rendering() const noexcept override { return true; }
  void reset() override;
  /// The rig chains are not latency-compensated, but their tails are counted.
  int tail_samples() const noexcept override {
    const int base = static_cast<int>(
        std::clamp<int64_t>(tail_samples_->load(std::memory_order_relaxed), 0,
                            static_cast<int64_t>(std::numeric_limits<int>::max())));
    return numeric::saturating_add(base, std::max(part_fx_.tail_samples(), 0));
  }
  /// A received GS envelope-time edit raises the tail (raise_tail).
  bool tail_follows_events() const noexcept override { return true; }
  /// CONTROL thread, offline tail probe: publish a pending inline EFX change
  /// without processing audio, so the host reads the chain that replayed events
  /// actually selected.
  bool materialize_tail_probe() override;
  void on_event(uint32_t destination_id, const MidiEvent& event) noexcept override;
  /// CONTROL thread: a host-pushed GS EFX block write or reset, realised here
  /// and handed to the audio thread wait-free. The live path's only EFX writer:
  /// a SysEx scheduled inside a clip reaches on_event, which ignores it unless
  /// realize_efx_inline is set.
  void on_control_sysex(const uint8_t* data, size_t size) noexcept override;
  /// CONTROL thread: install @p rig for part @p part, or for every part no entry
  /// names when @p part is kPartRigAllParts, and rebuild the chains when
  /// prepared. Applies whether or not use_gm_programs is set; kBank binds
  /// nothing without it.
  bool set_part_rig(uint8_t part, const PartRig& rig) noexcept override;
  /// CONTROL thread: the processor names of the chain last published for
  /// @p part, in signal order (test/diagnostic).
  std::vector<std::string> part_rig_stage_names(uint8_t part) const;
  /// AUDIO thread: the block's first device frame, which a render-path
  /// recorder stamps its records with. Ignored with no recorder attached.
  void set_transport(const transport::TransportState& state) noexcept override;
  /// CONTROL thread, offline renders only: record the realised signal path into
  /// @p recorder until it is detached with nullptr (diagnostic). Refused, and
  /// nothing attached, unless the synth realises its EFX inline.
  bool set_render_path_recorder(RenderPathRecorder* recorder) noexcept;
  int parameter_id_for_key(const std::string& key) const noexcept override;
  bool apply_parameter(unsigned int param_id, float value) noexcept override;
  bool describe_parameter(unsigned int param_id,
                          automation::ParameterDescription* out) const override;

  /// CONTROL thread: the bank the kSample engine reads; nullptr leaves that
  /// engine silent. Sounding voices keep the bank they started on, so swap it
  /// while nothing is playing.
  ///
  /// Two forms. The borrowed one is for a caller that already outlives the
  /// instrument; the owning one takes a share, which is what a host binding
  /// wants — a caller that drops its own handle mid-render then frees nothing a
  /// voice is reading.
  void set_sample_bank(const SampleBank* bank) noexcept {
    owned_sample_bank_.reset();
    sample_bank_ = bank;
    raise_tail();
  }
  void set_sample_bank(std::shared_ptr<const SampleBank> bank) noexcept {
    owned_sample_bank_ = std::move(bank);
    sample_bank_ = owned_sample_bank_.get();
    raise_tail();
  }
  const SampleBank* sample_bank() const noexcept { return sample_bank_; }

  const NativeSynthPatch& patch() const noexcept { return config_.patch; }
  /// Instrument master gain (test/diagnostic; the automated kGain target).
  float gain() const noexcept { return config_.gain; }

  /// Currently sounding voices (test/diagnostic).
  int active_voice_count() const noexcept { return pool_.active_count(); }

  /// The device spelling the synth reads its expression axes through. Replacing
  /// it clears every channel's axis values: the new profile's bindings say
  /// nothing about what the old ones had reached, and carrying them over would
  /// leave an axis held at a value no binding can now move. Always accepted —
  /// every engine here reads at least one axis through it.
  bool set_controller_profile(const ControllerProfile& profile) noexcept override;
  const ControllerProfile* controller_profile() const noexcept override {
    return &controller_profile_;
  }

  /// Always accepted — every engine here has an articulation, including the
  /// ones that decline to be carried: those answer a slur by counting a
  /// fallback and playing the note.
  bool set_articulation(uint8_t channel, ArticulationMode mode) noexcept override {
    channels_[channel & 0x0Fu].articulation = mode;
    return true;
  }
  bool articulation(uint8_t channel, ArticulationMode* out) const noexcept override {
    if (out == nullptr) return false;
    *out = channels_[channel & 0x0Fu].articulation;
    return true;
  }

  /// Both a declining engine and a pitch below the engine's delay line land
  /// here.
  bool legato_fallback_count(uint64_t* out) const noexcept override {
    if (out == nullptr) return false;
    *out = legato_fallbacks_;
    return true;
  }

  /// Channel-voice messages received but not acted on: reserved statuses, per-note controllers
  /// other than pitch, and relative controllers on a parameter this synth does not hold. Cleared
  /// by reset().
  uint64_t skipped_event_count() const noexcept { return skipped_events_; }

 private:
  /// gs_default_cc_positions() widened to the controller record's width.
  static std::array<Control32, 128> default_cc_positions() noexcept {
    std::array<Control32, 128> out{};
    const std::array<uint8_t, 128> seven = gs_default_cc_positions();
    for (size_t i = 0; i < out.size(); ++i) out[i] = Control32::from7(seven[i]);
    return out;
  }

  struct ChannelState {
    bool sustain = false;                           // CC64 >= 64 (dampers lifted)
    Control32 sustain_level = Control32::from7(0);  // CC64 (half-pedal damper position)
    /// CC66 pedal position. Sostenuto captures exactly the keys held at the
    /// DOWN EDGE, so the capture sweep must be edge-triggered: a pedal that
    /// keeps sending values >= 64 would otherwise capture notes struck after
    /// the press, which is the opposite of what the pedal means.
    bool sostenuto_down = false;
    bool una_corda = false;  // CC67 soft pedal
    /// Drawbar-organ percussion: charged, and spent by the next note-on that
    /// takes it. Recharges only when the channel has no key held, which is what
    /// makes percussion sound on the first note of a phrase and not on the ones
    /// played under it. Per channel rather than per voice (a voice cannot see
    /// its neighbours) and rather than per patch (a patch outlives the phrase).
    bool percussion_armed = true;
    /// Rhythm part: resolves through the drum map instead of the melodic
    /// programs. Channel 10 by default (GM), matching Sf2Player.
    bool drums = false;
    /// Controller values at MIDI 2.0 width: a float law reads f7(), a switch u7().
    Control32 volume = Control32::from7(100);      // CC7
    Control32 expression = Control32::from7(127);  // CC11
    Control32 pan = Control32::from7(64);          // CC10
    Control32 mod_wheel = Control32::from7(0);     // CC1
    /// CC2 and channel aftertouch as the channel last sent them, unscaled by
    /// any engine's own axis. The per-engine breath fields below carry a 255
    /// "untouched" sentinel because they override a preset value; these two are
    /// controller state and start at zero, which is what a channel that has
    /// sent nothing means.
    Control32 breath = Control32::from7(0);
    Control32 pressure = Control32::from7(0);
    uint8_t program = 0;  // last program change (or GM melodic program)
    uint8_t bank_msb = 0;
    uint8_t bank_lsb = 0;
    Bend32 pitch_bend = Bend32::center();
    /// Every controller as last sent, which is what an EFX CONTROL SOURCE reads.
    std::array<Control32, 128> cc_position = default_cc_positions();
    /// Expression axes as the channel's ControllerProfile has resolved them.
    /// Which controller reaches which axis is the profile's to say, and which
    /// axis an engine reads is engine_axis_capability()'s; nothing between them
    /// names a CC number. An axis no binding has reached is absent rather than
    /// zero, so the patch's own voicing, the channel's own pitch and a unit gain
    /// all stand until a controller actually arrives.
    ControllerAxisState axes;
    /// The last writer for profile axes that can be reached by MPE pressure,
    /// timbre, or bend.  A zero bit means an ordinary controller is the last
    /// writer, so a manager reset can clear only stale MPE-derived axes without
    /// erasing an unrelated CC mapping on the same axis.
    uint32_t mpe_pressure_axes = kAxisNone;
    uint32_t mpe_timbre_axes = kAxisNone;
    uint32_t mpe_bend_axes = kAxisNone;
    MpeDimension last_mpe_controller = MpeDimension::kPressure;
    ChannelParamState params;
    /// Persistent GS part edits (TONE MODIFY 1-8). They survive controller,
    /// program and bank changes; GS Reset, GM System On and reset() clear them.
    GsPartParams gs;
    /// 40 1x 0A RX NRPN; GM System On turns it off and GS Reset back on.
    bool rx_nrpn = true;
    float bend_range_cents = 200.0f;
    /// MODULATION LFO1 PITCH DEPTH (40 2x 04), the depth CC1 reaches at full.
    float mod_depth_cents = gs_mod_depth_cents(kGsModDepthDefault);
    /// Previous note's frequency (glide source; 0 = none yet).
    float last_freq_hz = 0.0f;
    /// What a second note-on does while the first is still held.
    ArticulationMode articulation = ArticulationMode::kPoly;
    /// Keys held on this channel, oldest first. Ordered rather than a bitmap
    /// because last-note priority is a question about the order they were
    /// pressed in, which a set cannot answer. The Note On attribute stays with
    /// its key so a legato return restores that key's absolute pitch.
    struct HeldKey {
      uint8_t note = 0;
      uint8_t attribute_type = 0;
      uint16_t attribute_data = 0;
      /// The source track that pressed the key; a key and the voice it sounds have one owner.
      uint32_t source_track_id = 0;
    };
    /// Ten fingers plus margin; a press past that is not recorded rather than
    /// displacing an older key, so the note still sounds and only the
    /// return-to priority loses it.
    std::array<HeldKey, 16> held_keys{};
    uint8_t held_count = 0;

    /// Records @p note as held by @p source. A repeat moves it to the top, which
    /// is what a re-press means for last-note priority.
    void hold_key(uint8_t note, uint8_t attribute_type, uint16_t attribute_data,
                  uint32_t source) noexcept {
      release_key(note, source);
      if (held_count < held_keys.size()) {
        held_keys[held_count++] = {note, attribute_type, attribute_data, source};
      }
    }
    /// Removes @p note held by @p source from the stack; a key that is not there is a no-op.
    void release_key(uint8_t note, uint32_t source) noexcept {
      for (uint8_t i = 0; i < held_count; ++i) {
        if (held_keys[i].note != note || held_keys[i].source_track_id != source) continue;
        for (uint8_t j = static_cast<uint8_t>(i + 1); j < held_count; ++j) {
          held_keys[j - 1] = held_keys[j];
        }
        --held_count;
        return;
      }
    }
    /// The most recently pressed held key and its Note On attribute, or null. With @p owned
    /// the search is limited to keys @p source pressed.
    const HeldKey* newest_held_key(bool owned = false, uint32_t source = 0) const noexcept {
      for (uint8_t i = held_count; i > 0; --i) {
        if (!owned || held_keys[i - 1].source_track_id == source) return &held_keys[i - 1];
      }
      return nullptr;
    }
  };

  /// The voice a note-off or a legato continuation on @p ch means: sounding on
  /// @p note, key still down, not yet releasing. One matching rule in one
  /// place, because a legato continuation moves `note` to the new key and every
  /// reader of it has to agree on that.
  NativeSynthVoice* find_sounding(uint8_t ch, uint8_t note, uint32_t source_track_id) noexcept;
  void note_on(uint8_t channel, uint8_t note, Velocity16 velocity, uint8_t attribute_type,
               uint16_t attribute_data, uint32_t source_track_id,
               const ExcitationAxes& velocity_excitation = {},
               uint32_t velocity_excitation_mask = kAxisNone) noexcept;
  void note_off(uint8_t channel, uint8_t note, uint32_t source_track_id) noexcept;
  /// Records a note-on velocity's engine axes on @p voice as their last writer.
  void record_velocity_axes(NativeSynthVoice& voice, const ExcitationAxes& velocity,
                            uint32_t mask) noexcept;
  /// The patch tail at the parts' current GS EG time edits plus the fixed bus
  /// tails, without allocation.
  int64_t recompute_tail() const noexcept;
  /// Raises the published tail to recompute_tail() and never lowers it: a voice
  /// struck under an edit since withdrawn may still be ringing.
  void raise_tail() noexcept;
  /// The slowest GS EG time multipliers any part holds; 1 where none lengthens.
  EnvelopeTimeScales gs_tail_scales() const noexcept;
  /// GS part SysEx (40 1x xx) and the system resets, on the channel state.
  void apply_part_sysex(const uint8_t* data, size_t size) noexcept;
  /// Whether the controller profile routes CC @p controller to an axis.
  bool profile_binds_cc(uint8_t controller) const noexcept;
  void process_impl(float* const* channels, const MidiInstrumentSourceOutput* source_outputs,
                    size_t source_output_count, int num_channels, int num_samples) noexcept;
  /// PartFxHost: where an EFX CONTROL source sits on @p part.
  float part_controller_position(int part, uint8_t source) const noexcept override;
  /// PartFxHost: the part pan a mono rig prefix is restored to.
  float part_pan_units(int part) const noexcept override {
    return channel_mods_[static_cast<size_t>(part & 0x0F)].pan_units;
  }
  /// Publishes the bank rig @p channel's program binds; offline marks the stage
  /// for a rebuild at the next block, live leaves it to the control thread.
  void refresh_part_rig(uint8_t channel) noexcept;
  /// Offline render thread: the EFX content of a GS SysEx, into the mirror.
  void apply_efx_sysex(const uint8_t* data, size_t size) noexcept;
  /// Offline render thread: rebuild and publish the stage. Allocates.
  void realize_part_fx();
  /// Offline render thread: hand the recorder the path this block realises.
  /// Allocates; reached only with a recorder attached.
  void record_render_path();
  void control_change(uint8_t channel, uint8_t controller, Control32 control) noexcept;
  void channel_pressure(uint8_t channel, Control32 pressure) noexcept;
  void poly_pressure(uint8_t channel, uint8_t note, Control32 pressure) noexcept;
  /// Damper pedal as the zone model scopes it: a manager's pedal reaches its whole zone.
  void sustain_cc(uint8_t channel, Control32 value) noexcept;
  /// The damper on exactly one channel.
  void sustain_channel(uint8_t channel, Control32 value) noexcept;
  /// RPN 0/0's Data Entry MSB (whole semitones) and LSB (cents), which a MIDI 2.0 Registered
  /// Controller 0/0 delivers together.
  void bend_range_msb(uint8_t channel, uint8_t semitones) noexcept;
  void bend_range_lsb(uint8_t channel, uint8_t cents) noexcept;
  /// A MIDI 2.0 Registered / Assignable Controller: 0/0 is read straight from the message, every
  /// other one takes the path its four MIDI 1.0 messages do.
  void registered_controller(const Ump& ump, const ChannelVoiceEvent& ev) noexcept;
  /// A MIDI 2.0 Relative Registered / Assignable Controller on a parameter this synth holds (RC
  /// 0/0 and 0/7), added and saturated. False for any other, which the caller counts as skipped.
  bool relative_controller(const ChannelVoiceEvent& ev) noexcept;
  /// Binds @p state to (channel, note) at note-on with the Note On attribute, the engine starting
  /// on @p zone_key.
  void bind_per_note(Sf2PerNoteVoice& state, uint8_t channel, uint8_t note, uint8_t attribute_type,
                     uint16_t attribute_data, int16_t zone_key) const noexcept;
  ComposedPitch compose_per_note(const Sf2PerNoteVoice& state) const noexcept;
  void refresh_per_note_pitch(Sf2PerNoteVoice& state) const noexcept;
  /// Re-evaluates every sounding voice on (channel, note), or on the whole channel when
  /// @p all_notes is set (a sensitivity change reaches every key of it).
  void refresh_per_note_voices(uint8_t channel, uint8_t note, bool all_notes) noexcept;
  /// Moves a legato-carried voice's per-note binding to the key it now sounds.
  void carry_per_note(const NativeSynthVoice& voice, uint8_t from_note, uint8_t to_note,
                      uint8_t attribute_type, uint16_t attribute_data) noexcept;
  /// What the one legato rule decided.
  enum class LegatoOutcome : uint8_t {
    kNone,      ///< nothing held to continue from; the caller plays or releases normally
    kCarried,   ///< the sounding voice now sounds the target key
    kReplaced,  ///< the previous voice was stopped (note-on) or declined (release)
  };
  struct LegatoResult {
    LegatoOutcome outcome = LegatoOutcome::kNone;
    NativeSynthVoice* voice = nullptr;  ///< the carried voice, for kCarried
  };
  /// The single legato rule, for both directions. At note-on (@p release false) the target is
  /// the incoming key and the voice is the one sounding the channel's newest held key; at a
  /// release (@p release true, the released key already removed) the target is the newest key
  /// the same source still holds and the voice is the one the released key sounded. Reach is
  /// judged on the composed sounding keys, and the voice's owner follows the key it now sounds.
  LegatoResult legato_continue(uint8_t ch, bool release, uint8_t note, uint8_t attribute_type,
                               uint16_t attribute_data, uint32_t source_track_id,
                               const NativeSynthPatch* patch) noexcept;
  /// Sostenuto pedal with the same zone scope as sustain_cc().
  void sostenuto_pedal(uint8_t channel, bool down) noexcept;
  /// The sostenuto on exactly one channel.
  void sostenuto_channel(uint8_t channel, bool down) noexcept;
  void all_notes_off(uint8_t channel) noexcept;
  void all_sound_off(uint8_t channel) noexcept;
  /// Recharges the channel's drawbar-organ percussion if no key is still held.
  void recharge_percussion(uint8_t channel) noexcept;
  /// The channel's expression axes as an engine axis set; @p present comes back
  /// naming the axes a controller has actually reached.
  static ExcitationAxes channel_excitation(const ControllerAxisState& axes,
                                           uint32_t& present) noexcept;
  /// Resolves @p ump through the controller profile and applies whatever axis
  /// values it produced. Runs before the protocol dispatch, so a note-on whose
  /// velocity is bound to an axis starts from its own value.
  ///
  /// Inside a zone this is also where the manager's fold happens, because the
  /// third dimension reaches an axis through the profile rather than through
  /// this class: a member's message is resolved carrying the combined value,
  /// and a manager's is resolved once per member of its zone as well as on the
  /// channel it arrived on.
  void apply_controller_input(const Ump& ump) noexcept;
  /// Resolves one message through the profile and applies its axis values.
  void apply_resolved_input(const Ump& ump) noexcept;
  /// Resolves note-on velocity, retaining engine-owned axes for the newly
  /// allocated voice while preserving channel-level velocity bindings.
  ExcitationAxes apply_note_on_controller_input(const Ump& ump, uint32_t* out_mask) noexcept;
  /// The per-note dimension @p ump carries, or false for a message that is not
  /// one of the three.
  static bool mpe_dimension_of(const Ump& ump, MpeDimension* out) noexcept;
  /// Fills mpe_notes_ with the channel's sounding notes, oldest first and one
  /// entry per note, and returns how many. Deduplicated because a layered patch
  /// is several voices of one key while the tracking rule names notes.
  size_t gather_mpe_notes(uint8_t channel) noexcept;
  /// The note a value addressed to the whole channel belongs to, or
  /// kControllerAnyNote when it belongs to every sounding note -- which is the
  /// answer outside a zone, on a manager channel whose values reach the whole
  /// zone, under kAllNotes, and whenever at most one note is sounding.
  uint8_t mpe_attributed_note(uint8_t channel, MpeDimension dimension) noexcept;
  /// Recomputes the per-voice mods of @p channel from its channel mod, so the
  /// bend and pressure a member channel carries reach the note they were
  /// attributed to and no other.
  void refresh_mpe_note_mods(uint8_t channel) noexcept;
  /// Captures the MPE dimensions at NoteOff while leaving ordinary channel
  /// controls live on a sustained/releasing voice.
  void freeze_mpe_voice(NativeSynthVoice& voice, uint8_t channel) noexcept;
  /// Profile axes @p dimension (bend, pressure or timbre) can write: the set
  /// a member voice freezes at its NoteOff.
  uint32_t mpe_profile_axis_mask(MpeDimension dimension) const noexcept;
  /// Rebuilds one frozen voice from current ordinary channel state and its
  /// captured MPE dimensions.
  void refresh_frozen_mpe_voice(NativeSynthVoice& voice, uint8_t channel) noexcept;
  /// Resolves @p channel's combined value through the profile, and every member
  /// of the zone as well when @p channel is the manager, whose value is a bias
  /// on each of them.
  void push_mpe_controller_axis(uint8_t channel, MpeDimension dimension) noexcept;
  /// Applies one combined MPE value to a channel's future-note template without
  /// pushing an attributed engine axis to currently sounding voices.
  void apply_mpe_channel_axes(uint8_t channel, MpeDimension dimension, Control32 combined) noexcept;
  /// Resolves one combined MPE value to one held voice. The member raw value is
  /// captured only for a member message; manager updates preserve the voice's
  /// captured member source and combine the current manager raw value with it.
  void apply_mpe_voice_axes(NativeSynthVoice& voice, uint8_t channel, MpeDimension dimension,
                            Control32 combined, bool update_member_raw, Control32 member_raw,
                            bool member_present) noexcept;
  /// Tracks the two combining dimensions ahead of the profile, so a value
  /// reaching an axis carries the manager's fold (2.2.7, 2.2.8).
  void track_mpe_input(const Ump& ump) noexcept;
  /// Pushes the channel's live excitation axes (and CC11 bow speed) to its
  /// sounding voices.
  void push_excitation_control(uint8_t channel, uint32_t changed_mask, bool mpe_dimension = false,
                               MpeDimension dimension = MpeDimension::kBend) noexcept;
  void restore_excitation_control(uint8_t channel, bool mpe_dimension = false) noexcept;
  void update_frozen_mpe_axis(uint8_t channel, ControllerAxis axis, float value) noexcept;
  void reset_controllers(uint8_t channel) noexcept;
  void refresh_channel_mod(uint8_t channel) noexcept;
  void refresh_all_channel_mods() noexcept;
  /// Refreshes every channel a zone-wide value arriving on @p channel reaches.
  void refresh_channel_mods_in_scope(uint8_t channel) noexcept;
  /// Accepts an MPE Configuration Message and does what accepting one obliges:
  /// every channel that entered or left the zone loses its sounding notes and
  /// its controllers, so a sender that re-zones mid-performance cannot leave a
  /// note hanging on a channel with no owner (M1-100-UM v1.1 section 2.2.3).
  void apply_mcm(uint8_t manager_channel, uint8_t member_count) noexcept;

  NativeSynthConfig config_{};
  ControllerProfile controller_profile_ = default_controller_profile();
  /// Empty until an MCM arrives, and while it is empty every channel reads as
  /// unassigned and nothing below behaves differently than it did before this
  /// member existed.
  MpeState mpe_{};
  /// Scratch for the note attribution and the allocation ages that order it,
  /// sized in prepare() to the polyphony, which bounds how many notes one
  /// channel can be sounding. Never resized on the audio thread.
  std::vector<MpeNote> mpe_notes_;
  std::vector<uint64_t> mpe_note_ages_;
  uint64_t legato_fallbacks_ = 0;
  uint64_t skipped_events_ = 0;
  /// MIDI 2.0 per-note pitch: the rows per key (surviving note-off and Reset All Controllers),
  /// RC 0/7 per channel, and each pool voice's binding, indexed like the pool and sized in
  /// prepare().
  PerNotePitchTable per_note_pitch_{};
  std::array<Control32, 16> per_note_bend_sensitivity_{};
  std::vector<Sf2PerNoteVoice> per_note_;
  double sample_rate_ = 0.0;
  bool prepared_ = false;
  /// Written on the audio thread by a live raise and read by the host, so
  /// atomic; held by pointer so the instrument stays movable.
  std::unique_ptr<std::atomic<int64_t>> tail_samples_ = std::make_unique<std::atomic<int64_t>>(0);
  /// Ids written by apply_parameter whose voice state awaits refresh_live (bit =
  /// NativeSynthParamId). Audio-thread state under config_.patch's contract.
  uint32_t live_dirty_ = 0;
  std::array<ChannelState, 16> channels_{};
  std::array<Sf2ChannelMod, 16> channel_mods_{};
  /// Mix-bus polish state (per stereo leg): DC blocker + drive constants.
  /// Channels that have started a note since the shared piano / guitar / swell buses were last
  /// cleared; those buses carry what any of them excited, so a channel's All Sound Off clears
  /// them only when no other channel is in the set.
  uint16_t bus_fed_channels_ = 0;
  std::array<float, 2> dc_x1_{};
  std::array<float, 2> dc_y1_{};
  float dc_r_ = 0.999f;
  float bus_drive_gain_ = 0.0f;
  /// Shared-bus residual (mix minus dry), split per component so each lands on
  /// the sources that produced it. prepare() has no block-length argument, so
  /// each is staged per bus chunk and flushed at its end (process_impl).
  SourceResidualSplitter residual_splitter_;         // remainder: swell, bus drive, DC block
  SourceResidualSplitter piano_residual_splitter_;   // piano body + sympathetic
  SourceResidualSplitter guitar_residual_splitter_;  // guitar/KS sound halo
  std::array<float, kResidualChunk> residual_l_{};
  std::array<float, kResidualChunk> residual_r_{};
  std::array<float, kResidualChunk> piano_residual_l_{};
  std::array<float, kResidualChunk> piano_residual_r_{};
  std::array<float, kResidualChunk> guitar_residual_l_{};
  std::array<float, kResidualChunk> guitar_residual_r_{};
  /// A bussed part's chain output, and a unit's, follow only the voices that
  /// fed that bus.
  std::array<SourceResidualSplitter, 16> part_bus_splitters_;
  /// Per-part piano bodies used whenever a factory is present. Keeping one
  /// bank per part lets a bussed return enter its rig before it can reach any
  /// other part, and lets an assignment change move the existing tail without
  /// migrating or mixing state.
  std::array<SourceResidualSplitter, 16> part_piano_splitters_;
  /// Per-part organ swell residuals use the same source attribution boundary
  /// as their raw organ voices. A global splitter would let an unrelated part
  /// absorb a channel's CC11 change.
  std::array<SourceResidualSplitter, 16> part_organ_splitters_;
  /// The same source boundary for a factory-backed per-part guitar halo.
  std::array<SourceResidualSplitter, 16> part_guitar_splitters_;
  std::array<SourceResidualSplitter, kGsEfxUnitCount> unit_splitters_;
  static_assert(kResidualChunk == kPartFxChunkFrames,
                "the residual staging is flushed once per bus chunk");
  /// The voice sums of one chunk, taken before the bus outputs are folded in
  /// and the post stages run. Pre-gain, like the per-sample sums they hold.
  std::array<float, kPartFxChunkFrames> chunk_mix_l_{};
  std::array<float, kPartFxChunkFrames> chunk_mix_r_{};
  std::array<float, kPartFxChunkFrames> chunk_piano_l_{};
  std::array<float, kPartFxChunkFrames> chunk_piano_r_{};
  std::array<float, kPartFxChunkFrames> chunk_guitar_l_{};
  std::array<float, kPartFxChunkFrames> chunk_guitar_r_{};
  /// Raw piano legs for each part when the factory-backed per-part bodies are
  /// active. The entries are replaced with that part's body residual after it
  /// has been fed, so no second scratch slab is needed.
  std::array<std::array<float, kPartFxChunkFrames>, 16> chunk_part_piano_l_{};
  std::array<std::array<float, kPartFxChunkFrames>, 16> chunk_part_piano_r_{};
  /// Raw organ legs for each part. They are replaced with the pre-rig swell
  /// residual after the part has been filtered.
  std::array<std::array<float, kPartFxChunkFrames>, 16> chunk_part_organ_l_{};
  std::array<std::array<float, kPartFxChunkFrames>, 16> chunk_part_organ_r_{};
  std::array<std::array<float, kPartFxChunkFrames>, 16> chunk_part_guitar_l_{};
  std::array<std::array<float, kPartFxChunkFrames>, 16> chunk_part_guitar_r_{};
  /// Part buses, rig chains and GS insertion units.
  PartFxStage part_fx_;
  /// Attached for one offline render only.
  RenderPathRecorder* path_recorder_ = nullptr;
  /// Open-string halo per part, for a bussed part's sympathetic voices: its
  /// return rides the part's bus and therefore the part's rig, as Sf2Player's
  /// does. Armed at the first qualifying note-on on the part.
  std::array<PianoResonanceBank, 16> part_halo_;
  std::array<bool, 16> part_halo_armed_{};
  struct PianoPartState {
    bool prepared = false;
    float soundboard_mix = -1.0f;
  };
  /// Factory-backed piano bodies are per-part for the same reason as the GS
  /// fallback bodies: a return must follow the part's current EFX assignment.
  /// These banks are allocated in prepare() only when an insert factory is
  /// wired, keeping the common no-EFX NativeSynth object small on the stack.
  std::vector<PianoSoundboard> part_soundboards_;
  std::vector<PianoResonanceBank> part_resonance_;
  std::array<PianoPartState, 16> part_piano_state_{};
  VoicePool<NativeSynthVoice> pool_;
  /// Host sample bank for the kSample engine. The raw pointer is what the audio
  /// thread reads; the share below is held only when the caller handed one over.
  const SampleBank* sample_bank_ = nullptr;
  std::shared_ptr<const SampleBank> owned_sample_bank_;
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
  /// Shared sympathetic resonance bank, piano patches only: driven pedal-gated
  /// (the sustain-pedal sound halo), tuned in prepare() for a configured piano
  /// or lazily at the first GM piano note-on.
  PianoResonanceBank resonance_;
  /// Shared open-string sound halo for Karplus-Strong patches that opt in
  /// (patch.ks.sympathetic): a second bank rather than reusing the one above,
  /// since GM mode can voice a piano and a guitar together and each needs its
  /// own tuning and its own ring state. Armed in prepare() from this synth's
  /// own configured patch, or lazily at the first qualifying GM note-on
  /// (guitar_halo_active_); Sf2Player arms its own copy per part instead.
  PianoResonanceBank guitar_halo_;
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
  /// The halo bank above is armed and should be driven this render: true for
  /// a configured Karplus-Strong sympathetic patch from prepare(), or set
  /// lazily at the first qualifying GM note-on (mirrors piano_body_active_).
  /// False leaves every existing KS voicing on its original render path.
  bool guitar_halo_active_ = false;
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
  /// Per-part organ wind supply parameters (tremulant / wind sag), armed at the
  /// note-on that resolves a pipe-organ patch on that part.
  struct OrganPartParams {
    float tremulant_rate_hz = -1.0f;
    float tremulant_depth = -1.0f;
    float wind_sag = -1.0f;
    bool armed = false;
  };
  /// One wind chest and swell box per MIDI part. GM resolves the pipe patch at
  /// note-on, so construction-time patch mode cannot own these states.
  std::array<OrganWindSupply, 16> part_wind_;
  std::array<OrganPartParams, 16> organ_part_params_{};
  std::array<float, 16> part_swell_depth_{};
  std::array<float, 16> part_swell_coeff_{};
  std::array<float, 16> part_swell_lp_l_{};
  std::array<float, 16> part_swell_lp_r_{};
};

/// Patch-clamp helpers. Named rather than local because `clamp_synth_patch`
/// is `constexpr`: the fallback tables it feeds are constant-initialised, so
/// its whole call tree has to be visible here rather than in a translation
/// unit of its own.
namespace patch_clamp_detail {

/// Finite test usable during constant evaluation. `std::isfinite` is not
/// `constexpr` in C++17; `value == value` rejects NaN and the magnitude
/// bounds reject both infinities, which is the same predicate for `float`.
constexpr float sanitize(float value, float fallback) noexcept {
  return (value == value && value <= std::numeric_limits<float>::max() &&
          value >= std::numeric_limits<float>::lowest())
             ? value
             : fallback;
}

constexpr DahdsrConfig clamp_env(const DahdsrConfig& env) noexcept {
  DahdsrConfig out{};
  out.delay_ms = std::clamp(sanitize(env.delay_ms, 0.0f), 0.0f, 5000.0f);
  out.attack_ms = std::clamp(sanitize(env.attack_ms, 5.0f), 0.0f, 20000.0f);
  out.hold_ms = std::clamp(sanitize(env.hold_ms, 0.0f), 0.0f, 5000.0f);
  out.decay_ms = std::clamp(sanitize(env.decay_ms, 60.0f), 0.0f, 20000.0f);
  out.sustain = std::clamp(sanitize(env.sustain, 0.7f), 0.0f, 1.0f);
  out.release_ms = std::clamp(sanitize(env.release_ms, 120.0f), 1.0f, 20000.0f);
  return out;
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
  // The one scale applied after the drive, the filter and the envelope, so the
  // only lever that moves a voice's level without its timbre. The drum bank's
  // raw voices span 33 dB and at 4 it ran out under the quietest of them.
  p.gain = std::clamp(patch_clamp_detail::sanitize(p.gain, 0.5f), 0.0f, 16.0f);
  p.amp_env = patch_clamp_detail::clamp_env(p.amp_env);
  p.cutoff_hz = std::clamp(patch_clamp_detail::sanitize(p.cutoff_hz, 12000.0f), 10.0f, 22000.0f);
  p.resonance_q = std::clamp(patch_clamp_detail::sanitize(p.resonance_q, constants::kButterworthQ),
                             0.5f, 30.0f);
  p.hp_cutoff_hz = std::clamp(patch_clamp_detail::sanitize(p.hp_cutoff_hz, 0.0f), 0.0f, 22000.0f);
  p.drive = std::clamp(patch_clamp_detail::sanitize(p.drive, 0.0f), 0.0f, 1.0f);
  // The lower bound is a rate the stage can still be heard as a rate at; below
  // it the held value lasts long enough to be a note of its own.
  p.sample_hold_hz =
      std::clamp(patch_clamp_detail::sanitize(p.sample_hold_hz, 0.0f), 0.0f, 192000.0f);
  if (p.sample_hold_hz > 0.0f) p.sample_hold_hz = std::max(p.sample_hold_hz, 100.0f);
  // 24 is the ceiling because a float voice already resolves further than that;
  // 1 bit is a square wave, which is where the stage stops being a converter and
  // is still a thing someone reaches for.
  p.bit_depth = std::clamp(patch_clamp_detail::sanitize(p.bit_depth, 0.0f), 0.0f, 24.0f);
  if (p.bit_depth > 0.0f) p.bit_depth = std::max(p.bit_depth, 1.0f);
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
  p.ks.mute_harmonic =
      std::clamp(patch_clamp_detail::sanitize(p.ks.mute_harmonic, 0.0f), 0.0f, 16.0f);
  p.ks.slap = std::clamp(patch_clamp_detail::sanitize(p.ks.slap, 0.0f), 0.0f, 1.0f);
  p.ks.polarization = std::clamp(patch_clamp_detail::sanitize(p.ks.polarization, 0.0f), 0.0f, 1.0f);
  p.ks.body_coupling = patch_clamp_detail::sanitize(p.ks.body_coupling, 0.0f);
  p.ks.pluck_style = patch_clamp_detail::sanitize(p.ks.pluck_style, 0.0f);
  p.ks.nail = patch_clamp_detail::sanitize(p.ks.nail, 0.0f);
  p.ks.pickup_pos = patch_clamp_detail::sanitize(p.ks.pickup_pos, 0.0f);
  p.ks.dispersion = patch_clamp_detail::sanitize(p.ks.dispersion, 0.0f);
  p.ks.tension_mod = patch_clamp_detail::sanitize(p.ks.tension_mod, 0.0f);
  p.ks.octave_mix = patch_clamp_detail::sanitize(p.ks.octave_mix, 0.0f);
  // Bounded here rather than only at use, so the touched node reports its real
  // range: a sweep over an unbounded divisor is a sweep over nothing.
  p.ks.harmonic_node =
      std::clamp(patch_clamp_detail::sanitize(p.ks.harmonic_node, 0.0f), 0.0f, 8.0f);
  p.ks.keyoff_noise = patch_clamp_detail::sanitize(p.ks.keyoff_noise, 0.0f);
  p.ks.pick_noise = patch_clamp_detail::sanitize(p.ks.pick_noise, 0.0f);
  p.ks.hf_decay_s = std::clamp(patch_clamp_detail::sanitize(p.ks.hf_decay_s, 0.07f), 0.0f, 60.0f);
  p.ks.mid_decay_s = std::clamp(patch_clamp_detail::sanitize(p.ks.mid_decay_s, 0.0f), 0.0f, 60.0f);
  p.ks.velocity_exponent =
      std::clamp(patch_clamp_detail::sanitize(p.ks.velocity_exponent, 1.19f), 0.0f, 4.0f);
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
  for (float& level : p.additive.drawbars_b)
    level = std::clamp(patch_clamp_detail::sanitize(level, 0.0f), 0.0f, 8.0f);
  p.additive.morph = std::clamp(patch_clamp_detail::sanitize(p.additive.morph, 0.0f), 0.0f, 1.0f);
  p.additive.key_click =
      std::clamp(patch_clamp_detail::sanitize(p.additive.key_click, 0.4f), 0.0f, 1.0f);
  p.additive.click_decay_ms =
      std::clamp(patch_clamp_detail::sanitize(p.additive.click_decay_ms, 6.0f), 0.5f, 100.0f);
  p.additive.percussion_harmonic = std::clamp(p.additive.percussion_harmonic, 0, 3);
  p.additive.percussion_decay_ms = std::clamp(
      patch_clamp_detail::sanitize(p.additive.percussion_decay_ms, 340.0f), 20.0f, 4000.0f);
  p.additive.percussion_level =
      std::clamp(patch_clamp_detail::sanitize(p.additive.percussion_level, 0.6f), 0.0f, 1.0f);
  p.percussion.num_modes = std::clamp(p.percussion.num_modes, 0, kMaxPercussionModes);
  for (float& ratio : p.percussion.mode_ratios) {
    ratio = std::clamp(patch_clamp_detail::sanitize(ratio, 0.0f), 0.0f, 64.0f);
  }
  p.percussion.mode_decay_s =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.mode_decay_s, 0.3f), 0.005f, 30.0f);
  p.percussion.mode_decay_exp =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.mode_decay_exp, 0.0f), 0.0f, 2.0f);
  p.percussion.tone_gain =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.tone_gain, 1.0f), 0.0f, 4.0f);
  // A share, so the interval is the whole of it: 0 and 1 are both real
  // settings and a fit has to be able to reach either end.
  p.percussion.tone_direct =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.tone_direct, 1.0f), 0.0f, 1.0f);
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
  // 50 ms is longer than any struck contact the kit models; the exponent is a
  // power of a normalized velocity, so above 1 a soft hit is brighter.
  p.percussion.mallet_ms =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.mallet_ms, 0.0f), 0.0f, 50.0f);
  p.percussion.mallet_vel_exp =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.mallet_vel_exp, 0.0f), 0.0f, 1.0f);
  // A cavity eight times the head's stiffness splits the pair by a factor of 3;
  // the two lengths bound a concert bass drum and leave a taiko room.
  p.percussion.air_spring =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.air_spring, 0.0f), 0.0f, 8.0f);
  p.percussion.head_diameter_m =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.head_diameter_m, 0.0f), 0.0f, 2.0f);
  p.percussion.shell_depth_m =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.shell_depth_m, 0.0f), 0.0f, 2.0f);
  p.percussion.noise_gain =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.noise_gain, 0.0f), 0.0f, 4.0f);
  p.percussion.noise_decay_ms =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.noise_decay_ms, 150.0f), 1.0f, 20000.0f);
  p.percussion.noise_cutoff_hz = std::clamp(
      patch_clamp_detail::sanitize(p.percussion.noise_cutoff_hz, 2500.0f), 20.0f, 20000.0f);
  p.percussion.noise_q =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.noise_q, 1.0f), 0.5f, 30.0f);
  p.percussion.noise_burst_interval_ms =
      patch_clamp_detail::sanitize(p.percussion.noise_burst_interval_ms, 10.0f);
  p.percussion.noise_burst_decay_ms =
      patch_clamp_detail::sanitize(p.percussion.noise_burst_decay_ms, 6.0f);
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
  // A fraction of the head's own peak swing, so the whole interval means
  // something: 0 is always in contact and 1 is never. It read [0, 4] while the
  // membrane swung around a tenth of that, which left most of the sweep range
  // in a dead zone and every other wire knob free once a fit landed there.
  p.percussion.wire_threshold =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.wire_threshold, 0.1f), 0.0f, 1.0f);
  p.percussion.wire_cutoff_hz = std::clamp(
      patch_clamp_detail::sanitize(p.percussion.wire_cutoff_hz, 4000.0f), 20.0f, 20000.0f);
  p.percussion.wire_decay_ms =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.wire_decay_ms, 0.0f), 0.0f, 2000.0f);
  p.percussion.shimmer =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.shimmer, 0.0f), 0.0f, 16.0f);
  p.percussion.shimmer_attack_ms = std::clamp(
      patch_clamp_detail::sanitize(p.percussion.shimmer_attack_ms, 40.0f), 1.0f, 2000.0f);
  p.percussion.shimmer_cutoff_hz = std::clamp(
      patch_clamp_detail::sanitize(p.percussion.shimmer_cutoff_hz, 8000.0f), 20.0f, 20000.0f);
  // Up to 16, the same ceiling `gain` carries. Four was under the plate's own
  // peak on every cymbal here, so a stick's radiation could not be the strike's
  // loudest moment - which on a ride played with the tip is most of what there
  // is to hear.
  p.percussion.contact =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.contact, 0.0f), 0.0f, 16.0f);
  // The ceiling is a felt mallet and the floor is the sample period at 96 kHz,
  // below which the pulse is a single sample whatever is asked for.
  p.percussion.contact_ms =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.contact_ms, 0.3f), 0.02f, 20.0f);
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
  // The same ceiling as the stick's direct radiation, which is the same pulse.
  p.percussion.plate_contact =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_contact, 0.0f), 0.0f, 16.0f);
  p.percussion.plate_cascade =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_cascade, 0.0f), 0.0f, 1.0e4f);
  p.percussion.plate_cascade_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_cascade_hz, 0.0f), 0.0f, 20000.0f);
  p.percussion.plate_cascade_drop_db = std::clamp(
      patch_clamp_detail::sanitize(p.percussion.plate_cascade_drop_db, 0.0f), 0.0f, 60.0f);
  // Same floor as plate_low_hz, 0 kept as "follow plate_low_hz".
  p.percussion.plate_floor_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_floor_hz, 0.0f), 0.0f, 4000.0f);
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
      std::clamp(patch_clamp_detail::sanitize(p.piano.strike_position, 0.085f), 0.0f, 0.5f);
  p.piano.hammer_exponent =
      std::clamp(patch_clamp_detail::sanitize(p.piano.hammer_exponent, 2.5f), 1.5f, 4.0f);
  p.piano.hammer_contact_ms =
      std::clamp(patch_clamp_detail::sanitize(p.piano.hammer_contact_ms, 1.2f), 0.2f, 10.0f);
  p.piano.hammer_dynamics =
      std::clamp(patch_clamp_detail::sanitize(p.piano.hammer_dynamics, 0.0f), 0.0f, 1.0f);
  p.piano.attack_hf_dynamics =
      std::clamp(patch_clamp_detail::sanitize(p.piano.attack_hf_dynamics, 0.0f), 0.0f, 8.0f);
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
  p.pipe_organ.keytrack = patch_clamp_detail::sanitize(p.pipe_organ.keytrack, 0.0f);
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
  p.bowed_string.stribeck = patch_clamp_detail::sanitize(p.bowed_string.stribeck, 0.5f);
  p.bowed_string.sympathetic = patch_clamp_detail::sanitize(p.bowed_string.sympathetic, 0.0f);
  p.bowed_string.polarization = patch_clamp_detail::sanitize(p.bowed_string.polarization, 0.0f);
  p.bowed_string.attack_noise =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.attack_noise, 0.0f), 0.0f, 1.0f);
  // Shares `attack_ms`'s upper bound; the lower bound is 0 because 0 is this
  // field's "keep the one-pole ramp" sentinel rather than an instant run-up.
  p.bowed_string.bow_accel_ms =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.bow_accel_ms, 0.0f), 0.0f, 2000.0f);
  p.bowed_string.corpus_scale =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.corpus_scale, 1.0f), 0.1f, 4.0f);
  p.bowed_string.corpus_tilt_hz =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.corpus_tilt_hz, 0.0f), 0.0f, 20000.0f);
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
  p.reed.closing_pressure =
      std::clamp(patch_clamp_detail::sanitize(p.reed.closing_pressure, 0.0f), 0.0f, 8.0f);
  p.reed.pressure_scale =
      std::clamp(patch_clamp_detail::sanitize(p.reed.pressure_scale, 1.0f), 0.0f, 4.0f);
  p.reed.flow_gain = std::clamp(patch_clamp_detail::sanitize(p.reed.flow_gain, 0.7f), 0.0f, 1.5f);
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
  p.brass.lip_aperture =
      std::clamp(patch_clamp_detail::sanitize(p.brass.lip_aperture, 0.0f), 0.0f, 1.0f);
  p.brass.bell_radiation_hz =
      std::clamp(patch_clamp_detail::sanitize(p.brass.bell_radiation_hz, 0.0f), 0.0f, 8000.0f);
  // Wider than bell_radiation_hz above, which is a fitted radiation corner: this
  // one is the flare itself and has to reach the corner a bore with no bell
  // would have, so the fit can be asked whether a bell is there at all.
  p.brass.bell_cutoff_hz =
      std::clamp(patch_clamp_detail::sanitize(p.brass.bell_cutoff_hz, 0.0f), 0.0f, 20000.0f);
  p.brass.brassiness =
      std::clamp(patch_clamp_detail::sanitize(p.brass.brassiness, 0.0f), 0.0f, 1.0f);
  p.brass.cuivre_dynamics =
      std::clamp(patch_clamp_detail::sanitize(p.brass.cuivre_dynamics, 0.0f), 0.0f, 1.0f);
  p.brass.bore_nonlinearity =
      std::clamp(patch_clamp_detail::sanitize(p.brass.bore_nonlinearity, 0.0f), 0.0f, 1.0f);
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
  p.free_reed.slot_duty =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.slot_duty, 0.0f), 0.0f, 0.9f);
  p.free_reed.slot_return =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.slot_return, 0.4f), 0.0f, 1.5f);
  p.free_reed.slot_gap =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.slot_gap, 0.14f), 0.02f, 0.5f);
  p.free_reed.radiation =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.radiation, 1.0f), 0.0f, 1.0f);
  p.harpsichord.pluck_8a =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.pluck_8a, 0.14f), 0.0f, 0.5f);
  p.harpsichord.pluck_8b =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.pluck_8b, 0.22f), 0.0f, 0.5f);
  p.harpsichord.pluck_4 =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.pluck_4, 0.11f), 0.0f, 0.5f);
  p.harpsichord.plectrum_edge =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.plectrum_edge, 0.8f), 0.0f, 1.0f);
  p.harpsichord.end_reflection =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.end_reflection, 1.0f), 0.0f, 1.0f);
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
  // The floor is the inherit sentinel, not a decay; the ceiling is the longest
  // the speaking strings themselves are allowed.
  p.harpsichord.rear_decay_s =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.rear_decay_s, 0.0f), 0.0f, 60.0f);
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
  // The floor is the off sentinel; the ceiling is the middle of the compass,
  // past which the board would not be radiating the instrument at all.
  p.harpsichord.board_radiating_from_hz = std::clamp(
      patch_clamp_detail::sanitize(p.harpsichord.board_radiating_from_hz, 0.0f), 0.0f, 500.0f);
  // The ceiling is one first-order section's whole slope: past it the board
  // would be differentiating its own drive rather than radiating it.
  p.harpsichord.board_tilt_db_oct =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.board_tilt_db_oct, 0.0f), 0.0f, 6.0f);
  // The floor is the off sentinel and the ceiling is the strings' own level: a
  // board radiating more than what drives it is not a board.
  p.harpsichord.board_diffuse_db = std::clamp(
      patch_clamp_detail::sanitize(p.harpsichord.board_diffuse_db, -120.0f), -120.0f, 0.0f);
  // A negative set is the "no keymap" sentinel; anything else is an index the
  // bank either has or does not, which is its business rather than the clamp's.
  if (p.sample.set_index < -1) p.sample.set_index = -1;
  p.sample.level = std::clamp(patch_clamp_detail::sanitize(p.sample.level, 1.0f), 0.0f, 8.0f);
  if (p.sample.loop_override != 1 && p.sample.loop_override != 3 && p.sample.loop_override != 0) {
    p.sample.loop_override = -1;
  }
  p.sample.start_offset01 =
      std::clamp(patch_clamp_detail::sanitize(p.sample.start_offset01, 0.0f), 0.0f, 0.999f);
  // Bound by the enum's last member, not a literal: the literal was 4 and
  // outlived kVocal being added at 5, so every vocal body was reset to none.
  if (static_cast<int>(p.body) < 0 ||
      static_cast<int>(p.body) > static_cast<int>(BodyType::kVocal)) {
    p.body = BodyType::kNone;
  }
  p.body_mix = std::clamp(patch_clamp_detail::sanitize(p.body_mix, 0.0f), 0.0f, 1.0f);
  p.stereo_spread = std::clamp(patch_clamp_detail::sanitize(p.stereo_spread, 0.0f), 0.0f, 1.0f);
  return p;
}

/// MIDI note -> equal-tempered frequency (A4 = 440 Hz).
float synth_note_to_hz(float note) noexcept;

}  // namespace sonare::midi::synth
