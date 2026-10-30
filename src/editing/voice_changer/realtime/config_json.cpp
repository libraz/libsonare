#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

#include "editing/voice_changer/realtime.h"
#include "editing/voice_changer/realtime/config_ranges.h"
#include "util/exception.h"
#include "util/json_budget.h"
#include "util/json_schema.h"
#include "util/number_format.h"

namespace sonare::editing::voice_changer {
using namespace detail;

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

// A preset document: schema version, identity (description optional), then the dsp object.
void dump_preset_envelope(std::string& out, const std::string& id, const std::string& name,
                          const std::string* description, const std::string& category,
                          const RealtimeVoiceChangerConfig& config) {
  out += "{\"schemaVersion\":";
  out += std::to_string(kVoiceChangerPresetSchemaVersion);
  out += ",\"id\":\"";
  out += sonare::util::json::escape_string(id);
  out += "\",\"name\":\"";
  out += sonare::util::json::escape_string(name);
  out += "\"";
  if (description != nullptr) {
    out += ",\"description\":\"";
    out += sonare::util::json::escape_string(*description);
    out += "\"";
  }
  out += ",\"category\":\"";
  out += sonare::util::json::escape_string(category);
  out += "\",";
  dump_dsp_section(out, config);
  out += "}";
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

// How a section key is checked. A null @c check is a range test over @c range; @c required
// is whether the key must be present at all.
using FieldCheck = bool (*)(const sonare::util::json::Value&, const char*, const char*,
                            std::string*);

struct FieldSpec {
  const char* key;
  FieldCheck check;
  Range range;
  bool integer;
  bool required;
};

constexpr FieldSpec range_field(const char* key, Range range, bool integer = false,
                                bool required = true) {
  return {key, nullptr, range, integer, required};
}

template <typename Enum>
constexpr FieldSpec enum_field(const char* key) {
  return {key, &require_enum<Enum>, {}, false, true};
}

constexpr FieldSpec bool_field(const char* key, bool required = true) {
  return {key, &require_bool, {}, false, required};
}

// A key whose value is checked elsewhere; only its presence is required here.
bool accept_any(const sonare::util::json::Value&, const char*, const char*, std::string*) {
  return true;
}

bool require_formant_mode(const sonare::util::json::Value& object, const char* key,
                          const char* path, std::string* error) {
  const auto* mode = object.find(key);
  if (!mode->is_string() || (mode->as_string() != "relative" && mode->as_string() != "absolute")) {
    if (error) *error = std::string("field must be 'relative' or 'absolute': ") + path + "." + key;
    return false;
  }
  return true;
}

template <std::size_t N>
bool has_allowed_fields(const sonare::util::json::Value& object, const FieldSpec (&fields)[N],
                        std::string_view path, std::string* error) {
  if (!object.is_object()) {
    if (error) *error = std::string(path) + " must be an object";
    return false;
  }
  for (const auto& [key, _value] : object.as_object()) {
    bool allowed = false;
    for (const FieldSpec& field : fields) {
      if (key == field.key) {
        allowed = true;
        break;
      }
    }
    if (!allowed) {
      if (error) *error = "unknown field: " + std::string(path) + "." + key;
      return false;
    }
  }
  return true;
}

// Allowed keys, then required keys, then each present key's check, in table order.
template <std::size_t N>
bool validate_range_section(const sonare::util::json::Value& section, const char* path,
                            const FieldSpec (&fields)[N], std::string* error) {
  if (!has_allowed_fields(section, fields, path, error)) return false;
  for (const FieldSpec& field : fields) {
    if (field.required && !require_key(section, field.key, path, error)) return false;
  }
  for (const FieldSpec& field : fields) {
    if (!field.required && section.find(field.key) == nullptr) continue;
    const bool ok = field.check != nullptr
                        ? field.check(section, field.key, path, error)
                        : require_in(section, field.key, field.range, path, error, field.integer);
    if (!ok) return false;
  }
  return true;
}

// A section checked only when present.
template <std::size_t N>
bool validate_optional_section(const sonare::util::json::Value& parent, const char* key,
                               const char* path, const FieldSpec (&fields)[N], std::string* error) {
  const auto* section = parent.find(key);
  return section == nullptr || validate_range_section(*section, path, fields, error);
}

constexpr FieldSpec kRetuneFields[] = {
    range_field("semitones", kSemitones),
    range_field("mix", kUnit),
    range_field("grainSize", kGrainSize, true),
};
constexpr FieldSpec kFormantFields[] = {
    range_field("factor", kFormantFactor), range_field("amount", kUnit),
    range_field("body", kBipolar),         range_field("brightness", kBipolar),
    range_field("nasal", kBipolar),        {"mode", &require_formant_mode, {}, false, false},
};
constexpr FieldSpec kEqFields[] = {
    range_field("highpassHz", kHighpassHz),
    range_field("bodyDb", kEqGainDb),
    range_field("presenceDb", kEqGainDb),
    range_field("airDb", kEqGainDb),
};
constexpr FieldSpec kGateFields[] = {
    range_field("thresholdDb", kGateThresholdDb),
    range_field("attackMs", kGateAttackMs),
    range_field("releaseMs", kGateReleaseMs),
    range_field("rangeDb", kGateRangeDb),
};
constexpr FieldSpec kCompressorFields[] = {
    range_field("thresholdDb", kCompThresholdDb), range_field("ratio", kCompRatio),
    range_field("attackMs", kCompAttackMs),       range_field("releaseMs", kCompReleaseMs),
    range_field("makeupGainDb", kMakeupGainDb),
};
constexpr FieldSpec kDeesserFields[] = {
    range_field("frequencyHz", kDeessFrequencyHz),
    range_field("thresholdDb", kDeessThresholdDb),
    range_field("ratio", kDeessRatio),
    range_field("rangeDb", kDeessRangeDb),
};
constexpr FieldSpec kReverbFields[] = {
    range_field("mix", kReverbMix),
    range_field("timeMs", kReverbTimeMs),
    range_field("damping", kUnit),
    range_field("seed", kReverbSeed, true),
};
// enableIspLimiter / ispCeilingDbtp are optional: presets authored before the ISP stage omit
// them and fall back to the POD defaults.
constexpr FieldSpec kLimiterFields[] = {
    range_field("ceilingDb", kLimiterCeilingDb),
    range_field("releaseMs", kLimiterReleaseMs),
    bool_field("enableIspLimiter", false),
    range_field("ispCeilingDbtp", kIspCeilingDbtp, false, false),
};

constexpr FieldSpec kVoiceEngineFields[] = {
    enum_field<PitchEngine>("engine"),
    range_field("f0MinHz", kF0MinHz),
};
constexpr FieldSpec kPitchCorrectionFields[] = {
    enum_field<PitchCorrectionMode>("mode"),
    range_field("amount", kUnit),
    range_field("speedMs", kCorrectionSpeedMs),
    range_field("keyRoot", kKeyRoot, true),
    enum_field<ScaleKind>("scale"),
    range_field("robotOctave", kRobotOctave, true),
};
constexpr FieldSpec kHarmonyFields[] = {
    range_field("voices", kHarmonyVoices, true),
    {"steps", &accept_any, {}, false, true},
    range_field("level", kUnit),
    range_field("leadLevel", kUnit),
    range_field("detuneCents", kDetuneCents),
    range_field("delayMs", kHarmonyDelayMs),
};
constexpr FieldSpec kVocoderFields[] = {
    bool_field("enabled"),
    range_field("bands", kVocoderBands, true),
    range_field("attackMs", kVocoderAttackMs),
    range_field("releaseMs", kVocoderReleaseMs),
    range_field("sibilance", kUnit),
    enum_field<VocoderCarrierSource>("carrierSource"),
    enum_field<VocoderCarrierWave>("carrierWave"),
    range_field("ensembleDetuneCents", kDetuneCents),
    range_field("level", kUnit),
};
constexpr FieldSpec kCharacterFields[] = {
    enum_field<CharacterType>("type"),
    range_field("amount", kUnit),
    range_field("tone", kBipolar),
};
constexpr FieldSpec kReverbVariationFields[] = {
    enum_field<ReverbVariation>("variation"), range_field("delayMs", kSpaceDelayMs),
    range_field("feedback", kSpaceFeedback),  range_field("tempoBpm", kTempoBpm),
    enum_field<NoteDivision>("division"),
};
constexpr FieldSpec kMidiFields[] = {
    enum_field<MidiRelease>("release"),
};

// The voice transformer sections of a dsp object. Present sections are validated in full.
bool validate_transform_sections(const sonare::util::json::Value& dsp, std::string* error) {
  if (!validate_optional_section(dsp, "voiceEngine", "dsp.voiceEngine", kVoiceEngineFields,
                                 error) ||
      !validate_optional_section(dsp, "pitchCorrection", "dsp.pitchCorrection",
                                 kPitchCorrectionFields, error)) {
    return false;
  }
  if (const auto* section = dsp.find("harmony")) {
    if (!validate_range_section(*section, "dsp.harmony", kHarmonyFields, error)) return false;
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
  return validate_optional_section(dsp, "vocoder", "dsp.vocoder", kVocoderFields, error) &&
         validate_optional_section(dsp, "character", "dsp.character", kCharacterFields, error) &&
         validate_optional_section(dsp, "reverbVariation", "dsp.reverbVariation",
                                   kReverbVariationFields, error) &&
         validate_optional_section(dsp, "midi", "dsp.midi", kMidiFields, error);
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

  if (!validate_range_section(*dsp.find("retune"), "dsp.retune", kRetuneFields, error) ||
      !validate_range_section(*dsp.find("formant"), "dsp.formant", kFormantFields, error) ||
      !validate_range_section(*dsp.find("eq"), "dsp.eq", kEqFields, error) ||
      !validate_range_section(*dsp.find("gate"), "dsp.gate", kGateFields, error) ||
      !validate_range_section(*dsp.find("compressor"), "dsp.compressor", kCompressorFields,
                              error) ||
      !validate_range_section(*dsp.find("deesser"), "dsp.deesser", kDeesserFields, error) ||
      !validate_range_section(*dsp.find("reverb"), "dsp.reverb", kReverbFields, error) ||
      !validate_range_section(*dsp.find("limiter"), "dsp.limiter", kLimiterFields, error)) {
    return false;
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
  // A normalized config is also a valid, saveable custom preset with the metadata the strict
  // preset validator requires.
  dump_preset_envelope(out, "custom", "Custom", nullptr, "custom", c);
  return out;
}

std::string realtime_voice_changer_preset_json(VoiceCharacterPreset preset) {
  const auto c = realtime_voice_changer_preset(preset);
  std::string out;
  dump_preset_envelope(out, realtime_voice_changer_preset_id(preset),
                       realtime_voice_changer_preset_display_name(preset), nullptr,
                       std::string(preset_metadata(preset).category), c);
  return out;
}

bool validate_realtime_voice_changer_preset_json(std::string_view json,
                                                 std::string* normalized_json, std::string* error) {
  if (normalized_json) normalized_json->clear();
  if (error) error->clear();
  try {
    const std::string trimmed = trim_copy(json);
    // Strict parse: a preset document with duplicate keys (e.g. two `"id"`
    // entries) is a user-config bug — fail fast rather than silently keeping
    // the last value. The lenient `realtime_voice_changer_config_from_json`
    // path below keeps using `admit` (not `admit_strict`) because it doubles
    // as the realtime C-API entry point and must stay maximally tolerant of
    // duplicate keys; both still run under the same resource budget.
    const auto root = sonare::util::json::admit_strict(trimmed);
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
    const auto config = realtime_voice_changer_config_from_json(trimmed);
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
      const auto* description = root.find("description");
      dump_preset_envelope(out, root.find("id")->as_string(), root.find("name")->as_string(),
                           description ? &description->as_string() : nullptr,
                           root.find("category")->as_string(), config);
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
