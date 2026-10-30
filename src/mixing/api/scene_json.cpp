#include <initializer_list>
#include <string>
#include <unordered_set>
#include <utility>

#include "mastering/api/legacy_insert_params.h"
#include "mastering/eq/eq_band_value.h"
#include "mastering/eq/parametric.h"
#include "mixing/alignment_delay.h"
#include "mixing/api/scene.h"
#include "mixing/api/scene_json_detail.h"
#include "mixing/meter.h"
#include "mixing/pan_law.h"
#include "mixing/panner.h"
#include "mixing/stereo_width.h"
#include "mixing/surround_panner.h"
#include "util/exception.h"
#include "util/json_budget.h"
#include "util/json_key.h"
#include "util/numeric_validation.h"

namespace sonare::mixing::api {
namespace {

using JsonValue = sonare::util::json::Value;

const char* to_string(InsertSlot slot) { return slot == InsertSlot::PreFader ? "pre" : "post"; }

const char* to_string(SendTiming timing) { return timing == SendTiming::PreFader ? "pre" : "post"; }

InsertSlot insert_slot_from_string(const std::string& value) {
  if (value == "pre") return InsertSlot::PreFader;
  if (value == "post") return InsertSlot::PostFader;
  throw SonareException(ErrorCode::InvalidFormat, "unknown insert slot: " + value);
}

SendTiming send_timing_from_string(const std::string& value) {
  if (value == "pre") return SendTiming::PreFader;
  if (value == "post") return SendTiming::PostFader;
  throw SonareException(ErrorCode::InvalidFormat, "unknown send timing: " + value);
}

// Reads an optional channel-layout string field. Absent -> fallback (so old
// scenes without the field round-trip as stereo). Present-but-invalid throws,
// matching the strict behavior of the slot/timing enums above.
ChannelLayout channel_layout_or(const JsonValue& object, const char* key, ChannelLayout fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  if (!value->is_string()) {
    throw SonareException(ErrorCode::InvalidFormat,
                          std::string("channel layout must be a string: ") + key);
  }
  ChannelLayout layout = fallback;
  if (!channel_layout_from_string(value->as_string(), layout)) {
    throw SonareException(ErrorCode::InvalidFormat,
                          "unknown channel layout: " + value->as_string());
  }
  return layout;
}

// ---------------------------------------------------------------------------
// Tree walkers. All parsing is delegated to util::json::parse (one shared
// grammar, one locale-safe number parser). The walkers below populate Scene
// types from the resulting Value tree; unknown fields never fail a load and are
// reported to the caller's warning list when it passes one.
// ---------------------------------------------------------------------------

using KeySet = std::unordered_set<std::string>;

// Direct children of `prefix` in the writer's canonical path list ("" is the root),
// plus the legacy spellings the readers still accept.
KeySet known_keys(const std::string& prefix, std::initializer_list<const char*> legacy) {
  KeySet keys(legacy.begin(), legacy.end());
  const std::string head = prefix.empty() ? std::string() : prefix + ".";
  for (const auto& path : scene_schema_paths()) {
    if (path.compare(0, head.size(), head) != 0) continue;
    const size_t end = path.find_first_of(".[", head.size());
    keys.insert(path.substr(head.size(), end == std::string::npos ? end : end - head.size()));
  }
  return keys;
}

constexpr char kUnknownKeyPrefix[] = "unknown scene key '";

// `$`- and `x-`-prefixed keys are annotations, never reported.
void report_unknown_keys(const JsonValue& object, const std::string& path, const KeySet& known,
                         std::vector<std::string>* warnings) {
  if (warnings == nullptr) return;
  for (const auto& entry : object.as_object()) {
    const std::string& key = entry.first;
    if (known.count(key) != 0 || util::json::is_annotation_key(key)) {
      continue;
    }
    warnings->push_back(std::string(kUnknownKeyPrefix) + (path.empty() ? "" : path + ".") +
                        util::json::escape_key(key) + "'");
  }
}

std::string child_path(const std::string& path, const char* key) {
  return path.empty() ? std::string(key) : path + "." + key;
}

// A present key of the wrong type (a JSON null included) is refused naming its path, as a
// container of the wrong type is; only an absent key keeps the fallback.
[[noreturn]] void refuse_type(const std::string& path, const char* key, const char* expected) {
  throw SonareException(ErrorCode::InvalidFormat, child_path(path, key) + " must be " + expected);
}

const JsonValue* value_or_legacy(const JsonValue& object, const char* key, const char* legacy_key) {
  if (const auto* value = object.find(key)) return value;
  return object.find(legacy_key);
}

// The spelling a present value was written under, for the refusal's path.
const char* spelling_of(const JsonValue& object, const char* key, const char* legacy_key) {
  return object.find(key) ? key : legacy_key;
}

float float_from(const JsonValue& value, const char* key, const char* field_path) {
  float converted = 0.0f;
  if (!numeric::checked_float_cast(value.as_number(), &converted)) {
    throw SonareException(
        ErrorCode::InvalidFormat,
        std::string("floating-point field is non-finite or out of float range: ") +
            (field_path != nullptr ? field_path : key));
  }
  return converted;
}

int int_from(const JsonValue& value, const char* key) {
  int converted = 0;
  if (!numeric::checked_integral_cast(value.as_number(), &converted)) {
    throw SonareException(ErrorCode::InvalidFormat,
                          std::string("integer field is fractional or out of range: ") + key);
  }
  return converted;
}

float number_or(const JsonValue& object, const std::string& path, const char* key, float fallback,
                const char* field_path = nullptr) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  if (!value->is_number()) refuse_type(path, key, "a number");
  return float_from(*value, key, field_path);
}

float number_or_legacy(const JsonValue& object, const std::string& path, const char* key,
                       const char* legacy_key, float fallback, const char* field_path = nullptr) {
  const auto* value = value_or_legacy(object, key, legacy_key);
  if (!value) return fallback;
  if (!value->is_number()) refuse_type(path, spelling_of(object, key, legacy_key), "a number");
  return float_from(*value, key, field_path);
}

// Pan and width are refused outside their documented domains, naming the field.
float checked_pan(float value, const char* field_path) {
  if (!pan_in_range(value)) {
    throw SonareException(ErrorCode::InvalidFormat,
                          std::string(field_path) + " must be within [-1, 1]");
  }
  return value;
}

float checked_width(float value, const char* field_path) {
  if (!width_in_range(value)) {
    throw SonareException(ErrorCode::InvalidFormat,
                          std::string(field_path) + " must be within [0, 2]");
  }
  return value;
}

// Surround placement is refused outside its domains with the same rule as the setters.
float checked_surround_azimuth(float value, const char* field_path) {
  if (!surround_azimuth_in_range(value)) {
    throw SonareException(ErrorCode::InvalidFormat,
                          std::string(field_path) + " must be within [-180, 180]");
  }
  return value;
}

float checked_surround_unit(float value, const char* field_path) {
  if (!surround_unit_in_range(value)) {
    throw SonareException(ErrorCode::InvalidFormat,
                          std::string(field_path) + " must be within [0, 1]");
  }
  return value;
}

int int_or_legacy(const JsonValue& object, const std::string& path, const char* key,
                  const char* legacy_key, int fallback) {
  const auto* value = value_or_legacy(object, key, legacy_key);
  if (!value) return fallback;
  if (!value->is_number()) refuse_type(path, spelling_of(object, key, legacy_key), "an integer");
  return int_from(*value, key);
}

bool bool_or_legacy(const JsonValue& object, const std::string& path, const char* key,
                    const char* legacy_key, bool fallback) {
  const auto* value = value_or_legacy(object, key, legacy_key);
  if (!value) return fallback;
  if (!value->is_bool()) refuse_type(path, spelling_of(object, key, legacy_key), "a boolean");
  return value->as_bool();
}

int int_or(const JsonValue& object, const std::string& path, const char* key, int fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  if (!value->is_number()) refuse_type(path, key, "an integer");
  return int_from(*value, key);
}

bool bool_or(const JsonValue& object, const std::string& path, const char* key, bool fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  if (!value->is_bool()) refuse_type(path, key, "a boolean");
  return value->as_bool();
}

std::string string_or(const JsonValue& object, const std::string& path, const char* key,
                      const std::string& fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  if (!value->is_string()) refuse_type(path, key, "a string");
  return value->as_string();
}

std::string string_or_legacy(const JsonValue& object, const std::string& path, const char* key,
                             const char* legacy_key, const std::string& fallback) {
  const auto* value = value_or_legacy(object, key, legacy_key);
  if (!value) return fallback;
  if (!value->is_string()) refuse_type(path, spelling_of(object, key, legacy_key), "a string");
  return value->as_string();
}

Insert insert_from_value(const JsonValue& object, const std::string& path,
                         std::vector<std::string>* warnings) {
  static const KeySet kKnown =
      known_keys("strips[].inserts[]", {"processor_name", "params_json", "sidechain_key"});
  report_unknown_keys(object, path, kKnown, warnings);
  Insert insert;
  if (const auto* slot = object.find("slot")) {
    if (!slot->is_string()) {
      throw SonareException(ErrorCode::InvalidFormat,
                            "insert slot must be a string (\"pre\" or \"post\")");
    }
    insert.slot = insert_slot_from_string(slot->as_string());
  }
  insert.processor_name = mastering::api::canonical_processor_id(
      string_or_legacy(object, path, "processor", "processor_name", insert.processor_name));
  // The writer embeds the params bag as an object; the string form is what every
  // project file saved before that carries, so both load into the same text.
  if (const auto* params = value_or_legacy(object, "params", "params_json")) {
    if (params->is_object()) {
      insert.params_json = sonare::util::json::dump(*params);
    } else if (params->is_string()) {
      insert.params_json = params->as_string();
    } else {
      throw SonareException(ErrorCode::InvalidFormat,
                            "insert params must be a JSON object (or the legacy JSON string)");
    }
  }
  insert.params_json =
      mastering::api::canonical_insert_params_text(insert.processor_name, insert.params_json);
  insert.sidechain_key =
      string_or_legacy(object, path, "sidechainKey", "sidechain_key", insert.sidechain_key);
  return insert;
}

// Every entity array walker below refuses a present container that is not an array and a
// non-object element (InvalidFormat, named by its path), as the project document's containers
// do; callers pass only present keys, so an absent one keeps its empty default.
template <typename T>
std::vector<T> array_from_value(const JsonValue& array, const std::string& path,
                                std::vector<std::string>* warnings,
                                T (*parse)(const JsonValue&, const std::string&,
                                           std::vector<std::string>*)) {
  if (!array.is_array()) {
    throw SonareException(ErrorCode::InvalidFormat, path + " must be an array");
  }
  std::vector<T> out;
  out.reserve(array.as_array().size());
  size_t index = 0;
  for (const auto& entry : array.as_array()) {
    const size_t position = index++;
    const std::string element_path = path + "[" + std::to_string(position) + "]";
    if (!entry.is_object()) {
      throw SonareException(ErrorCode::InvalidFormat, element_path + " must be an object");
    }
    out.push_back(parse(entry, element_path, warnings));
  }
  return out;
}

std::vector<Insert> inserts_from_value(const JsonValue& array, const std::string& path,
                                       std::vector<std::string>* warnings) {
  return array_from_value<Insert>(array, path, warnings, insert_from_value);
}

Send send_from_value(const JsonValue& object, const std::string& path,
                     std::vector<std::string>* warnings) {
  static const KeySet kKnown = known_keys("strips[].sends[]", {"destination_bus_id", "send_db"});
  report_unknown_keys(object, path, kKnown, warnings);
  Send send;
  send.id = string_or(object, path, "id", send.id);
  send.destination_bus_id = string_or_legacy(object, path, "destinationBusId", "destination_bus_id",
                                             send.destination_bus_id);
  send.send_db = number_or_legacy(object, path, "sendDb", "send_db", send.send_db,
                                  "scene.strips[].sends[].sendDb");
  if (const auto* timing = object.find("timing")) {
    if (!timing->is_string()) {
      throw SonareException(ErrorCode::InvalidFormat,
                            "send timing must be a string (\"pre\" or \"post\")");
    }
    send.timing = send_timing_from_string(timing->as_string());
  }
  return send;
}

std::vector<Send> sends_from_value(const JsonValue& array, const std::string& path,
                                   std::vector<std::string>* warnings) {
  return array_from_value<Send>(array, path, warnings, send_from_value);
}

// Strip and Bus share one EQ shape (StripEq) and one validation. `field_prefix`
// names the enclosing entity ("scene.strips[]" or "scene.buses[]") for error
// messages, matching the convention the other scene fields use.
StripEq eq_from_value(const JsonValue& object, const char* field_prefix, const std::string& path,
                      std::vector<std::string>* warnings) {
  static const KeySet kKnownEq = known_keys("strips[].eq", {});
  static const KeySet kKnownBand(mastering::eq::eq_band_known_keys().begin(),
                                 mastering::eq::eq_band_known_keys().end());
  StripEq eq;
  const auto* eq_value = object.find("eq");
  if (!eq_value) return eq;
  const std::string eq_path = child_path(path, "eq");
  if (!eq_value->is_object()) {
    throw SonareException(ErrorCode::InvalidFormat, eq_path + " must be an object");
  }
  report_unknown_keys(*eq_value, eq_path, kKnownEq, warnings);
  eq.enabled = bool_or(*eq_value, eq_path, "enabled", eq.enabled);
  const auto* bands = eq_value->find("bands");
  if (!bands) return eq;
  if (!bands->is_array()) {
    throw SonareException(ErrorCode::InvalidFormat, eq_path + ".bands must be an array");
  }
  const auto& array = bands->as_array();
  if (array.size() > mastering::eq::ParametricEq::kMaxBands) {
    throw SonareException(ErrorCode::InvalidFormat,
                          std::string(field_prefix) + ".eq.bands has more than 24 bands");
  }
  const std::string band_context = std::string(field_prefix) + ".eq.bands[]: ";
  eq.bands.reserve(array.size());
  for (size_t index = 0; index < array.size(); ++index) {
    const auto& entry = array[index];
    if (entry.is_object()) {
      report_unknown_keys(entry, eq_path + ".bands[" + std::to_string(index) + "]", kKnownBand,
                          warnings);
    }
    const mastering::eq::EqBand band =
        mastering::eq::eq_band_from_value(entry, band_context.c_str());
    // ParametricEq has no tilt design; the shared codec stays permissive for EqualizerProcessor.
    if (band.type == mastering::eq::EqBandType::TiltShelf ||
        band.type == mastering::eq::EqBandType::FlatTilt) {
      throw SonareException(
          ErrorCode::InvalidFormat,
          band_context + "TiltShelf/FlatTilt is not supported on a strip or bus EQ");
    }
    eq.bands.push_back(band);
  }
  return eq;
}

Strip strip_from_value(const JsonValue& object, const std::string& path,
                       std::vector<std::string>* warnings) {
  static const KeySet kKnown =
      known_keys("strips[]", {"input_trim_db", "fader_db", "vca_offset_db", "solo_safe", "pan_mode",
                              "dual_pan_left", "dual_pan_right", "polarity_invert_left",
                              "polarity_invert_right", "pan_law", "channel_delay_samples"});
  static const KeySet kKnownSurround = known_keys("strips[].surroundPan", {});
  static const KeySet kKnownMetering = known_keys("strips[].metering", {});
  report_unknown_keys(object, path, kKnown, warnings);
  Strip strip;
  strip.id = string_or(object, path, "id", strip.id);
  strip.input_trim_db = number_or_legacy(object, path, "inputTrimDb", "input_trim_db",
                                         strip.input_trim_db, "scene.strips[].inputTrimDb");
  strip.fader_db = number_or_legacy(object, path, "faderDb", "fader_db", strip.fader_db,
                                    "scene.strips[].faderDb");
  strip.vca_offset_db = number_or_legacy(object, path, "vcaOffsetDb", "vca_offset_db",
                                         strip.vca_offset_db, "scene.strips[].vcaOffsetDb");
  // Pan and width outside their domains are refused, like the enums and counts below.
  strip.pan = checked_pan(number_or(object, path, "pan", strip.pan, "scene.strips[].pan"),
                          "scene.strips[].pan");
  strip.width = checked_width(number_or(object, path, "width", strip.width, "scene.strips[].width"),
                              "scene.strips[].width");
  strip.muted = bool_or(object, path, "muted", strip.muted);
  strip.soloed = bool_or(object, path, "soloed", strip.soloed);
  strip.solo_safe = bool_or_legacy(object, path, "soloSafe", "solo_safe", strip.solo_safe);
  strip.pan_mode = int_or_legacy(object, path, "panMode", "pan_mode", strip.pan_mode);
  // Reject out-of-range enum/count values in the one canonical scene walker,
  // so standalone mixer JSON and project-embedded scene JSON stay identical.
  if (strip.pan_mode < 0 || strip.pan_mode > 2) {
    throw SonareException(ErrorCode::InvalidFormat, "panMode enum is out of range");
  }
  strip.dual_pan_left =
      checked_pan(number_or_legacy(object, path, "dualPanLeft", "dual_pan_left",
                                   strip.dual_pan_left, "scene.strips[].dualPanLeft"),
                  "scene.strips[].dualPanLeft");
  strip.dual_pan_right =
      checked_pan(number_or_legacy(object, path, "dualPanRight", "dual_pan_right",
                                   strip.dual_pan_right, "scene.strips[].dualPanRight"),
                  "scene.strips[].dualPanRight");
  strip.polarity_invert_left = bool_or_legacy(object, path, "polarityInvertLeft",
                                              "polarity_invert_left", strip.polarity_invert_left);
  strip.polarity_invert_right = bool_or_legacy(
      object, path, "polarityInvertRight", "polarity_invert_right", strip.polarity_invert_right);
  strip.pan_law = int_or_legacy(object, path, "panLaw", "pan_law", strip.pan_law);
  if (strip.pan_law < 0 || strip.pan_law >= kPanLawCount) {
    throw SonareException(ErrorCode::InvalidFormat, "panLaw enum is out of range");
  }
  strip.channel_delay_samples = int_or_legacy(object, path, "channelDelaySamples",
                                              "channel_delay_samples", strip.channel_delay_samples);
  if (strip.channel_delay_samples < 0 || strip.channel_delay_samples > kMaxAlignmentDelaySamples) {
    throw SonareException(ErrorCode::InvalidFormat, "channelDelaySamples must be in [0, 192000]");
  }
  strip.source_layout = channel_layout_or(object, "sourceLayout", strip.source_layout);
  if (const auto* sp = object.find("surroundPan")) {
    const std::string surround_path = child_path(path, "surroundPan");
    if (!sp->is_object()) {
      throw SonareException(ErrorCode::InvalidFormat, surround_path + " must be an object");
    }
    report_unknown_keys(*sp, surround_path, kKnownSurround, warnings);
    SurroundPanParams parsed;
    parsed.azimuth = checked_surround_azimuth(
        number_or(*sp, surround_path, "azimuth", strip.surround_pan.azimuth,
                  "scene.strips[].surroundPan.azimuth"),
        "scene.strips[].surroundPan.azimuth");
    parsed.elevation = number_or(*sp, surround_path, "elevation", strip.surround_pan.elevation,
                                 "scene.strips[].surroundPan.elevation");
    parsed.divergence = checked_surround_unit(
        number_or(*sp, surround_path, "divergence", strip.surround_pan.divergence,
                  "scene.strips[].surroundPan.divergence"),
        "scene.strips[].surroundPan.divergence");
    parsed.lfe = checked_surround_unit(number_or(*sp, surround_path, "lfe", strip.surround_pan.lfe,
                                                 "scene.strips[].surroundPan.lfe"),
                                       "scene.strips[].surroundPan.lfe");
    parsed.distance = number_or(*sp, surround_path, "distance", strip.surround_pan.distance,
                                "scene.strips[].surroundPan.distance");
    // Domains are refused above; the panner's own resolution settles the distance default.
    const SurroundPanParams stored = clamp_surround_pan_params(parsed);
    strip.surround_pan.azimuth = stored.azimuth;
    strip.surround_pan.elevation = stored.elevation;
    strip.surround_pan.divergence = stored.divergence;
    strip.surround_pan.lfe = stored.lfe;
    strip.surround_pan.distance = stored.distance;
  }
  if (const auto* metering = object.find("metering")) {
    const std::string metering_path = child_path(path, "metering");
    if (!metering->is_object()) {
      throw SonareException(ErrorCode::InvalidFormat, metering_path + " must be an object");
    }
    report_unknown_keys(*metering, metering_path, kKnownMetering, warnings);
    strip.metering.enabled = bool_or(*metering, metering_path, "enabled", strip.metering.enabled);
    strip.metering.lufs = bool_or(*metering, metering_path, "lufs", strip.metering.lufs);
    strip.metering.true_peak =
        bool_or(*metering, metering_path, "truePeak", strip.metering.true_peak);
    strip.metering.true_peak_oversample =
        int_or(*metering, metering_path, "truePeakOversample", strip.metering.true_peak_oversample);
    // The realtime meter's own rule, so a scene never stores a factor it cannot run.
    if (!is_realtime_true_peak_oversample(strip.metering.true_peak_oversample)) {
      throw SonareException(ErrorCode::InvalidFormat,
                            "metering.truePeakOversample must be in {4, 8}");
    }
  }
  if (const auto* inserts = object.find("inserts")) {
    strip.inserts = inserts_from_value(*inserts, child_path(path, "inserts"), warnings);
  }
  if (const auto* sends = object.find("sends")) {
    strip.sends = sends_from_value(*sends, child_path(path, "sends"), warnings);
  }
  strip.eq = eq_from_value(object, "scene.strips[]", path, warnings);
  return strip;
}

std::vector<Strip> strips_from_value(const JsonValue& array, const std::string& path,
                                     std::vector<std::string>* warnings) {
  return array_from_value<Strip>(array, path, warnings, strip_from_value);
}

Bus bus_from_value(const JsonValue& object, const std::string& path,
                   std::vector<std::string>* warnings) {
  static const KeySet kKnown =
      known_keys("buses[]", {"input_trim_db", "polarity_invert_left", "polarity_invert_right",
                             "pan_mode", "dual_pan_left", "dual_pan_right", "pan_law"});
  report_unknown_keys(object, path, kKnown, warnings);
  Bus bus;
  bus.id = string_or(object, path, "id", bus.id);
  bus.role = string_or(object, path, "role", bus.role);
  bus.layout = channel_layout_or(object, "layout", bus.layout);
  bus.input_trim_db = number_or_legacy(object, path, "inputTrimDb", "input_trim_db",
                                       bus.input_trim_db, "scene.buses[].inputTrimDb");
  // Same domain as scene.strips[].width above.
  bus.width = checked_width(number_or(object, path, "width", bus.width, "scene.buses[].width"),
                            "scene.buses[].width");
  bus.polarity_invert_left = bool_or_legacy(object, path, "polarityInvertLeft",
                                            "polarity_invert_left", bus.polarity_invert_left);
  bus.polarity_invert_right = bool_or_legacy(object, path, "polarityInvertRight",
                                             "polarity_invert_right", bus.polarity_invert_right);
  // Pan: same field names and ranges as Strip's above.
  bus.pan = checked_pan(number_or(object, path, "pan", bus.pan, "scene.buses[].pan"),
                        "scene.buses[].pan");
  bus.pan_mode = int_or_legacy(object, path, "panMode", "pan_mode", bus.pan_mode);
  if (bus.pan_mode < 0 || bus.pan_mode > 2) {
    throw SonareException(ErrorCode::InvalidFormat, "panMode enum is out of range");
  }
  bus.dual_pan_left = checked_pan(number_or_legacy(object, path, "dualPanLeft", "dual_pan_left",
                                                   bus.dual_pan_left, "scene.buses[].dualPanLeft"),
                                  "scene.buses[].dualPanLeft");
  bus.dual_pan_right =
      checked_pan(number_or_legacy(object, path, "dualPanRight", "dual_pan_right",
                                   bus.dual_pan_right, "scene.buses[].dualPanRight"),
                  "scene.buses[].dualPanRight");
  bus.pan_law = int_or_legacy(object, path, "panLaw", "pan_law", bus.pan_law);
  if (bus.pan_law < 0 || bus.pan_law >= kPanLawCount) {
    throw SonareException(ErrorCode::InvalidFormat, "panLaw enum is out of range");
  }
  // A surround bus has no pan or stereo-width stage, so a non-default value there is
  // refused rather than dropped.
  if (channel_count(bus.layout) > 2) {
    const Bus defaults;
    const char* offending = nullptr;
    if (bus.width != defaults.width) {
      offending = "width";
    } else if (bus.pan != defaults.pan) {
      offending = "pan";
    } else if (bus.pan_mode != defaults.pan_mode) {
      offending = "panMode";
    } else if (bus.pan_law != defaults.pan_law) {
      offending = "panLaw";
    } else if (bus.dual_pan_left != defaults.dual_pan_left) {
      offending = "dualPanLeft";
    } else if (bus.dual_pan_right != defaults.dual_pan_right) {
      offending = "dualPanRight";
    }
    if (offending != nullptr) {
      throw SonareException(ErrorCode::InvalidFormat, "bus '" + bus.id + "' has a non-default " +
                                                          offending +
                                                          " on a surround (>2 channel) layout");
    }
  }
  bus.eq = eq_from_value(object, "scene.buses[]", path, warnings);
  if (const auto* inserts = object.find("inserts")) {
    bus.inserts = inserts_from_value(*inserts, child_path(path, "inserts"), warnings);
  }
  return bus;
}

std::vector<Bus> buses_from_value(const JsonValue& array, const std::string& path,
                                  std::vector<std::string>* warnings) {
  return array_from_value<Bus>(array, path, warnings, bus_from_value);
}

VcaGroup vca_group_from_value(const JsonValue& object, const std::string& path,
                              std::vector<std::string>* warnings) {
  static const KeySet kKnown = known_keys("vcaGroups[]", {"gain_db"});
  report_unknown_keys(object, path, kKnown, warnings);
  VcaGroup group;
  group.id = string_or(object, path, "id", group.id);
  group.gain_db = number_or_legacy(object, path, "gainDb", "gain_db", group.gain_db,
                                   "scene.vcaGroups[].gainDb");
  if (const auto* members = object.find("members")) {
    const std::string members_path = child_path(path, "members");
    if (!members->is_array()) {
      throw SonareException(ErrorCode::InvalidFormat, members_path + " must be an array");
    }
    group.members.reserve(members->as_array().size());
    size_t index = 0;
    for (const auto& entry : members->as_array()) {
      if (!entry.is_string()) {
        throw SonareException(ErrorCode::InvalidFormat,
                              members_path + "[" + std::to_string(index) + "] must be a string");
      }
      group.members.push_back(entry.as_string());
      ++index;
    }
  }
  return group;
}

std::vector<VcaGroup> vca_groups_from_value(const JsonValue& array, const std::string& path,
                                            std::vector<std::string>* warnings) {
  return array_from_value<VcaGroup>(array, path, warnings, vca_group_from_value);
}

Connection connection_from_value(const JsonValue& object, const std::string& path,
                                 std::vector<std::string>* warnings) {
  static const KeySet kKnown = known_keys("connections[]", {});
  report_unknown_keys(object, path, kKnown, warnings);
  Connection connection;
  connection.source = string_or(object, path, "source", connection.source);
  connection.destination = string_or(object, path, "destination", connection.destination);
  return connection;
}

std::vector<Connection> connections_from_value(const JsonValue& array, const std::string& path,
                                               std::vector<std::string>* warnings) {
  return array_from_value<Connection>(array, path, warnings, connection_from_value);
}

// ---------------------------------------------------------------------------
// Tree builders. util::json::dump emits numbers with max_digits10 precision
// and a "." decimal separator whatever LC_NUMERIC says, which matches the
// format the walkers above expect and survives a dump -> parse round-trip
// without coefficient drift.
// ---------------------------------------------------------------------------

// The params bag is embedded as an object, so a blob that is not one is refused
// here rather than travelling to insert construction as unparsed text.
// Budgeted like every caller-supplied document; duplicate keys stay tolerated.
JsonValue insert_params_to_value(const Insert& insert) {
  if (insert.params_json.empty()) return JsonValue(sonare::util::json::Object());
  JsonValue params;
  try {
    params = sonare::util::json::admit(insert.params_json);
  } catch (const sonare::util::json::JsonError& error) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "insert params are not valid JSON (" + insert.processor_name + "): " + error.what());
  }
  if (!params.is_object()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "insert params must be a JSON object: " + insert.processor_name);
  }
  return params;
}

