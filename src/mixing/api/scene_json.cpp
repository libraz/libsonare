#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

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
#include "util/json_schema.h"
#include "util/numeric_validation.h"

namespace sonare::mixing::api {
namespace {

using JsonValue = sonare::util::json::Value;
namespace schema = sonare::util::json::schema;

const char* to_string(InsertSlot slot) { return slot == InsertSlot::PreFader ? "pre" : "post"; }

const char* to_string(SendTiming timing) {
  return timing == SendTiming::PreFader ? "preFader" : "postFader";
}

const char* to_string(PanLaw law) {
  switch (law) {
    case PanLaw::Const3dB:
      return "const3Db";
    case PanLaw::Const4p5dB:
      return "const4.5Db";
    case PanLaw::Const6dB:
      return "const6Db";
    case PanLaw::Linear0dB:
      return "linear0Db";
  }
  return "const3Db";
}

const char* to_string(PanMode mode) {
  switch (mode) {
    case PanMode::Balance:
      return "balance";
    case PanMode::StereoPan:
      return "stereoPan";
    case PanMode::DualPan:
      return "dualPan";
  }
  return "balance";
}

std::optional<InsertSlot> insert_slot_from_string(const std::string& value) {
  if (value == "pre") return InsertSlot::PreFader;
  if (value == "post") return InsertSlot::PostFader;
  return std::nullopt;
}

std::optional<SendTiming> send_timing_from_string(const std::string& value) {
  if (value == "preFader") return SendTiming::PreFader;
  if (value == "postFader") return SendTiming::PostFader;
  return std::nullopt;
}

std::optional<PanLaw> pan_law_from_string(const std::string& value) {
  if (value == "const3Db") return PanLaw::Const3dB;
  if (value == "const4.5Db") return PanLaw::Const4p5dB;
  if (value == "const6Db") return PanLaw::Const6dB;
  if (value == "linear0Db") return PanLaw::Linear0dB;
  return std::nullopt;
}

std::optional<PanMode> pan_mode_from_string(const std::string& value) {
  if (value == "balance") return PanMode::Balance;
  if (value == "stereoPan") return PanMode::StereoPan;
  if (value == "dualPan") return PanMode::DualPan;
  return std::nullopt;
}

std::optional<ChannelLayout> channel_layout_token(const std::string& value) {
  ChannelLayout layout = ChannelLayout::Stereo;
  if (!channel_layout_from_string(value, layout)) return std::nullopt;
  return layout;
}

// Every refusal names the document path, already in the caller's spelling, as its subject; a
// facade that wrapped its own object into the document re-roots it. The code stays InvalidFormat.
[[noreturn]] void refuse_at(const std::string& at, const std::string& predicate) {
  throw SonareParameterRefusal::predicate(ErrorCode::InvalidFormat, at, predicate)
      .as_caller_spelled();
}

[[noreturn]] void refuse_kind(const std::string& at, schema::Kind kind) {
  if (at.empty()) throw SonareException(ErrorCode::InvalidFormat, schema::must_be(at, kind));
  throw SonareParameterRefusal::wrong_type(ErrorCode::InvalidFormat, at,
                                           std::string("must be ") + schema::kind_text(kind))
      .as_caller_spelled();
}

[[noreturn]] void refuse_unknown(const std::string& at, const std::string& expected) {
  throw SonareParameterRefusal::unknown(ErrorCode::InvalidFormat, at, std::string(), expected)
      .as_caller_spelled();
}

// A closed set is read only as its token; a number is refused naming the tokens.
template <typename Enum>
Enum token_value(const JsonValue& value, const std::string& at, const char* tokens,
                 std::optional<Enum> (*from_string)(const std::string&)) {
  if (!value.is_string()) {
    if (value.is_number()) refuse_at(at, std::string("must be one of ") + tokens);
    refuse_kind(at, schema::Kind::String);
  }
  const auto parsed = from_string(value.as_string());
  if (!parsed) refuse_at(at, std::string("must be one of ") + tokens);
  return *parsed;
}

// Reads an optional channel-layout string field. Absent -> fallback (so old
// scenes without the field round-trip as stereo).
ChannelLayout channel_layout_or(const JsonValue& object, const std::string& path, const char* key,
                                ChannelLayout fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  return token_value<ChannelLayout>(*value, schema::child_path(path, key), "mono, stereo, 5.1, 7.1",
                                    &channel_layout_token);
}

// ---------------------------------------------------------------------------
// Tree walkers. All parsing is delegated to util::json::parse (one shared
// grammar, one locale-safe number parser). The walkers below populate Scene
// types from the resulting Value tree; unknown fields never fail a load and are
// reported to the caller's warning list when it passes one.
// ---------------------------------------------------------------------------

using KeySet = std::unordered_set<std::string>;

// Direct children of `prefix` in the writer's canonical path list ("" is the root).
KeySet known_keys(const std::string& prefix) {
  KeySet keys;
  const std::string head = prefix.empty() ? std::string() : prefix + ".";
  for (const auto& path : scene_schema_paths()) {
    if (path.compare(0, head.size(), head) != 0) continue;
    const size_t end = path.find_first_of(".[", head.size());
    keys.insert(path.substr(head.size(), end == std::string::npos ? end : end - head.size()));
  }
  return keys;
}

// Refuses a key outside the record's fields, listing them. `$`- and `x-`-prefixed keys are
// annotations, never refused.
void refuse_unknown_keys(const JsonValue& object, const std::string& path, const KeySet& known) {
  for (const auto& entry : object.as_object()) {
    const std::string& key = entry.first;
    if (known.count(key) != 0 || util::json::is_annotation_key(key)) continue;
    refuse_unknown(schema::child_path(path, key),
                   schema::expected_keys(std::vector<std::string>(known.begin(), known.end())));
  }
}

// A present key of the wrong type (a JSON null included) is refused naming its path, as a
// container of the wrong type is; only an absent key keeps the fallback.
[[noreturn]] void refuse_type(const std::string& path, const char* key, schema::Kind kind) {
  refuse_kind(schema::child_path(path, key), kind);
}

float float_from(const JsonValue& value, const std::string& at) {
  float converted = 0.0f;
  if (!numeric::checked_float_cast(value.as_number(), &converted)) {
    refuse_at(at, schema::kFloatRangePredicate);
  }
  return converted;
}

int int_from(const JsonValue& value, const std::string& at) {
  int converted = 0;
  if (!numeric::checked_integral_cast(value.as_number(), &converted)) {
    refuse_at(at, schema::integer_range_predicate(std::numeric_limits<int>::min(),
                                                  std::numeric_limits<int>::max()));
  }
  return converted;
}

float number_or(const JsonValue& object, const std::string& path, const char* key, float fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  if (!value->is_number()) refuse_type(path, key, schema::Kind::Number);
  return float_from(*value, schema::child_path(path, key));
}

// Pan and width are refused outside their documented domains, naming the field.
float checked_pan(float value, const std::string& path, const char* key) {
  if (!pan_in_range(value)) {
    refuse_at(schema::child_path(path, key), schema::range_predicate(-1.0, 1.0));
  }
  return value;
}

float checked_width(float value, const std::string& path, const char* key) {
  if (!width_in_range(value)) {
    refuse_at(schema::child_path(path, key), schema::range_predicate(0.0, 2.0));
  }
  return value;
}

// Surround placement is refused outside its domains with the same rule as the setters.
float checked_surround_azimuth(float value, const std::string& path, const char* key) {
  if (!surround_azimuth_in_range(value)) {
    refuse_at(schema::child_path(path, key), schema::range_predicate(-180.0, 180.0));
  }
  return value;
}

float checked_surround_unit(float value, const std::string& path, const char* key) {
  if (!surround_unit_in_range(value)) {
    refuse_at(schema::child_path(path, key), schema::range_predicate(0.0, 1.0));
  }
  return value;
}

template <typename Enum>
int token_or(const JsonValue& object, const std::string& path, const char* key, int fallback,
             const char* tokens, std::optional<Enum> (*from_string)(const std::string&)) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  return static_cast<int>(
      token_value<Enum>(*value, schema::child_path(path, key), tokens, from_string));
}

int int_or(const JsonValue& object, const std::string& path, const char* key, int fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  if (!value->is_number()) refuse_type(path, key, schema::Kind::Number);
  return int_from(*value, schema::child_path(path, key));
}

bool bool_or(const JsonValue& object, const std::string& path, const char* key, bool fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  if (!value->is_bool()) refuse_type(path, key, schema::Kind::Boolean);
  return value->as_bool();
}

std::string string_or(const JsonValue& object, const std::string& path, const char* key,
                      const std::string& fallback) {
  const auto* value = object.find(key);
  if (!value) return fallback;
  if (!value->is_string()) refuse_type(path, key, schema::Kind::String);
  return value->as_string();
}

Insert insert_from_value(const JsonValue& object, const std::string& path) {
  static const KeySet kKnown = known_keys("strips[].inserts[]");
  Insert insert;
  if (const auto* slot = object.find("slot")) {
    insert.slot = token_value<InsertSlot>(*slot, schema::child_path(path, "slot"), "pre, post",
                                          &insert_slot_from_string);
  }
  insert.processor_name = string_or(object, path, "processor", insert.processor_name);
  if (const auto* params = object.find("params")) {
    if (!params->is_object()) refuse_type(path, "params", schema::Kind::Object);
    insert.params_json = sonare::util::json::dump(*params);
  }
  insert.sidechain_key = string_or(object, path, "sidechainKey", insert.sidechain_key);
  refuse_unknown_keys(object, path, kKnown);
  return insert;
}

// Every entity array walker below refuses a present container that is not an array and a
// non-object element (InvalidFormat, named by its path), as the project document's containers
// do; callers pass only present keys, so an absent one keeps its empty default.
template <typename T>
std::vector<T> array_from_value(const JsonValue& array, const std::string& path,
                                T (*parse)(const JsonValue&, const std::string&)) {
  if (!array.is_array()) refuse_kind(path, schema::Kind::Array);
  std::vector<T> out;
  out.reserve(array.as_array().size());
  size_t index = 0;
  for (const auto& entry : array.as_array()) {
    const std::string element_path = schema::index_path(path, index++);
    if (!entry.is_object()) refuse_kind(element_path, schema::Kind::Object);
    out.push_back(parse(entry, element_path));
  }
  return out;
}

std::vector<Insert> inserts_from_value(const JsonValue& array, const std::string& path) {
  return array_from_value<Insert>(array, path, insert_from_value);
}

Send send_from_value(const JsonValue& object, const std::string& path) {
  static const KeySet kKnown = known_keys("strips[].sends[]");
  Send send;
  send.id = string_or(object, path, "id", send.id);
  send.destination_bus_id = string_or(object, path, "destinationBusId", send.destination_bus_id);
  send.send_db = number_or(object, path, "sendDb", send.send_db);
  if (const auto* timing = object.find("timing")) {
    send.timing = token_value<SendTiming>(*timing, schema::child_path(path, "timing"),
                                          "preFader, postFader", &send_timing_from_string);
  }
  refuse_unknown_keys(object, path, kKnown);
  return send;
}

std::vector<Send> sends_from_value(const JsonValue& array, const std::string& path) {
  return array_from_value<Send>(array, path, send_from_value);
}

// Strip and Bus share one EQ shape (StripEq) and one validation.
StripEq eq_from_value(const JsonValue& object, const std::string& path) {
  static const KeySet kKnownEq = known_keys("strips[].eq");
  static const KeySet kKnownBand(mastering::eq::eq_band_known_keys().begin(),
                                 mastering::eq::eq_band_known_keys().end());
  StripEq eq;
  const auto* eq_value = object.find("eq");
  if (!eq_value) return eq;
  const std::string eq_path = schema::child_path(path, "eq");
  if (!eq_value->is_object()) refuse_kind(eq_path, schema::Kind::Object);
  refuse_unknown_keys(*eq_value, eq_path, kKnownEq);
  eq.enabled = bool_or(*eq_value, eq_path, "enabled", eq.enabled);
  const auto* bands = eq_value->find("bands");
  if (!bands) return eq;
  const std::string bands_path = schema::child_path(eq_path, "bands");
  if (!bands->is_array()) refuse_kind(bands_path, schema::Kind::Array);
  const auto& array = bands->as_array();
  if (array.size() > mastering::eq::ParametricEq::kMaxBands) {
    refuse_at(bands_path, "must have at most 24 bands");
  }
  eq.bands.reserve(array.size());
  for (size_t index = 0; index < array.size(); ++index) {
    const auto& entry = array[index];
    const std::string band_path = schema::index_path(bands_path, index);
    const mastering::eq::EqBand band = mastering::eq::eq_band_from_value(entry, band_path);
    refuse_unknown_keys(entry, band_path, kKnownBand);
    // ParametricEq has no tilt design; the shared codec stays permissive for EqualizerProcessor.
    if (band.type == mastering::eq::EqBandType::TiltShelf ||
        band.type == mastering::eq::EqBandType::FlatTilt) {
      refuse_at(schema::child_path(band_path, "type"),
                "must not be tiltShelf or flatTilt on a strip or bus EQ");
    }
    eq.bands.push_back(band);
  }
  return eq;
}

Strip strip_from_value(const JsonValue& object, const std::string& path) {
  static const KeySet kKnown = known_keys("strips[]");
  static const KeySet kKnownSurround = known_keys("strips[].surroundPan");
  static const KeySet kKnownMetering = known_keys("strips[].metering");
  Strip strip;
  strip.id = string_or(object, path, "id", strip.id);
  strip.input_trim_db = number_or(object, path, "inputTrimDb", strip.input_trim_db);
  strip.fader_db = number_or(object, path, "faderDb", strip.fader_db);
  strip.vca_offset_db = number_or(object, path, "vcaOffsetDb", strip.vca_offset_db);
  // Pan and width outside their domains are refused, like the enums and counts below.
  strip.pan = checked_pan(number_or(object, path, "pan", strip.pan), path, "pan");
  strip.width = checked_width(number_or(object, path, "width", strip.width), path, "width");
  strip.muted = bool_or(object, path, "muted", strip.muted);
  strip.soloed = bool_or(object, path, "soloed", strip.soloed);
  strip.solo_safe = bool_or(object, path, "soloSafe", strip.solo_safe);
  // Out-of-range values are refused in the one canonical scene walker, so standalone mixer JSON
  // and project-embedded scene JSON stay identical.
  strip.pan_mode = token_or<PanMode>(object, path, "panMode", strip.pan_mode,
                                     "balance, stereoPan, dualPan", &pan_mode_from_string);
  strip.dual_pan_left =
      checked_pan(number_or(object, path, "dualPanLeft", strip.dual_pan_left), path, "dualPanLeft");
  strip.dual_pan_right = checked_pan(number_or(object, path, "dualPanRight", strip.dual_pan_right),
                                     path, "dualPanRight");
  strip.polarity_invert_left =
      bool_or(object, path, "polarityInvertLeft", strip.polarity_invert_left);
  strip.polarity_invert_right =
      bool_or(object, path, "polarityInvertRight", strip.polarity_invert_right);
  strip.pan_law =
      token_or<PanLaw>(object, path, "panLaw", strip.pan_law,
                       "const3Db, const4.5Db, const6Db, linear0Db", &pan_law_from_string);
  strip.channel_delay_samples =
      int_or(object, path, "channelDelaySamples", strip.channel_delay_samples);
  if (strip.channel_delay_samples < 0 || strip.channel_delay_samples > kMaxAlignmentDelaySamples) {
    refuse_at(schema::child_path(path, "channelDelaySamples"),
              schema::integer_range_predicate(0, kMaxAlignmentDelaySamples));
  }
  strip.source_layout = channel_layout_or(object, path, "sourceLayout", strip.source_layout);
  if (const auto* sp = object.find("surroundPan")) {
    const std::string surround_path = schema::child_path(path, "surroundPan");
    if (!sp->is_object()) refuse_kind(surround_path, schema::Kind::Object);
    refuse_unknown_keys(*sp, surround_path, kKnownSurround);
    SurroundPanParams parsed;
    parsed.azimuth = checked_surround_azimuth(
        number_or(*sp, surround_path, "azimuth", strip.surround_pan.azimuth), surround_path,
        "azimuth");
    parsed.divergence = checked_surround_unit(
        number_or(*sp, surround_path, "divergence", strip.surround_pan.divergence), surround_path,
        "divergence");
    parsed.lfe = checked_surround_unit(number_or(*sp, surround_path, "lfe", strip.surround_pan.lfe),
                                       surround_path, "lfe");
    // Domains are refused above; the panner's own resolution settles the rest.
    const SurroundPanParams stored = clamp_surround_pan_params(parsed);
    strip.surround_pan.azimuth = stored.azimuth;
    strip.surround_pan.divergence = stored.divergence;
    strip.surround_pan.lfe = stored.lfe;
  }
  if (const auto* metering = object.find("metering")) {
    const std::string metering_path = schema::child_path(path, "metering");
    if (!metering->is_object()) refuse_kind(metering_path, schema::Kind::Object);
    refuse_unknown_keys(*metering, metering_path, kKnownMetering);
    strip.metering.enabled = bool_or(*metering, metering_path, "enabled", strip.metering.enabled);
    strip.metering.lufs = bool_or(*metering, metering_path, "lufs", strip.metering.lufs);
    strip.metering.true_peak =
        bool_or(*metering, metering_path, "truePeak", strip.metering.true_peak);
    strip.metering.true_peak_oversample =
        int_or(*metering, metering_path, "truePeakOversample", strip.metering.true_peak_oversample);
    // The realtime meter's own rule, so a scene never stores a factor it cannot run.
    if (!is_realtime_true_peak_oversample(strip.metering.true_peak_oversample)) {
      refuse_at(schema::child_path(metering_path, "truePeakOversample"), "must be 4 or 8");
    }
  }
  if (const auto* inserts = object.find("inserts")) {
    strip.inserts = inserts_from_value(*inserts, schema::child_path(path, "inserts"));
  }
  if (const auto* sends = object.find("sends")) {
    strip.sends = sends_from_value(*sends, schema::child_path(path, "sends"));
  }
  strip.eq = eq_from_value(object, path);
  refuse_unknown_keys(object, path, kKnown);
  return strip;
}

std::vector<Strip> strips_from_value(const JsonValue& array, const std::string& path) {
  return array_from_value<Strip>(array, path, strip_from_value);
}

Bus bus_from_value(const JsonValue& object, const std::string& path) {
  static const KeySet kKnown = known_keys("buses[]");
  Bus bus;
  bus.id = string_or(object, path, "id", bus.id);
  if (const auto* role = object.find("role")) {
    // A number is refused naming the roles, like every other closed set here.
    const std::string at = schema::child_path(path, "role");
    if (role->is_string()) bus.role = role->as_string();
    if (role->is_number() || (role->is_string() && !bus_role::is_declared(bus.role))) {
      refuse_at(at, std::string("must be one of ") + bus_role::kChoices);
    }
    if (!role->is_string()) refuse_kind(at, schema::Kind::String);
  }
  bus.layout = channel_layout_or(object, path, "layout", bus.layout);
  bus.input_trim_db = number_or(object, path, "inputTrimDb", bus.input_trim_db);
  // Same domain as a strip's width above.
  bus.width = checked_width(number_or(object, path, "width", bus.width), path, "width");
  bus.polarity_invert_left = bool_or(object, path, "polarityInvertLeft", bus.polarity_invert_left);
  bus.polarity_invert_right =
      bool_or(object, path, "polarityInvertRight", bus.polarity_invert_right);
  // Pan: same field names and ranges as Strip's above.
  bus.pan = checked_pan(number_or(object, path, "pan", bus.pan), path, "pan");
  bus.pan_mode = token_or<PanMode>(object, path, "panMode", bus.pan_mode,
                                   "balance, stereoPan, dualPan", &pan_mode_from_string);
  bus.dual_pan_left =
      checked_pan(number_or(object, path, "dualPanLeft", bus.dual_pan_left), path, "dualPanLeft");
  bus.dual_pan_right = checked_pan(number_or(object, path, "dualPanRight", bus.dual_pan_right),
                                   path, "dualPanRight");
  bus.pan_law = token_or<PanLaw>(object, path, "panLaw", bus.pan_law,
                                 "const3Db, const4.5Db, const6Db, linear0Db", &pan_law_from_string);
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
      refuse_at(schema::child_path(path, offending),
                "must stay at its default on a surround (>2 channel) layout");
    }
  }
  bus.eq = eq_from_value(object, path);
  if (const auto* inserts = object.find("inserts")) {
    bus.inserts = inserts_from_value(*inserts, schema::child_path(path, "inserts"));
  }
  refuse_unknown_keys(object, path, kKnown);
  return bus;
}

