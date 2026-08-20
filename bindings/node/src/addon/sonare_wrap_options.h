#ifndef SONARE_NODE_SONARE_WRAP_OPTIONS_H_
#define SONARE_NODE_SONARE_WRAP_OPTIONS_H_

#include <napi.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace sonare_node {

// Canonical positional-argument readers, shared by every addon TU. Semantics
// match the recurring inline sites: a missing OR present-but-non-number
// argument at @p index falls back to @p fallback (a type-checked fallback, not
// a presence-only check). Preserve the int/float/double distinction per call
// site (Int32Value vs FloatValue vs DoubleValue).

/// @brief Read an int positional argument, falling back if absent or non-number.
inline int node_arg_int(const Napi::CallbackInfo& info, size_t index, int fallback) {
  return index < info.Length() && info[index].IsNumber()
             ? info[index].As<Napi::Number>().Int32Value()
             : fallback;
}

/// @brief Read a uint32 positional argument, falling back if absent or non-number.
inline uint32_t node_arg_uint32(const Napi::CallbackInfo& info, size_t index, uint32_t fallback) {
  return index < info.Length() && info[index].IsNumber()
             ? info[index].As<Napi::Number>().Uint32Value()
             : fallback;
}

/// @brief Read a float positional argument, falling back if absent or non-number.
inline float node_arg_float(const Napi::CallbackInfo& info, size_t index, float fallback) {
  return index < info.Length() && info[index].IsNumber()
             ? info[index].As<Napi::Number>().FloatValue()
             : fallback;
}

/// @brief Read a double positional argument, falling back if absent or non-number.
inline double node_arg_double(const Napi::CallbackInfo& info, size_t index, double fallback) {
  return index < info.Length() && info[index].IsNumber()
             ? info[index].As<Napi::Number>().DoubleValue()
             : fallback;
}

/// @brief Read a bool positional argument, falling back if absent or non-boolean.
inline bool node_arg_bool(const Napi::CallbackInfo& info, size_t index, bool fallback) {
  return index < info.Length() && info[index].IsBoolean() ? info[index].As<Napi::Boolean>().Value()
                                                          : fallback;
}

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