JsonValue insert_to_value(const Insert& insert) {
  sonare::util::json::Object object;
  util::json::put(object, "slot", to_string(insert.slot));
  const std::string processor = mastering::api::canonical_processor_id(insert.processor_name);
  Insert current = insert;
  current.params_json = mastering::api::canonical_insert_params_text(processor, insert.params_json);
  util::json::put(object, "processor", processor);
  util::json::put(object, "params", insert_params_to_value(current));
  // Omit `sidechainKey` when empty: the walker treats a missing field and an
  // empty string identically, but the legacy serializer also dropped the field
  // so existing snapshots can still byte-compare against new output.
  if (!insert.sidechain_key.empty()) {
    util::json::put(object, "sidechainKey", insert.sidechain_key);
  }
  return JsonValue(std::move(object));
}

JsonValue inserts_to_value(const std::vector<Insert>& inserts) {
  sonare::util::json::Array array;
  array.reserve(inserts.size());
  for (const auto& insert : inserts) array.emplace_back(insert_to_value(insert));
  return JsonValue(std::move(array));
}

JsonValue send_to_value(const Send& send) {
  sonare::util::json::Object object;
  util::json::put(object, "id", send.id);
  util::json::put(object, "destinationBusId", send.destination_bus_id);
  util::json::put(object, "sendDb", send.send_db);
  util::json::put(object, "timing", to_string(send.timing));
  return JsonValue(std::move(object));
}

