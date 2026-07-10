#ifndef SONARE_NODE_SONARE_WRAP_OPTIONS_H_
#define SONARE_NODE_SONARE_WRAP_OPTIONS_H_

#include <napi.h>

#include <cstdint>

namespace sonare_node {

// Two helper families with deliberately different lenience, shared by every
// addon TU (do not re-declare per-file copies):
//   * node_*_option  — type-checked: a present-but-wrong-typed value falls
//     back to the default (used by analysis/effects options bags).
//   * *Property      — presence + type checked: undefined/null falls back to the
//     default, but any other value is read with a typed N-API accessor, so a
//     type mismatch raises a pending JS exception (it does NOT silently fall
//     back like node_*_option). Used by engine/project structs whose values are
//     further validated downstream by the C ABI.

/// @brief Read an integer option from a JS object, falling back if missing.
inline int node_int_option(const Napi::Object& object, const char* key, int fallback) {
  Napi::Value value = object.Get(key);
  return value.IsNumber() ? value.As<Napi::Number>().Int32Value() : fallback;
}

/// @brief Read a float option from a JS object, falling back if missing.
inline float node_float_option(const Napi::Object& object, const char* key, float fallback) {
  Napi::Value value = object.Get(key);
  return value.IsNumber() ? value.As<Napi::Number>().FloatValue() : fallback;
}

/// @brief Read a double option from a JS object, falling back if missing.
inline double node_double_option(const Napi::Object& object, const char* key, double fallback) {
  Napi::Value value = object.Get(key);
  return value.IsNumber() ? value.As<Napi::Number>().DoubleValue() : fallback;
}

/// @brief Read a boolean option from a JS object, falling back if missing.
inline bool node_bool_option(const Napi::Object& object, const char* key, bool fallback) {
  Napi::Value value = object.Get(key);
  return value.IsBoolean() ? value.As<Napi::Boolean>().Value() : fallback;
}

/// @brief Read an int property: undefined/null returns the fallback, otherwise a
///        typed read (a non-number raises a pending JS exception; see note above).
inline int IntProperty(const Napi::Object& obj, const char* key, int fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() || value.IsNull() ? fallback : value.As<Napi::Number>().Int32Value();
}

/// @brief Read an int64 property: undefined/null returns the fallback, otherwise a
///        typed read (a non-number raises a pending JS exception).
inline int64_t Int64Property(const Napi::Object& obj, const char* key, int64_t fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() || value.IsNull()
             ? fallback
             : static_cast<int64_t>(value.As<Napi::Number>().Int64Value());
}

/// @brief Read a float property: undefined/null returns the fallback, otherwise a
///        typed read (a non-number raises a pending JS exception).
inline float FloatProperty(const Napi::Object& obj, const char* key, float fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() || value.IsNull() ? fallback : value.As<Napi::Number>().FloatValue();
}

/// @brief Read a double property: undefined/null returns the fallback, otherwise a
///        typed read (a non-number raises a pending JS exception).
inline double DoubleProperty(const Napi::Object& obj, const char* key, double fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() || value.IsNull() ? fallback : value.As<Napi::Number>().DoubleValue();
}

/// @brief Read a bool property: undefined/null returns the fallback, otherwise a
///        typed read (a non-boolean raises a pending JS exception).
inline bool BoolProperty(const Napi::Object& obj, const char* key, bool fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() || value.IsNull() ? fallback : value.As<Napi::Boolean>().Value();
}

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_OPTIONS_H_
