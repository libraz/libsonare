#pragma once

// Ranges, enum spellings and field visitors shared by the voice changer config translation units.

#include <array>
#include <string>
#include <string_view>

#include "editing/voice_changer/realtime.h"
#include "util/exception.h"

namespace sonare::editing::voice_changer::detail {

// Spellings of the voice transformer enums in JSON documents: each enumerator in snake_case.
struct EnumName {
  int value;
  std::string_view name;
};

template <typename Enum>
struct EnumNames;

template <>
struct EnumNames<PitchEngine> {
  static constexpr std::array<EnumName, 2> kNames = {{{0, "grain"}, {1, "psola"}}};
};
template <>
struct EnumNames<PitchCorrectionMode> {
  static constexpr std::array<EnumName, 3> kNames = {{{0, "off"}, {1, "auto"}, {2, "robot"}}};
};
template <>
struct EnumNames<ScaleKind> {
  static constexpr std::array<EnumName, 3> kNames = {
      {{0, "major"}, {1, "natural_minor"}, {2, "chromatic"}}};
};
template <>
struct EnumNames<VocoderCarrierSource> {
  static constexpr std::array<EnumName, 2> kNames = {{{0, "internal"}, {1, "external"}}};
};
template <>
struct EnumNames<VocoderCarrierWave> {
  static constexpr std::array<EnumName, 3> kNames = {{{0, "saw"}, {1, "pulse"}, {2, "ensemble"}}};
};
template <>
struct EnumNames<CharacterType> {
  static constexpr std::array<EnumName, 5> kNames = {
      {{0, "none"}, {1, "megaphone"}, {2, "radio"}, {3, "bbd_chorus"}, {4, "strobo"}}};
};
template <>
struct EnumNames<ReverbVariation> {
  static constexpr std::array<EnumName, 4> kNames = {
      {{0, "reverb"}, {1, "echo"}, {2, "tempo_delay"}, {3, "dub_echo"}}};
};
template <>
struct EnumNames<NoteDivision> {
  static constexpr std::array<EnumName, 4> kNames = {
      {{0, "quarter"}, {1, "eighth"}, {2, "dotted_eighth"}, {3, "sixteenth"}}};
};
template <>
struct EnumNames<MidiRelease> {
  static constexpr std::array<EnumName, 2> kNames = {{{0, "hold"}, {1, "input"}}};
};

template <typename Enum>
bool enum_is_declared(Enum value) noexcept {
  for (const EnumName& row : EnumNames<Enum>::kNames) {
    if (row.value == static_cast<int>(value)) return true;
  }
  return false;
}

/// The document spelling of @p value; the first spelling for an undeclared value.
template <typename Enum>
const char* enum_spelling(Enum value) noexcept {
  for (const EnumName& row : EnumNames<Enum>::kNames) {
    if (row.value == static_cast<int>(value)) return row.name.data();
  }
  return EnumNames<Enum>::kNames[0].name.data();
}

template <typename Enum>
bool enum_from_spelling(std::string_view name, Enum* out) noexcept {
  for (const EnumName& row : EnumNames<Enum>::kNames) {
    if (row.name == name) {
      *out = static_cast<Enum>(row.value);
      return true;
    }
  }
  return false;
}

/// "'a', 'b' or 'c'" over every spelling of @p Enum.
template <typename Enum>
std::string enum_spelling_list() {
  std::string out;
  const auto& names = EnumNames<Enum>::kNames;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i > 0) out += i + 1 == names.size() ? " or " : ", ";
    out += '\'';
    out += names[i].name;
    out += '\'';
  }
  return out;
}

struct Range {
  double lo;
  double hi;
};

// Version 1 documents predate the voice transformer sections and still validate.
constexpr int kOldestPresetSchemaVersion = 1;

// The accepted ranges: the one definition behind validation, normalization and the preset schema.
constexpr Range kUnit{0.0, 1.0};
constexpr Range kBipolar{-1.0, 1.0};
constexpr Range kInputGainDb{-24.0, 24.0};
constexpr Range kOutputGainDb{-36.0, 12.0};
constexpr Range kSemitones{-24.0, 24.0};
constexpr Range kGrainSize{0.0, static_cast<double>(kMaxRetuneGrainSize)};
constexpr Range kFormantFactor{0.55, 1.65};
constexpr Range kHighpassHz{static_cast<double>(kVoiceHighpassMinHz), 300.0};
constexpr Range kEqGainDb{-static_cast<double>(kVoiceEqGainLimitDb),
                          static_cast<double>(kVoiceEqGainLimitDb)};