JsonValue sends_to_value(const std::vector<Send>& sends) {
  sonare::util::json::Array array;
  array.reserve(sends.size());
  for (const auto& send : sends) array.emplace_back(send_to_value(send));
  return JsonValue(std::move(array));
}

// Writes `*out` and returns true only when the EQ carries something worth
// serializing (enabled with no bands is the identity), so a strip/bus that
// never touched its EQ carries neither the object nor the "eq" key and an
// existing scene stays byte-identical.
bool eq_to_value(const StripEq& eq, JsonValue* out) {
  static const mastering::eq::EqBand kDefaultBand{};
  sonare::util::json::Array bands;
  bands.reserve(eq.bands.size());
  // Trailing default bands are trimmed; an interior one keeps its slot as {"enabled":false}.
  size_t kept = 0;
  for (size_t i = 0; i < eq.bands.size(); ++i) {
    bands.emplace_back(mastering::eq::eq_band_to_value(eq.bands[i]));
    if (eq.bands[i] != kDefaultBand) kept = i + 1;
  }
  bands.resize(kept);
  if (eq.enabled && bands.empty()) return false;
  sonare::util::json::Object object;
  util::json::put(object, "enabled", eq.enabled);
  util::json::put(object, "bands", std::move(bands));
  *out = JsonValue(std::move(object));
  return true;
}

