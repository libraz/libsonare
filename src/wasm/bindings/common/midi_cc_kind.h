#pragma once

/// @file midi_cc_kind.h
/// @brief The MIDI CC binding kind as a token, read and written through the core's
///        own spelling so the Project and realtime readers agree.

#include <emscripten/val.h>

#include <string>

#include "midi/cc_map.h"
#include "wasm/bindings/common/common.h"

/// @brief Read `kind`: absent or null is controlChange7, a non-string is a TypeError and
///        a name the core does not write a RangeError, both naming the valid set.
/// @param at The binding's place in a list, as @ref elementField takes it.
inline sonare::midi::CcBindingKind ccBindingKindProperty(const emscripten::val& object,
                                                         const std::string& at = {}) {
  const emscripten::val value = object["kind"];
  if (value.isUndefined() || value.isNull()) return sonare::midi::CcBindingKind::kControlChange7;
  const std::string name = elementField(at, "kind");
  std::string valid;
  for (const sonare::midi::CcBindingKind kind : sonare::midi::kCcBindingKinds) {
    valid += (valid.empty() ? "'" : ", '") + std::string(sonare::midi::to_string(kind)) + "'";
  }
  if (!value.isString()) {
    throw WasmTypeError(name + " must be one of " + valid + " (a name, not a number)");
  }
  const std::string token = stringFromVal(value, name.c_str());
  sonare::midi::CcBindingKind kind{};
  if (!sonare::midi::cc_binding_kind_from_string(token, &kind)) {
    throw WasmRangeError(name + " must be one of " + valid + ", got '" + token + "'");
  }
  return kind;
}

/// @brief The token @ref ccBindingKindProperty reads for a C ABI kind ordinal.
inline const char* ccBindingKindToken(uint8_t kind) {
  return sonare::midi::to_string(static_cast<sonare::midi::CcBindingKind>(kind));
}