constexpr Range kGateThresholdDb{-90.0, -12.0};
constexpr Range kGateAttackMs{0.05, 100.0};
constexpr Range kGateReleaseMs{5.0, 1000.0};
constexpr Range kGateRangeDb{0.0, 80.0};
constexpr Range kCompThresholdDb{-60.0, 0.0};
constexpr Range kCompRatio{1.0, 20.0};
constexpr Range kCompAttackMs{0.05, 200.0};
constexpr Range kCompReleaseMs{5.0, 2000.0};
constexpr Range kMakeupGainDb{-12.0, 12.0};
constexpr Range kDeessFrequencyHz{3000.0, 12000.0};
constexpr Range kDeessThresholdDb{-60.0, -6.0};
constexpr Range kDeessRatio{1.0, 20.0};
constexpr Range kDeessRangeDb{0.0, 24.0};
constexpr Range kReverbMix{0.0, 0.45};
constexpr Range kReverbTimeMs{40.0, static_cast<double>(RealtimeVoiceChanger::kMaxReverbTimeMs)};
// True-peak headroom: the sample-domain ceiling stays one dB below full scale.
constexpr Range kLimiterCeilingDb{-12.0, -1.0};
constexpr Range kLimiterReleaseMs{1.0, 500.0};
// The ISP stage detects peaks at 4x oversampling, so its ceiling may reach 0 dBTP.
constexpr Range kIspCeilingDbtp{-12.0, 0.0};
constexpr Range kReverbSeed{-2147483648.0, 2147483647.0};
constexpr Range kF0MinHz{50.0, 200.0};
constexpr Range kCorrectionSpeedMs{0.0, 400.0};
constexpr Range kKeyRoot{0.0, 11.0};
constexpr Range kRobotOctave{-1.0, 1.0};
constexpr Range kHarmonyVoices{0.0, static_cast<double>(kMaxHarmonyVoices)};
constexpr Range kHarmonyStep{-7.0, 7.0};
constexpr Range kDetuneCents{0.0, 30.0};
constexpr Range kHarmonyDelayMs{0.0, static_cast<double>(kHarmonyMaxDelayMs)};
constexpr Range kVocoderBands{8.0, 32.0};
constexpr Range kVocoderAttackMs{0.5, 50.0};
constexpr Range kVocoderReleaseMs{2.0, 300.0};
constexpr Range kSpaceDelayMs{1.0, static_cast<double>(kMaxSpaceDelayMs)};
constexpr Range kSpaceFeedback{0.0, 0.95};
constexpr Range kTempoBpm{30.0, 300.0};

/// Calls @p v(path, field, range, fallback) for every float field; fallback replaces a non-finite
/// one.
template <class Config, class Visit>
void visit_scalar_fields(Config& c, Visit&& v) {
  v("inputGainDb", c.input_gain_db, kInputGainDb, 0.0f);
  v("outputGainDb", c.output_gain_db, kOutputGainDb, 0.0f);
  v("wetMix", c.wet_mix, kUnit, 1.0f);
  v("retune.semitones", c.retune.semitones, kSemitones, 0.0f);
  v("retune.mix", c.retune.mix, kUnit, 1.0f);
  v("formant.factor", c.formant.factor, kFormantFactor, 1.0f);
  v("formant.amount", c.formant.amount, kUnit, 1.0f);
  v("formant.body", c.formant.body, kBipolar, 0.0f);
  v("formant.brightness", c.formant.brightness, kBipolar, 0.0f);
  v("formant.nasal", c.formant.nasal, kBipolar, 0.0f);
  v("eq.highpassHz", c.eq.highpass_hz, kHighpassHz, 80.0f);
  v("eq.bodyDb", c.eq.body_db, kEqGainDb, 0.0f);
  v("eq.presenceDb", c.eq.presence_db, kEqGainDb, 0.0f);
  v("eq.airDb", c.eq.air_db, kEqGainDb, 0.0f);
  v("gate.thresholdDb", c.gate.threshold_db, kGateThresholdDb, -55.0f);
  v("gate.attackMs", c.gate.attack_ms, kGateAttackMs, 2.0f);
  v("gate.releaseMs", c.gate.release_ms, kGateReleaseMs, 100.0f);
  v("gate.rangeDb", c.gate.range_db, kGateRangeDb, 18.0f);
  v("compressor.thresholdDb", c.compressor.threshold_db, kCompThresholdDb, -22.0f);
  v("compressor.ratio", c.compressor.ratio, kCompRatio, 2.5f);
  v("compressor.attackMs", c.compressor.attack_ms, kCompAttackMs, 6.0f);
  v("compressor.releaseMs", c.compressor.release_ms, kCompReleaseMs, 90.0f);
  v("compressor.makeupGainDb", c.compressor.makeup_gain_db, kMakeupGainDb, 1.0f);
  v("deesser.frequencyHz", c.deesser.frequency_hz, kDeessFrequencyHz, 7200.0f);
  v("deesser.thresholdDb", c.deesser.threshold_db, kDeessThresholdDb, -28.0f);
  v("deesser.ratio", c.deesser.ratio, kDeessRatio, 4.0f);
  v("deesser.rangeDb", c.deesser.range_db, kDeessRangeDb, 8.0f);
  v("reverb.mix", c.reverb.mix, kReverbMix, 0.04f);
  v("reverb.timeMs", c.reverb.time_ms, kReverbTimeMs, 320.0f);
  v("reverb.damping", c.reverb.damping, kUnit, 0.55f);
  v("limiter.ceilingDb", c.limiter.ceiling_db, kLimiterCeilingDb, -1.0f);
  v("limiter.releaseMs", c.limiter.release_ms, kLimiterReleaseMs, 50.0f);
  v("limiter.ispCeilingDbtp", c.limiter.isp_ceiling_dbtp, kIspCeilingDbtp, -1.0f);
  v("voiceEngine.f0MinHz", c.voice_engine.f0_min_hz, kF0MinHz, 70.0f);
  v("pitchCorrection.amount", c.pitch_correction.amount, kUnit, 1.0f);
  v("pitchCorrection.speedMs", c.pitch_correction.speed_ms, kCorrectionSpeedMs, 20.0f);
  v("harmony.level", c.harmony.level, kUnit, 0.7f);
  v("harmony.leadLevel", c.harmony.lead_level, kUnit, 1.0f);
  v("harmony.detuneCents", c.harmony.detune_cents, kDetuneCents, 6.0f);
  v("harmony.delayMs", c.harmony.delay_ms, kHarmonyDelayMs, 12.0f);
  v("vocoder.attackMs", c.vocoder.attack_ms, kVocoderAttackMs, 2.0f);
  v("vocoder.releaseMs", c.vocoder.release_ms, kVocoderReleaseMs, 20.0f);
  v("vocoder.sibilance", c.vocoder.sibilance, kUnit, 0.3f);
  v("vocoder.ensembleDetuneCents", c.vocoder.ensemble_detune_cents, kDetuneCents, 12.0f);
  v("vocoder.level", c.vocoder.level, kUnit, 1.0f);
  v("character.amount", c.character.amount, kUnit, 0.5f);
  v("character.tone", c.character.tone, kBipolar, 0.0f);
  v("reverbVariation.delayMs", c.reverb_variation.delay_ms, kSpaceDelayMs, 250.0f);
  v("reverbVariation.feedback", c.reverb_variation.feedback, kSpaceFeedback, 0.35f);
  v("reverbVariation.tempoBpm", c.reverb_variation.tempo_bpm, kTempoBpm, 120.0f);
}