JsonValue strip_to_value(const Strip& strip) {
  sonare::util::json::Object object;
  util::json::put(object, "id", strip.id);
  util::json::put(object, "inputTrimDb", strip.input_trim_db);
  util::json::put(object, "faderDb", strip.fader_db);
  util::json::put(object, "vcaOffsetDb", strip.vca_offset_db);
  util::json::put(object, "pan", strip.pan);
  util::json::put(object, "width", strip.width);
  util::json::put(object, "muted", strip.muted);
  util::json::put(object, "soloed", strip.soloed);
  util::json::put(object, "soloSafe", strip.solo_safe);
  util::json::put(object, "panMode", strip.pan_mode);
  util::json::put(object, "dualPanLeft", strip.dual_pan_left);
  util::json::put(object, "dualPanRight", strip.dual_pan_right);
  util::json::put(object, "polarityInvertLeft", strip.polarity_invert_left);
  util::json::put(object, "polarityInvertRight", strip.polarity_invert_right);
  util::json::put(object, "panLaw", strip.pan_law);
  util::json::put(object, "channelDelaySamples", strip.channel_delay_samples);
  // Omit when stereo (the default) so existing stereo scenes serialize
  // byte-identically; only surround sources carry the field.
  if (strip.source_layout != ChannelLayout::Stereo) {
    util::json::put(object, "sourceLayout", channel_layout_to_string(strip.source_layout));
  }
  // Omit at the centered point-source default so existing scenes are unchanged;
  // only a moved surround pan carries the object.
  const SurroundPan& sp = strip.surround_pan;
  if (sp.azimuth != 0.0f || sp.elevation != 0.0f || sp.divergence != 0.0f || sp.lfe != 0.0f ||
      sp.distance != 1.0f) {
    sonare::util::json::Object pan;
    util::json::put(pan, "azimuth", sp.azimuth);
    util::json::put(pan, "elevation", sp.elevation);
    util::json::put(pan, "divergence", sp.divergence);
    util::json::put(pan, "lfe", sp.lfe);
    util::json::put(pan, "distance", sp.distance);
    util::json::put(object, "surroundPan", std::move(pan));
  }
  // Omit at the full-metering default, for the same byte-identity reason: only
  // a strip that has opted out of some of its metering carries the object.
  const StripMetering& m = strip.metering;
  if (!m.enabled || !m.lufs || !m.true_peak || m.true_peak_oversample != 4) {
    sonare::util::json::Object metering;
    util::json::put(metering, "enabled", m.enabled);
    util::json::put(metering, "lufs", m.lufs);
    util::json::put(metering, "truePeak", m.true_peak);
    util::json::put(metering, "truePeakOversample", m.true_peak_oversample);
    util::json::put(object, "metering", std::move(metering));
  }
  util::json::put(object, "inserts", inserts_to_value(strip.inserts));
  util::json::put(object, "sends", sends_to_value(strip.sends));
  // Omitted at the identity default so an existing scene stays byte-identical.
  JsonValue eq_value;
  if (eq_to_value(strip.eq, &eq_value)) util::json::put(object, "eq", std::move(eq_value));
  return JsonValue(std::move(object));
}

