#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
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

namespace detail {

bool enum_value_from_spelling(const EnumName* rows, std::size_t count, std::string_view name,
                              int* out) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    if (rows[i].name == name) {
      *out = rows[i].value;
      return true;
    }
  }
  return false;
}

std::string enum_spelling_list(const EnumName* rows, std::size_t count) {
  std::string out;
  for (std::size_t i = 0; i < count; ++i) {
    if (i > 0) out += i + 1 == count ? " or " : ", ";
    out += '\'';
    out += rows[i].name;
    out += '\'';
  }
  return out;
}

}  // namespace detail

using namespace detail;

namespace {

namespace schema = sonare::util::json::schema;
using schema::Kind;

[[noreturn]] void refuse(const std::string& message) {
  throw SonareException(ErrorCode::InvalidFormat, message);
}

// The readers below run on a validated document; a present key of the wrong type is still refused
// by its path, and only an absent key keeps the fallback. @p section is the enclosing object's
// path.
const sonare::util::json::Value* object_section(const sonare::util::json::Value& object,
                                                const char* section, const char* key) {
  const auto* value = object.find(key);
  if (value != nullptr && !value->is_object()) {
    refuse(schema::must_be(schema::child_path(section, key), Kind::Object));
  }
  return value;
}

float object_number(const sonare::util::json::Value& object, const char* section, const char* key,
                    float fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  if (!value->is_number()) refuse(schema::must_be(schema::child_path(section, key), Kind::Number));
  // An overflow reads as an infinity, which the config validator refuses by name.
  return value->as_float();
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
  if (!value->is_number()) refuse(schema::must_be(schema::child_path(section, key), Kind::Number));
  return json_number_to_int(value->as_number(), fallback);
}

constexpr const char* kGrainZeroPredicate = "must not be 0; omit it for the automatic grain";

// The retune grain: an absent key is the automatic grain (0 in the config), and an explicit 0 is
// refused so that "automatic" has one spelling in a document.
int object_grain(const sonare::util::json::Value& object, const char* section, const char* key) {
  const int grain = object_int(object, section, key, 0);
  if (object.find(key) != nullptr && grain == 0) {
    refuse(schema::refusal(schema::child_path(section, key), kGrainZeroPredicate));
  }
  return grain;
}

// The harmony steps as an array of up to kMaxHarmonyVoices numbers; entries past its end keep
// their current value.
void object_steps(const sonare::util::json::Value& object, const char* section, const char* key,
                  int (&steps)[kMaxHarmonyVoices]) {
  const auto* value = object.find(key);
  if (!value) return;
  const std::string path = schema::child_path(section, key);
  if (!value->is_array()) refuse(schema::must_be(path, Kind::Array));
  const auto& items = value->as_array();
  if (items.size() > static_cast<std::size_t>(kMaxHarmonyVoices)) {
    refuse(schema::refusal(path,
                           "must have at most " + std::to_string(kMaxHarmonyVoices) + " entries"));
  }
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (!items[i].is_number()) refuse(schema::must_be(schema::index_path(path, i), Kind::Number));
    steps[i] = json_number_to_int(items[i].as_number(), steps[i]);
  }
}

int object_enum_value(const sonare::util::json::Value& object, const char* section, const char* key,
                      const EnumName* rows, std::size_t count, int fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  const std::string path = schema::child_path(section, key);
  if (!value->is_string()) refuse(schema::must_be(path, Kind::String));
  int parsed = fallback;
  if (!enum_value_from_spelling(rows, count, value->as_string(), &parsed)) {
    refuse(schema::refusal(path, "must be " + enum_spelling_list(rows, count)));
  }
  return parsed;
}

template <typename Enum>
Enum object_enum(const sonare::util::json::Value& object, const char* section, const char* key,
                 Enum fallback) {
  const auto& names = EnumNames<Enum>::kNames;
  return static_cast<Enum>(object_enum_value(object, section, key, names.data(), names.size(),
                                             static_cast<int>(fallback)));
}

constexpr const char* kFormantModePredicate = "must be 'relative' or 'absolute'";

