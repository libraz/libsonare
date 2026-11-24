#pragma once

/// @file midi_cc_kind.h
/// @brief The MIDI CC binding kind as a token, read and written through the core's
///        own spelling so the Project and realtime readers agree.

#include <emscripten/val.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "midi/cc_map.h"
#include "wasm/bindings/common/common.h"

/// @brief Read `kind`: absent or null is controlChange7, a non-string is a TypeError and
///        a name the core does not write a RangeError, both naming the valid set.
/// @param at The binding's place in a list, as @ref elementField takes it.
inline sonare::midi::CcBindingKind ccBindingKindProperty(const emscripten::val& object,
                                                         const std::string& at = std::string()) {
  const emscripten::val value = object["kind"];
  if (value.isUndefined() || value.isNull()) return sonare::midi::CcBindingKind::kControlChange7;
  const std::string name = elementField(at, "kind");
  std::string valid;
  for (const sonare::midi::CcBindingKind kind : sonare::midi::kCcBindingKinds) {
    valid += (valid.empty() ? "" : ", ") + std::string(sonare::midi::to_string(kind));
  }
  if (!value.isString()) {
    throw WasmTypeError(name + " must be one of " + valid);
  }
  const std::string token = stringFromVal(value, name.c_str());
  sonare::midi::CcBindingKind kind{};
  if (!sonare::midi::cc_binding_kind_from_string(token, &kind)) {
    throw WasmRangeError(name + " must be one of " + valid);
  }
  return kind;
}

/// @brief Type-only preflight of one CC binding descriptor. Call this for every
///        descriptor before any range or kind-token check so a later wrong type
///        cannot be hidden by an earlier out-of-domain value.
inline void requireCcBindingShape(const emscripten::val& object, const std::string& at) {
  if (object.isNull() || object.isUndefined() || object.typeOf().as<std::string>() != "object" ||
      emscripten::val::global("Array").call<bool>("isArray", object)) {
    throw WasmTypeError((at.empty() ? "binding" : at) + std::string(" must be an object"));
  }
  static constexpr std::array<std::string_view, 10> allowed = {
      "ccNumber",    "channel", "kind",     "ccLsbNumber", "selectorMsb",
      "selectorLsb", "paramId", "minValue", "maxValue",    "consume"};
  emscripten::val keys = emscripten::val::global("Object").call<emscripten::val>("keys", object);
  const unsigned length = keys["length"].as<unsigned>();
  for (unsigned i = 0; i < length; ++i) {
    const std::string key = keys[i].as<std::string>();
    const bool known = std::find(allowed.begin(), allowed.end(), key) != allowed.end();
    if (!known) {
      std::vector<std::string> accepted;
      accepted.reserve(allowed.size());
      for (const std::string_view name : allowed) accepted.emplace_back(name);
      const std::string path = at.empty() ? key : at + "." + key;
      throw WasmTypeError("unknown key " + path + " (expected " +
                          sonare::util::json::schema::expected_keys(accepted) + ")");
    }
  }
  const auto number = [&](const char* key, bool optional = false) {
    const emscripten::val value = object[key];
    if (optional && (value.isUndefined() || value.isNull())) return;
    if (!value.isNumber()) throw WasmTypeError(elementField(at, key) + " must be a number");
  };
  number("ccNumber");
  number("channel", true);
  const emscripten::val kind = object["kind"];
  if (!kind.isUndefined() && !kind.isNull() && !kind.isString()) {
    throw WasmTypeError(elementField(at, "kind") + " must be a string");
  }
  number("ccLsbNumber", true);
  number("selectorMsb", true);
  number("selectorLsb", true);
  number("paramId");
  number("minValue", true);
  number("maxValue", true);
  const emscripten::val consume = object["consume"];
  if (!consume.isUndefined() && !consume.isNull() &&
      consume.typeOf().as<std::string>() != "boolean") {
    throw WasmTypeError(elementField(at, "consume") + " must be a boolean");
  }
}

/// @brief The token @ref ccBindingKindProperty reads for a C ABI kind ordinal.
inline const char* ccBindingKindToken(uint8_t kind) {
  if (kind >= sonare::midi::kCcBindingKinds.size()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "unknown MIDI CC binding kind ordinal: " + std::to_string(kind));
  }
  return sonare::midi::to_string(sonare::midi::kCcBindingKinds[kind]);
}
