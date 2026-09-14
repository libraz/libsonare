#ifndef SONARE_NODE_SONARE_WRAP_OPTIONS_H_
#define SONARE_NODE_SONARE_WRAP_OPTIONS_H_

#include <napi.h>
#include <sonare/sonare_c.h>
// ReadBuiltinWaveform names SonareSynthWaveform and the name resolver directly;
// the umbrella does not pull this one in, so the header owns its own include
// rather than relying on a consumer that happens to include it first.
#include <sonare/sonare_c_project_instruments.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace sonare_node {

/// @brief The one narrowing every integer reader below performs.
/// @details Int32Value(), Uint32Value() and Int64Value() are the ECMAScript
///   ToInt32 / ToUint32 / ToBigInt64 conversions, which WRAP: 2^32 arrives as 0
///   -- what most of these fields read as "keep the default" -- 2^32 + 1 as 1,
///   and -1 as the largest unsigned value, which on more than one field here is
///   the all-or-none wildcard. A wrapped value is always inside the target type,
///   so no guard downstream, in the C ABI or in the core, can tell it from a
///   setting the caller chose. The range is the only thing that differs between
///   the widths, so this is written once rather than per reader.
///
///   Truncation is deliberately left alone: ToInt32 already dropped the
///   fraction at every one of these sites and 31.5 reaching the callee as 31
///   does not change a magnitude, so refusing it would be a separate contract
///   change rather than closing this wrap. It is not inert on a field whose 0
///   means "keep the default" -- there anything in (-1, 1) selects the default
///   and reports success, which is a category change rather than a magnitude
///   one, so such a field takes its integrality check in the TS facade.
///
///   The refusal UNWINDS rather than leaving a pending JS exception, because
///   these readers are called from entry points that keep working afterwards.
///   A pending exception makes every later N-API allocation return null, and the
///   result builders memcpy into it, so a reader that merely reported the error
///   would trade a silently wrong answer for a crash. A thrown Napi::Error stops
///   the entry point where it stands and SONARE_NODE_CATCH turns it back into
///   the same JS RangeError.
/// @throws Napi::RangeError naming @p name.
inline double node_narrow_number(Napi::Env env, const Napi::Value& value, const char* name,
                                 double low, double high) {
  const double number = value.As<Napi::Number>().DoubleValue();
  const double truncated = std::trunc(number);
  if (!std::isfinite(number) || truncated < low || truncated > high) {
    throw Napi::RangeError::New(env, std::string(name) + " must be a finite number within [" +
                                         std::to_string(static_cast<long long>(low)) + ", " +
                                         std::to_string(static_cast<long long>(high)) + "]");
  }
  return truncated;
}

/// @brief int-width sibling of @ref node_narrow_number.
inline int node_narrow_int(Napi::Env env, const Napi::Value& value, const char* name) {
  return static_cast<int>(node_narrow_number(env, value, name,
                                             static_cast<double>(std::numeric_limits<int>::min()),
                                             static_cast<double>(std::numeric_limits<int>::max())));
}

/// @brief uint32 sibling of @ref node_narrow_number.
inline uint32_t node_narrow_uint32(Napi::Env env, const Napi::Value& value, const char* name) {
  return static_cast<uint32_t>(node_narrow_number(
      env, value, name, 0.0, static_cast<double>(std::numeric_limits<uint32_t>::max())));
}

/// @brief Raw 32-bit word sibling of @ref node_narrow_number (a UMP word, a
///        packed MIDI 1.0 message).
/// @details Deliberately NOT @ref node_narrow_uint32: the whole 32-bit range is
///   legal here, and the idiomatic JS spelling `(0x4 << 28) | ...` is a SIGNED
///   int once bit 31 is set, so a negative is reinterpreted as its two's
///   complement word rather than refused. Only a value outside [-2^31, 2^32) is
///   a caller error. Matches the WASM reader of the same name.
/// @throws Napi::RangeError naming @p name.
inline uint32_t node_narrow_word(Napi::Env env, const Napi::Value& value, const char* name) {
  static constexpr double kSignedMin = -2147483648.0;   // -2^31
  static constexpr double kUnsignedMax = 4294967295.0;  // 2^32 - 1
  const double number = node_narrow_number(env, value, name, kSignedMin, kUnsignedMax);
  return number < 0.0 ? static_cast<uint32_t>(static_cast<int64_t>(number))
                      : static_cast<uint32_t>(number);
}