JsonValue bus_to_value(const Bus& bus) {
  sonare::util::json::Object object;
  util::json::put(object, "id", bus.id);
  util::json::put(object, "role", bus.role);
  // Omit when stereo (the default) so existing stereo scenes serialize
  // byte-identically; only surround buses carry the field.
  if (bus.layout != ChannelLayout::Stereo) {
    util::json::put(object, "layout", channel_layout_to_string(bus.layout));
  }
  // Trim / width / polarity are likewise omitted at their defaults so a bus that
  // never engages them stays byte-identical to a pre-existing scene.
  if (bus.input_trim_db != 0.0f) {
    util::json::put(object, "inputTrimDb", bus.input_trim_db);
  }
  if (bus.width != 1.0f) {
    util::json::put(object, "width", bus.width);
  }
  if (bus.polarity_invert_left) {
    util::json::put(object, "polarityInvertLeft", bus.polarity_invert_left);
  }
  if (bus.polarity_invert_right) {
    util::json::put(object, "polarityInvertRight", bus.polarity_invert_right);
  }
  // Pan is likewise omitted at the default, for the same byte-identity reason.
  if (bus.pan != 0.0f) util::json::put(object, "pan", bus.pan);
  if (bus.pan_mode != 0) util::json::put(object, "panMode", bus.pan_mode);
  if (bus.dual_pan_left != -1.0f) util::json::put(object, "dualPanLeft", bus.dual_pan_left);
  if (bus.dual_pan_right != 1.0f) util::json::put(object, "dualPanRight", bus.dual_pan_right);
  if (bus.pan_law != 0) util::json::put(object, "panLaw", bus.pan_law);
  util::json::put(object, "inserts", inserts_to_value(bus.inserts));
  JsonValue eq_value;
  if (eq_to_value(bus.eq, &eq_value)) util::json::put(object, "eq", std::move(eq_value));
  return JsonValue(std::move(object));
}

