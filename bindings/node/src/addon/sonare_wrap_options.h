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

/// @brief Read a UTF-8 string option from a JS object, falling back if missing.
inline std::string node_string_option(const Napi::Object& object, const char* key,
                                      const char* fallback) {
  Napi::Value value = object.Get(key);
  return value.IsString() ? value.As<Napi::String>().Utf8Value() : std::string(fallback);
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

/// @brief Read a MIDI-byte-wide property destined for a uint8_t C-ABI field.
/// @details Unlike Uint32Property, this rejects a value that would silently wrap
///          through the narrowing cast (256 -> 0) and so arrive at the C ABI
///          already inside the range its own check accepts. Throws a JS
///          RangeError for a non-integer or out-of-[0,255] value; the caller
///          must bail on a pending exception before the native call. The finer
///          MIDI range (group < 16, note < 128, ...) stays the C ABI's to
///          enforce, so this only closes the wrap.
inline uint8_t MidiByteProperty(Napi::Env env, const Napi::Object& obj, const char* key,
                                uint8_t fallback) {
  // An earlier field of the same struct may have left an exception pending; a
  // RangeError raised on top of it would abort the process instead of reaching
  // JS. Report nothing and let the caller bail on the first error.
  if (env.IsExceptionPending()) return fallback;
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return fallback;
  const double number = value.As<Napi::Number>().DoubleValue();
  if (!(number >= 0.0) || number > 255.0 || std::floor(number) != number) {
    Napi::RangeError::New(env, std::string(key) + " must be an integer in [0, 255]")
        .ThrowAsJavaScriptException();
    return fallback;
  }
  return static_cast<uint8_t>(number);
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

/// @brief Read a required int64 value.
inline bool RequiredInt64Value(Napi::Env env, const Napi::Value& value, const std::string& label,
                               int64_t* out) {
  if (!RequireNumberValue(env, value, label)) return false;
  *out = static_cast<int64_t>(value.As<Napi::Number>().Int64Value());
  return !env.IsExceptionPending();
}

/// @brief Read a required boolean value.
inline bool RequiredBoolValue(Napi::Env env, const Napi::Value& value, const std::string& label,
                              bool* out) {
  if (env.IsExceptionPending()) return false;
  if (!value.IsBoolean()) {
    Napi::TypeError::New(env, label + " must be a boolean").ThrowAsJavaScriptException();
    return false;
  }
  *out = value.As<Napi::Boolean>().Value();
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

/// @brief Value-level counterpart of MidiByteProperty: read a required value
///        destined for a uint8_t C-ABI field, rejecting anything the narrowing
///        cast would wrap (256 -> 0). A non-number is a TypeError, a
///        non-integer or out-of-[0,255] number a RangeError.
inline bool RequiredMidiByteValue(Napi::Env env, const Napi::Value& value, const std::string& label,
                                  uint8_t* out) {
  if (!RequireNumberValue(env, value, label)) return false;
  const double number = value.As<Napi::Number>().DoubleValue();
  if (env.IsExceptionPending()) return false;
  if (!(number >= 0.0) || number > 255.0 || std::floor(number) != number) {
    Napi::RangeError::New(env, label + " must be an integer in [0, 255]")
        .ThrowAsJavaScriptException();
    return false;
  }
  *out = static_cast<uint8_t>(number);
  return true;
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

// Fourth family, the positional-argument counterpart of the Required*/*Property
// readers, for entry points whose arguments arrive as info[i] rather than as
// object keys:
//   * Required*Arg — the argument must be present with the right type.
//   * Optional*Arg — an absent, undefined, or null argument reads as @p
//     fallback; a present argument of the wrong type is exactly ONE catchable
//     TypeError.
//
// Both return false without touching @p out on rejection, so a caller can bail
// out before its C-ABI call. That bail-out is the point: the inline
// `info[i].As<Napi::Number>().Uint32Value()` form these replace yields a dummy
// 0 alongside a pending exception, and the C-ABI call built from it then runs
// with the dummy value, flipping engine state to the opposite of what the
// caller asked for before the error is ever reported.
//
// Both halves are enforced mechanically by tests/addon-abort-guards.test.ts,
// which scans these sources: a reader of this shape defined outside this file
// fails, and so does a call to one whose false return is not consumed as
// `if (!Reader(...)) return ...;`.

/// @brief Read a required int positional argument.
inline bool RequiredIntArg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                           const char* name, int* out) {
  return RequiredIntValue(env, info[index], name, out);
}

/// @brief Read a required int64 positional argument.
inline bool RequiredInt64Arg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                             const char* name, int64_t* out) {
  return RequiredInt64Value(env, info[index], name, out);
}

/// @brief Read an optional int positional argument.
inline bool OptionalIntArg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                           const char* name, int fallback, int* out) {
  if (env.IsExceptionPending()) return false;
  const Napi::Value value = info[index];
  if (value.IsUndefined() || value.IsNull()) {
    *out = fallback;
    return true;
  }
  return RequiredIntValue(env, value, name, out);
}

/// @brief Read an optional positional argument as an int, rejecting anything a
///        float-to-integer cast cannot represent. An absent, undefined or null
///        argument reads as @p fallback; a non-number is a TypeError and a
///        non-finite, fractional or out-of-int-range number a RangeError.
///
/// This is the strict sibling of OptionalIntArg: that one narrows through
/// Int32Value(), whose result is undefined for NaN, an infinity, or a magnitude
/// past INT_MAX, so an argument headed for an int C-ABI parameter that must not
/// silently wrap is checked here instead.
inline bool Int32Arg(Napi::Env env, const Napi::CallbackInfo& info, size_t index, const char* name,
                     int fallback, int* out) {
  if (env.IsExceptionPending() || out == nullptr) return false;
  const Napi::Value value = info[index];
  if (value.IsUndefined() || value.IsNull()) {
    *out = fallback;
    return true;
  }
  if (!value.IsNumber()) {
    Napi::TypeError::New(env, std::string(name) + " must be a number").ThrowAsJavaScriptException();
    return false;
  }

  const double number = value.As<Napi::Number>().DoubleValue();
  constexpr double kMinInt = static_cast<double>(std::numeric_limits<int>::min());
  constexpr double kMaxInt = static_cast<double>(std::numeric_limits<int>::max());
  if (!std::isfinite(number) || std::trunc(number) != number || number < kMinInt ||
      number > kMaxInt) {
    Napi::RangeError::New(
        env, std::string(name) + " must be a finite integer within the native int range")
        .ThrowAsJavaScriptException();
    return false;
  }

  *out = static_cast<int>(number);
  return true;
}

/// @brief Read an optional uint32 positional argument.
inline bool OptionalUint32Arg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                              const char* name, uint32_t fallback, uint32_t* out) {
  if (env.IsExceptionPending()) return false;
  const Napi::Value value = info[index];
  if (value.IsUndefined() || value.IsNull()) {
    *out = fallback;
    return true;
  }
  return RequiredUint32Value(env, value, name, out);
}

