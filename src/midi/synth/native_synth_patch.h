#pragma once

/// @file native_synth_patch.h
/// @brief NativeSynthPatch, the patch POD every NativeSynth engine reads, with its
///        subtractive-engine section and the unison/retrigger constants.

#include <array>
#include <cstdint>

#include "midi/synth/additive_voice.h"
#include "midi/synth/body_resonator.h"
#include "midi/synth/bowed_string_voice.h"
#include "midi/synth/brass_voice.h"
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
#include "midi/synth/percussion_voice.h"
#include "midi/synth/piano_voice.h"
#include "midi/synth/pipe_organ_voice.h"
#include "midi/synth/plucked_string_voice.h"
#include "midi/synth/reed_voice.h"
#include "midi/synth/sample_voice.h"
#include "midi/synth/sf2_voice.h"
#include "midi/synth/vocal_voice.h"

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

/// The subtractive engine's own section: a second oscillator a fixed interval
/// from the unison stack and white noise, both summed ahead of the filter, and
/// a band of noise added after it. At zero level each is absent and the voice
/// is bit-identical to one without it.
struct VaPatchParams {
  /// Second oscillator's offset from the played pitch (cents), same waveform.
  float osc2_interval_cents = 0.0f;
  /// Second oscillator's linear level against one unison oscillator; 0 = absent.
  float osc2_level = 0.0f;
  /// Share of the second oscillator that joins after the filter, a layer the
  /// filter does not shape; 0 = all of it filtered.
  float osc2_dry = 0.0f;
  /// Seeded white noise's linear level in the oscillator sum; 0 = absent.
  float noise_level = 0.0f;
  /// RMS depth of slow seeded noise multiplying the oscillator sum, so a breath
  /// skirt rides on every partial at the partial's own level; 0 = absent.
  float noise_am = 0.0f;
  /// Seeded white noise added after the filter through its own band, so the
  /// floor between partials holds still while the filter tracks the key; 0 = absent.
  float band_noise_level = 0.0f;
  /// That band's 2-pole highpass corner at middle C (Hz).
  float band_noise_low_hz = 20.0f;
  /// How far the highpass corner follows the played pitch (1 = by its interval from middle C).
  float band_noise_low_key_track = 0.0f;
  /// That band's 4th-order lowpass corner at middle C (Hz).
  float band_noise_high_hz = 4000.0f;
  /// How far the lowpass corner follows the played pitch (1 = by its interval from middle C).
  float band_noise_high_key_track = 0.0f;
  /// Exponent of the note-on level curve, (velocity / 127)^exponent; 2 is the SF2 square law.
  float velocity_exponent = 2.0f;
  /// Fraction of each period a square oscillator spends high; 0.5 is the symmetric square.
  float pulse_width = 0.5f;
  /// How far the unison stack spreads across the stereo field: each oscillator
  /// pans by this times its detune position, through its own copy of the filter;
  /// 0 = every oscillator centred, bit-identical.
  float unison_width = 0.0f;
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
  /// Waveform of both LFOs, the same at every destination each reaches (vibrato,
  /// the GS LFO cutoff and amplitude depths, ModSource::kLfo1 and kLfo2).
  LfoShape lfo_shape = LfoShape::kTriangle;
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

  /// Second oscillator and noise (used when mode == kSubtractive).
  VaPatchParams va;

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

/// MIDI note -> equal-tempered frequency (A4 = 440 Hz).
float synth_note_to_hz(float note) noexcept;

}  // namespace sonare::midi::synth
