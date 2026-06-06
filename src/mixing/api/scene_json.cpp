#include <string>
#include <utility>

#include "mixing/api/scene.h"
#include "util/exception.h"
#include "util/json.h"

namespace sonare::mixing::api {
namespace {

using JsonValue = sonare::util::json::Value;

const char* to_string(InsertSlot slot) { return slot == InsertSlot::PreFader ? "pre" : "post"; }

const char* to_string(SendTiming timing) { return timing == SendTiming::PreFader ? "pre" : "post"; }

InsertSlot insert_slot_from_string(const std::string& value) {
  if (value == "pre") return InsertSlot::PreFader;
  if (value == "post") return InsertSlot::PostFader;
  throw SonareException(ErrorCode::InvalidParameter, "unknown insert slot: " + value);
}

SendTiming send_timing_from_string(const std::string& value) {
  if (value == "pre") return SendTiming::PreFader;
  if (value == "post") return SendTiming::PostFader;
  throw SonareException(ErrorCode::InvalidParameter, "unknown send timing: " + value);
}

// ---------------------------------------------------------------------------
// Tree walkers. All parsing is delegated to util::json::parse (one shared
// grammar, one locale-safe number parser). The walkers below populate Scene
// types from the resulting Value tree; unknown fields are silently ignored,
// matching the previous streaming parser's permissive behavior.
// ---------------------------------------------------------------------------

float number_or(const JsonValue& object, const char* key, float fallback) {
  const auto* value = object.find(key);
  if (!value || !value->is_number()) return fallback;
  return value->as_float();
}

int int_or(const JsonValue& object, const char* key, int fallback) {
  const auto* value = object.find(key);
  if (!value || !value->is_number()) return fallback;
  return value->as_int();
}

bool bool_or(const JsonValue& object, const char* key, bool fallback) {
  const auto* value = object.find(key);
  if (!value || !value->is_bool()) return fallback;
  return value->as_bool();
}

std::string string_or(const JsonValue& object, const char* key, const std::string& fallback) {
  const auto* value = object.find(key);
  if (!value || !value->is_string()) return fallback;
  return value->as_string();
}

Insert insert_from_value(const JsonValue& object) {
  Insert insert;
  if (const auto* slot = object.find("slot"); slot && slot->is_string()) {
    insert.slot = insert_slot_from_string(slot->as_string());
  }
  insert.processor_name = string_or(object, "processor", insert.processor_name);
  insert.params_json = string_or(object, "params", insert.params_json);
  insert.sidechain_key = string_or(object, "sidechainKey", insert.sidechain_key);
  return insert;
}

std::vector<Insert> inserts_from_value(const JsonValue& array) {
  std::vector<Insert> out;
  if (!array.is_array()) return out;
  out.reserve(array.as_array().size());
  for (const auto& entry : array.as_array()) {
    out.push_back(insert_from_value(entry));
  }
  return out;
}

Send send_from_value(const JsonValue& object) {
  Send send;
  send.id = string_or(object, "id", send.id);
  send.destination_bus_id = string_or(object, "destinationBusId", send.destination_bus_id);
  send.send_db = number_or(object, "sendDb", send.send_db);
  if (const auto* timing = object.find("timing"); timing && timing->is_string()) {
    send.timing = send_timing_from_string(timing->as_string());
  }
  return send;
}

std::vector<Send> sends_from_value(const JsonValue& array) {
  std::vector<Send> out;
  if (!array.is_array()) return out;
  out.reserve(array.as_array().size());
  for (const auto& entry : array.as_array()) {
    out.push_back(send_from_value(entry));
  }
  return out;
}

Strip strip_from_value(const JsonValue& object) {
  Strip strip;
  strip.id = string_or(object, "id", strip.id);
  strip.input_trim_db = number_or(object, "inputTrimDb", strip.input_trim_db);
  strip.fader_db = number_or(object, "faderDb", strip.fader_db);
  strip.vca_offset_db = number_or(object, "vcaOffsetDb", strip.vca_offset_db);
  strip.pan = number_or(object, "pan", strip.pan);
  strip.width = number_or(object, "width", strip.width);
  strip.muted = bool_or(object, "muted", strip.muted);
  strip.soloed = bool_or(object, "soloed", strip.soloed);
  strip.solo_safe = bool_or(object, "soloSafe", strip.solo_safe);
  strip.pan_mode = int_or(object, "panMode", strip.pan_mode);
  strip.dual_pan_left = number_or(object, "dualPanLeft", strip.dual_pan_left);
  strip.dual_pan_right = number_or(object, "dualPanRight", strip.dual_pan_right);
  strip.polarity_invert_left = bool_or(object, "polarityInvertLeft", strip.polarity_invert_left);
  strip.polarity_invert_right = bool_or(object, "polarityInvertRight", strip.polarity_invert_right);
  strip.pan_law = int_or(object, "panLaw", strip.pan_law);
  strip.channel_delay_samples = int_or(object, "channelDelaySamples", strip.channel_delay_samples);
  if (const auto* inserts = object.find("inserts")) strip.inserts = inserts_from_value(*inserts);
  if (const auto* sends = object.find("sends")) strip.sends = sends_from_value(*sends);
  return strip;
}

std::vector<Strip> strips_from_value(const JsonValue& array) {
  std::vector<Strip> out;
  if (!array.is_array()) return out;
  out.reserve(array.as_array().size());
  for (const auto& entry : array.as_array()) {
    out.push_back(strip_from_value(entry));
  }
  return out;
}

Bus bus_from_value(const JsonValue& object) {
  Bus bus;
  bus.id = string_or(object, "id", bus.id);
  bus.role = string_or(object, "role", bus.role);
  if (const auto* inserts = object.find("inserts")) bus.inserts = inserts_from_value(*inserts);
  return bus;
}

std::vector<Bus> buses_from_value(const JsonValue& array) {
  std::vector<Bus> out;
  if (!array.is_array()) return out;
  out.reserve(array.as_array().size());
  for (const auto& entry : array.as_array()) {
    out.push_back(bus_from_value(entry));
  }
  return out;
}

VcaGroup vca_group_from_value(const JsonValue& object) {
  VcaGroup group;
  group.id = string_or(object, "id", group.id);
  group.gain_db = number_or(object, "gainDb", group.gain_db);
  if (const auto* members = object.find("members"); members && members->is_array()) {
    group.members.reserve(members->as_array().size());
    for (const auto& entry : members->as_array()) {
      if (entry.is_string()) group.members.push_back(entry.as_string());
    }
  }
  return group;
}

std::vector<VcaGroup> vca_groups_from_value(const JsonValue& array) {
  std::vector<VcaGroup> out;
  if (!array.is_array()) return out;
  out.reserve(array.as_array().size());
  for (const auto& entry : array.as_array()) {
    out.push_back(vca_group_from_value(entry));
  }
  return out;
}

Connection connection_from_value(const JsonValue& object) {
  Connection connection;
  connection.source = string_or(object, "source", connection.source);
  connection.destination = string_or(object, "destination", connection.destination);
  return connection;
}

std::vector<Connection> connections_from_value(const JsonValue& array) {
  std::vector<Connection> out;
  if (!array.is_array()) return out;
  out.reserve(array.as_array().size());
  for (const auto& entry : array.as_array()) {
    out.push_back(connection_from_value(entry));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Tree builders. util::json::dump emits numbers with max_digits10 precision
// in the classic locale, which matches the format the walkers above expect
// and survives a dump -> parse round-trip without coefficient drift.
// ---------------------------------------------------------------------------

JsonValue insert_to_value(const Insert& insert) {
  sonare::util::json::Object object;
  object.emplace("slot", JsonValue(to_string(insert.slot)));
  object.emplace("processor", JsonValue(insert.processor_name));
  object.emplace("params", JsonValue(insert.params_json));
  // Omit `sidechainKey` when empty: the walker treats a missing field and an
  // empty string identically, but the legacy serializer also dropped the field
  // so existing snapshots can still byte-compare against new output.
  if (!insert.sidechain_key.empty()) {
    object.emplace("sidechainKey", JsonValue(insert.sidechain_key));
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
  object.emplace("id", JsonValue(send.id));
  object.emplace("destinationBusId", JsonValue(send.destination_bus_id));
  object.emplace("sendDb", JsonValue(send.send_db));
  object.emplace("timing", JsonValue(to_string(send.timing)));
  return JsonValue(std::move(object));
}

JsonValue sends_to_value(const std::vector<Send>& sends) {
  sonare::util::json::Array array;
  array.reserve(sends.size());
  for (const auto& send : sends) array.emplace_back(send_to_value(send));
  return JsonValue(std::move(array));
}

JsonValue strip_to_value(const Strip& strip) {
  sonare::util::json::Object object;
  object.emplace("id", JsonValue(strip.id));
  object.emplace("inputTrimDb", JsonValue(strip.input_trim_db));
  object.emplace("faderDb", JsonValue(strip.fader_db));
  object.emplace("vcaOffsetDb", JsonValue(strip.vca_offset_db));
  object.emplace("pan", JsonValue(strip.pan));
  object.emplace("width", JsonValue(strip.width));
  object.emplace("muted", JsonValue(strip.muted));
  object.emplace("soloed", JsonValue(strip.soloed));
  object.emplace("soloSafe", JsonValue(strip.solo_safe));
  object.emplace("panMode", JsonValue(strip.pan_mode));
  object.emplace("dualPanLeft", JsonValue(strip.dual_pan_left));
  object.emplace("dualPanRight", JsonValue(strip.dual_pan_right));
  object.emplace("polarityInvertLeft", JsonValue(strip.polarity_invert_left));
  object.emplace("polarityInvertRight", JsonValue(strip.polarity_invert_right));
  object.emplace("panLaw", JsonValue(strip.pan_law));
  object.emplace("channelDelaySamples", JsonValue(strip.channel_delay_samples));
  object.emplace("inserts", inserts_to_value(strip.inserts));
  object.emplace("sends", sends_to_value(strip.sends));
  return JsonValue(std::move(object));
}

JsonValue bus_to_value(const Bus& bus) {
  sonare::util::json::Object object;
  object.emplace("id", JsonValue(bus.id));
  object.emplace("role", JsonValue(bus.role));
  object.emplace("inserts", inserts_to_value(bus.inserts));
  return JsonValue(std::move(object));
}

JsonValue vca_group_to_value(const VcaGroup& group) {
  sonare::util::json::Object object;
  object.emplace("id", JsonValue(group.id));
  object.emplace("gainDb", JsonValue(group.gain_db));
  sonare::util::json::Array members;
  members.reserve(group.members.size());
  for (const auto& member : group.members) members.emplace_back(JsonValue(member));
  object.emplace("members", JsonValue(std::move(members)));
  return JsonValue(std::move(object));
}

JsonValue connection_to_value(const Connection& connection) {
  sonare::util::json::Object object;
  object.emplace("source", JsonValue(connection.source));
  object.emplace("destination", JsonValue(connection.destination));
  return JsonValue(std::move(object));
}

}  // namespace

std::string scene_to_json(const Scene& scene) {
  sonare::util::json::Object root;
  root.emplace("version", JsonValue(scene.version));

  sonare::util::json::Array strips;
  strips.reserve(scene.strips.size());
  for (const auto& strip : scene.strips) strips.emplace_back(strip_to_value(strip));
  root.emplace("strips", JsonValue(std::move(strips)));

  sonare::util::json::Array buses;
  buses.reserve(scene.buses.size());
  for (const auto& bus : scene.buses) buses.emplace_back(bus_to_value(bus));
  root.emplace("buses", JsonValue(std::move(buses)));

  sonare::util::json::Array groups;
  groups.reserve(scene.vca_groups.size());
  for (const auto& group : scene.vca_groups) groups.emplace_back(vca_group_to_value(group));
  root.emplace("vcaGroups", JsonValue(std::move(groups)));

  sonare::util::json::Array connections;
  connections.reserve(scene.connections.size());
  for (const auto& connection : scene.connections) {
    connections.emplace_back(connection_to_value(connection));
  }
  root.emplace("connections", JsonValue(std::move(connections)));

  return sonare::util::json::dump(JsonValue(std::move(root)));
}

Scene scene_from_json(const std::string& json) {
  const auto root = sonare::util::json::parse(json);
  if (!root.is_object()) {
    throw SonareException(ErrorCode::InvalidParameter, "scene JSON must be an object");
  }
  Scene scene;
  scene.version = int_or(root, "version", 1);
  if (scene.version != 1) {
    throw SonareException(ErrorCode::InvalidParameter, "unsupported scene JSON version");
  }
  if (const auto* strips = root.find("strips")) scene.strips = strips_from_value(*strips);
  if (const auto* buses = root.find("buses")) scene.buses = buses_from_value(*buses);
  if (const auto* groups = root.find("vcaGroups"))
    scene.vca_groups = vca_groups_from_value(*groups);
  if (const auto* connections = root.find("connections"))
    scene.connections = connections_from_value(*connections);
  return scene;
}

}  // namespace sonare::mixing::api