/// @brief Read an int64 option from a JS object, falling back if missing.
inline int64_t node_int64_option(const Napi::Object& object, const char* key, int64_t fallback) {
  Napi::Value value = object.Get(key);
  return value.IsNumber() ? static_cast<int64_t>(value.As<Napi::Number>().Int64Value()) : fallback;
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

/// @brief Read a uint32 property: undefined/null returns the fallback, otherwise a
///        typed read (a non-number raises a pending JS exception).
inline uint32_t Uint32Property(const Napi::Object& obj, const char* key, uint32_t fallback) {
  Napi::Value value = obj.Get(key);
  return value.IsUndefined() || value.IsNull() ? fallback : value.As<Napi::Number>().Uint32Value();
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

// Third family, for struct fields that have no meaningful default:
//   * Required*  — the value must be present with the right type. A missing or
//     wrong-typed value becomes exactly ONE catchable TypeError and the reader
//     returns false so the caller can return immediately.
//
// Stopping at the first bad field is a hard requirement rather than a nicety:
// the addon is built with NAPI_DISABLE_CPP_EXCEPTIONS, so a typed accessor
// signals a failure by leaving a pending JS exception and returning a dummy
// value. A second N-API throw raised while an exception is already pending is a
// fatal abort (SIGABRT), not a JS-visible error, so every reader below refuses
// to throw once env.IsExceptionPending() is true and every caller must bail out
// on a false return before touching native state.

/// @brief Reject @p value unless it is a number, naming @p label in the error.
inline bool RequireNumberValue(Napi::Env env, const Napi::Value& value, const std::string& label) {
  if (env.IsExceptionPending()) return false;
  if (!value.IsNumber()) {
    Napi::TypeError::New(env, label + " must be a number").ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

/// @brief Read a required int value.
inline bool RequiredIntValue(Napi::Env env, const Napi::Value& value, const std::string& label,
                             int* out) {
  if (!RequireNumberValue(env, value, label)) return false;
  *out = value.As<Napi::Number>().Int32Value();
  return !env.IsExceptionPending();
}

/// @brief Read a required uint32 value.
inline bool RequiredUint32Value(Napi::Env env, const Napi::Value& value, const std::string& label,
                                uint32_t* out) {
  if (!RequireNumberValue(env, value, label)) return false;
  *out = value.As<Napi::Number>().Uint32Value();
  return !env.IsExceptionPending();
}

/// @brief Read a required float value.
inline bool RequiredFloatValue(Napi::Env env, const Napi::Value& value, const std::string& label,
                               float* out) {
  if (!RequireNumberValue(env, value, label)) return false;
  *out = value.As<Napi::Number>().FloatValue();
  return !env.IsExceptionPending();
}

/// @brief Read a required double value.
inline bool RequiredDoubleValue(Napi::Env env, const Napi::Value& value, const std::string& label,
                                double* out) {
  if (!RequireNumberValue(env, value, label)) return false;
  *out = value.As<Napi::Number>().DoubleValue();
  return !env.IsExceptionPending();
}

/// @brief Read a required UTF-8 string value.
inline bool RequiredStringValue(Napi::Env env, const Napi::Value& value, const std::string& label,
                                std::string* out) {
  if (env.IsExceptionPending()) return false;
  if (!value.IsString()) {
    Napi::TypeError::New(env, label + " must be a string").ThrowAsJavaScriptException();
    return false;
  }
  *out = value.As<Napi::String>().Utf8Value();
  return !env.IsExceptionPending();
}

/// @brief Read a required int property from a JS object.
inline bool RequiredIntProperty(Napi::Env env, const Napi::Object& obj, const char* key, int* out) {
  return RequiredIntValue(env, obj.Get(key), key, out);
}

/// @brief Read a required uint32 property from a JS object.
inline bool RequiredUint32Property(Napi::Env env, const Napi::Object& obj, const char* key,
                                   uint32_t* out) {
  return RequiredUint32Value(env, obj.Get(key), key, out);
}

/// @brief Read a required float property from a JS object.
inline bool RequiredFloatProperty(Napi::Env env, const Napi::Object& obj, const char* key,
                                  float* out) {
  return RequiredFloatValue(env, obj.Get(key), key, out);
}

/// @brief Read a required double property from a JS object.
inline bool RequiredDoubleProperty(Napi::Env env, const Napi::Object& obj, const char* key,
                                   double* out) {
  return RequiredDoubleValue(env, obj.Get(key), key, out);
}

/// @brief Read a required UTF-8 string property from a JS object.
inline bool RequiredStringProperty(Napi::Env env, const Napi::Object& obj, const char* key,
                                   std::string* out) {
  return RequiredStringValue(env, obj.Get(key), key, out);
}

/// @brief Read a positional argument as a size_t, rejecting anything that is not
///        a finite non-negative integer representable as both a JS number and a
///        native size_t. A wrong type is a TypeError, an out-of-domain number a
///        RangeError; both leave @p out untouched and return false.
inline bool NonNegativeSizeTArg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                                const char* name, size_t* out) {
  if (out == nullptr || info.Length() <= index || !info[index].IsNumber()) {
    Napi::TypeError::New(env, std::string(name) + " must be a number").ThrowAsJavaScriptException();
    return false;
  }

  const double value = info[index].As<Napi::Number>().DoubleValue();
  constexpr double kMaxSafeInteger = 9007199254740991.0;  // Number.MAX_SAFE_INTEGER
  const double max_size_t = static_cast<double>(std::numeric_limits<size_t>::max());
  if (!std::isfinite(value) || std::trunc(value) != value || value < 0.0 ||
      value > kMaxSafeInteger || value > max_size_t) {
    Napi::RangeError::New(
        env, std::string(name) +
                 " must be a finite non-negative integer no greater than Number.MAX_SAFE_INTEGER "
                 "or the native size_t maximum")
        .ThrowAsJavaScriptException();
    return false;
  }

  *out = static_cast<size_t>(value);
  return true;
}

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_OPTIONS_H_