/// @brief int64 sibling of @ref node_narrow_number.
/// @details The bounds are written as powers of two rather than as
///   numeric_limits: INT64_MAX is not representable as a double, and converting
///   it rounds the bound up past the values it is meant to exclude. 2^63 - 1024
///   is the largest double below 2^63, so it is the inclusive bound.
inline int64_t node_narrow_int64(Napi::Env env, const Napi::Value& value, const char* name) {
  static constexpr double kBound = 9223372036854775808.0;  // 2^63
  static constexpr double kMax = 9223372036854774784.0;    // 2^63 - 1024
  return static_cast<int64_t>(node_narrow_number(env, value, name, -kBound, kMax));
}

/// @brief Refuses a finite value no 32-bit float can hold, giving the float
///        readers the range discipline @ref node_narrow_number gives the integer
///        ones.
/// @details Non-finite is passed through rather than refused: an infinity or a
///   NaN is the "unspecified" spelling several of these fields document, and
///   which of them mean it is a separate question from this one. What this
///   reader settles is that a caller who wrote a number gets that number or an
///   error, never an infinity the overflow invented for them.
/// @throws Napi::RangeError naming @p name.
inline float node_narrow_float(Napi::Env env, const Napi::Value& value, const char* name) {
  const double number = value.As<Napi::Number>().DoubleValue();
  if (std::isfinite(number) &&
      std::abs(number) > static_cast<double>(std::numeric_limits<float>::max())) {
    throw Napi::RangeError::New(
        env, std::string(name) + " must be a finite number within the 32-bit float range");
  }
  return static_cast<float>(number);
}

/// @brief The subject an out-of-range positional argument is named by.
inline std::string node_arg_label(size_t index) { return "argument " + std::to_string(index); }

// Canonical positional-argument readers, shared by every addon TU. Semantics
// match the recurring inline sites: a missing OR present-but-non-number
// argument at @p index falls back to @p fallback (a type-checked fallback, not
// a presence-only check). Preserve the int/float/double distinction per call
// site (Int32Value vs FloatValue vs DoubleValue).

/// @brief Read an int positional argument, falling back if absent or non-number,
///        and refusing a number the narrowing would wrap (@ref node_narrow_number).
inline int node_arg_int(const Napi::CallbackInfo& info, size_t index, int fallback) {
  if (index >= info.Length() || !info[index].IsNumber()) return fallback;
  return node_narrow_int(info.Env(), info[index], node_arg_label(index).c_str());
}

/// @brief Read an int positional argument with this family's type-checked
///        fallback, but refuse a NUMBER the narrowing would wrap.
/// @details Reads through @ref node_narrow_int like the rest of the family, so
///          the refusal is the same one; what this adds is a bool return, for a
///          caller that wants to stop explicitly rather than let the throw
///          unwind past it. A missing or non-number argument still falls back.
///          Use Int32Arg where a wrong TYPE should be refused too; this is for
///          the sites whose documented contract is the fallback.
///
///          A RangeError here and a SonareError for the same 2^32 + 1 in the TS
///          facade is not a contradiction: each layer guards a different
///          boundary. The facade guards the library's public domain, so it
///          pre-empts a native refusal and reports that code; this guards the C
///          int itself, where the value has no faithful representation at all,
///          which is the addon's own RangeError class (MidiByteProperty,
///          Int32Arg). A facade caller never reaches this check.
/// @return false when the argument could not be read; a refused number throws.
inline bool node_arg_int_no_wrap(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                                 const char* name, int fallback, int* out) {
  if (env.IsExceptionPending() || out == nullptr) return false;
  if (index >= info.Length() || !info[index].IsNumber()) {
    *out = fallback;
    return true;
  }
  *out = node_narrow_int(env, info[index], name);
  return true;
}

