#pragma once

/// @file voice_transform_types.h
/// @brief Shared configuration and target types of the voice transformer chain (pitch engine,
///        pitch correction, harmony, vocoder, character, reverb variation, MIDI notes).
///
/// Every config struct is trivially copyable so it can travel through rt::SeqlockCell and
/// rt::SpscQueue without allocation.

#include <cstdint>
#include <type_traits>

namespace sonare::editing::voice_changer {

/// Pitch-shifting engine of the voice stage.
enum class PitchEngine : int { Grain = 0, Psola = 1 };
/// How the target pitch is chosen.
enum class PitchCorrectionMode : int { Off = 0, Auto = 1, Robot = 2 };
/// Scale used by auto pitch and harmony.
enum class ScaleKind : int { Major = 0, NaturalMinor = 1, Chromatic = 2 };
/// Where the vocoder carrier comes from.
enum class VocoderCarrierSource : int { Internal = 0, External = 1 };
/// Waveform of the internal vocoder carrier.
enum class VocoderCarrierWave : int { Saw = 0, Pulse = 1, Ensemble = 2 };
/// Colouration applied after the voice stage.
enum class CharacterType : int { None = 0, Megaphone = 1, Radio = 2, BbdChorus = 3, Strobo = 4 };
/// Space effect selected by the reverb stage.
enum class ReverbVariation : int { Reverb = 0, Echo = 1, TempoDelay = 2, DubEcho = 3 };
/// Note value of the tempo-synced delay.
enum class NoteDivision : int { Quarter = 0, Eighth = 1, DottedEighth = 2, Sixteenth = 3 };
/// Behaviour once every held MIDI note has been released.
enum class MidiRelease : int { Hold = 0, Input = 1 };

constexpr int kMaxHarmonyVoices = 3;
constexpr int kMaxVoices = 1 + kMaxHarmonyVoices;  // lead + harmony
constexpr int kMaxHeldNotes = 4;
constexpr int kNoteQueueCapacity = 256;  // power of two (rt::SpscQueue)
/// Highest fraction of the sample rate any fixed design corner in this chain may sit at.
constexpr float kMaxDesignFrequencyRatio = 0.45f;

/// Voice-stage engine and the lowest f0 it tracks (fixes the shifter latency).
struct VoiceEngineConfig {
  PitchEngine engine = PitchEngine::Psola;
  float f0_min_hz = 70.0f;
};

/// Pitch correction (auto / robot) parameters.
struct PitchCorrectionConfig {
  PitchCorrectionMode mode = PitchCorrectionMode::Off;
  float amount = 1.0f;
  float speed_ms = 20.0f;
  int key_root = 0;
  ScaleKind scale = ScaleKind::Major;
  int robot_octave = 0;
};

/// Harmony voices: scale steps, level, lead level, detune and delay.
struct HarmonyConfig {
  int voices = 0;
  int steps[kMaxHarmonyVoices] = {2, 4, -3};
  float level = 0.7f;
  float lead_level = 1.0f;
  float detune_cents = 6.0f;
  float delay_ms = 12.0f;
};

/// Channel vocoder parameters. `bands` is fixed at construction.
struct VocoderConfig {
  bool enabled = false;
  int bands = 20;
  float attack_ms = 2.0f;
  float release_ms = 20.0f;
  float sibilance = 0.3f;
  VocoderCarrierSource carrier_source = VocoderCarrierSource::Internal;
  VocoderCarrierWave carrier_wave = VocoderCarrierWave::Saw;
  float ensemble_detune_cents = 12.0f;
  float level = 1.0f;
};

/// Character stage selection and its two controls.
struct CharacterConfig {
  CharacterType type = CharacterType::None;
  float amount = 0.5f;
  float tone = 0.0f;
};

/// Echo-family variations of the space stage.
struct ReverbVariationConfig {
  ReverbVariation variation = ReverbVariation::Reverb;
  float delay_ms = 250.0f;
  float feedback = 0.35f;
  float tempo_bpm = 120.0f;
  NoteDivision division = NoteDivision::Eighth;
};

/// MIDI note handling.
struct MidiConfig {
  MidiRelease release = MidiRelease::Hold;
};

/// One tracker decision. centre_sample is the absolute input-rate index of the analysis window
/// centre (reset() of tracker and shifter zeroes the count together). Emitted in the process()
/// call whose input reaches centre + T_max + decimation group delay, rounded up to the next hop.
struct F0Estimate {
  float f0_hz;
  float confidence;
  bool voiced;
  std::int64_t centre_sample;
};

/// Per-voice target. Output f0 at a mark of input f0 f_m is
///   f_out = (f_m * ratio)^(1 - absolute_weight) * target_hz^absolute_weight
struct VoiceTarget {
  float ratio = 1.0f;            ///< relative part: 2^(semitones/12) * detune * harmony interval
  float target_hz = 0.0f;        ///< absolute part incl. semitones and detune; held when unvoiced
  float absolute_weight = 0.0f;  ///< 0 = follow the input, 1 = fixed target
  float formant_alpha = 1.0f;    ///< grain resampling factor
  float gain = 0.0f;
  float delay_ms = 0.0f;  ///< effect delay applied after the shifter (harmony only)
  bool active = false;
};

/// Targets of every voice; voices[0] is the lead.
struct VoiceTargets {
  VoiceTarget voices[kMaxVoices];
  int count = 0;
};

/// A MIDI note event handed from the control thread to the audio thread.
struct NoteEvent {
  enum class Kind : int { On, Off } kind;
  int note;
  float velocity;  ///< Validated into (0, 1]; apply_note does not read it.
};

static_assert(std::is_trivially_copyable<VoiceEngineConfig>::value, "config must be trivial");
static_assert(std::is_trivially_copyable<PitchCorrectionConfig>::value, "config must be trivial");
static_assert(std::is_trivially_copyable<HarmonyConfig>::value, "config must be trivial");
static_assert(std::is_trivially_copyable<VocoderConfig>::value, "config must be trivial");
static_assert(std::is_trivially_copyable<CharacterConfig>::value, "config must be trivial");
static_assert(std::is_trivially_copyable<ReverbVariationConfig>::value, "config must be trivial");
static_assert(std::is_trivially_copyable<MidiConfig>::value, "config must be trivial");
static_assert(std::is_trivially_copyable<NoteEvent>::value, "SpscQueue record must be trivial");

}  // namespace sonare::editing::voice_changer