bool formant_mode_from_spelling(std::string_view name, FormantMode* out) noexcept {
  if (name == "relative") {
    *out = FormantMode::Relative;
    return true;
  }
  if (name == "absolute") {
    *out = FormantMode::Absolute;
    return true;
  }
  return false;
}

FormantMode object_formant_mode(const sonare::util::json::Value& object, const char* section,
                                const char* key, FormantMode fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  const std::string path = schema::child_path(section, key);
  if (!value->is_string()) refuse(schema::must_be(path, Kind::String));
  FormantMode mode;
  if (formant_mode_from_spelling(value->as_string(), &mode)) return mode;
  refuse(schema::refusal(path, kFormantModePredicate));
}

bool object_bool(const sonare::util::json::Value& object, const char* section, const char* key,
                 bool fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  if (!value->is_bool()) refuse(schema::must_be(schema::child_path(section, key), Kind::Boolean));
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
  dump_field(out, "pitchSemitones", c.retune.semitones);
  // The automatic grain is written by omission, the one spelling a reader accepts for it.
  dump_field(out, "mix", c.retune.mix, c.retune.grain_size == 0);
  if (c.retune.grain_size != 0) dump_int_field(out, "grainSizeSamples", c.retune.grain_size, true);
  out += "},\"formant\":{";
  dump_field(out, "factor", c.formant.factor);
  dump_field(out, "amount", c.formant.amount);
  dump_field(out, "body", c.formant.body);
  dump_field(out, "brightness", c.formant.brightness);
  dump_field(out, "nasal", c.formant.nasal);
  out += "\"mode\":\"";
  out += formant_mode_name(c.formant_mode);
  out += "\"},\"eq\":{";
  dump_field(out, "highPassHz", c.eq.highpass_hz);
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

using sonare::util::json::schema::has_allowed_keys;
using sonare::util::json::schema::require_key;
using sonare::util::json::schema::require_number;
using sonare::util::json::schema::require_string;

bool require_in(const sonare::util::json::Value& object, const char* key, const Range& range,
                const char* path, std::string* error, bool integer = false) {
  return require_number(object, key, range.lo, range.hi, path, error, integer);
}

/// Verifies the preset id matches the schema regex `^[a-z0-9][A-Za-z0-9._-]*$` and
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
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

bool require_enum_in(const sonare::util::json::Value& object, const char* key, const char* path,
                     std::string* error, const EnumName* rows, std::size_t count) {
  const auto* value = object.find(key);
  int parsed = 0;
  if (value != nullptr && value->is_string() &&
      enum_value_from_spelling(rows, count, value->as_string(), &parsed)) {
    return true;
  }
  if (error) {
    *error = schema::refusal(schema::child_path(path, key),
                             "must be " + enum_spelling_list(rows, count));
  }
  return false;
}

template <typename Enum>
bool require_enum(const sonare::util::json::Value& object, const char* key, const char* path,
                  std::string* error) {
  const auto& names = EnumNames<Enum>::kNames;
  return require_enum_in(object, key, path, error, names.data(), names.size());
}

bool require_bool(const sonare::util::json::Value& object, const char* key, const char* path,
                  std::string* error) {
  const auto* value = object.find(key);
  if (value != nullptr && value->is_bool()) return true;
  if (error) *error = schema::must_be(schema::child_path(path, key), Kind::Boolean);
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
  FormantMode parsed;
  if (!mode->is_string() || !formant_mode_from_spelling(mode->as_string(), &parsed)) {
    if (error) *error = schema::refusal(schema::child_path(path, key), kFormantModePredicate);
    return false;
  }
  return true;
}

template <std::size_t N>
bool has_allowed_fields(const sonare::util::json::Value& object, const FieldSpec (&fields)[N],
                        std::string_view path, std::string* error) {
  if (!object.is_object()) {
    if (error) *error = schema::must_be(path, Kind::Object);
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
      if (error) *error = schema::unknown_key(schema::child_path(path, key));
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

bool grain_in_domain(double n) {
  return n >= kGrainSize.lo && n <= kGrainSize.hi && std::floor(n) == n &&
         static_cast<int>(n) % kRetuneGrainStep == 0;
}

std::string grain_domain_predicate() {
  return "must be a multiple of " + std::to_string(kRetuneGrainStep) + " in [" +
         std::to_string(kRetuneGrainStep) + ", " + std::to_string(kMaxRetuneGrainSize) + "]";
}

// An absent grain is the automatic one; a written 0 is refused by name rather than as out of range.
bool require_grain(const sonare::util::json::Value& object, const char* key, const char* path,
                   std::string* error) {
  const auto* value = object.find(key);
  if (value->is_number() && value->as_number() == 0.0) {
    if (error) *error = schema::refusal(schema::child_path(path, key), kGrainZeroPredicate);
    return false;
  }
  if (value->is_number() && !grain_in_domain(value->as_number())) {
    if (error) *error = schema::refusal(schema::child_path(path, key), grain_domain_predicate());
    return false;
  }
  return require_in(object, key, kGrainSize, path, error, true);
}

constexpr FieldSpec kRetuneFields[] = {
    range_field("pitchSemitones", kSemitones),
    range_field("mix", kUnit),
    {"grainSizeSamples", &require_grain, {}, true, false},
};
constexpr FieldSpec kFormantFields[] = {
    range_field("factor", kFormantFactor), range_field("amount", kUnit),
    range_field("body", kBipolar),         range_field("brightness", kBipolar),
    range_field("nasal", kBipolar),        {"mode", &require_formant_mode, {}, false, false},
};
constexpr FieldSpec kEqFields[] = {
    range_field("highPassHz", kHighpassHz),
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
        *error = schema::refusal(
            schema::child_path("dsp.harmony", "steps"),
            "must be an array of at most " + std::to_string(kMaxHarmonyVoices) + " integers in [" +
                schema::bound_text(kHarmonyStep.lo) + ", " + schema::bound_text(kHarmonyStep.hi) +
                "], at least one per voice of dsp.harmony.voices");
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

bool validate_dsp_section(const sonare::util::json::Value& dsp, std::string* error) {
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
  for (const char* key : kTransformSections) {
    if (!require_key(dsp, key, "dsp", error)) return false;
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

// A flat POD's harmonySteps: exactly kMaxHarmonyVoices whole numbers that fit an int.
bool is_step_triple(const sonare::util::json::Value& value) {
  if (!value.is_array() || value.as_array().size() != static_cast<std::size_t>(kMaxHarmonyVoices)) {
    return false;
  }
  for (const auto& item : value.as_array()) {
    if (!item.is_number() || std::floor(item.as_number()) != item.as_number() ||
        item.as_number() < kReverbSeed.lo || item.as_number() > kReverbSeed.hi) {
      return false;
    }
  }
  return true;
}

// Reads a document whose keys and types were already checked; the config validator runs after it.
RealtimeVoiceChangerConfig parse_config_value(const sonare::util::json::Value& root) {
  RealtimeVoiceChangerConfig c;
  if (const auto* macros = root.find("macros")) {
    std::string error;
    if (!apply_macro_section(*macros, &c, &error)) refuse(error);
    return c;
  }
  const bool flat = root.find("retunePitchSemitones") != nullptr;
  const auto* dsp = flat ? &root : object_section(root, "", "dsp");
  if (dsp == nullptr) refuse(schema::missing_key("dsp"));
  const auto& object = *dsp;
  const char* base = flat ? "" : "dsp";
  c.input_gain_db = object_number(object, base, "inputGainDb", c.input_gain_db);
  c.output_gain_db = object_number(object, base, "outputGainDb", c.output_gain_db);
  c.wet_mix = object_number(object, base, "wetMix", c.wet_mix);

  // JS bindings expose preset configs as a flat POD for efficient FFI. Accept
  // that camelCase shape here, at the one parser shared by C ABI, Node, and
  // WASM, rather than making every binding hand-expand it into nested JSON.
  if (flat) {
    c.retune.semitones = object_number(object, "", "retunePitchSemitones", c.retune.semitones);
    c.retune.mix = object_number(object, "", "retuneMix", c.retune.mix);
    c.retune.grain_size = object_grain(object, "", "retuneGrainSizeSamples");
    c.formant.factor = object_number(object, "", "formantFactor", c.formant.factor);
    c.formant.amount = object_number(object, "", "formantAmount", c.formant.amount);
    c.formant.body = object_number(object, "", "formantBody", c.formant.body);
    c.formant.brightness = object_number(object, "", "formantBrightness", c.formant.brightness);
    c.formant.nasal = object_number(object, "", "formantNasal", c.formant.nasal);
    c.formant_mode = object_formant_mode(object, "", "formantMode", c.formant_mode);
    c.eq.highpass_hz = object_number(object, "", "eqHighPassHz", c.eq.highpass_hz);
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
    c.voice_engine.engine = object_enum(object, "", "voiceEngine", c.voice_engine.engine);
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
    object_steps(object, "", "harmonySteps", c.harmony.steps);
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
        object_enum(object, "", "reverbVariation", c.reverb_variation.variation);
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

  if (const auto* v = object_section(object, "dsp", "retune")) {
    c.retune.semitones = object_number(*v, "dsp.retune", "pitchSemitones", c.retune.semitones);
    c.retune.mix = object_number(*v, "dsp.retune", "mix", c.retune.mix);
    c.retune.grain_size = object_grain(*v, "dsp.retune", "grainSizeSamples");
  }
  if (const auto* v = object_section(object, "dsp", "formant")) {
    c.formant.factor = object_number(*v, "dsp.formant", "factor", c.formant.factor);
    c.formant.amount = object_number(*v, "dsp.formant", "amount", c.formant.amount);
    c.formant.body = object_number(*v, "dsp.formant", "body", c.formant.body);
    c.formant.brightness = object_number(*v, "dsp.formant", "brightness", c.formant.brightness);
    c.formant.nasal = object_number(*v, "dsp.formant", "nasal", c.formant.nasal);
    c.formant_mode = object_formant_mode(*v, "dsp.formant", "mode", c.formant_mode);
  }
  if (const auto* v = object_section(object, "dsp", "eq")) {
    c.eq.highpass_hz = object_number(*v, "dsp.eq", "highPassHz", c.eq.highpass_hz);
    c.eq.body_db = object_number(*v, "dsp.eq", "bodyDb", c.eq.body_db);
    c.eq.presence_db = object_number(*v, "dsp.eq", "presenceDb", c.eq.presence_db);
    c.eq.air_db = object_number(*v, "dsp.eq", "airDb", c.eq.air_db);
  }
  if (const auto* v = object_section(object, "dsp", "gate")) {
    c.gate.threshold_db = object_number(*v, "dsp.gate", "thresholdDb", c.gate.threshold_db);
    c.gate.attack_ms = object_number(*v, "dsp.gate", "attackMs", c.gate.attack_ms);
    c.gate.release_ms = object_number(*v, "dsp.gate", "releaseMs", c.gate.release_ms);
    c.gate.range_db = object_number(*v, "dsp.gate", "rangeDb", c.gate.range_db);
  }
  if (const auto* v = object_section(object, "dsp", "compressor")) {
    c.compressor.threshold_db =
        object_number(*v, "dsp.compressor", "thresholdDb", c.compressor.threshold_db);
    c.compressor.ratio = object_number(*v, "dsp.compressor", "ratio", c.compressor.ratio);
    c.compressor.attack_ms =
        object_number(*v, "dsp.compressor", "attackMs", c.compressor.attack_ms);
    c.compressor.release_ms =
        object_number(*v, "dsp.compressor", "releaseMs", c.compressor.release_ms);
    c.compressor.makeup_gain_db =
        object_number(*v, "dsp.compressor", "makeupGainDb", c.compressor.makeup_gain_db);
  }
  if (const auto* v = object_section(object, "dsp", "deesser")) {
    c.deesser.frequency_hz =
        object_number(*v, "dsp.deesser", "frequencyHz", c.deesser.frequency_hz);
    c.deesser.threshold_db =
        object_number(*v, "dsp.deesser", "thresholdDb", c.deesser.threshold_db);
    c.deesser.ratio = object_number(*v, "dsp.deesser", "ratio", c.deesser.ratio);
    c.deesser.range_db = object_number(*v, "dsp.deesser", "rangeDb", c.deesser.range_db);
  }
  if (const auto* v = object_section(object, "dsp", "reverb")) {
    c.reverb.mix = object_number(*v, "dsp.reverb", "mix", c.reverb.mix);
    c.reverb.time_ms = object_number(*v, "dsp.reverb", "timeMs", c.reverb.time_ms);
    c.reverb.damping = object_number(*v, "dsp.reverb", "damping", c.reverb.damping);
    c.reverb.seed = object_int(*v, "dsp.reverb", "seed", c.reverb.seed);
  }
  if (const auto* v = object_section(object, "dsp", "limiter")) {
    c.limiter.ceiling_db = object_number(*v, "dsp.limiter", "ceilingDb", c.limiter.ceiling_db);
    c.limiter.release_ms = object_number(*v, "dsp.limiter", "releaseMs", c.limiter.release_ms);
    c.limiter.enable_isp_limiter =
        object_bool(*v, "dsp.limiter", "enableIspLimiter", c.limiter.enable_isp_limiter);
    c.limiter.isp_ceiling_dbtp =
        object_number(*v, "dsp.limiter", "ispCeilingDbtp", c.limiter.isp_ceiling_dbtp);
  }
  if (const auto* v = object_section(object, "dsp", "voiceEngine")) {
    c.voice_engine.engine = object_enum(*v, "dsp.voiceEngine", "engine", c.voice_engine.engine);
    c.voice_engine.f0_min_hz =
        object_number(*v, "dsp.voiceEngine", "f0MinHz", c.voice_engine.f0_min_hz);
  }
  if (const auto* v = object_section(object, "dsp", "pitchCorrection")) {
    PitchCorrectionConfig& pc = c.pitch_correction;
    pc.mode = object_enum(*v, "dsp.pitchCorrection", "mode", pc.mode);
    pc.amount = object_number(*v, "dsp.pitchCorrection", "amount", pc.amount);
    pc.speed_ms = object_number(*v, "dsp.pitchCorrection", "speedMs", pc.speed_ms);
    pc.key_root = object_int(*v, "dsp.pitchCorrection", "keyRoot", pc.key_root);
    pc.scale = object_enum(*v, "dsp.pitchCorrection", "scale", pc.scale);
    pc.robot_octave = object_int(*v, "dsp.pitchCorrection", "robotOctave", pc.robot_octave);
  }
  if (const auto* v = object_section(object, "dsp", "harmony")) {
    HarmonyConfig& hm = c.harmony;
    hm.voices = object_int(*v, "dsp.harmony", "voices", hm.voices);
    object_steps(*v, "dsp.harmony", "steps", hm.steps);
    hm.level = object_number(*v, "dsp.harmony", "level", hm.level);
    hm.lead_level = object_number(*v, "dsp.harmony", "leadLevel", hm.lead_level);
    hm.detune_cents = object_number(*v, "dsp.harmony", "detuneCents", hm.detune_cents);
    hm.delay_ms = object_number(*v, "dsp.harmony", "delayMs", hm.delay_ms);
  }
  if (const auto* v = object_section(object, "dsp", "vocoder")) {
    VocoderConfig& vc = c.vocoder;
    vc.enabled = object_bool(*v, "dsp.vocoder", "enabled", vc.enabled);
    vc.bands = object_int(*v, "dsp.vocoder", "bands", vc.bands);
    vc.attack_ms = object_number(*v, "dsp.vocoder", "attackMs", vc.attack_ms);
    vc.release_ms = object_number(*v, "dsp.vocoder", "releaseMs", vc.release_ms);
    vc.sibilance = object_number(*v, "dsp.vocoder", "sibilance", vc.sibilance);
    vc.carrier_source = object_enum(*v, "dsp.vocoder", "carrierSource", vc.carrier_source);
    vc.carrier_wave = object_enum(*v, "dsp.vocoder", "carrierWave", vc.carrier_wave);
    vc.ensemble_detune_cents =
        object_number(*v, "dsp.vocoder", "ensembleDetuneCents", vc.ensemble_detune_cents);
    vc.level = object_number(*v, "dsp.vocoder", "level", vc.level);
  }
  if (const auto* v = object_section(object, "dsp", "character")) {
    c.character.type = object_enum(*v, "dsp.character", "type", c.character.type);
    c.character.amount = object_number(*v, "dsp.character", "amount", c.character.amount);
    c.character.tone = object_number(*v, "dsp.character", "tone", c.character.tone);
  }
  if (const auto* v = object_section(object, "dsp", "reverbVariation")) {
    ReverbVariationConfig& rv = c.reverb_variation;
    rv.variation = object_enum(*v, "dsp.reverbVariation", "variation", rv.variation);
    rv.delay_ms = object_number(*v, "dsp.reverbVariation", "delayMs", rv.delay_ms);
    rv.feedback = object_number(*v, "dsp.reverbVariation", "feedback", rv.feedback);
    rv.tempo_bpm = object_number(*v, "dsp.reverbVariation", "tempoBpm", rv.tempo_bpm);
    rv.division = object_enum(*v, "dsp.reverbVariation", "division", rv.division);
  }
  if (const auto* v = object_section(object, "dsp", "midi")) {
    c.midi.release = object_enum(*v, "dsp.midi", "release", c.midi.release);
  }
  return c;
}

// The preset document reader: every key known, every required key given, every value typed and
// in its domain, then the whole configuration through the config validator.
bool read_preset_document(const sonare::util::json::Value& root, RealtimeVoiceChangerConfig* config,
                          std::string* error) {
  if (!has_allowed_keys(root,
                        {"schemaVersion", "id", "name", "description", "category", "macros", "dsp"},
                        "", error)) {
    return false;
  }
  for (const char* key : {"schemaVersion", "id", "name", "category"}) {
    if (!require_key(root, key, "", error)) return false;
  }
  const auto& schema_version = *root.find("schemaVersion");
  if (!schema_version.is_number()) {
    if (error) *error = schema::must_be("schemaVersion", Kind::Number);
    return false;
  }
  if (const double n = schema_version.as_number();
      n != static_cast<double>(kVoiceChangerPresetSchemaVersion)) {
    if (error) {
      const bool whole = std::isfinite(n) && std::floor(n) == n;
      const char* written_for = !whole ? ""
                                : n < kVoiceChangerPresetSchemaVersion
                                    ? "; the document was written for an older schema"
                                    : "; the document was written for a newer schema";
      *error = schema::refusal("schemaVersion",
                               "must be " + std::to_string(kVoiceChangerPresetSchemaVersion) +
                                   ", the version this release reads" + written_for);
    }
    return false;
  }
  if (!require_string(root, "id", 1, 96, "", error)) return false;
  if (!is_valid_preset_id(root.find("id")->as_string())) {
    if (error) {
      *error = schema::refusal("id",
                               "must start with a lowercase letter or a digit and hold only "
                               "letters, digits, '.', '_' and '-'");
    }
    return false;
  }
  if (!require_string(root, "name", 1, 96, "", error)) return false;
  if (root.find("description") && !require_string(root, "description", 0, 512, "", error)) {
    return false;
  }
  if (!require_string(root, "category", 1, 16, "", error)) return false;
  {
    static constexpr std::array<EnumName, 11> kCategories = {{{0, "monitor"},
                                                              {0, "bright"},
                                                              {0, "soft"},
                                                              {0, "deep"},
                                                              {0, "robot"},
                                                              {0, "dark"},
                                                              {0, "tune"},
                                                              {0, "harmony"},
                                                              {0, "vocoder"},
                                                              {0, "character"},
                                                              {0, "custom"}}};
    const auto& category = root.find("category")->as_string();
    int unused = 0;
    if (!enum_value_from_spelling(kCategories.data(), kCategories.size(), category, &unused)) {
      if (error) {
        *error = schema::refusal(
            "category", "must be " + enum_spelling_list(kCategories.data(), kCategories.size()));
      }
      return false;
    }
  }
  const auto* dsp = root.find("dsp");
  const auto* macros = root.find("macros");
  if ((dsp == nullptr) == (macros == nullptr)) {
    if (error) *error = "the document must hold exactly one of dsp and macros";
    return false;
  }
  if (dsp != nullptr && !validate_dsp_section(*dsp, error)) return false;
  *config = parse_config_value(root);
  // What only the whole configuration can say; the same rule create and set_config apply.
  return validate_realtime_voice_changer_config(
      *config, nullptr, error,
      dsp != nullptr ? ConfigFieldSpelling::PresetDocument : ConfigFieldSpelling::Member);
}

}  // namespace

RealtimeVoiceChangerConfig realtime_voice_changer_config_from_json(std::string_view text) {
  RealtimeVoiceChangerConfig config;
  std::string error;
  if (!realtime_voice_changer_config_from_input(text, &config, &error)) {
    throw SonareException(ErrorCode::InvalidFormat, error);
  }
  return config;
}

std::string realtime_voice_changer_config_to_json(const RealtimeVoiceChangerConfig& config) {
  std::string error;
  if (!validate_realtime_voice_changer_config(config, nullptr, &error)) {
    throw SonareException(ErrorCode::InvalidParameter, error);
  }
  std::string out;
  // A valid config is also a saveable custom preset with the metadata the strict preset validator
  // requires.
  dump_preset_envelope(out, "custom", "Custom", nullptr, "custom", config);
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
    const auto root = sonare::util::json::admit_strict(trim_copy(json));
    RealtimeVoiceChangerConfig config;
    if (!read_preset_document(root, &config, error)) return false;
    if (normalized_json) {
      // The caller's own identity is written back, so a pack's ids survive the round trip.
      const auto* description = root.find("description");
      dump_preset_envelope(*normalized_json, root.find("id")->as_string(),
                           root.find("name")->as_string(),
                           description ? &description->as_string() : nullptr,
                           root.find("category")->as_string(), config);
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

    const auto root = sonare::util::json::admit_strict(input);
    static constexpr std::array<const char*, 35> kFlatPodKeys = {
        "inputGainDb",
        "outputGainDb",
        "wetMix",
        "retunePitchSemitones",
        "retuneMix",
        "formantFactor",
        "formantAmount",
        "formantBody",
        "formantBrightness",
        "formantNasal",
        "eqHighPassHz",
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
    static constexpr std::array<const char*, 32> kTransformKeys = {
        "voiceEngine",
        "voiceEngineF0MinHz",
        "pitchCorrectionMode",
        "pitchCorrectionAmount",
        "pitchCorrectionSpeedMs",
        "pitchCorrectionKeyRoot",
        "pitchCorrectionScale",
        "pitchCorrectionRobotOctave",
        "harmonyVoices",
        "harmonySteps",
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
        "reverbVariation",
        "reverbVariationDelayMs",
        "reverbVariationFeedback",
        "reverbVariationTempoBpm",
        "reverbVariationDivision",
        "midiRelease",
    };
    // The flat POD the bindings build from a whole config: an object holding a flat key and no
    // preset-document key. It must be complete and typed key by key, so a partial object is
    // refused by the key it lacks rather than read as a preset document.
    const auto holds_flat_key = [&root] {
      for (const auto& [key, _value] : root.as_object()) {
        for (const char* flat : kFlatPodKeys)
          if (key == flat) return true;
        for (const char* flat : kTransformKeys)
          if (key == flat) return true;
      }
      return false;
    };
    if (root.is_object() && root.find("schemaVersion") == nullptr && root.find("dsp") == nullptr &&
        root.find("macros") == nullptr && holds_flat_key()) {
      static constexpr std::array<std::string_view, 9> kTransformEnumKeys = {
          "voiceEngine",          "pitchCorrectionMode",     "pitchCorrectionScale",
          "vocoderCarrierSource", "vocoderCarrierWave",      "characterType",
          "reverbVariation",      "reverbVariationDivision", "midiRelease"};
      struct IntegerKey {
        std::string_view key;
        Range range;
      };
      static constexpr std::array<IntegerKey, 5> kIntegerKeys = {
          {{"reverbSeed", kReverbSeed},
           {"pitchCorrectionKeyRoot", kKeyRoot},
           {"pitchCorrectionRobotOctave", kRobotOctave},
           {"harmonyVoices", kHarmonyVoices},
           {"vocoderBands", kVocoderBands}}};
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
          if (key == "harmonySteps") {
            if (!is_step_triple(_value)) {
              if (error) {
                *error = schema::refusal(
                    key, "must be an array of " + std::to_string(kMaxHarmonyVoices) + " integers");
              }
              return false;
            }
          } else {
            const Kind kind = listed(kTransformEnumKeys, key) ? Kind::String
                              : key == "vocoderEnabled"       ? Kind::Boolean
                                                              : Kind::Number;
            const bool typed = kind == Kind::String    ? _value.is_string()
                               : kind == Kind::Boolean ? _value.is_bool()
                                                       : _value.is_number();
            if (!typed) {
              if (error) *error = schema::must_be(key, kind);
              return false;
            }
          }
          allowed = true;
        }
        // The automatic grain is the absent key; a present one is a whole size in range.
        if (key == "retuneGrainSizeSamples") {
          if (!_value.is_number()) {
            if (error) *error = schema::must_be(key, Kind::Number);
            return false;
          }
          if (_value.as_number() == 0.0) {
            if (error) *error = schema::refusal(key, kGrainZeroPredicate);
            return false;
          }
          if (!grain_in_domain(_value.as_number())) {
            if (error) *error = schema::refusal(key, grain_domain_predicate());
            return false;
          }
          allowed = true;
        }
        // formantMode is optional: documents written before the mode existed omit it.
        if (key == "formantMode") {
          if (!_value.is_string()) {
            if (error) *error = schema::must_be(key, Kind::String);
            return false;
          }
          allowed = true;
        }
        if (!allowed) {
          if (error) *error = schema::unknown_key(key);
          return false;
        }
      }
      for (const char* key : kFlatPodKeys) {
        if (!require_key(root, key, "", error)) return false;
        const auto* value = root.find(key);
        const bool is_isp_enabled = std::string_view(key) == "limiterEnableIspLimiter";
        if (is_isp_enabled ? !value->is_bool() : !value->is_number()) {
          if (error) *error = schema::must_be(key, is_isp_enabled ? Kind::Boolean : Kind::Number);
          return false;
        }
      }
      for (const char* key : kTransformKeys) {
        if (!require_key(root, key, "", error)) return false;
      }
      // An integer key is checked before the int conversion could clamp it; a float key's overflow
      // reads as an infinity, which the config validator refuses by name.
      for (const IntegerKey& integer : kIntegerKeys) {
        const double n = root.find(std::string(integer.key))->as_number();
        if (!(std::floor(n) == n && n >= integer.range.lo && n <= integer.range.hi)) {
          if (error) {
            *error = schema::refusal(
                integer.key, schema::integer_range_predicate(integer.range.lo, integer.range.hi));
          }
          return false;
        }
      }
      RealtimeVoiceChangerConfig parsed = parse_config_value(root);
      if (!validate_realtime_voice_changer_config(parsed, nullptr, error,
                                                  ConfigFieldSpelling::FlatPod)) {
        return false;
      }
      *config = parsed;
      return true;
    }

    RealtimeVoiceChangerConfig parsed;
    if (!read_preset_document(root, &parsed, error)) return false;
    *config = parsed;
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

}  // namespace sonare::editing::voice_changer