JsonValue vca_group_to_value(const VcaGroup& group) {
  sonare::util::json::Object object;
  util::json::put(object, "id", group.id);
  util::json::put(object, "gainDb", group.gain_db);
  sonare::util::json::Array members;
  members.reserve(group.members.size());
  for (const auto& member : group.members) members.emplace_back(JsonValue(member));
  util::json::put(object, "members", std::move(members));
  return JsonValue(std::move(object));
}

JsonValue connection_to_value(const Connection& connection) {
  sonare::util::json::Object object;
  util::json::put(object, "source", connection.source);
  util::json::put(object, "destination", connection.destination);
  return JsonValue(std::move(object));
}

}  // namespace

namespace detail {

sonare::util::json::Value scene_to_value(const Scene& scene) {
  sonare::util::json::Object root;
  util::json::put(root, "version", scene.version);

  sonare::util::json::Array strips;
  strips.reserve(scene.strips.size());
  for (const auto& strip : scene.strips) strips.emplace_back(strip_to_value(strip));
  util::json::put(root, "strips", std::move(strips));

  sonare::util::json::Array buses;
  buses.reserve(scene.buses.size());
  for (const auto& bus : scene.buses) buses.emplace_back(bus_to_value(bus));
  util::json::put(root, "buses", std::move(buses));

  sonare::util::json::Array groups;
  groups.reserve(scene.vca_groups.size());
  for (const auto& group : scene.vca_groups) groups.emplace_back(vca_group_to_value(group));
  util::json::put(root, "vcaGroups", std::move(groups));

  sonare::util::json::Array connections;
  connections.reserve(scene.connections.size());
  for (const auto& connection : scene.connections) {
    connections.emplace_back(connection_to_value(connection));
  }
  util::json::put(root, "connections", std::move(connections));

  return sonare::util::json::Value(std::move(root));
}

}  // namespace detail