/// @brief Read a uint32 positional argument, falling back if absent or non-number,
///        and refusing a number the narrowing would wrap (@ref node_narrow_number).
inline uint32_t node_arg_uint32(const Napi::CallbackInfo& info, size_t index, uint32_t fallback) {
  if (index >= info.Length() || !info[index].IsNumber()) return fallback;
  return node_narrow_uint32(info.Env(), info[index], node_arg_label(index).c_str());
}

/// @brief Read an int64 positional argument, falling back if absent or non-number,
///        and refusing a number the narrowing would wrap (@ref node_narrow_number).
inline int64_t node_arg_int64(const Napi::CallbackInfo& info, size_t index, int64_t fallback) {
  if (index >= info.Length() || !info[index].IsNumber()) return fallback;
  return node_narrow_int64(info.Env(), info[index], node_arg_label(index).c_str());
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
  if (!value.IsNumber()) return fallback;
  return node_narrow_int(object.Env(), value, key);
}

/// @brief Read a float option from a JS object, falling back if missing.
inline float node_float_option(const Napi::Object& object, const char* key, float fallback) {
  Napi::Value value = object.Get(key);
  return value.IsNumber() ? node_narrow_float(object.Env(), value, key) : fallback;
}

/// @brief Read a double option from a JS object, falling back if missing.
inline double node_double_option(const Napi::Object& object, const char* key, double fallback) {
  Napi::Value value = object.Get(key);
  return value.IsNumber() ? value.As<Napi::Number>().DoubleValue() : fallback;
}

/// @brief Read an int64 option from a JS object, falling back if missing.
inline int64_t node_int64_option(const Napi::Object& object, const char* key, int64_t fallback) {
  Napi::Value value = object.Get(key);
  if (!value.IsNumber()) return fallback;
  return node_narrow_int64(object.Env(), value, key);
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
  if (value.IsUndefined() || value.IsNull()) return fallback;
  return node_narrow_int(obj.Env(), value, key);
}

/// @brief Read a raw 32-bit word property, accepting both the unsigned and the
///        bit-31-signed spelling (@ref node_narrow_word).
inline uint32_t WordProperty(const Napi::Object& obj, const char* key, uint32_t fallback) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return fallback;
  return node_narrow_word(obj.Env(), value, key);
}

/// @brief Read a uint32 property: undefined/null returns the fallback, otherwise a
///        typed read (a non-number raises a pending JS exception).
inline uint32_t Uint32Property(const Napi::Object& obj, const char* key, uint32_t fallback) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return fallback;
  return node_narrow_uint32(obj.Env(), value, key);
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
  if (value.IsUndefined() || value.IsNull()) return fallback;
  return node_narrow_int64(obj.Env(), value, key);
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

/// @brief Read a float-array property off a record object (a Float32Array, or a
///        plain number array whose non-numeric entries read as NaN).
/// @details undefined/null is an empty vector rather than an error, so an
///   optional per-band array reads the same whether it was omitted or reported
///   absent. Any other non-array value throws.
inline std::vector<float> FloatArrayProperty(const Napi::Object& obj, const char* key) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return {};
  if (value.IsTypedArray()) {
    auto typed = value.As<Napi::TypedArray>();
    if (typed.TypedArrayType() != napi_float32_array) {
      throw std::runtime_error(std::string(key) + " must be a Float32Array or a number array");
    }
    auto floats = value.As<Napi::Float32Array>();
    return std::vector<float>(floats.Data(), floats.Data() + floats.ElementLength());
  }
  if (!value.IsArray()) {
    throw std::runtime_error(std::string(key) + " must be a Float32Array or a number array");
  }
  auto array = value.As<Napi::Array>();
  std::vector<float> values;
  values.reserve(array.Length());
  for (uint32_t i = 0; i < array.Length(); ++i) {
    Napi::Value item = array.Get(i);
    values.push_back(item.IsNumber() ? item.As<Napi::Number>().FloatValue()
                                     : std::numeric_limits<float>::quiet_NaN());
  }
  return values;
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
  *out = node_narrow_int(env, value, label.c_str());
  return true;
}