/// @brief Read an optional int64 positional argument.
inline bool OptionalInt64Arg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                             const char* name, int64_t fallback, int64_t* out) {
  if (env.IsExceptionPending()) return false;
  const Napi::Value value = info[index];
  if (value.IsUndefined() || value.IsNull()) {
    *out = fallback;
    return true;
  }
  return RequiredInt64Value(env, value, name, out);
}

/// @brief Read an optional float positional argument.
inline bool OptionalFloatArg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                             const char* name, float fallback, float* out) {
  if (env.IsExceptionPending()) return false;
  const Napi::Value value = info[index];
  if (value.IsUndefined() || value.IsNull()) {
    *out = fallback;
    return true;
  }
  return RequiredFloatValue(env, value, name, out);
}

/// @brief Read an optional double positional argument.
inline bool OptionalDoubleArg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                              const char* name, double fallback, double* out) {
  if (env.IsExceptionPending()) return false;
  const Napi::Value value = info[index];
  if (value.IsUndefined() || value.IsNull()) {
    *out = fallback;
    return true;
  }
  return RequiredDoubleValue(env, value, name, out);
}

/// @brief Read an optional boolean positional argument.
inline bool OptionalBoolArg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                            const char* name, bool fallback, bool* out) {
  if (env.IsExceptionPending()) return false;
  const Napi::Value value = info[index];
  if (value.IsUndefined() || value.IsNull()) {
    *out = fallback;
    return true;
  }
  return RequiredBoolValue(env, value, name, out);
}

/// @brief Read an optional UTF-8 string positional argument.
inline bool OptionalStringArg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                              const char* name, const char* fallback, std::string* out) {
  if (env.IsExceptionPending()) return false;
  const Napi::Value value = info[index];
  if (value.IsUndefined() || value.IsNull()) {
    *out = fallback;
    return true;
  }
  return RequiredStringValue(env, value, name, out);
}

/// @brief Read an optional positional argument destined for a uint8_t C-ABI
///        field, with the wrap rejection MidiByteProperty applies to keys.
inline bool OptionalMidiByteArg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                                const char* name, uint8_t fallback, uint8_t* out) {
  if (env.IsExceptionPending()) return false;
  const Napi::Value value = info[index];
  if (value.IsUndefined() || value.IsNull()) {
    *out = fallback;
    return true;
  }
  return RequiredMidiByteValue(env, value, name, out);
}

/// @brief Read a positional argument as a size_t, rejecting anything that is not
///        a finite non-negative integer representable as both a JS number and a
///        native size_t. A wrong type is a TypeError, an out-of-domain number a
///        RangeError; both leave @p out untouched and return false.
inline bool NonNegativeSizeTArg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                                const char* name, size_t* out) {
  if (env.IsExceptionPending()) return false;
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