std::vector<Bus> buses_from_value(const JsonValue& array, const std::string& path) {
  return array_from_value<Bus>(array, path, bus_from_value);
}

VcaGroup vca_group_from_value(const JsonValue& object, const std::string& path) {
  static const KeySet kKnown = known_keys("vcaGroups[]");
  VcaGroup group;
  group.id = string_or(object, path, "id", group.id);
  group.gain_db = number_or(object, path, "gainDb", group.gain_db);
  if (const auto* members = object.find("members")) {
    const std::string members_path = schema::child_path(path, "members");
    if (!members->is_array()) refuse_kind(members_path, schema::Kind::Array);
    group.members.reserve(members->as_array().size());
    size_t index = 0;
    for (const auto& entry : members->as_array()) {
      if (!entry.is_string()) {
        refuse_kind(schema::index_path(members_path, index), schema::Kind::String);
      }
      group.members.push_back(entry.as_string());
      ++index;
    }
  }
  refuse_unknown_keys(object, path, kKnown);
  return group;
}

std::vector<VcaGroup> vca_groups_from_value(const JsonValue& array, const std::string& path) {
  return array_from_value<VcaGroup>(array, path, vca_group_from_value);
}

Connection connection_from_value(const JsonValue& object, const std::string& path) {
  static const KeySet kKnown = known_keys("connections[]");
  Connection connection;
  connection.source = string_or(object, path, "source", connection.source);
  connection.destination = string_or(object, path, "destination", connection.destination);
  refuse_unknown_keys(object, path, kKnown);
  return connection;
}