/// Calls @p v(path, field, range) for every integer field of the voice transformer sections.
template <class Config, class Visit>
void visit_integer_fields(Config& c, Visit&& v) {
  v("pitchCorrection.keyRoot", c.pitch_correction.key_root, kKeyRoot);
  v("pitchCorrection.robotOctave", c.pitch_correction.robot_octave, kRobotOctave);
  v("harmony.voices", c.harmony.voices, kHarmonyVoices);
  v("harmony.steps[0]", c.harmony.steps[0], kHarmonyStep);
  v("harmony.steps[1]", c.harmony.steps[1], kHarmonyStep);
  v("harmony.steps[2]", c.harmony.steps[2], kHarmonyStep);
  v("vocoder.bands", c.vocoder.bands, kVocoderBands);
}

/// Calls @p v(path, declared) for every enum field; @p declared says whether its value is named.
template <class Config, class Visit>
void visit_enum_fields(Config& c, Visit&& v) {
  v("voiceEngine.engine", enum_is_declared(c.voice_engine.engine));
  v("pitchCorrection.mode", enum_is_declared(c.pitch_correction.mode));
  v("pitchCorrection.scale", enum_is_declared(c.pitch_correction.scale));
  v("vocoder.carrierSource", enum_is_declared(c.vocoder.carrier_source));
  v("vocoder.carrierWave", enum_is_declared(c.vocoder.carrier_wave));
  v("character.type", enum_is_declared(c.character.type));
  v("reverbVariation.variation", enum_is_declared(c.reverb_variation.variation));
  v("reverbVariation.division", enum_is_declared(c.reverb_variation.division));
  v("midi.release", enum_is_declared(c.midi.release));
}

/// Returns an undeclared enum value to the field's default.
template <typename Enum>
void default_if_undeclared(Enum& value, Enum fallback) noexcept {
  if (!enum_is_declared(value)) value = fallback;
}

/// Single source of truth for preset metadata. Adding a new preset is a
/// one-row table edit + a switch arm in @ref realtime_voice_changer_preset
/// (which carries the divergent default values); the id/display-name
/// accessors below all derive from this table so they cannot drift.
struct PresetMetadata {
  VoiceCharacterPreset preset;
  std::string_view id;            ///< Stable identifier exposed across all bindings.
  std::string_view display_name;  ///< Human-readable label used in UI surfaces.
  std::string_view category;      ///< Schema category emitted with the preset document.
};

/// Normalizes @p input into range; defined in config.cpp.
RealtimeVoiceChangerConfig normalize_config(const RealtimeVoiceChangerConfig& input);

const PresetMetadata& preset_metadata(VoiceCharacterPreset preset) noexcept;

const char* realtime_voice_changer_preset_display_name(VoiceCharacterPreset preset) noexcept;

VoiceCharacterPreset preset_from_id_or_throw(std::string_view id, ErrorCode code);

}  // namespace sonare::editing::voice_changer::detail