std::string scene_to_json(const Scene& scene) {
  return sonare::util::json::dump(detail::scene_to_value(scene));
}

const std::vector<std::string>& scene_schema_paths() {
  static const std::vector<std::string> paths = {
      "version",
      "strips",
      "strips[].id",
      "strips[].inputTrimDb",
      "strips[].faderDb",
      "strips[].vcaOffsetDb",
      "strips[].pan",
      "strips[].width",
      "strips[].muted",
      "strips[].soloed",
      "strips[].soloSafe",
      "strips[].panMode",
      "strips[].dualPanLeft",
      "strips[].dualPanRight",
      "strips[].polarityInvertLeft",
      "strips[].polarityInvertRight",
      "strips[].panLaw",
      "strips[].channelDelaySamples",
      "strips[].sourceLayout",
      "strips[].surroundPan",
      "strips[].surroundPan.azimuth",
      "strips[].surroundPan.elevation",
      "strips[].surroundPan.divergence",
      "strips[].surroundPan.lfe",
      "strips[].surroundPan.distance",
      "strips[].metering",
      "strips[].metering.enabled",
      "strips[].metering.lufs",
      "strips[].metering.truePeak",
      "strips[].metering.truePeakOversample",
      "strips[].inserts",
      "strips[].inserts[].slot",
      "strips[].inserts[].processor",
      "strips[].inserts[].params",
      "strips[].inserts[].sidechainKey",
      "strips[].sends",
      "strips[].sends[].id",
      "strips[].sends[].destinationBusId",
      "strips[].sends[].sendDb",
      "strips[].sends[].timing",
      "strips[].eq",
      "strips[].eq.enabled",
      "strips[].eq.bands",
      "strips[].eq.bands[].type",
      "strips[].eq.bands[].frequencyHz",
      "strips[].eq.bands[].gainDb",
      "strips[].eq.bands[].q",
      "strips[].eq.bands[].enabled",
      "strips[].eq.bands[].coeffMode",
      "strips[].eq.bands[].slopeDbOct",
      "strips[].eq.bands[].placement",
      "strips[].eq.bands[].phase",
      "strips[].eq.bands[].soloed",
      "strips[].eq.bands[].bypassed",
      "strips[].eq.bands[].proportionalQ",
      "strips[].eq.bands[].proportionalQStrength",
      "strips[].eq.bands[].dynamic",
      "strips[].eq.bands[].thresholdDb",
      "strips[].eq.bands[].autoThreshold",
      "strips[].eq.bands[].ratio",
      "strips[].eq.bands[].rangeDb",
      "strips[].eq.bands[].attackMs",
      "strips[].eq.bands[].releaseMs",
      "strips[].eq.bands[].detectorDelayMs",
      "strips[].eq.bands[].externalSidechain",
      "strips[].eq.bands[].sidechainFreqHz",
      "strips[].eq.bands[].sidechainQ",
      "buses",
      "buses[].id",
      "buses[].role",
      "buses[].layout",
      "buses[].inputTrimDb",
      "buses[].width",
      "buses[].polarityInvertLeft",
      "buses[].polarityInvertRight",
      "buses[].pan",
      "buses[].panMode",
      "buses[].dualPanLeft",
      "buses[].dualPanRight",
      "buses[].panLaw",
      "buses[].inserts",
      "buses[].inserts[].slot",
      "buses[].inserts[].processor",
      "buses[].inserts[].params",
      "buses[].inserts[].sidechainKey",
      "buses[].eq",
      "buses[].eq.enabled",
      "buses[].eq.bands",
      "buses[].eq.bands[].type",
      "buses[].eq.bands[].frequencyHz",
      "buses[].eq.bands[].gainDb",
      "buses[].eq.bands[].q",
      "buses[].eq.bands[].enabled",
      "buses[].eq.bands[].coeffMode",
      "buses[].eq.bands[].slopeDbOct",
      "buses[].eq.bands[].placement",
      "buses[].eq.bands[].phase",
      "buses[].eq.bands[].soloed",
      "buses[].eq.bands[].bypassed",
      "buses[].eq.bands[].proportionalQ",
      "buses[].eq.bands[].proportionalQStrength",
      "buses[].eq.bands[].dynamic",
      "buses[].eq.bands[].thresholdDb",
      "buses[].eq.bands[].autoThreshold",
      "buses[].eq.bands[].ratio",
      "buses[].eq.bands[].rangeDb",
      "buses[].eq.bands[].attackMs",
      "buses[].eq.bands[].releaseMs",
      "buses[].eq.bands[].detectorDelayMs",
      "buses[].eq.bands[].externalSidechain",
      "buses[].eq.bands[].sidechainFreqHz",
      "buses[].eq.bands[].sidechainQ",
      "vcaGroups",
      "vcaGroups[].id",
      "vcaGroups[].gainDb",
      "vcaGroups[].members",
      "connections",
      "connections[].source",
      "connections[].destination",
  };
  return paths;
}

