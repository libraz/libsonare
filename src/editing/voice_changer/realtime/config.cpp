#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>

#include "editing/voice_changer/realtime.h"
#include "effects/formant_warp.h"
#include "util/exception.h"
#include "util/json_budget.h"
#include "util/json_schema.h"
#include "util/number_format.h"

namespace sonare::editing::voice_changer {
namespace {

float finite_or(float value, float fallback) noexcept {
  return std::isfinite(value) ? value : fallback;
}

// A present key of the wrong type (a JSON null included) is refused naming its path; only an
// absent key keeps the fallback. `section` is the enclosing object's key, "" at the top level.
[[noreturn]] void refuse_type(const char* section, const char* key, const char* expected) {
  const std::string path = section[0] == '\0' ? std::string(key) : std::string(section) + "." + key;
  throw SonareException(ErrorCode::InvalidFormat, path + " must be " + expected);
}

// The nested object under @p key, or null when absent; a present non-object is refused.
const sonare::util::json::Value* object_section(const sonare::util::json::Value& object,
                                                const char* key) {
  const auto* value = object.find(key);
  if (value != nullptr && !value->is_object()) refuse_type("", key, "an object");
  return value;
}

float object_number(const sonare::util::json::Value& object, const char* section, const char* key,
                    float fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  if (!value->is_number()) refuse_type(section, key, "a number");
  return finite_or(value->as_float(), fallback);
}

int json_number_to_int(double n, int fallback) noexcept {
  if (!std::isfinite(n)) return fallback;
  // Clamp to the representable int range BEFORE the cast: a finite JSON number
  // outside [INT_MIN, INT_MAX] (e.g. a 1e12 reverb seed) would otherwise be an
  // out-of-range static_cast<int>, which is undefined behaviour.
  const double clamped = std::clamp(n, static_cast<double>(std::numeric_limits<int>::min()),
                                    static_cast<double>(std::numeric_limits<int>::max()));
  return static_cast<int>(clamped);
}

int object_int(const sonare::util::json::Value& object, const char* section, const char* key,
               int fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  if (!value->is_number()) refuse_type(section, key, "a number");
  return json_number_to_int(value->as_number(), fallback);
}

// The harmony steps as an array of up to kMaxHarmonyVoices numbers; entries past its end keep
// their current value.
void object_steps(const sonare::util::json::Value& object, const char* section, const char* key,
                  int (&steps)[kMaxHarmonyVoices]) {
  const auto* value = object.find(key);
  if (!value) return;
  if (!value->is_array()) refuse_type(section, key, "an array of numbers");
  const auto& items = value->as_array();
  if (items.size() > static_cast<std::size_t>(kMaxHarmonyVoices)) {
    throw SonareException(ErrorCode::InvalidFormat,
                          std::string(section) + "." + key + " must have at most " +
                              std::to_string(kMaxHarmonyVoices) + " entries");
  }
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (!items[i].is_number()) refuse_type(section, key, "an array of numbers");
    steps[i] = json_number_to_int(items[i].as_number(), steps[i]);
  }
}

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

template <typename Enum>
Enum object_enum(const sonare::util::json::Value& object, const char* section, const char* key,
                 Enum fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  if (!value->is_string()) refuse_type(section, key, "a string");
  Enum parsed = fallback;
  if (!enum_from_spelling(value->as_string(), &parsed)) {
    refuse_type(section, key, enum_spelling_list<Enum>().c_str());
  }
  return parsed;
}

FormantMode object_formant_mode(const sonare::util::json::Value& object, const char* section,
                                const char* key, FormantMode fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  if (!value->is_string()) refuse_type(section, key, "a string");
  return parse_formant_mode(value->as_string());
}

bool object_bool(const sonare::util::json::Value& object, const char* section, const char* key,
                 bool fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  if (!value->is_bool()) refuse_type(section, key, "a boolean");
  return value->as_bool();
}

void dump_number(std::string& out, float value) {
  // max_digits10 (17 for IEEE-754 double) ensures lossless roundtrip — matches
  // util/json::dump_value. The formatting is locale-independent, so the decimal
  // separator is always "." regardless of LC_NUMERIC.
  out += sonare::util::format_general(static_cast<double>(value),
                                      std::numeric_limits<double>::max_digits10);
}

void dump_field(std::string& out, const char* key, float value, bool last = false) {
  out += '"';
  out += key;
  out += "\":";
  dump_number(out, value);
  if (!last) out += ',';
}

void dump_int_field(std::string& out, const char* key, int value, bool last = false) {
  out += '"';
  out += key;
  out += "\":";
  out += std::to_string(value);
  if (!last) out += ',';
}

void dump_string_field(std::string& out, const char* key, const char* value, bool last = false) {
  out += '"';
  out += key;
  out += "\":\"";
  out += value;
  out += '"';
  if (!last) out += ',';
}

void dump_bool_field(std::string& out, const char* key, bool value, bool last = false) {
  out += '"';
  out += key;
  out += "\":";
  out += (value ? "true" : "false");
  if (!last) out += ',';
}

void dump_dsp_section(std::string& out, const RealtimeVoiceChangerConfig& c) {
  out += "\"dsp\":{";
  dump_field(out, "inputGainDb", c.input_gain_db);
  dump_field(out, "outputGainDb", c.output_gain_db);
  dump_field(out, "wetMix", c.wet_mix);
  out += "\"retune\":{";
  dump_field(out, "semitones", c.retune.semitones);
  dump_field(out, "mix", c.retune.mix);
  dump_int_field(out, "grainSize", c.retune.grain_size, true);
  out += "},\"formant\":{";
  dump_field(out, "factor", c.formant.factor);
  dump_field(out, "amount", c.formant.amount);
  dump_field(out, "body", c.formant.body);
  dump_field(out, "brightness", c.formant.brightness);
  dump_field(out, "nasal", c.formant.nasal);
  out += "\"mode\":\"";
  out += formant_mode_name(c.formant_mode);
  out += "\"},\"eq\":{";
  dump_field(out, "highpassHz", c.eq.highpass_hz);
  dump_field(out, "bodyDb", c.eq.body_db);
  dump_field(out, "presenceDb", c.eq.presence_db);
  dump_field(out, "airDb", c.eq.air_db, true);
  out += "},\"gate\":{";
  dump_field(out, "thresholdDb", c.gate.threshold_db);
  dump_field(out, "attackMs", c.gate.attack_ms);
  dump_field(out, "releaseMs", c.gate.release_ms);
  dump_field(out, "rangeDb", c.gate.range_db, true);
  out += "},\"compressor\":{";
  dump_field(out, "thresholdDb", c.compressor.threshold_db);
  dump_field(out, "ratio", c.compressor.ratio);
  dump_field(out, "attackMs", c.compressor.attack_ms);
  dump_field(out, "releaseMs", c.compressor.release_ms);
  dump_field(out, "makeupGainDb", c.compressor.makeup_gain_db, true);
  out += "},\"deesser\":{";
  dump_field(out, "frequencyHz", c.deesser.frequency_hz);
  dump_field(out, "thresholdDb", c.deesser.threshold_db);
  dump_field(out, "ratio", c.deesser.ratio);
  dump_field(out, "rangeDb", c.deesser.range_db, true);
  out += "},\"reverb\":{";
  dump_field(out, "mix", c.reverb.mix);
  dump_field(out, "timeMs", c.reverb.time_ms);
  dump_field(out, "damping", c.reverb.damping);
  dump_int_field(out, "seed", c.reverb.seed, true);
  out += "},\"limiter\":{";
  dump_field(out, "ceilingDb", c.limiter.ceiling_db);
  dump_field(out, "releaseMs", c.limiter.release_ms);
  dump_bool_field(out, "enableIspLimiter", c.limiter.enable_isp_limiter);
  dump_field(out, "ispCeilingDbtp", c.limiter.isp_ceiling_dbtp, true);
  out += "},\"voiceEngine\":{";
  dump_string_field(out, "engine", enum_spelling(c.voice_engine.engine));
  dump_field(out, "f0MinHz", c.voice_engine.f0_min_hz, true);
  out += "},\"pitchCorrection\":{";
  dump_string_field(out, "mode", enum_spelling(c.pitch_correction.mode));
  dump_field(out, "amount", c.pitch_correction.amount);
  dump_field(out, "speedMs", c.pitch_correction.speed_ms);
  dump_int_field(out, "keyRoot", c.pitch_correction.key_root);
  dump_string_field(out, "scale", enum_spelling(c.pitch_correction.scale));
  dump_int_field(out, "robotOctave", c.pitch_correction.robot_octave, true);
  out += "},\"harmony\":{";
  dump_int_field(out, "voices", c.harmony.voices);
  out += "\"steps\":[";
  for (int i = 0; i < kMaxHarmonyVoices; ++i) {
    if (i > 0) out += ',';
    out += std::to_string(c.harmony.steps[i]);
  }
  out += "],";
  dump_field(out, "level", c.harmony.level);
  dump_field(out, "leadLevel", c.harmony.lead_level);
  dump_field(out, "detuneCents", c.harmony.detune_cents);
  dump_field(out, "delayMs", c.harmony.delay_ms, true);
  out += "},\"vocoder\":{";
  dump_bool_field(out, "enabled", c.vocoder.enabled);
  dump_int_field(out, "bands", c.vocoder.bands);
  dump_field(out, "attackMs", c.vocoder.attack_ms);
  dump_field(out, "releaseMs", c.vocoder.release_ms);
  dump_field(out, "sibilance", c.vocoder.sibilance);
  dump_string_field(out, "carrierSource", enum_spelling(c.vocoder.carrier_source));
  dump_string_field(out, "carrierWave", enum_spelling(c.vocoder.carrier_wave));
  dump_field(out, "ensembleDetuneCents", c.vocoder.ensemble_detune_cents);
  dump_field(out, "level", c.vocoder.level, true);
  out += "},\"character\":{";
  dump_string_field(out, "type", enum_spelling(c.character.type));
  dump_field(out, "amount", c.character.amount);
  dump_field(out, "tone", c.character.tone, true);
  out += "},\"reverbVariation\":{";
  dump_string_field(out, "variation", enum_spelling(c.reverb_variation.variation));
  dump_field(out, "delayMs", c.reverb_variation.delay_ms);
  dump_field(out, "feedback", c.reverb_variation.feedback);
  dump_field(out, "tempoBpm", c.reverb_variation.tempo_bpm);
  dump_string_field(out, "division", enum_spelling(c.reverb_variation.division), true);
  out += "},\"midi\":{";
  dump_string_field(out, "release", enum_spelling(c.midi.release), true);
  out += "}}";
}

std::string trim_copy(std::string_view text) {
  std::size_t first = 0;
  while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
  std::size_t last = text.size();
  while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
  return std::string(text.substr(first, last - first));
}

// Validator helpers (has_allowed_keys, require_key, require_number,
// require_string) now live in util/json_schema.h. Pull them into this anonymous
// namespace under their old unqualified names so the validate_*() helpers below
// stay readable.
using sonare::util::json::schema::has_allowed_keys;
using sonare::util::json::schema::require_key;
using sonare::util::json::schema::require_number;
using sonare::util::json::schema::require_string;

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

/// Forces @p input into range (non-finite fields take their default); only preset construction and
/// the tolerant JSON entry point may clamp.
RealtimeVoiceChangerConfig normalize_config(const RealtimeVoiceChangerConfig& input) {
  RealtimeVoiceChangerConfig c = input;
  visit_scalar_fields(c, [](const char*, float& v, const Range& r, float fallback) {
    if (!std::isfinite(v)) v = fallback;
    v = std::clamp(v, static_cast<float>(r.lo), static_cast<float>(r.hi));
  });
  c.retune.grain_size = std::clamp(c.retune.grain_size, 0, kMaxRetuneGrainSize);
  if (c.formant_mode != FormantMode::Absolute) c.formant_mode = FormantMode::Relative;
  visit_integer_fields(c, [](const char*, int& v, const Range& r) {
    v = std::clamp(v, static_cast<int>(r.lo), static_cast<int>(r.hi));
  });
  const RealtimeVoiceChangerConfig defaults;
  default_if_undeclared(c.voice_engine.engine, defaults.voice_engine.engine);
  default_if_undeclared(c.pitch_correction.mode, defaults.pitch_correction.mode);
  default_if_undeclared(c.pitch_correction.scale, defaults.pitch_correction.scale);
  default_if_undeclared(c.vocoder.carrier_source, defaults.vocoder.carrier_source);
  default_if_undeclared(c.vocoder.carrier_wave, defaults.vocoder.carrier_wave);
  default_if_undeclared(c.character.type, defaults.character.type);
  default_if_undeclared(c.reverb_variation.variation, defaults.reverb_variation.variation);
  default_if_undeclared(c.reverb_variation.division, defaults.reverb_variation.division);
  default_if_undeclared(c.midi.release, defaults.midi.release);
  return c;
}

bool require_in(const sonare::util::json::Value& object, const char* key, const Range& range,
                const char* path, std::string* error, bool integer = false) {
  return require_number(object, key, range.lo, range.hi, path, error, integer);
}

/// Verifies the preset id matches the schema regex `^[a-z0-9][a-z0-9._-]*$` and
/// is within the 1..96 length bound. Kept inline because @c require_string in
/// util/json_schema.h is intentionally regex-free — only this one field needs
/// the slug constraint, so embedding a generic regex matcher there would be
/// dead weight.
inline bool is_valid_preset_id(std::string_view s) noexcept {
  if (s.empty() || s.size() > 96) return false;
  const auto first = static_cast<unsigned char>(s.front());
  const bool first_ok = (first >= 'a' && first <= 'z') || (first >= '0' && first <= '9');
  if (!first_ok) return false;
  for (std::size_t i = 1; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    const bool ok =
        (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
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

constexpr std::array<PresetMetadata, 12> kPresetMetadata = {{
    {VoiceCharacterPreset::NeutralMonitor, "neutral-monitor", "Neutral Monitor", "monitor"},
    {VoiceCharacterPreset::BrightIdol, "bright-idol", "Bright Idol", "bright"},
    {VoiceCharacterPreset::SoftWhisper, "soft-whisper", "Soft Whisper", "soft"},
    {VoiceCharacterPreset::DeepNarrator, "deep-narrator", "Deep Narrator", "deep"},
    {VoiceCharacterPreset::RobotMascot, "robot-mascot", "Robot Mascot", "robot"},
    {VoiceCharacterPreset::DarkVillain, "dark-villain", "Dark Villain", "dark"},
    {VoiceCharacterPreset::HardTune, "hard-tune", "Hard Tune", "tune"},
    {VoiceCharacterPreset::RobotMonotone, "robot-monotone", "Robot Monotone", "tune"},
    {VoiceCharacterPreset::ChoirHarmony, "choir-harmony", "Choir Harmony", "harmony"},
    {VoiceCharacterPreset::VocoderStrings, "vocoder-strings", "Vocoder Strings", "vocoder"},
    {VoiceCharacterPreset::Megaphone, "megaphone", "Megaphone", "character"},
    {VoiceCharacterPreset::RadioVoice, "radio-voice", "Radio Voice", "character"},
}};

// Compile-time check that every enumerator gets a metadata row. Adding a new
// preset value to VoiceCharacterPreset without updating the table now fails
// the build rather than silently falling back to NeutralMonitor at runtime.
static_assert(kPresetMetadata.size() ==
                  static_cast<std::size_t>(VoiceCharacterPreset::RadioVoice) + 1,
              "kPresetMetadata is out of sync with VoiceCharacterPreset");

const PresetMetadata& preset_metadata(VoiceCharacterPreset preset) noexcept {
  for (const auto& row : kPresetMetadata) {
    if (row.preset == preset) return row;
  }
  // Unreachable for any value of the enum (every case is in the table).
  return kPresetMetadata[0];
}

const char* realtime_voice_changer_preset_display_name(VoiceCharacterPreset preset) noexcept {
  return preset_metadata(preset).display_name.data();
}

template <typename Enum>
bool require_enum(const sonare::util::json::Value& object, const char* key, const char* path,
                  std::string* error) {
  const auto* value = object.find(key);
  Enum parsed{};
  if (value != nullptr && value->is_string() && enum_from_spelling(value->as_string(), &parsed)) {
    return true;
  }
  if (error) {
    *error = "field must be " + enum_spelling_list<Enum>() + ": " + path + "." + key;
  }
  return false;
}

bool require_bool(const sonare::util::json::Value& object, const char* key, const char* path,
                  std::string* error) {
  const auto* value = object.find(key);
  if (value != nullptr && value->is_bool()) return true;
  if (error) *error = std::string("field must be a boolean: ") + path + "." + key;
  return false;
}

// Every key of a voice transformer section is required once the section is present.
bool require_section_keys(const sonare::util::json::Value& section,
                          std::initializer_list<const char*> keys, const char* path,
                          std::string* error) {
  for (const char* key : keys) {
    if (!require_key(section, key, path, error)) return false;
  }
  return true;
}

// The voice transformer sections of a dsp object. Present sections are validated in full.
bool validate_transform_sections(const sonare::util::json::Value& dsp, std::string* error) {
  if (const auto* section = dsp.find("voiceEngine")) {
    constexpr const char* kPath = "dsp.voiceEngine";
    if (!has_allowed_keys(*section, {"engine", "f0MinHz"}, kPath, error) ||
        !require_section_keys(*section, {"engine", "f0MinHz"}, kPath, error) ||
        !require_enum<PitchEngine>(*section, "engine", kPath, error) ||
        !require_in(*section, "f0MinHz", kF0MinHz, kPath, error)) {
      return false;
    }
  }
  if (const auto* section = dsp.find("pitchCorrection")) {
    constexpr const char* kPath = "dsp.pitchCorrection";
    if (!has_allowed_keys(*section,
                          {"mode", "amount", "speedMs", "keyRoot", "scale", "robotOctave"}, kPath,
                          error) ||
        !require_section_keys(*section,
                              {"mode", "amount", "speedMs", "keyRoot", "scale", "robotOctave"},
                              kPath, error) ||
        !require_enum<PitchCorrectionMode>(*section, "mode", kPath, error) ||
        !require_in(*section, "amount", kUnit, kPath, error) ||
        !require_in(*section, "speedMs", kCorrectionSpeedMs, kPath, error) ||
        !require_in(*section, "keyRoot", kKeyRoot, kPath, error, true) ||
        !require_enum<ScaleKind>(*section, "scale", kPath, error) ||
        !require_in(*section, "robotOctave", kRobotOctave, kPath, error, true)) {
      return false;
    }
  }
  if (const auto* section = dsp.find("harmony")) {
    constexpr const char* kPath = "dsp.harmony";
    if (!has_allowed_keys(*section,
                          {"voices", "steps", "level", "leadLevel", "detuneCents", "delayMs"},
                          kPath, error) ||
        !require_section_keys(*section,
                              {"voices", "steps", "level", "leadLevel", "detuneCents", "delayMs"},
                              kPath, error) ||
        !require_in(*section, "voices", kHarmonyVoices, kPath, error, true) ||
        !require_in(*section, "level", kUnit, kPath, error) ||
        !require_in(*section, "leadLevel", kUnit, kPath, error) ||
        !require_in(*section, "detuneCents", kDetuneCents, kPath, error) ||
        !require_in(*section, "delayMs", kHarmonyDelayMs, kPath, error)) {
      return false;
    }
    // One step per harmony voice at least, three at most.
    const auto& steps = *section->find("steps");
    const auto voices = static_cast<std::size_t>(section->find("voices")->as_number());
    bool steps_ok = steps.is_array() && steps.as_array().size() >= voices &&
                    steps.as_array().size() <= static_cast<std::size_t>(kMaxHarmonyVoices);
    if (steps_ok) {
      for (const auto& step : steps.as_array()) {
        const double n = step.is_number() ? step.as_number() : kHarmonyStep.hi + 1.0;
        steps_ok = steps_ok && std::floor(n) == n && n >= kHarmonyStep.lo && n <= kHarmonyStep.hi;
      }
    }
    if (!steps_ok) {
      if (error) {
        *error = "field must be an array of voices to 3 integers in [-7, 7]: dsp.harmony.steps";
      }
      return false;
    }
  }
  if (const auto* section = dsp.find("vocoder")) {
    constexpr const char* kPath = "dsp.vocoder";
    if (!has_allowed_keys(*section,
                          {"enabled", "bands", "attackMs", "releaseMs", "sibilance",
                           "carrierSource", "carrierWave", "ensembleDetuneCents", "level"},
                          kPath, error) ||
        !require_section_keys(*section,
                              {"enabled", "bands", "attackMs", "releaseMs", "sibilance",
                               "carrierSource", "carrierWave", "ensembleDetuneCents", "level"},
                              kPath, error) ||
        !require_bool(*section, "enabled", kPath, error) ||
        !require_in(*section, "bands", kVocoderBands, kPath, error, true) ||
        !require_in(*section, "attackMs", kVocoderAttackMs, kPath, error) ||
        !require_in(*section, "releaseMs", kVocoderReleaseMs, kPath, error) ||
        !require_in(*section, "sibilance", kUnit, kPath, error) ||
        !require_enum<VocoderCarrierSource>(*section, "carrierSource", kPath, error) ||
        !require_enum<VocoderCarrierWave>(*section, "carrierWave", kPath, error) ||
        !require_in(*section, "ensembleDetuneCents", kDetuneCents, kPath, error) ||
        !require_in(*section, "level", kUnit, kPath, error)) {
      return false;
    }
  }
  if (const auto* section = dsp.find("character")) {
    constexpr const char* kPath = "dsp.character";
    if (!has_allowed_keys(*section, {"type", "amount", "tone"}, kPath, error) ||
        !require_section_keys(*section, {"type", "amount", "tone"}, kPath, error) ||
        !require_enum<CharacterType>(*section, "type", kPath, error) ||
        !require_in(*section, "amount", kUnit, kPath, error) ||
        !require_in(*section, "tone", kBipolar, kPath, error)) {
      return false;
    }
  }
  if (const auto* section = dsp.find("reverbVariation")) {
    constexpr const char* kPath = "dsp.reverbVariation";
    if (!has_allowed_keys(*section, {"variation", "delayMs", "feedback", "tempoBpm", "division"},
                          kPath, error) ||
        !require_section_keys(
            *section, {"variation", "delayMs", "feedback", "tempoBpm", "division"}, kPath, error) ||
        !require_enum<ReverbVariation>(*section, "variation", kPath, error) ||
        !require_in(*section, "delayMs", kSpaceDelayMs, kPath, error) ||
        !require_in(*section, "feedback", kSpaceFeedback, kPath, error) ||
        !require_in(*section, "tempoBpm", kTempoBpm, kPath, error) ||
        !require_enum<NoteDivision>(*section, "division", kPath, error)) {
      return false;
    }
  }
  if (const auto* section = dsp.find("midi")) {
    constexpr const char* kPath = "dsp.midi";
    if (!has_allowed_keys(*section, {"release"}, kPath, error) ||
        !require_section_keys(*section, {"release"}, kPath, error) ||
        !require_enum<MidiRelease>(*section, "release", kPath, error)) {
      return false;
    }
  }
  return true;
}

constexpr std::array<const char*, 7> kTransformSections = {
    "voiceEngine", "pitchCorrection", "harmony", "vocoder", "character", "reverbVariation", "midi"};

// A version 1 document predates the voice transformer sections and may omit them; version 2
// requires them.
bool validate_dsp_section(const sonare::util::json::Value& dsp, int schema_version,
                          std::string* error) {
  if (!has_allowed_keys(
          dsp,
          {"inputGainDb", "outputGainDb", "wetMix", "retune", "formant", "eq", "gate", "compressor",
           "deesser", "reverb", "limiter", "voiceEngine", "pitchCorrection", "harmony", "vocoder",
           "character", "reverbVariation", "midi"},
          "dsp", error)) {
    return false;
  }
  for (const char* key :
       {"retune", "formant", "eq", "gate", "compressor", "deesser", "reverb", "limiter"}) {
    if (!require_key(dsp, key, "dsp", error)) return false;
  }
  if (schema_version >= 2) {
    for (const char* key : kTransformSections) {
      if (!require_key(dsp, key, "dsp", error)) return false;
    }
  }
  if (!validate_transform_sections(dsp, error)) return false;
  if (dsp.find("inputGainDb") && !require_in(dsp, "inputGainDb", kInputGainDb, "dsp", error)) {
    return false;
  }
  if (dsp.find("outputGainDb") && !require_in(dsp, "outputGainDb", kOutputGainDb, "dsp", error)) {
    return false;
  }
  if (dsp.find("wetMix") && !require_in(dsp, "wetMix", kUnit, "dsp", error)) return false;

  const auto& retune = *dsp.find("retune");
  if (!has_allowed_keys(retune, {"semitones", "mix", "grainSize"}, "dsp.retune", error)) {
    return false;
  }
  for (const char* key : {"semitones", "mix", "grainSize"}) {
    if (!require_key(retune, key, "dsp.retune", error)) return false;
  }
  if (!require_in(retune, "semitones", kSemitones, "dsp.retune", error)) return false;
  if (!require_in(retune, "mix", kUnit, "dsp.retune", error)) return false;
  if (!require_in(retune, "grainSize", kGrainSize, "dsp.retune", error, true)) {
    return false;
  }

  const auto& formant = *dsp.find("formant");
  if (!has_allowed_keys(formant, {"factor", "amount", "body", "brightness", "nasal", "mode"},
                        "dsp.formant", error)) {
    return false;
  }
  for (const char* key : {"factor", "amount", "body", "brightness", "nasal"}) {
    if (!require_key(formant, key, "dsp.formant", error)) return false;
  }
  if (!require_in(formant, "factor", kFormantFactor, "dsp.formant", error)) return false;
  if (!require_in(formant, "amount", kUnit, "dsp.formant", error)) return false;
  if (!require_in(formant, "body", kBipolar, "dsp.formant", error)) return false;
  if (!require_in(formant, "brightness", kBipolar, "dsp.formant", error)) return false;
  if (!require_in(formant, "nasal", kBipolar, "dsp.formant", error)) return false;
  if (const auto* mode = formant.find("mode")) {
    if (!mode->is_string() ||
        (mode->as_string() != "relative" && mode->as_string() != "absolute")) {
      if (error) *error = "field must be 'relative' or 'absolute': dsp.formant.mode";
      return false;
    }
  }

  const auto& eq = *dsp.find("eq");
  if (!has_allowed_keys(eq, {"highpassHz", "bodyDb", "presenceDb", "airDb"}, "dsp.eq", error)) {
    return false;
  }
  for (const char* key : {"highpassHz", "bodyDb", "presenceDb", "airDb"}) {
    if (!require_key(eq, key, "dsp.eq", error)) return false;
  }
  if (!require_in(eq, "highpassHz", kHighpassHz, "dsp.eq", error)) return false;
  if (!require_in(eq, "bodyDb", kEqGainDb, "dsp.eq", error)) return false;
  if (!require_in(eq, "presenceDb", kEqGainDb, "dsp.eq", error)) return false;
  if (!require_in(eq, "airDb", kEqGainDb, "dsp.eq", error)) return false;

  const auto& gate = *dsp.find("gate");
  if (!has_allowed_keys(gate, {"thresholdDb", "attackMs", "releaseMs", "rangeDb"}, "dsp.gate",
                        error)) {
    return false;
  }
  for (const char* key : {"thresholdDb", "attackMs", "releaseMs", "rangeDb"}) {
    if (!require_key(gate, key, "dsp.gate", error)) return false;
  }
  if (!require_in(gate, "thresholdDb", kGateThresholdDb, "dsp.gate", error)) return false;
  if (!require_in(gate, "attackMs", kGateAttackMs, "dsp.gate", error)) return false;
  if (!require_in(gate, "releaseMs", kGateReleaseMs, "dsp.gate", error)) return false;
  if (!require_in(gate, "rangeDb", kGateRangeDb, "dsp.gate", error)) return false;

  const auto& compressor = *dsp.find("compressor");
  if (!has_allowed_keys(compressor,
                        {"thresholdDb", "ratio", "attackMs", "releaseMs", "makeupGainDb"},
                        "dsp.compressor", error)) {
    return false;
  }
  for (const char* key : {"thresholdDb", "ratio", "attackMs", "releaseMs", "makeupGainDb"}) {
    if (!require_key(compressor, key, "dsp.compressor", error)) return false;
  }
  if (!require_in(compressor, "thresholdDb", kCompThresholdDb, "dsp.compressor", error)) {
    return false;
  }
  if (!require_in(compressor, "ratio", kCompRatio, "dsp.compressor", error)) return false;
  if (!require_in(compressor, "attackMs", kCompAttackMs, "dsp.compressor", error)) return false;
  if (!require_in(compressor, "releaseMs", kCompReleaseMs, "dsp.compressor", error)) {
    return false;
  }
  if (!require_in(compressor, "makeupGainDb", kMakeupGainDb, "dsp.compressor", error)) {
    return false;
  }

  const auto& deesser = *dsp.find("deesser");
  if (!has_allowed_keys(deesser, {"frequencyHz", "thresholdDb", "ratio", "rangeDb"}, "dsp.deesser",
                        error)) {
    return false;
  }
  for (const char* key : {"frequencyHz", "thresholdDb", "ratio", "rangeDb"}) {
    if (!require_key(deesser, key, "dsp.deesser", error)) return false;
  }
  if (!require_in(deesser, "frequencyHz", kDeessFrequencyHz, "dsp.deesser", error)) {
    return false;
  }
  if (!require_in(deesser, "thresholdDb", kDeessThresholdDb, "dsp.deesser", error)) return false;
  if (!require_in(deesser, "ratio", kDeessRatio, "dsp.deesser", error)) return false;
  if (!require_in(deesser, "rangeDb", kDeessRangeDb, "dsp.deesser", error)) return false;

  const auto& reverb = *dsp.find("reverb");
  if (!has_allowed_keys(reverb, {"mix", "timeMs", "damping", "seed"}, "dsp.reverb", error)) {
    return false;
  }
  for (const char* key : {"mix", "timeMs", "damping", "seed"}) {
    if (!require_key(reverb, key, "dsp.reverb", error)) return false;
  }
  if (!require_in(reverb, "mix", kReverbMix, "dsp.reverb", error)) return false;
  if (!require_in(reverb, "timeMs", kReverbTimeMs, "dsp.reverb", error)) {
    return false;
  }
  if (!require_in(reverb, "damping", kUnit, "dsp.reverb", error)) return false;
  if (!require_in(reverb, "seed", kReverbSeed, "dsp.reverb", error, true)) {
    return false;
  }

  const auto& limiter = *dsp.find("limiter");
  // enableIspLimiter / ispCeilingDbtp are optional: presets authored before the
  // ISP stage was added omit them and fall back to the POD defaults (enabled,
  // -1.0 dBTP). When present they are validated below.
  if (!has_allowed_keys(limiter, {"ceilingDb", "releaseMs", "enableIspLimiter", "ispCeilingDbtp"},
                        "dsp.limiter", error)) {
    return false;
  }
  for (const char* key : {"ceilingDb", "releaseMs"}) {
    if (!require_key(limiter, key, "dsp.limiter", error)) return false;
  }
  // True-peak headroom: sample-domain limiter cannot bound inter-sample peaks,
  // so the upper bound is -1.0 dBFS (one full dB of inter-sample headroom)
  // rather than -0.3 dBFS. Builds that opt in to a 4x-oversampled TP detector
  // can later relax this.
  if (!require_in(limiter, "ceilingDb", kLimiterCeilingDb, "dsp.limiter", error)) return false;
  if (!require_in(limiter, "releaseMs", kLimiterReleaseMs, "dsp.limiter", error)) return false;
  if (const auto* isp = limiter.find("enableIspLimiter")) {
    if (!isp->is_bool()) {
      if (error) *error = "field must be a boolean: dsp.limiter.enableIspLimiter";
      return false;
    }
  }
  // The ISP stage detects inter-sample peaks via 4x oversampling, so its ceiling
  // may legitimately reach 0.0 dBTP (no extra headroom needed) unlike the
  // sample-domain ceilingDb above.
  if (limiter.find("ispCeilingDbtp") != nullptr) {
    if (!require_in(limiter, "ispCeilingDbtp", kIspCeilingDbtp, "dsp.limiter", error)) return false;
  }
  return true;
}

// Macros are a control-thread-only input shorthand. They deliberately expand
// into the ordinary DSP config before a RealtimeVoiceChanger is constructed;
// no macro state reaches the audio-thread POD path.
bool apply_macro_section(const sonare::util::json::Value& macros,
                         RealtimeVoiceChangerConfig* config, std::string* error) {
  if (!has_allowed_keys(
          macros,
          {"pitch", "formant", "brightness", "space", "intensity", "noiseControl", "sibilance"},
          "macros", error)) {
    return false;
  }
  auto optional_number = [&](const char* key, double min, double max, float* target) -> bool {
    if (macros.find(key) == nullptr) return true;
    if (!require_number(macros, key, min, max, "macros", error)) return false;
    *target = macros.find(key)->as_float();
    return true;
  };

  if (!optional_number("pitch", kSemitones.lo, kSemitones.hi, &config->retune.semitones) ||
      !optional_number("formant", kFormantFactor.lo, kFormantFactor.hi, &config->formant.factor)) {
    return false;
  }
  if (const auto* value = macros.find("brightness")) {
    if (!require_number(macros, "brightness", 0.0, 1.0, "macros", error)) return false;
    const float centered = value->as_float() * 2.0f - 1.0f;
    config->formant.brightness = centered;
    config->eq.presence_db = centered * 6.0f;
    config->eq.air_db = centered * 4.0f;
  }
  if (const auto* value = macros.find("space")) {
    if (!require_number(macros, "space", 0.0, 1.0, "macros", error)) return false;
    config->reverb.mix = value->as_float() * 0.45f;
  }
  if (const auto* value = macros.find("intensity")) {
    if (!require_number(macros, "intensity", 0.0, 1.0, "macros", error)) return false;
    config->compressor.ratio = 1.0f + value->as_float() * 4.0f;
  }
  if (const auto* value = macros.find("noiseControl")) {
    if (!require_number(macros, "noiseControl", 0.0, 1.0, "macros", error)) return false;
    config->gate.threshold_db = -90.0f + value->as_float() * 45.0f;
    config->gate.range_db = value->as_float() * 48.0f;
  }
  if (const auto* value = macros.find("sibilance")) {
    if (!require_number(macros, "sibilance", 0.0, 1.0, "macros", error)) return false;
    config->deesser.threshold_db = -6.0f - value->as_float() * 30.0f;
    config->deesser.range_db = value->as_float() * 18.0f;
  }
  return true;
}

}  // namespace

bool validate_realtime_voice_changer_config(const RealtimeVoiceChangerConfig& config,
                                            RealtimeVoiceChangerConfig* normalized,
                                            std::string* error) {
  if (error) error->clear();
  bool ok = true;
  // The success path builds no strings: text is composed only once a field fails.
  visit_scalar_fields(config, [&](const char* name, const float& v, const Range&, float) {
    if (!ok || std::isfinite(v)) return;
    ok = false;
    if (error) *error = std::string("config field is not finite: ") + name;
  });
  if (ok && config.formant_mode != FormantMode::Relative &&
      config.formant_mode != FormantMode::Absolute) {
    ok = false;
    if (error) *error = "formant mode must be 'relative' or 'absolute'";
  }
  visit_scalar_fields(config, [&](const char* name, const float& v, const Range& r, float) {
    if (!ok || (v >= static_cast<float>(r.lo) && v <= static_cast<float>(r.hi))) return;
    ok = false;
    if (error) {
      char text[160];
      std::snprintf(text, sizeof(text), "%s out of range [%.6g, %.6g], got %.6g", name, r.lo, r.hi,
                    static_cast<double>(v));
      *error = text;
    }
  });
  if (ok && (config.retune.grain_size < 0 || config.retune.grain_size > kMaxRetuneGrainSize)) {
    ok = false;
    if (error) {
      char text[160];
      std::snprintf(text, sizeof(text), "retune.grainSize out of range [0, %d], got %d",
                    kMaxRetuneGrainSize, config.retune.grain_size);
      *error = text;
    }
  }
  visit_enum_fields(config, [&](const char* name, bool declared) {
    if (!ok || declared) return;
    ok = false;
    if (error) *error = std::string("config field names no declared value: ") + name;
  });
  visit_integer_fields(config, [&](const char* name, const int& v, const Range& r) {
    if (!ok || (v >= static_cast<int>(r.lo) && v <= static_cast<int>(r.hi))) return;
    ok = false;
    if (error) {
      char text[160];
      std::snprintf(text, sizeof(text), "%s out of range [%d, %d], got %d", name,
                    static_cast<int>(r.lo), static_cast<int>(r.hi), v);
      *error = text;
    }
  });
  // The grain engine has no f0, so nothing that needs one can run on it.
  if (ok && config.voice_engine.engine == PitchEngine::Grain) {
    const char* needs_f0 = nullptr;
    if (config.pitch_correction.mode != PitchCorrectionMode::Off) {
      needs_f0 = "pitchCorrection.mode other than 'off'";
    } else if (config.harmony.voices > 0) {
      needs_f0 = "harmony.voices above 0";
    } else if (config.vocoder.enabled) {
      needs_f0 = "vocoder.enabled";
    }
    if (needs_f0 != nullptr) {
      ok = false;
      if (error) {
        *error = std::string(needs_f0) +
                 " needs voiceEngine.engine 'psola': the grain engine tracks no f0";
      }
    }
  }
  if (ok && space_delay_exceeds_limit(config.reverb_variation)) {
    ok = false;
    if (error) {
      char text[200];
      std::snprintf(text, sizeof(text),
                    "reverbVariation delay out of range: %.6g ms exceeds the %.6g ms delay line",
                    static_cast<double>(space_delay_ms(config.reverb_variation)),
                    static_cast<double>(kMaxSpaceDelayMs));
      *error = text;
    }
  }
  // Only the grain engine warps the formants ahead of the shift; psola resamples its grains.
  if (ok && config.voice_engine.engine == PitchEngine::Grain &&
      !formant_warp_is_reachable(config, error)) {
    ok = false;
  }
  if (normalized) *normalized = ok ? config : RealtimeVoiceChangerConfig{};
  return ok;
}

bool formant_warp_is_reachable(const RealtimeVoiceChangerConfig& config, std::string* error) {
  if (config.formant_mode != FormantMode::Absolute) return true;
  double lo = 0.0;
  double hi = 0.0;
  reachable_formant_factor_range(config.retune.semitones, &lo, &hi);
  const double factor = static_cast<double>(config.formant.factor);
  if (factor >= lo && factor <= hi) return true;
  // The factor itself lives in the warp's own range, so that is the most that can be named.
  const double named_lo = std::max(lo, static_cast<double>(kFormantFactorMin));
  const double named_hi = std::min(hi, static_cast<double>(kFormantFactorMax));
  if (error) {
    char text[360];
    const double semitones = static_cast<double>(config.retune.semitones);
    if (named_lo > named_hi) {
      std::snprintf(text, sizeof(text),
                    "absolute formant mode: no formant factor is reachable at a pitch shift of "
                    "%.4g semitones (the warp reaches [%.4g, %.4g] there, outside formantFactor's "
                    "[%.4g, %.4g])",
                    semitones, lo, hi, static_cast<double>(kFormantFactorMin),
                    static_cast<double>(kFormantFactorMax));
    } else {
      std::snprintf(text, sizeof(text),
                    "absolute formant mode: formant factor must be in [%.4g, %.4g] at a pitch "
                    "shift of %.4g semitones (the warp reaches [%.4g, %.4g] there, limited by "
                    "formantFactor's [%.4g, %.4g]), got %.4g",
                    named_lo, named_hi, semitones, lo, hi, static_cast<double>(kFormantFactorMin),
                    static_cast<double>(kFormantFactorMax), factor);
    }
    *error = text;
  }
  return false;
}

RealtimeVoiceChangerConfig realtime_voice_changer_preset(VoiceCharacterPreset preset) {
  RealtimeVoiceChangerConfig c;
  c.limiter.ceiling_db = -1.0f;
  switch (preset) {
    case VoiceCharacterPreset::NeutralMonitor:
      c.retune = {0.0f, 0.0f, 0};
      c.formant = {1.0f, 0.0f, 0.0f, 0.1f, 0.0f};
      c.eq = {75.0f, 0.0f, 1.2f, 0.5f};
      c.reverb.mix = 0.0f;
      c.compressor = {-24.0f, 1.8f, 8.0f, 120.0f, 0.5f};
      break;
    case VoiceCharacterPreset::BrightIdol:
      c.output_gain_db = 2.2f;
      c.retune = {4.0f, 1.0f, 0};
      // formant.body kept at 0 because the body-region EQ already drops 2.5 dB
      // around 180 Hz; doubling the cut here muddied the lower-mids audibly.
      c.formant = {1.18f, 1.0f, 0.0f, 0.7f, 0.05f};
      c.eq = {160.0f, -2.5f, 4.5f, 3.5f};
      c.gate = {-50.0f, 2.0f, 80.0f, 22.0f};
      c.compressor = {-23.0f, 3.2f, 4.5f, 75.0f, 2.0f};
      c.deesser = {7600.0f, -30.0f, 4.0f, 9.0f};
      c.reverb = {0.11f, 380.0f, 0.35f, 7};
      break;
    case VoiceCharacterPreset::SoftWhisper:
      c.output_gain_db = 3.0f;
      c.retune = {2.0f, 1.0f, 0};
      c.formant = {1.10f, 0.85f, 0.2f, 0.25f, -0.2f};
      c.eq = {110.0f, 1.0f, -2.0f, 2.5f};
      // Release shortened from 180→90 ms: whisper passages have short
      // inter-word gaps; the previous 180 ms kept the gate open so background
      // breath leaked between words.
      c.gate = {-62.0f, 6.0f, 90.0f, 10.0f};
      c.compressor = {-28.0f, 2.6f, 12.0f, 180.0f, 3.0f};
      c.deesser = {7000.0f, -32.0f, 4.5f, 10.0f};
      c.reverb = {0.09f, 520.0f, 0.65f, 11};
      break;
    case VoiceCharacterPreset::DeepNarrator:
      c.output_gain_db = 3.5f;
      c.retune = {-5.0f, 1.0f, 0};
      c.formant = {0.84f, 1.0f, 0.65f, -0.25f, -0.15f};
      c.eq = {70.0f, 4.0f, 2.0f, -1.5f};
      c.gate = {-52.0f, 2.5f, 120.0f, 18.0f};
      c.compressor = {-24.0f, 3.0f, 14.0f, 140.0f, 1.2f};
      c.deesser = {6500.0f, -26.0f, 3.5f, 5.0f};
      c.reverb = {0.08f, 650.0f, 0.8f, 13};
      break;
    case VoiceCharacterPreset::RobotMascot:
      c.output_gain_db = 4.0f;
      // Retune capped at +7 (from +9): grain-based pitch shift has no
      // anti-aliasing, so combining +9 with formant 1.35 and presence +6 dB
      // produced audible aliasing above ~5 kHz, especially at 22050 Hz.
      // factor lowered 1.35→1.30 for the same reason.
      c.retune = {7.0f, 1.0f, 0};
      c.formant = {1.30f, 1.0f, -0.6f, 0.75f, 0.35f};
      c.eq = {240.0f, -6.0f, 4.5f, 0.0f};
      c.gate = {-45.0f, 1.0f, 55.0f, 34.0f};
      c.compressor = {-26.0f, 5.5f, 2.0f, 55.0f, 2.5f};
      c.deesser = {8200.0f, -31.0f, 5.0f, 8.0f};
      c.reverb = {0.06f, 260.0f, 0.3f, 17};
      break;
    case VoiceCharacterPreset::DarkVillain:
      c.output_gain_db = 4.5f;
      c.retune = {-9.0f, 1.0f, 0};
      c.formant = {0.72f, 1.0f, 0.8f, -0.7f, -0.2f};
      // HPF raised 55→80 Hz to keep the long, dense reverb tail (820 ms,
      // 0.13 mix) from making sustained low-mid frequencies muddy. The dark
      // character is carried by formant+brightness shaping, not sub-bass.
      c.eq = {80.0f, 5.5f, -2.0f, -5.0f};
      c.gate = {-50.0f, 2.0f, 110.0f, 26.0f};
      c.compressor = {-25.0f, 4.2f, 8.0f, 120.0f, 1.0f};
      c.deesser = {6100.0f, -24.0f, 3.0f, 4.0f};
      c.reverb = {0.13f, 820.0f, 0.9f, 19};
      break;
    case VoiceCharacterPreset::HardTune:
      c.output_gain_db = 1.5f;
      c.retune = {0.0f, 1.0f, 0};
      c.formant = {1.0f, 0.0f, 0.0f, 0.2f, 0.0f};
      c.eq = {90.0f, 0.0f, 2.5f, 2.0f};
      c.compressor = {-24.0f, 3.0f, 5.0f, 80.0f, 1.5f};
      c.deesser = {7400.0f, -30.0f, 4.0f, 7.0f};
      c.reverb = {0.08f, 420.0f, 0.4f, 23};
      // Instant snap to C major: the audible stepping is the effect.
      c.pitch_correction = {PitchCorrectionMode::Auto, 1.0f, 0.0f, 0, ScaleKind::Major, 0};
      break;
    case VoiceCharacterPreset::RobotMonotone:
      c.output_gain_db = 2.0f;
      c.retune = {0.0f, 1.0f, 0};
      c.formant = {1.05f, 0.0f, -0.2f, 0.4f, 0.2f};
      c.eq = {180.0f, -3.0f, 4.0f, 1.0f};
      c.compressor = {-26.0f, 4.0f, 3.0f, 70.0f, 2.0f};
      c.deesser = {7800.0f, -30.0f, 4.0f, 7.0f};
      c.reverb = {0.06f, 300.0f, 0.4f, 25};
      c.pitch_correction = {PitchCorrectionMode::Robot, 1.0f, 0.0f, 0, ScaleKind::Major, 0};
      break;
    case VoiceCharacterPreset::ChoirHarmony:
      c.output_gain_db = 1.0f;
      c.retune = {0.0f, 1.0f, 0};
      c.formant = {1.0f, 0.0f, 0.1f, 0.2f, 0.0f};
      c.eq = {100.0f, 0.5f, 1.5f, 2.0f};
      c.compressor = {-24.0f, 2.5f, 8.0f, 120.0f, 1.0f};
      c.reverb = {0.2f, 1100.0f, 0.6f, 29};
      // Light correction keeps the stacked voices in tune with each other.
      c.pitch_correction = {PitchCorrectionMode::Auto, 0.5f, 60.0f, 0, ScaleKind::Major, 0};
      c.harmony.voices = 3;
      c.harmony.level = 0.6f;
      c.harmony.detune_cents = 8.0f;
      c.harmony.delay_ms = 18.0f;
      break;
    case VoiceCharacterPreset::VocoderStrings:
      c.output_gain_db = 2.0f;
      c.retune = {0.0f, 1.0f, 0};
      c.formant = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
      c.eq = {120.0f, 0.0f, 1.5f, 1.0f};
      c.compressor = {-24.0f, 3.0f, 6.0f, 100.0f, 1.0f};
      c.reverb = {0.15f, 900.0f, 0.5f, 31};
      c.vocoder.enabled = true;
      c.vocoder.attack_ms = 4.0f;
      c.vocoder.release_ms = 60.0f;
      c.vocoder.sibilance = 0.25f;
      c.vocoder.carrier_wave = VocoderCarrierWave::Ensemble;
      break;
    case VoiceCharacterPreset::Megaphone:
      c.output_gain_db = 1.0f;
      c.retune = {0.0f, 1.0f, 0};
      c.formant = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
      c.eq = {300.0f, -4.0f, 3.0f, -6.0f};
      c.compressor = {-28.0f, 6.0f, 2.0f, 60.0f, 3.0f};
      c.reverb = {0.05f, 250.0f, 0.5f, 37};
      c.character = {CharacterType::Megaphone, 0.6f, 0.0f};
      break;
    case VoiceCharacterPreset::RadioVoice:
      c.output_gain_db = 1.0f;
      c.retune = {0.0f, 1.0f, 0};
      c.formant = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
      c.eq = {200.0f, -2.0f, 2.0f, -4.0f};
      c.compressor = {-26.0f, 5.0f, 3.0f, 80.0f, 2.0f};
      c.reverb = {0.0f, 320.0f, 0.5f, 41};
      c.character = {CharacterType::Radio, 0.4f, 0.0f};
      break;
  }
  return normalize_config(c);
}

const char* realtime_voice_changer_preset_id(VoiceCharacterPreset preset) noexcept {
  return preset_metadata(preset).id.data();
}

namespace {

// A bare preset name inside a config document is document content; the public lookup below takes
// the name as the caller's own argument. The two differ only in the code they refuse with.
VoiceCharacterPreset preset_from_id_or_throw(std::string_view id, ErrorCode code) {
  for (const auto& row : kPresetMetadata) {
    if (row.id == id) return row.preset;
  }
  throw SonareException(code, "unknown realtime voice changer preset");
}

}  // namespace

VoiceCharacterPreset realtime_voice_changer_preset_from_id(std::string_view id) {
  return preset_from_id_or_throw(id, ErrorCode::InvalidParameter);
}

std::vector<std::string> realtime_voice_changer_preset_names() {
  std::vector<std::string> out;
  out.reserve(kPresetMetadata.size());
  for (const auto& row : kPresetMetadata) {
    out.emplace_back(row.id);
  }
  return out;
}

namespace {

// Parses without range handling; callers decide between clamping and refusing.
RealtimeVoiceChangerConfig parse_config_json(std::string_view text) {
  const std::string input = trim_copy(text);
  if (input.empty()) return realtime_voice_changer_preset(VoiceCharacterPreset::NeutralMonitor);
  if (input.front() != '{')
    return realtime_voice_changer_preset(preset_from_id_or_throw(input, ErrorCode::InvalidFormat));

  const auto root = sonare::util::json::admit(input);
  RealtimeVoiceChangerConfig c;
  const auto* dsp = root.find("dsp");
  const auto* macros = root.find("macros");
  if (dsp != nullptr && macros != nullptr) {
    throw SonareException(ErrorCode::InvalidFormat,
                          "preset must contain exactly one of dsp or macros");
  }
  if (macros != nullptr) {
    std::string error;
    if (!apply_macro_section(*macros, &c, &error)) {
      throw SonareException(ErrorCode::InvalidFormat, error);
    }
    return c;
  }
  if (dsp != nullptr && !dsp->is_object()) refuse_type("", "dsp", "an object");
  const auto& object = dsp ? *dsp : root;
  c.input_gain_db = object_number(object, "", "inputGainDb", c.input_gain_db);
  c.output_gain_db = object_number(object, "", "outputGainDb", c.output_gain_db);
  c.wet_mix = object_number(object, "", "wetMix", c.wet_mix);

  // JS bindings expose preset configs as a flat POD for efficient FFI. Accept
  // that camelCase shape here, at the one parser shared by C ABI, Node, and
  // WASM, rather than making every binding hand-expand it into nested JSON.
  if (object.find("retuneSemitones") != nullptr) {
    c.retune.semitones = object_number(object, "", "retuneSemitones", c.retune.semitones);
    c.retune.mix = object_number(object, "", "retuneMix", c.retune.mix);
    c.retune.grain_size = object_int(object, "", "retuneGrainSize", c.retune.grain_size);
    c.formant.factor = object_number(object, "", "formantFactor", c.formant.factor);
    c.formant.amount = object_number(object, "", "formantAmount", c.formant.amount);
    c.formant.body = object_number(object, "", "formantBody", c.formant.body);
    c.formant.brightness = object_number(object, "", "formantBrightness", c.formant.brightness);
    c.formant.nasal = object_number(object, "", "formantNasal", c.formant.nasal);
    c.formant_mode = object_formant_mode(object, "", "formantMode", c.formant_mode);
    c.eq.highpass_hz = object_number(object, "", "eqHighpassHz", c.eq.highpass_hz);
    c.eq.body_db = object_number(object, "", "eqBodyDb", c.eq.body_db);
    c.eq.presence_db = object_number(object, "", "eqPresenceDb", c.eq.presence_db);
    c.eq.air_db = object_number(object, "", "eqAirDb", c.eq.air_db);
    c.gate.threshold_db = object_number(object, "", "gateThresholdDb", c.gate.threshold_db);
    c.gate.attack_ms = object_number(object, "", "gateAttackMs", c.gate.attack_ms);
    c.gate.release_ms = object_number(object, "", "gateReleaseMs", c.gate.release_ms);
    c.gate.range_db = object_number(object, "", "gateRangeDb", c.gate.range_db);
    c.compressor.threshold_db =
        object_number(object, "", "compressorThresholdDb", c.compressor.threshold_db);
    c.compressor.ratio = object_number(object, "", "compressorRatio", c.compressor.ratio);
    c.compressor.attack_ms =
        object_number(object, "", "compressorAttackMs", c.compressor.attack_ms);
    c.compressor.release_ms =
        object_number(object, "", "compressorReleaseMs", c.compressor.release_ms);
    c.compressor.makeup_gain_db =
        object_number(object, "", "compressorMakeupGainDb", c.compressor.makeup_gain_db);
    c.deesser.frequency_hz =
        object_number(object, "", "deesserFrequencyHz", c.deesser.frequency_hz);
    c.deesser.threshold_db =
        object_number(object, "", "deesserThresholdDb", c.deesser.threshold_db);
    c.deesser.ratio = object_number(object, "", "deesserRatio", c.deesser.ratio);
    c.deesser.range_db = object_number(object, "", "deesserRangeDb", c.deesser.range_db);
    c.reverb.mix = object_number(object, "", "reverbMix", c.reverb.mix);
    c.reverb.time_ms = object_number(object, "", "reverbTimeMs", c.reverb.time_ms);
    c.reverb.damping = object_number(object, "", "reverbDamping", c.reverb.damping);
    c.reverb.seed = object_int(object, "", "reverbSeed", c.reverb.seed);
    c.limiter.ceiling_db = object_number(object, "", "limiterCeilingDb", c.limiter.ceiling_db);
    c.limiter.release_ms = object_number(object, "", "limiterReleaseMs", c.limiter.release_ms);
    c.limiter.enable_isp_limiter =
        object_bool(object, "", "limiterEnableIspLimiter", c.limiter.enable_isp_limiter);
    c.limiter.isp_ceiling_dbtp =
        object_number(object, "", "limiterIspCeilingDbtp", c.limiter.isp_ceiling_dbtp);
    c.voice_engine.engine = object_enum(object, "", "voiceEngineEngine", c.voice_engine.engine);
    c.voice_engine.f0_min_hz =
        object_number(object, "", "voiceEngineF0MinHz", c.voice_engine.f0_min_hz);
    c.pitch_correction.mode =
        object_enum(object, "", "pitchCorrectionMode", c.pitch_correction.mode);
    c.pitch_correction.amount =
        object_number(object, "", "pitchCorrectionAmount", c.pitch_correction.amount);
    c.pitch_correction.speed_ms =
        object_number(object, "", "pitchCorrectionSpeedMs", c.pitch_correction.speed_ms);
    c.pitch_correction.key_root =
        object_int(object, "", "pitchCorrectionKeyRoot", c.pitch_correction.key_root);
    c.pitch_correction.scale =
        object_enum(object, "", "pitchCorrectionScale", c.pitch_correction.scale);
    c.pitch_correction.robot_octave =
        object_int(object, "", "pitchCorrectionRobotOctave", c.pitch_correction.robot_octave);
    c.harmony.voices = object_int(object, "", "harmonyVoices", c.harmony.voices);
    c.harmony.steps[0] = object_int(object, "", "harmonyStep1", c.harmony.steps[0]);
    c.harmony.steps[1] = object_int(object, "", "harmonyStep2", c.harmony.steps[1]);
    c.harmony.steps[2] = object_int(object, "", "harmonyStep3", c.harmony.steps[2]);
    c.harmony.level = object_number(object, "", "harmonyLevel", c.harmony.level);
    c.harmony.lead_level = object_number(object, "", "harmonyLeadLevel", c.harmony.lead_level);
    c.harmony.detune_cents =
        object_number(object, "", "harmonyDetuneCents", c.harmony.detune_cents);
    c.harmony.delay_ms = object_number(object, "", "harmonyDelayMs", c.harmony.delay_ms);
    c.vocoder.enabled = object_bool(object, "", "vocoderEnabled", c.vocoder.enabled);
    c.vocoder.bands = object_int(object, "", "vocoderBands", c.vocoder.bands);
    c.vocoder.attack_ms = object_number(object, "", "vocoderAttackMs", c.vocoder.attack_ms);
    c.vocoder.release_ms = object_number(object, "", "vocoderReleaseMs", c.vocoder.release_ms);
    c.vocoder.sibilance = object_number(object, "", "vocoderSibilance", c.vocoder.sibilance);
    c.vocoder.carrier_source =
        object_enum(object, "", "vocoderCarrierSource", c.vocoder.carrier_source);
    c.vocoder.carrier_wave = object_enum(object, "", "vocoderCarrierWave", c.vocoder.carrier_wave);
    c.vocoder.ensemble_detune_cents =
        object_number(object, "", "vocoderEnsembleDetuneCents", c.vocoder.ensemble_detune_cents);
    c.vocoder.level = object_number(object, "", "vocoderLevel", c.vocoder.level);
    c.character.type = object_enum(object, "", "characterType", c.character.type);
    c.character.amount = object_number(object, "", "characterAmount", c.character.amount);
    c.character.tone = object_number(object, "", "characterTone", c.character.tone);
    c.reverb_variation.variation =
        object_enum(object, "", "reverbVariationVariation", c.reverb_variation.variation);
    c.reverb_variation.delay_ms =
        object_number(object, "", "reverbVariationDelayMs", c.reverb_variation.delay_ms);
    c.reverb_variation.feedback =
        object_number(object, "", "reverbVariationFeedback", c.reverb_variation.feedback);
    c.reverb_variation.tempo_bpm =
        object_number(object, "", "reverbVariationTempoBpm", c.reverb_variation.tempo_bpm);
    c.reverb_variation.division =
        object_enum(object, "", "reverbVariationDivision", c.reverb_variation.division);
    c.midi.release = object_enum(object, "", "midiRelease", c.midi.release);
    return c;
  }

  if (const auto* v = object_section(object, "retune")) {
    c.retune.semitones = object_number(*v, "retune", "semitones", c.retune.semitones);
    c.retune.mix = object_number(*v, "retune", "mix", c.retune.mix);
    c.retune.grain_size = object_int(*v, "retune", "grainSize", c.retune.grain_size);
  }
  if (const auto* v = object_section(object, "formant")) {
    c.formant.factor = object_number(*v, "formant", "factor", c.formant.factor);
    c.formant.amount = object_number(*v, "formant", "amount", c.formant.amount);
    c.formant.body = object_number(*v, "formant", "body", c.formant.body);
    c.formant.brightness = object_number(*v, "formant", "brightness", c.formant.brightness);
    c.formant.nasal = object_number(*v, "formant", "nasal", c.formant.nasal);
    c.formant_mode = object_formant_mode(*v, "formant", "mode", c.formant_mode);
  }
  if (const auto* v = object_section(object, "eq")) {
    c.eq.highpass_hz = object_number(*v, "eq", "highpassHz", c.eq.highpass_hz);
    c.eq.body_db = object_number(*v, "eq", "bodyDb", c.eq.body_db);
    c.eq.presence_db = object_number(*v, "eq", "presenceDb", c.eq.presence_db);
    c.eq.air_db = object_number(*v, "eq", "airDb", c.eq.air_db);
  }
  if (const auto* v = object_section(object, "gate")) {
    c.gate.threshold_db = object_number(*v, "gate", "thresholdDb", c.gate.threshold_db);
    c.gate.attack_ms = object_number(*v, "gate", "attackMs", c.gate.attack_ms);
    c.gate.release_ms = object_number(*v, "gate", "releaseMs", c.gate.release_ms);
    c.gate.range_db = object_number(*v, "gate", "rangeDb", c.gate.range_db);
  }
  if (const auto* v = object_section(object, "compressor")) {
    c.compressor.threshold_db =
        object_number(*v, "compressor", "thresholdDb", c.compressor.threshold_db);
    c.compressor.ratio = object_number(*v, "compressor", "ratio", c.compressor.ratio);
    c.compressor.attack_ms = object_number(*v, "compressor", "attackMs", c.compressor.attack_ms);
    c.compressor.release_ms = object_number(*v, "compressor", "releaseMs", c.compressor.release_ms);
    c.compressor.makeup_gain_db =
        object_number(*v, "compressor", "makeupGainDb", c.compressor.makeup_gain_db);
  }
  if (const auto* v = object_section(object, "deesser")) {
    c.deesser.frequency_hz = object_number(*v, "deesser", "frequencyHz", c.deesser.frequency_hz);
    c.deesser.threshold_db = object_number(*v, "deesser", "thresholdDb", c.deesser.threshold_db);
    c.deesser.ratio = object_number(*v, "deesser", "ratio", c.deesser.ratio);
    c.deesser.range_db = object_number(*v, "deesser", "rangeDb", c.deesser.range_db);
  }
  if (const auto* v = object_section(object, "reverb")) {
    c.reverb.mix = object_number(*v, "reverb", "mix", c.reverb.mix);
    c.reverb.time_ms = object_number(*v, "reverb", "timeMs", c.reverb.time_ms);
    c.reverb.damping = object_number(*v, "reverb", "damping", c.reverb.damping);
    c.reverb.seed = object_int(*v, "reverb", "seed", c.reverb.seed);
  }
  if (const auto* v = object_section(object, "limiter")) {
    c.limiter.ceiling_db = object_number(*v, "limiter", "ceilingDb", c.limiter.ceiling_db);
    c.limiter.release_ms = object_number(*v, "limiter", "releaseMs", c.limiter.release_ms);
    c.limiter.enable_isp_limiter =
        object_bool(*v, "limiter", "enableIspLimiter", c.limiter.enable_isp_limiter);
    c.limiter.isp_ceiling_dbtp =
        object_number(*v, "limiter", "ispCeilingDbtp", c.limiter.isp_ceiling_dbtp);
  }
  // Voice transformer sections; a document without them (schema version 1) keeps the defaults.
  if (const auto* v = object_section(object, "voiceEngine")) {
    c.voice_engine.engine = object_enum(*v, "voiceEngine", "engine", c.voice_engine.engine);
    c.voice_engine.f0_min_hz =
        object_number(*v, "voiceEngine", "f0MinHz", c.voice_engine.f0_min_hz);
  }
  if (const auto* v = object_section(object, "pitchCorrection")) {
    PitchCorrectionConfig& pc = c.pitch_correction;
    pc.mode = object_enum(*v, "pitchCorrection", "mode", pc.mode);
    pc.amount = object_number(*v, "pitchCorrection", "amount", pc.amount);
    pc.speed_ms = object_number(*v, "pitchCorrection", "speedMs", pc.speed_ms);
    pc.key_root = object_int(*v, "pitchCorrection", "keyRoot", pc.key_root);
    pc.scale = object_enum(*v, "pitchCorrection", "scale", pc.scale);
    pc.robot_octave = object_int(*v, "pitchCorrection", "robotOctave", pc.robot_octave);
  }
  if (const auto* v = object_section(object, "harmony")) {
    HarmonyConfig& hm = c.harmony;
    hm.voices = object_int(*v, "harmony", "voices", hm.voices);
    object_steps(*v, "harmony", "steps", hm.steps);
    hm.level = object_number(*v, "harmony", "level", hm.level);
    hm.lead_level = object_number(*v, "harmony", "leadLevel", hm.lead_level);
    hm.detune_cents = object_number(*v, "harmony", "detuneCents", hm.detune_cents);
    hm.delay_ms = object_number(*v, "harmony", "delayMs", hm.delay_ms);
  }
  if (const auto* v = object_section(object, "vocoder")) {
    VocoderConfig& vc = c.vocoder;
    vc.enabled = object_bool(*v, "vocoder", "enabled", vc.enabled);
    vc.bands = object_int(*v, "vocoder", "bands", vc.bands);
    vc.attack_ms = object_number(*v, "vocoder", "attackMs", vc.attack_ms);
    vc.release_ms = object_number(*v, "vocoder", "releaseMs", vc.release_ms);
    vc.sibilance = object_number(*v, "vocoder", "sibilance", vc.sibilance);
    vc.carrier_source = object_enum(*v, "vocoder", "carrierSource", vc.carrier_source);
    vc.carrier_wave = object_enum(*v, "vocoder", "carrierWave", vc.carrier_wave);
    vc.ensemble_detune_cents =
        object_number(*v, "vocoder", "ensembleDetuneCents", vc.ensemble_detune_cents);
    vc.level = object_number(*v, "vocoder", "level", vc.level);
  }
  if (const auto* v = object_section(object, "character")) {
    c.character.type = object_enum(*v, "character", "type", c.character.type);
    c.character.amount = object_number(*v, "character", "amount", c.character.amount);
    c.character.tone = object_number(*v, "character", "tone", c.character.tone);
  }
  if (const auto* v = object_section(object, "reverbVariation")) {
    ReverbVariationConfig& rv = c.reverb_variation;
    rv.variation = object_enum(*v, "reverbVariation", "variation", rv.variation);
    rv.delay_ms = object_number(*v, "reverbVariation", "delayMs", rv.delay_ms);
    rv.feedback = object_number(*v, "reverbVariation", "feedback", rv.feedback);
    rv.tempo_bpm = object_number(*v, "reverbVariation", "tempoBpm", rv.tempo_bpm);
    rv.division = object_enum(*v, "reverbVariation", "division", rv.division);
  }
  if (const auto* v = object_section(object, "midi")) {
    c.midi.release = object_enum(*v, "midi", "release", c.midi.release);
  }
  return c;
}

}  // namespace

RealtimeVoiceChangerConfig realtime_voice_changer_config_from_json(std::string_view text) {
  return normalize_config(parse_config_json(text));
}

std::string realtime_voice_changer_config_to_json(const RealtimeVoiceChangerConfig& config) {
  const auto c = normalize_config(config);
  std::string out;
  // A normalized config is also a valid, saveable custom preset. Include the
  // metadata required by the strict preset validator so callers can validate
  // its output again without reconstructing an envelope around the DSP object.
  out += "{\"schemaVersion\":";
  out += std::to_string(kVoiceChangerPresetSchemaVersion);
  out += ",\"id\":\"custom\",\"name\":\"Custom\",\"category\":\"custom\",";
  dump_dsp_section(out, c);
  out += "}";
  return out;
}

std::string realtime_voice_changer_preset_json(VoiceCharacterPreset preset) {
  const auto c = realtime_voice_changer_preset(preset);
  std::string out;
  out += "{\"schemaVersion\":";
  out += std::to_string(kVoiceChangerPresetSchemaVersion);
  out += ",\"id\":\"";
  out += sonare::util::json::escape_string(realtime_voice_changer_preset_id(preset));
  out += "\",\"name\":\"";
  out += sonare::util::json::escape_string(realtime_voice_changer_preset_display_name(preset));
  out += "\",\"category\":\"";
  out += sonare::util::json::escape_string(std::string(preset_metadata(preset).category));
  out += "\",";
  dump_dsp_section(out, c);
  out += "}";
  return out;
}

bool validate_realtime_voice_changer_preset_json(std::string_view json,
                                                 std::string* normalized_json, std::string* error) {
  if (normalized_json) normalized_json->clear();
  if (error) error->clear();
  try {
    // Strict parse: a preset document with duplicate keys (e.g. two `"id"`
    // entries) is a user-config bug — fail fast rather than silently keeping
    // the last value. The lenient `realtime_voice_changer_config_from_json`
    // path below keeps using `admit` (not `admit_strict`) because it doubles
    // as the realtime C-API entry point and must stay maximally tolerant of
    // duplicate keys; both still run under the same resource budget.
    const auto root = sonare::util::json::admit_strict(trim_copy(json));
    if (!has_allowed_keys(
            root, {"schemaVersion", "id", "name", "description", "category", "macros", "dsp"}, "$",
            error)) {
      return false;
    }
    for (const char* key : {"schemaVersion", "id", "name", "category"}) {
      if (!require_key(root, key, "$", error)) return false;
    }
    // schemaVersion: JSON-Schema "type: integer, enum: [1, 2]" demands an
    // *exact* integer. as_int() silently truncates 1.5 -> 1, so check the
    // raw double for both integral-ness and range before comparing.
    const auto* schema = root.find("schemaVersion");
    if (!schema || !schema->is_number()) {
      if (error) *error = "field is not a valid schemaVersion: $.schemaVersion (must be integer)";
      return false;
    }
    int schema_version = 0;
    {
      const double n = schema->as_number();
      if (!std::isfinite(n) || std::floor(n) != n || n < kOldestPresetSchemaVersion ||
          n > static_cast<double>(kVoiceChangerPresetSchemaVersion)) {
        if (error) {
          *error = "field is not a valid schemaVersion: $.schemaVersion (must be an integer from " +
                   std::to_string(kOldestPresetSchemaVersion) + " to " +
                   std::to_string(kVoiceChangerPresetSchemaVersion) + ")";
        }
        return false;
      }
      schema_version = static_cast<int>(n);
    }
    // id: schema regex ^[a-z0-9][a-z0-9._-]*$ + length 1..96. The C++ validator
    // historically accepted any non-empty string up to 96 bytes, which let
    // upper-case / whitespace / punctuation slip through and break downstream
    // consumers that treat the id as a TS enum literal or comparison key.
    if (!require_string(root, "id", 1, 96, "$", error)) return false;
    if (const auto* id_value = root.find("id"); !is_valid_preset_id(id_value->as_string())) {
      if (error) *error = "field is not a valid preset id: $.id";
      return false;
    }
    if (!require_string(root, "name", 1, 96, "$", error)) return false;
    if (root.find("description") && !require_string(root, "description", 0, 512, "$", error)) {
      return false;
    }
    if (!require_string(root, "category", 1, 16, "$", error)) return false;
    {
      static constexpr std::array<std::string_view, 11> kCategories = {
          "monitor", "bright",  "soft",    "deep",      "robot", "dark",
          "tune",    "harmony", "vocoder", "character", "custom"};
      const auto& category = root.find("category")->as_string();
      if (std::find(kCategories.begin(), kCategories.end(), category) == kCategories.end()) {
        if (error) *error = "preset category is invalid";
        return false;
      }
    }
    const auto* dsp = root.find("dsp");
    const auto* macros = root.find("macros");
    if ((dsp == nullptr) == (macros == nullptr)) {
      if (error) *error = "preset must contain exactly one of dsp or macros";
      return false;
    }
    if (dsp != nullptr && !validate_dsp_section(*dsp, schema_version, error)) return false;
    RealtimeVoiceChangerConfig macro_config;
    if (macros != nullptr && !apply_macro_section(*macros, &macro_config, error)) return false;
    const auto config = realtime_voice_changer_config_from_json(trim_copy(json));
    // What only the whole configuration can say; the same rule create and set_config apply.
    if (!validate_realtime_voice_changer_config(config, nullptr, error)) return false;
    if (normalized_json) {
      // Re-attach the caller's own preset identity (id/name/description/category)
      // instead of routing through realtime_voice_changer_config_to_json, whose
      // "custom"/"Custom" placeholders are only correct for config-only entry
      // points that never had a preset identity to begin with. Losing the real
      // id here breaks find_voice_preset_in_pack lookups and a pack's own
      // id-uniqueness contract on every round trip through this validator.
      std::string out;
      out += "{\"schemaVersion\":";
      out += std::to_string(kVoiceChangerPresetSchemaVersion);
      out += ",\"id\":\"";
      out += sonare::util::json::escape_string(root.find("id")->as_string());
      out += "\",\"name\":\"";
      out += sonare::util::json::escape_string(root.find("name")->as_string());
      out += "\"";
      if (const auto* description = root.find("description")) {
        out += ",\"description\":\"";
        out += sonare::util::json::escape_string(description->as_string());
        out += "\"";
      }
      if (const auto* category = root.find("category")) {
        out += ",\"category\":\"";
        out += sonare::util::json::escape_string(category->as_string());
        out += "\"";
      }
      out += ",";
      dump_dsp_section(out, config);
      out += "}";
      *normalized_json = out;
    }
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

bool realtime_voice_changer_config_from_input(std::string_view text,
                                              RealtimeVoiceChangerConfig* config,
                                              std::string* error) {
  if (config) *config = {};
  if (error) error->clear();
  if (!config) {
    if (error) *error = "output config must not be null";
    return false;
  }

  try {
    const std::string input = trim_copy(text);
    if (input.empty()) {
      if (error) *error = "voice changer config must not be empty";
      return false;
    }
    if (input.front() != '{') {
      *config = realtime_voice_changer_preset(realtime_voice_changer_preset_from_id(input));
      return true;
    }

    // Public JS bindings also pass the 70-field camelCase POD returned by
    // realtimeVoiceChangerPresetConfig(). It is intentionally not a preset
    // document, but must be complete and type-correct so a partial object
    // cannot silently fall back to unrelated defaults.
    const auto root = sonare::util::json::admit_strict(input);
    if (root.is_object() && root.find("retuneSemitones") != nullptr) {
      static constexpr std::array<const char*, 36> kFlatPodKeys = {
          "inputGainDb",
          "outputGainDb",
          "wetMix",
          "retuneSemitones",
          "retuneMix",
          "retuneGrainSize",
          "formantFactor",
          "formantAmount",
          "formantBody",
          "formantBrightness",
          "formantNasal",
          "eqHighpassHz",
          "eqBodyDb",
          "eqPresenceDb",
          "eqAirDb",
          "gateThresholdDb",
          "gateAttackMs",
          "gateReleaseMs",
          "gateRangeDb",
          "compressorThresholdDb",
          "compressorRatio",
          "compressorAttackMs",
          "compressorReleaseMs",
          "compressorMakeupGainDb",
          "deesserFrequencyHz",
          "deesserThresholdDb",
          "deesserRatio",
          "deesserRangeDb",
          "reverbMix",
          "reverbTimeMs",
          "reverbDamping",
          "reverbSeed",
          "limiterCeilingDb",
          "limiterReleaseMs",
          "limiterEnableIspLimiter",
          "limiterIspCeilingDbtp",
      };
      // The voice transformer keys are part of the same complete shape: the bindings build
      // this object at runtime from the whole POD and never persist it, so a missing key is
      // refused like any other. Each carries its own type.
      static constexpr std::array<const char*, 34> kTransformKeys = {
          "voiceEngineEngine",
          "voiceEngineF0MinHz",
          "pitchCorrectionMode",
          "pitchCorrectionAmount",
          "pitchCorrectionSpeedMs",
          "pitchCorrectionKeyRoot",
          "pitchCorrectionScale",
          "pitchCorrectionRobotOctave",
          "harmonyVoices",
          "harmonyStep1",
          "harmonyStep2",
          "harmonyStep3",
          "harmonyLevel",
          "harmonyLeadLevel",
          "harmonyDetuneCents",
          "harmonyDelayMs",
          "vocoderEnabled",
          "vocoderBands",
          "vocoderAttackMs",
          "vocoderReleaseMs",
          "vocoderSibilance",
          "vocoderCarrierSource",
          "vocoderCarrierWave",
          "vocoderEnsembleDetuneCents",
          "vocoderLevel",
          "characterType",
          "characterAmount",
          "characterTone",
          "reverbVariationVariation",
          "reverbVariationDelayMs",
          "reverbVariationFeedback",
          "reverbVariationTempoBpm",
          "reverbVariationDivision",
          "midiRelease",
      };
      static constexpr std::array<std::string_view, 9> kTransformEnumKeys = {
          "voiceEngineEngine",        "pitchCorrectionMode",     "pitchCorrectionScale",
          "vocoderCarrierSource",     "vocoderCarrierWave",      "characterType",
          "reverbVariationVariation", "reverbVariationDivision", "midiRelease"};
      static constexpr std::array<std::string_view, 9> kIntegerKeys = {
          "retuneGrainSize", "reverbSeed",   "pitchCorrectionKeyRoot", "pitchCorrectionRobotOctave",
          "harmonyVoices",   "harmonyStep1", "harmonyStep2",           "harmonyStep3",
          "vocoderBands"};
      const auto listed = [](const auto& keys, std::string_view key) {
        return std::find(keys.begin(), keys.end(), key) != keys.end();
      };
      for (const auto& [key, _value] : root.as_object()) {
        bool allowed = false;
        for (const char* allowed_key : kFlatPodKeys) {
          if (key == allowed_key) {
            allowed = true;
            break;
          }
        }
        for (const char* transform_key : kTransformKeys) {
          if (key != transform_key) continue;
          const bool typed = listed(kTransformEnumKeys, key) ? _value.is_string()
                             : key == "vocoderEnabled"       ? _value.is_bool()
                                                             : _value.is_number();
          if (!typed) {
            if (error) *error = "flat voice changer POD has an invalid field: $." + key;
            return false;
          }
          allowed = true;
        }
        // formantMode is optional: documents written before the mode existed omit it.
        if (key == "formantMode") {
          if (!_value.is_string()) {
            if (error) *error = "flat voice changer POD has an invalid field: $.formantMode";
            return false;
          }
          allowed = true;
        }
        if (!allowed) {
          if (error) *error = "unknown field: $." + key;
          return false;
        }
      }
      for (const char* key : kFlatPodKeys) {
        if (!require_key(root, key, "$", error)) return false;
        const auto* value = root.find(key);
        const bool is_isp_enabled = std::string_view(key) == "limiterEnableIspLimiter";
        if (!value || (is_isp_enabled ? !value->is_bool() : !value->is_number())) {
          if (error) *error = "flat voice changer POD has an invalid field: $." + std::string(key);
          return false;
        }
      }
      for (const char* key : kTransformKeys) {
        if (!require_key(root, key, "$", error)) return false;
      }
      for (const auto& [key, value] : root.as_object()) {
        if (!value.is_number()) continue;
        const double n = value.as_number();
        const bool is_int_key = listed(kIntegerKeys, key);
        const bool fits = is_int_key ? (n >= kReverbSeed.lo && n <= kReverbSeed.hi)
                                     : std::fabs(n) <= std::numeric_limits<float>::max();
        if (!fits) {
          if (error) *error = "flat voice changer POD field is out of range: $." + key;
          return false;
        }
        if (is_int_key && std::floor(n) != n) {
          if (error) *error = "flat voice changer POD field must be a whole number: $." + key;
          return false;
        }
      }
      RealtimeVoiceChangerConfig normalized;
      if (!validate_realtime_voice_changer_config(parse_config_json(input), &normalized, error)) {
        return false;
      }
      *config = normalized;
      return true;
    }

    std::string normalized_json;
    if (!validate_realtime_voice_changer_preset_json(input, &normalized_json, error)) return false;
    *config = realtime_voice_changer_config_from_json(normalized_json);
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

}  // namespace sonare::editing::voice_changer