std::vector<Connection> connections_from_value(const JsonValue& array, const std::string& path) {
  return array_from_value<Connection>(array, path, connection_from_value);
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
  util::json::put(object, "processor", insert.processor_name);
  util::json::put(object, "params", insert_params_to_value(insert));
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
  util::json::put(object, "panMode", to_string(static_cast<PanMode>(strip.pan_mode)));
  util::json::put(object, "dualPanLeft", strip.dual_pan_left);
  util::json::put(object, "dualPanRight", strip.dual_pan_right);
  util::json::put(object, "polarityInvertLeft", strip.polarity_invert_left);
  util::json::put(object, "polarityInvertRight", strip.polarity_invert_right);
  util::json::put(object, "panLaw", to_string(static_cast<PanLaw>(strip.pan_law)));
  util::json::put(object, "channelDelaySamples", strip.channel_delay_samples);
  // Omit when stereo (the default) so existing stereo scenes serialize
  // byte-identically; only surround sources carry the field.
  if (strip.source_layout != ChannelLayout::Stereo) {
    util::json::put(object, "sourceLayout", channel_layout_to_string(strip.source_layout));
  }
  // Omit at the centered point-source default so existing scenes are unchanged;
  // only a moved surround pan carries the object.
  const SurroundPan& sp = strip.surround_pan;
  if (sp.azimuth != 0.0f || sp.divergence != 0.0f || sp.lfe != 0.0f) {
    sonare::util::json::Object pan;
    util::json::put(pan, "azimuth", sp.azimuth);
    util::json::put(pan, "divergence", sp.divergence);
    util::json::put(pan, "lfe", sp.lfe);
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
  if (bus.pan_mode != 0) {
    util::json::put(object, "panMode", to_string(static_cast<PanMode>(bus.pan_mode)));
  }
  if (bus.dual_pan_left != -1.0f) util::json::put(object, "dualPanLeft", bus.dual_pan_left);
  if (bus.dual_pan_right != 1.0f) util::json::put(object, "dualPanRight", bus.dual_pan_right);
  if (bus.pan_law != 0) {
    util::json::put(object, "panLaw", to_string(static_cast<PanLaw>(bus.pan_law)));
  }
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
      "strips[].surroundPan.divergence",
      "strips[].surroundPan.lfe",
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

Scene scene_from_value(const JsonValue& root, const std::string& root_path) {
  if (!root.is_object()) refuse_kind(root_path, schema::Kind::Object);
  static const KeySet kKnownRoot = known_keys("");
  Scene scene;
  scene.version = int_or(root, root_path, "version", kSceneFormatVersion);
  if (scene.version != kSceneFormatVersion) {
    throw SonareParameterRefusal::predicate_with_reason(
        ErrorCode::InvalidFormat, schema::child_path(root_path, "version"), "must be 2",
        ErrorReason::UnsupportedSchemaVersion)
        .as_caller_spelled();
  }
  if (const auto* strips = root.find("strips")) {
    scene.strips = strips_from_value(*strips, schema::child_path(root_path, "strips"));
  }
  if (const auto* buses = root.find("buses")) {
    scene.buses = buses_from_value(*buses, schema::child_path(root_path, "buses"));
  }
  if (const auto* groups = root.find("vcaGroups")) {
    scene.vca_groups = vca_groups_from_value(*groups, schema::child_path(root_path, "vcaGroups"));
  }
  if (const auto* connections = root.find("connections")) {
    scene.connections =
        connections_from_value(*connections, schema::child_path(root_path, "connections"));
  }
  refuse_unknown_keys(root, root_path, kKnownRoot);
  return scene;
}

// Budgeted like every caller-supplied document; a repeated key is refused.
Scene scene_from_json(const std::string& json) {
  try {
    return scene_from_value(sonare::util::json::admit_strict(json), std::string());
  } catch (const sonare::util::json::JsonResourceError&) {
    throw;
  } catch (const sonare::util::json::JsonError& error) {
    throw SonareException(ErrorCode::InvalidFormat, error.what(), ErrorReason::MalformedJson);
  }
}

namespace {

// One settings object read by @p read at @p root, every refusal coded as the caller's argument.
template <typename Read>
auto settings_from_json(const std::string& json, const std::string& root,
                        const Read& read) -> decltype(read(JsonValue(), root)) {
  JsonValue value;
  try {
    value = sonare::util::json::admit_strict(json);
  } catch (const sonare::util::json::JsonError& e) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, root,
                                            std::string("must be valid JSON: ") + e.what());
  }
  if (!value.is_object()) {
    throw SonareParameterRefusal::wrong_type(
        ErrorCode::InvalidParameter, root,
        std::string("must be ") + schema::kind_text(schema::Kind::Object));
  }
  try {
    return read(value, root);
  } catch (const SonareParameterRefusal& e) {
    throw e.respelled(
        ErrorCode::InvalidParameter, [](const std::string& subject) { return subject; },
        e.caller_spelled());
  } catch (const SonareException& e) {
    // The band reader the walker shares refuses in text; its paths are already under @p root.
    if (e.code() != ErrorCode::InvalidFormat) throw;
    throw SonareException(ErrorCode::InvalidParameter, e.what());
  }
}

}  // namespace

Strip strip_settings_from_json(const std::string& json, const std::string& root) {
  return settings_from_json(json, root, &strip_from_value);
}

Bus bus_settings_from_json(const std::string& json, const std::string& root) {
  return settings_from_json(json, root, &bus_from_value);
}

}  // namespace sonare::mixing::api