Scene scene_from_value(const JsonValue& root, std::vector<std::string>* warnings) {
  if (!root.is_object()) {
    throw SonareException(ErrorCode::InvalidFormat, "scene JSON must be an object");
  }
  static const KeySet kKnownRoot = known_keys("", {"vca_groups"});
  report_unknown_keys(root, "", kKnownRoot, warnings);
  Scene scene;
  scene.version = int_or(root, std::string(), "version", 1);
  if (scene.version != 1) {
    throw SonareException(ErrorCode::InvalidFormat, "unsupported scene JSON version");
  }
  if (const auto* strips = root.find("strips"))
    scene.strips = strips_from_value(*strips, "strips", warnings);
  if (const auto* buses = root.find("buses"))
    scene.buses = buses_from_value(*buses, "buses", warnings);
  if (const auto* groups = value_or_legacy(root, "vcaGroups", "vca_groups"))
    scene.vca_groups = vca_groups_from_value(*groups, "vcaGroups", warnings);
  if (const auto* connections = root.find("connections"))
    scene.connections = connections_from_value(*connections, "connections", warnings);
  return scene;
}

// Budgeted like every caller-supplied document; duplicate keys stay tolerated.
std::string unknown_scene_key_path(const std::string& warning) {
  const size_t prefix = sizeof(kUnknownKeyPrefix) - 1;
  if (warning.size() <= prefix || warning.compare(0, prefix, kUnknownKeyPrefix) != 0) {
    return warning;
  }
  return warning.substr(prefix, warning.size() - prefix - 1);
}

void refuse_unknown_strip_keys(const std::vector<std::string>& warnings) {
  if (warnings.empty()) return;
  std::string message = "unknown strip key '" + unknown_scene_key_path(warnings.front()) + "'";
  if (warnings.size() > 1) message += " (and " + std::to_string(warnings.size() - 1) + " more)";
  throw SonareException(ErrorCode::InvalidFormat, message);
}

Scene scene_from_json(const std::string& json, std::vector<std::string>* warnings) {
  return scene_from_value(sonare::util::json::admit(json), warnings);
}

}  // namespace sonare::mixing::api