/// @brief Read a required uint32 value.
inline bool RequiredUint32Value(Napi::Env env, const Napi::Value& value, const std::string& label,
                                uint32_t* out) {
  if (!RequireNumberValue(env, value, label)) return false;
  *out = node_narrow_uint32(env, value, label.c_str());
  return true;
}

/// @brief Read a required raw 32-bit word value (@ref node_narrow_word).
inline bool RequiredWordValue(Napi::Env env, const Napi::Value& value, const std::string& label,
                              uint32_t* out) {
  if (!RequireNumberValue(env, value, label)) return false;
  *out = node_narrow_word(env, value, label.c_str());
  return true;
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
  *out = node_narrow_int64(env, value, label.c_str());
  return true;
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

/// @brief Resolve a built-in oscillator waveform given as a JS string or a JS
///        number to its @ref SonareSynthWaveform ordinal.
/// @details Both spellings reach the same rejection naming the accepted set.
///          Validating only the string form leaves the numeric form -- the one a
///          generated binding produces -- silently falling back to sine, and the
///          first value past the enum is 4, not some implausible number. An
///          undefined value leaves @p out untouched and succeeds, so a config
///          object that omits the field keeps its caller's default.
/// @return false with a pending JS RangeError for any value the reader can
///         interpret but not resolve -- an unknown name, an ordinal outside the
///         enum, a number the C int cannot hold -- matching resolveEnumOrdinal
///         on the TS side. A value that is neither a string nor a number is the
///         one TypeError, per the width families above.
inline bool ReadBuiltinWaveform(Napi::Env env, const Napi::Value& value, int* out) {
  static const char* kExpected = "' (expected sine, saw, sawtooth, square, or triangle)";
  if (value.IsUndefined() || value.IsNull()) {
    return true;
  }
  if (value.IsString()) {
    const std::string name = value.As<Napi::String>().Utf8Value();
    const int mapped = sonare_synth_builtin_waveform_from_name(name.c_str());
    if (mapped < 0) {
      Napi::RangeError::New(env, "Unknown synth waveform: '" + name + kExpected)
          .ThrowAsJavaScriptException();
      return false;
    }
    *out = mapped;
    return true;
  }
  if (!RequireNumberValue(env, value, "waveform")) return false;
  // Range-checked before the int read rather than after it. Int32Value() WRAPS,
  // so 2^31 arrives as -2147483648 and the enum rejection below then names a
  // number the caller never wrote. The WASM reader refuses the same input as
  // out of 32-bit range, and this is the only field where the two are required
  // to answer in the same words.
  const double number = value.As<Napi::Number>().DoubleValue();
  if (!std::isfinite(number) || number < static_cast<double>(std::numeric_limits<int>::min()) ||
      number > static_cast<double>(std::numeric_limits<int>::max())) {
    Napi::RangeError::New(env, "waveform must be a finite number within the 32-bit integer range")
        .ThrowAsJavaScriptException();
    return false;
  }
  const int ordinal = static_cast<int>(number);
  if (ordinal < SONARE_SYNTH_WAVEFORM_SINE || ordinal > SONARE_SYNTH_WAVEFORM_TRIANGLE) {
    Napi::RangeError::New(env, "Unknown synth waveform: '" + std::to_string(ordinal) + kExpected)
        .ThrowAsJavaScriptException();
    return false;
  }
  *out = ordinal;
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
/// This is the strict sibling of OptionalIntArg. Both refuse a value the int
/// cannot hold; this one additionally refuses a wrong TYPE and rejects a
/// fractional number, and it reports by leaving a pending exception rather than
/// unwinding, so a caller that must stage several fields can bail on the first.
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
