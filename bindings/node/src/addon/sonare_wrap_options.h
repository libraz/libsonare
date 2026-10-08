#ifndef SONARE_NODE_SONARE_WRAP_OPTIONS_H_
#define SONARE_NODE_SONARE_WRAP_OPTIONS_H_

#include <napi.h>
#include <sonare/sonare_c.h>
// ReadBuiltinWaveform names SonareSynthWaveform and the name resolver directly;
// the umbrella does not pull this one in, so the header owns its own include
// rather than relying on a consumer that happens to include it first.
#include <sonare/sonare_c_project_instruments.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
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
///   A fraction is refused here rather than truncated, because every caller of
///   this function narrows onto an integer C type and 31.5 arriving as 31 is a
///   setting the caller never chose that nothing downstream can separate from
///   one they did. Doing it here rather than per entry point is what makes it
///   hold for an entry point added later: the facade guard it would otherwise
///   depend on is written by hand, so it covers whichever functions someone
///   remembered. A field whose 0 means "keep the default" still reads through
///   @ref ZeroIsSentinel, which names that consequence instead.
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
  // After the range, so an out-of-range fraction is reported by the property it
  // is furthest outside rather than by whichever check runs first.
  if (truncated != number) {
    throw Napi::RangeError::New(env, std::string(name) + " must be an integer");
  }
  return number;
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

/// @brief uint16 sibling of @ref node_narrow_number.
/// @details The narrowing cast is what makes this a member rather than a cast at
///   the site: 65536 lands on 0 and -1 on 0xFFFF, both inside the width, so a C
///   ABI taking a uint16_t mask or index cannot tell either from a caller's
///   choice. A field with a domain narrower than the width still owns that
///   bound; this only closes the wrap.
inline uint16_t node_narrow_uint16(Napi::Env env, const Napi::Value& value, const char* name) {
  return static_cast<uint16_t>(node_narrow_number(
      env, value, name, 0.0, static_cast<double>(std::numeric_limits<uint16_t>::max())));
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

/// @brief Reads a canonical decimal string as a uint64.
/// @details A uint64 does not fit a JS number, so its one spelling is the decimal
///   string: a number or bigint is refused rather than accepted as a second
///   spelling, and so is any non-canonical string ("007", "+7", "7.0", "", or a
///   value past 2^64 - 1), which keeps one value to one spelling.
/// @throws Napi::TypeError for a non-string, Napi::RangeError for a bad string,
///   both naming @p name.
inline uint64_t node_narrow_uint64(Napi::Env env, const Napi::Value& value, const char* name) {
  const std::string expected = std::string(name) + " must be a decimal uint64 string";
  if (!value.IsString()) throw Napi::TypeError::New(env, expected);
  const std::string text = value.As<Napi::String>().Utf8Value();
  uint64_t number = 0;
  const char* end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, number, 10);
  if (text.empty() || (text.size() > 1 && text.front() == '0') || parsed.ec != std::errc{} ||
      parsed.ptr != end) {
    throw Napi::RangeError::New(env, expected);
  }
  return number;
}

/// @brief The C ABI's own float conversion, saturation included, for the total
///        functions whose core answers a non-finite input rather than failing on
///        it.
/// @details The permissive end of this family, and the only member that refuses
///   nothing. Reserved for the entry points where the C ABI is the oracle and
///   passes the value straight through: `hz_to_note` answers a non-finite with
///   "?", the same answer it gives a non-positive frequency, and the librosa
///   mirrors propagate a NaN the way the reference does. `pyin`'s default fills
///   unvoiced frames with NaN, so its own output is a legitimate argument here.
///   Refusing either would leave the C ABI and WASM permissive and put this
///   surface alone out of step with the oracle. Expects a value already known to
///   be a number.
inline float node_float_as_c_abi(const Napi::Value& value) {
  return value.As<Napi::Number>().FloatValue();
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

/// @brief Finite sibling of @ref node_narrow_float, for a field with no
///        "unspecified" spelling.
/// @details @ref node_narrow_float passes a non-finite through because several
///   fields document one as meaning unspecified. Where a field documents no such
///   value, an infinity is out of domain rather than a request, and this refuses
///   it in the same words the overflow is refused in -- one wording, because a
///   caller cannot act on which of the two produced the value.
/// @throws Napi::RangeError naming @p name.
inline float node_narrow_finite_float(Napi::Env env, const Napi::Value& value, const char* name) {
  const double number = value.As<Napi::Number>().DoubleValue();
  if (!std::isfinite(number)) {
    throw Napi::RangeError::New(
        env, std::string(name) + " must be a finite number within the 32-bit float range");
  }
  return node_narrow_float(env, value, name);
}

/// @brief Read one element of a plain JS number array as a finite float, named
///        by index.
/// @details The element shape of @ref node_narrow_finite_float, for the array
///   readers whose caller-visible subject is `name[i]` rather than `name`. A
///   non-number entry is refused rather than substituted: the two array readers
///   this serves used to answer it as a silent 0.0f and as a dummy beside a
///   pending exception, and 0 is in domain on both of their fields.
/// @throws Napi::TypeError for a non-number entry, Napi::RangeError for a value
///         outside the finite 32-bit float range.
inline float node_narrow_finite_float_element(Napi::Env env, const Napi::Value& value,
                                              const char* name, uint32_t index) {
  const std::string element = std::string(name) + "[" + std::to_string(index) + "]";
  if (!value.IsNumber()) {
    throw Napi::TypeError::New(env, element + " must be a number");
  }
  return node_narrow_finite_float(env, value, element.c_str());
}

/// @brief Marks a key whose 0 the library reads as "keep the default" rather
///        than as a quantity.
/// @details Written where a fallback would go, because it IS the fallback: such
///   a key defaults at 0. It selects the reader overloads below, which name the
///   sentinel when they refuse a fraction -- anything in (-1, 1) would select the
///   default and report success, which is a different mistake from a magnitude
///   landing one step away, and a caller can only act on the one they made. The
///   narrowing itself refuses a fraction for every key; this decides the wording.
struct ZeroIsSentinel {};
inline constexpr ZeroIsSentinel kZeroIsSentinel{};

/// @brief Whether @p value is a finite number carrying a fractional part.
/// @details Non-finite and out-of-range are left to @ref node_narrow_number, so
///   each input is reported by the check that owns it.
inline bool node_is_fraction(const Napi::Value& value) {
  const double number = value.As<Napi::Number>().DoubleValue();
  return std::isfinite(number) && std::trunc(number) != number;
}

/// @brief The one wording every @ref ZeroIsSentinel refusal reports.
inline std::string node_fraction_message(const char* name) {
  return std::string(name) + " must be a whole number: its zero selects the library default";
}

/// @brief Refuses a finite value that is not a whole number, naming @p name.
/// @throws Napi::RangeError naming @p name.
inline void node_refuse_fraction(Napi::Env env, const Napi::Value& value, const char* name) {
  if (env.IsExceptionPending()) return;
  if (node_is_fraction(value)) {
    throw Napi::RangeError::New(env, node_fraction_message(name));
  }
}

/// @brief The subject an out-of-range positional argument is named by.
inline std::string node_arg_label(size_t index) { return "argument " + std::to_string(index); }

// Object-key readers are the *Property family: undefined/null falls back to the
// default, and any other value of the wrong type is refused by name. They report
// by throwing, which SONARE_NODE_CATCH turns back into a JS error.

/// @brief Refuses a present value the reader cannot read, naming @p key.
/// @details The typed N-API accessors report a mismatch by leaving a pending JS
///   exception and returning a dummy, and the entry point then runs to
///   completion on it: every later N-API allocation returns null and the result
///   builders memcpy into it. Checking the type first makes the refusal unwind
///   instead, which is how @ref node_narrow_number reports and what
///   SONARE_NODE_CATCH turns back into a JS error.
/// @throws Napi::TypeError naming @p key.
inline void node_require_property_type(Napi::Env env, bool ok, const char* key,
                                       const char* expected) {
  if (env.IsExceptionPending() || ok) return;
  throw Napi::TypeError::New(env, std::string(key) + " must be " + expected);
}

/// @brief Read an int property: undefined/null returns the fallback, any other
///        non-number is refused by name (see note above).
inline int IntProperty(const Napi::Object& obj, const char* key, int fallback) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return fallback;
  node_require_property_type(obj.Env(), value.IsNumber(), key, "a number");
  return node_narrow_int(obj.Env(), value, key);
}

/// @brief Read an int property whose 0 is the library default
///        (@ref ZeroIsSentinel), refusing a fraction.
inline int IntProperty(const Napi::Object& obj, const char* key, ZeroIsSentinel) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return 0;
  node_require_property_type(obj.Env(), value.IsNumber(), key, "a number");
  node_refuse_fraction(obj.Env(), value, key);
  return node_narrow_int(obj.Env(), value, key);
}

/// @brief Read a raw 32-bit word property, accepting both the unsigned and the
///        bit-31-signed spelling (@ref node_narrow_word).
inline uint32_t WordProperty(const Napi::Object& obj, const char* key, uint32_t fallback) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return fallback;
  node_require_property_type(obj.Env(), value.IsNumber(), key, "a number");
  return node_narrow_word(obj.Env(), value, key);
}

/// @brief Read a uint32 property: undefined/null returns the fallback, any other
///        non-number is refused by name.
inline uint32_t Uint32Property(const Napi::Object& obj, const char* key, uint32_t fallback) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return fallback;
  node_require_property_type(obj.Env(), value.IsNumber(), key, "a number");
  return node_narrow_uint32(obj.Env(), value, key);
}

/// @brief Read a uint32 property whose 0 is the library default
///        (@ref ZeroIsSentinel), refusing a fraction.
inline uint32_t Uint32Property(const Napi::Object& obj, const char* key, ZeroIsSentinel) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return 0;
  node_require_property_type(obj.Env(), value.IsNumber(), key, "a number");
  node_refuse_fraction(obj.Env(), value, key);
  return node_narrow_uint32(obj.Env(), value, key);
}

/// @brief Read a MIDI-byte-wide property destined for a uint8_t C-ABI field.
/// @details Unlike Uint32Property, this rejects a value that would silently wrap
///          through the narrowing cast (256 -> 0) and so arrive at the C ABI
///          already inside the range its own check accepts. Presence- and
///          type-checked like the rest of the *Property family (undefined/null
///          takes @p fallback, any other wrong-typed value is refused by name
///          via @ref node_require_property_type), then throws a JS RangeError
///          naming @p key for a non-integer or out-of-[0,255] value. The finer
///          MIDI range (group < 16, note < 128, ...) stays the C ABI's to
///          enforce, so this only closes the wrap.
/// @throws Napi::TypeError or Napi::RangeError naming @p key.
inline uint8_t MidiByteProperty(Napi::Env env, const Napi::Object& obj, const char* key,
                                uint8_t fallback) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return fallback;
  node_require_property_type(env, value.IsNumber(), key, "a number");
  const double number = value.As<Napi::Number>().DoubleValue();
  if (!(number >= 0.0) || number > 255.0 || std::floor(number) != number) {
    throw Napi::RangeError::New(env, std::string(key) + " must be an integer in [0, 255]");
  }
  return static_cast<uint8_t>(number);
}

/// @brief Read a property destined for an int8_t C-ABI field: undefined/null
///        returns the fallback, a non-number is refused by name, and a value the
///        narrowing cast would wrap (256 -> 0, 200 -> -56) is a RangeError naming
///        @p key. The field's own domain stays the C ABI's to enforce.
inline int8_t Int8Property(const Napi::Object& obj, const char* key, int8_t fallback) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return fallback;
  node_require_property_type(obj.Env(), value.IsNumber(), key, "a number");
  return static_cast<int8_t>(node_narrow_number(
      obj.Env(), value, key, static_cast<double>(std::numeric_limits<int8_t>::min()),
      static_cast<double>(std::numeric_limits<int8_t>::max())));
}

/// @brief Read an int64 property: undefined/null returns the fallback, any other
///        non-number is refused by name.
inline int64_t Int64Property(const Napi::Object& obj, const char* key, int64_t fallback) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return fallback;
  node_require_property_type(obj.Env(), value.IsNumber(), key, "a number");
  return node_narrow_int64(obj.Env(), value, key);
}

/// @brief Read an int64 property whose 0 is the library default
///        (@ref ZeroIsSentinel), refusing a fraction.
inline int64_t Int64Property(const Napi::Object& obj, const char* key, ZeroIsSentinel) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return 0;
  node_require_property_type(obj.Env(), value.IsNumber(), key, "a number");
  node_refuse_fraction(obj.Env(), value, key);
  return node_narrow_int64(obj.Env(), value, key);
}

/// @brief Read a uint64 property written as a decimal string: undefined/null
///        returns the fallback, any other value is refused by name
///        (@ref node_narrow_uint64).
inline uint64_t Uint64Property(const Napi::Object& obj, const char* key, uint64_t fallback) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return fallback;
  return node_narrow_uint64(obj.Env(), value, key);
}

/// @brief Read a float property: undefined/null returns the fallback, any other
///        non-number is refused by name.
/// @details Narrows through @ref node_narrow_float, so a finite value no 32-bit
///   float can hold is refused rather than arriving downstream as an infinity.
inline float FloatProperty(const Napi::Object& obj, const char* key, float fallback) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return fallback;
  node_require_property_type(obj.Env(), value.IsNumber(), key, "a number");
  return node_narrow_float(obj.Env(), value, key);
}

/// @brief Read a float property that must be finite
///        (@ref node_narrow_finite_float).
/// @details Use where the field documents no "unspecified" spelling, so an
///   infinity is refused by name instead of travelling on as one.
inline float FiniteFloatProperty(const Napi::Object& obj, const char* key, float fallback) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return fallback;
  node_require_property_type(obj.Env(), value.IsNumber(), key, "a number");
  return node_narrow_finite_float(obj.Env(), value, key);
}

/// @brief Read a finite float property that also accepts the string `"auto"`.
/// @details `"auto"` sets @p is_auto and returns @p fallback; a number is read as
///   @ref FiniteFloatProperty does, and any other type is refused by name.
inline float AutoFiniteFloatProperty(const Napi::Object& obj, const char* key, float fallback,
                                     bool* is_auto) {
  *is_auto = false;
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return fallback;
  if (value.IsString()) {
    if (value.As<Napi::String>().Utf8Value() == "auto") {
      *is_auto = true;
      return fallback;
    }
    throw Napi::TypeError::New(obj.Env(), std::string(key) + " must be a number or 'auto'");
  }
  node_require_property_type(obj.Env(), value.IsNumber(), key, "a number or 'auto'");
  return node_narrow_finite_float(obj.Env(), value, key);
}

/// @brief Read a double property: undefined/null returns the fallback, any other
///        non-number is refused by name.
inline double DoubleProperty(const Napi::Object& obj, const char* key, double fallback) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return fallback;
  node_require_property_type(obj.Env(), value.IsNumber(), key, "a number");
  return value.As<Napi::Number>().DoubleValue();
}

/// @brief Read a bool property: undefined/null returns the fallback, any other
///        non-boolean is refused by name.
inline bool BoolProperty(const Napi::Object& obj, const char* key, bool fallback) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return fallback;
  node_require_property_type(obj.Env(), value.IsBoolean(), key, "a boolean");
  return value.As<Napi::Boolean>().Value();
}

/// @brief Read a UTF-8 string property: undefined/null returns the fallback, any
///        other non-string is refused by name.
inline std::string StringProperty(const Napi::Object& obj, const char* key, const char* fallback) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return std::string(fallback);
  node_require_property_type(obj.Env(), value.IsString(), key, "a string");
  return value.As<Napi::String>().Utf8Value();
}

/// @brief Read the GS insertion-effect realisation ("modern" or "classic") as
///        its C ABI ordinal: undefined/null is modern, a non-string is a
///        TypeError and any other name a RangeError, both naming @p key.
inline int GsEfxRealizationProperty(const Napi::Object& obj, const char* key) {
  const std::string name = StringProperty(obj, key, "modern");
  if (name == "modern") return 0;
  if (name == "classic") return 1;
  throw Napi::RangeError::New(
      obj.Env(), std::string(key) + " must be 'modern' or 'classic', got '" + name + "'");
}

/// @brief Read a selector given by name.
/// @details undefined/null is @p fallback. A string is looked up in @p names, whose positions are
///   the selector values, and a name outside it is a RangeError listing the valid set; any other
///   type, an integer included, is a TypeError naming the valid set.
template <std::size_t N>
inline int NamedSelectorProperty(const Napi::Object& obj, const char* key,
                                 const std::array<const char*, N>& names, int fallback) {
  Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) return fallback;
  std::string valid;
  for (std::size_t i = 0; i < N; ++i) {
    valid += (i == 0 ? "'" : ", '") + std::string(names[i]) + "'";
  }
  if (!value.IsString()) {
    throw Napi::TypeError::New(
        obj.Env(), std::string(key) + " must be one of " + valid + " (a name, not a number)");
  }
  const std::string name = value.As<Napi::String>().Utf8Value();
  for (std::size_t i = 0; i < N; ++i) {
    if (name == names[i]) return static_cast<int>(i);
  }
  throw Napi::RangeError::New(
      obj.Env(), std::string(key) + " must be one of " + valid + ", got '" + name + "'");
}

/// @brief Read a float-array property off a record object (a Float32Array, or a
///        plain number array whose non-numeric entries read as NaN).
/// @details undefined/null is an empty vector rather than an error, so an
///   optional per-band array reads the same whether it was omitted or reported
///   absent. Any other non-array value throws.
/// @note One rule runs down both paths and they answer differently because the
///   values differ, not because the rule does. A Float32Array element arrives
///   already folded to an infinity, which @ref node_narrow_float passes; a plain
///   array still holds the finite double the caller wrote, which it refuses. The
///   typed path CANNOT refuse -- the only copy of what was asked for was lost in
///   JS -- and declining to refuse the plain one would destroy the surviving copy
///   to match a case that never had it.
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
    if (!item.IsNumber()) {
      values.push_back(std::numeric_limits<float>::quiet_NaN());
      continue;
    }
    // Indexed, because the key alone cannot say which element was refused.
    const std::string element = std::string(key) + "[" + std::to_string(i) + "]";
    values.push_back(node_narrow_float(obj.Env(), item, element.c_str()));
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

/// @brief Read a required float value (@ref node_narrow_float).
inline bool RequiredFloatValue(Napi::Env env, const Napi::Value& value, const std::string& label,
                               float* out) {
  if (!RequireNumberValue(env, value, label)) return false;
  *out = node_narrow_float(env, value, label.c_str());
  return true;
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

/// @brief Read a required uint64 value spelled as a decimal string
///        (@ref node_narrow_uint64).
inline bool RequiredUint64Value(Napi::Env env, const Napi::Value& value, const std::string& label,
                                uint64_t* out) {
  if (env.IsExceptionPending()) return false;
  *out = node_narrow_uint64(env, value, label.c_str());
  return true;
}

/// @brief Read a required finite double value within [@p minimum, @p maximum].
/// @details Unlike @ref RequiredDoubleValue this refuses NaN, infinities and
///   out-of-range values with a RangeError naming @p label.
inline bool RequiredFiniteDoubleValue(Napi::Env env, const Napi::Value& value,
                                      const std::string& label, double* out,
                                      double minimum = -std::numeric_limits<double>::infinity(),
                                      double maximum = std::numeric_limits<double>::infinity()) {
  if (!RequireNumberValue(env, value, label)) return false;
  const double number = value.As<Napi::Number>().DoubleValue();
  if (!std::isfinite(number) || number < minimum || number > maximum) {
    Napi::RangeError::New(env, label + " must be finite and within the supported range")
        .ThrowAsJavaScriptException();
    return false;
  }
  *out = number;
  return true;
}

/// @brief Read a required whole number that a JS number holds exactly, within
///        [@p minimum, @p maximum] (default: the whole safe-integer range).
inline bool RequiredSafeIntegerValue(Napi::Env env, const Napi::Value& value,
                                     const std::string& label, int64_t* out,
                                     int64_t minimum = -9007199254740991LL,
                                     int64_t maximum = 9007199254740991LL) {
  if (!RequireNumberValue(env, value, label)) return false;
  *out = static_cast<int64_t>(
      node_narrow_number(env, value, label.c_str(),
                         static_cast<double>(std::max<int64_t>(minimum, -9007199254740991LL)),
                         static_cast<double>(std::min<int64_t>(maximum, 9007199254740991LL))));
  return true;
}

/// @brief Read a required plain object (not an array or function).
inline bool RequiredObjectValue(Napi::Env env, const Napi::Value& value, const std::string& label,
                                Napi::Object* out) {
  if (env.IsExceptionPending()) return false;
  if (!value.IsObject() || value.IsArray() || value.IsFunction()) {
    Napi::TypeError::New(env, label + " must be an object").ThrowAsJavaScriptException();
    return false;
  }
  *out = value.As<Napi::Object>();
  return true;
}

/// @brief Read a required array.
inline bool RequiredArrayValue(Napi::Env env, const Napi::Value& value, const std::string& label,
                               Napi::Array* out) {
  if (env.IsExceptionPending()) return false;
  if (!value.IsArray()) {
    Napi::TypeError::New(env, label + " must be an array").ThrowAsJavaScriptException();
    return false;
  }
  *out = value.As<Napi::Array>();
  return true;
}

/// @brief Read a required Float32Array.
inline bool RequiredFloat32ArrayValue(Napi::Env env, const Napi::Value& value,
                                      const std::string& label, Napi::Float32Array* out) {
  if (env.IsExceptionPending()) return false;
  if (!value.IsTypedArray() ||
      value.As<Napi::TypedArray>().TypedArrayType() != napi_float32_array) {
    Napi::TypeError::New(env, label + " must be a Float32Array").ThrowAsJavaScriptException();
    return false;
  }
  *out = value.As<Napi::Float32Array>();
  return true;
}

/// @brief Read a required Uint8Array.
inline bool RequiredUint8ArrayValue(Napi::Env env, const Napi::Value& value,
                                    const std::string& label, Napi::Uint8Array* out) {
  if (env.IsExceptionPending()) return false;
  if (!value.IsTypedArray() || value.As<Napi::TypedArray>().TypedArrayType() != napi_uint8_array) {
    Napi::TypeError::New(env, label + " must be a Uint8Array").ThrowAsJavaScriptException();
    return false;
  }
  *out = value.As<Napi::Uint8Array>();
  return true;
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

/// @brief Read a value destined for a uint16_t C-ABI field, rejecting anything
///        that would silently wrap through the narrowing cast.
/// @details The 16-bit sibling of @ref RequiredMidiByteValue, written for the
///          fields a MIDI byte cannot spell: a pitch bend is 14-bit, so reading
///          it through the 7-bit family would refuse half its domain. It closes
///          the same hole at the wider width — 65536 lands on 0 and -1 on
///          0xFFFF, both inside the type, so a C ABI checking its own range
///          cannot tell either from a value the caller chose. The narrower MIDI
///          domain (a bend is 0..16383) stays the C ABI's to enforce, as it does
///          for the byte reader.
inline bool RequiredUint16Value(Napi::Env env, const Napi::Value& value, const std::string& label,
                                uint16_t* out) {
  if (!RequireNumberValue(env, value, label)) return false;
  const double number = value.As<Napi::Number>().DoubleValue();
  if (env.IsExceptionPending()) return false;
  if (!(number >= 0.0) || number > 65535.0 || std::floor(number) != number) {
    Napi::RangeError::New(env, label + " must be an integer in [0, 65535]")
        .ThrowAsJavaScriptException();
    return false;
  }
  *out = static_cast<uint16_t>(number);
  return true;
}

/// @brief Read a value as an int, rejecting anything a float-to-integer cast
///        cannot represent: a non-number is a TypeError, a non-finite,
///        fractional or out-of-int-range number a RangeError.
/// @details The strict sibling of @ref RequiredIntValue. Both refuse a value the
///   int cannot hold; this one additionally refuses a FRACTION, because it
///   serves the fields whose int is an ordinal or a count rather than a
///   magnitude, where 1.5 truncating to 1 selects something the caller did not
///   name. It reports by leaving a pending exception rather than unwinding, so a
///   caller staging several fields can bail on the first.
///
///   A RangeError here and a SonareError for the same 2^32 + 1 in the TS facade
///   is not a contradiction: each layer guards a different boundary. The facade
///   guards the library's public domain, so it pre-empts a native refusal and
///   reports that code; this guards the C int itself, where the value has no
///   faithful representation at all. A facade caller never reaches this check.
/// @return false without touching @p out on rejection.
inline bool Int32Value(Napi::Env env, const Napi::Value& value, const char* name, int* out) {
  if (env.IsExceptionPending() || out == nullptr) return false;
  if (!value.IsNumber()) {
    Napi::TypeError::New(env, std::string(name) + " must be a number").ThrowAsJavaScriptException();
    return false;
  }

  const double number = value.As<Napi::Number>().DoubleValue();
  constexpr double kMinInt = static_cast<double>(std::numeric_limits<int>::min());
  constexpr double kMaxInt = static_cast<double>(std::numeric_limits<int>::max());
  // Three refusals rather than one, so each names the property the value
  // actually lacks: a fraction and a magnitude are different mistakes, and a
  // caller can only act on the one they made.
  if (!std::isfinite(number)) {
    Napi::RangeError::New(env, std::string(name) + " must be a finite integer")
        .ThrowAsJavaScriptException();
    return false;
  }
  const double truncated = std::trunc(number);
  if (truncated < kMinInt || truncated > kMaxInt) {
    Napi::RangeError::New(env, std::string(name) + " must be an integer in [" +
                                   std::to_string(std::numeric_limits<int>::min()) + ", " +
                                   std::to_string(std::numeric_limits<int>::max()) + "]")
        .ThrowAsJavaScriptException();
    return false;
  }
  // After the range, so an out-of-range fraction is reported by the property it
  // is furthest outside rather than by whichever check runs first.
  if (truncated != number) {
    Napi::RangeError::New(env, std::string(name) + " must be an integer")
        .ThrowAsJavaScriptException();
    return false;
  }

  *out = static_cast<int>(number);
  return true;
}

/// @brief Read one element of a plain JS number array as a strict int, named by
///        index.
/// @details The element shape of @ref Int32Value, for the array readers whose
///   caller-visible subject is `name[i]` rather than `name`: a fraction such as
///   1000.7 is refused here instead of truncating onto a legal index. Int32Value
///   reports through a pending exception; this converts that into a throw so an
///   array-reader loop unwinds instead of running to completion on a dummy 0.
/// @throws Napi::TypeError for a non-number entry, Napi::RangeError for a
///         fractional, non-finite, or out-of-int-range value.
inline int node_narrow_int_element(Napi::Env env, const Napi::Value& value, const char* name,
                                   uint32_t index) {
  const std::string element = std::string(name) + "[" + std::to_string(index) + "]";
  int out = 0;
  if (!Int32Value(env, value, element.c_str(), &out)) {
    throw env.GetAndClearPendingException();
  }
  return out;
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

/// @brief Object-key form of @ref Int32Value: undefined/null reads as
///        @p fallback, anything else goes through the strict int read.
inline bool Int32Property(Napi::Env env, const Napi::Object& obj, const char* key, int fallback,
                          int* out) {
  if (env.IsExceptionPending() || out == nullptr) return false;
  const Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) {
    *out = fallback;
    return true;
  }
  return Int32Value(env, value, key, out);
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

/// @brief Read a required uint32 positional argument.
inline bool RequiredUint32Arg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                              const char* name, uint32_t* out) {
  return RequiredUint32Value(env, info[index], name, out);
}

/// @brief Read a required float positional argument.
inline bool RequiredFloatArg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                             const char* name, float* out) {
  return RequiredFloatValue(env, info[index], name, out);
}

/// @brief Read a required double positional argument.
inline bool RequiredDoubleArg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                              const char* name, double* out) {
  return RequiredDoubleValue(env, info[index], name, out);
}

/// @brief Read a required boolean positional argument.
inline bool RequiredBoolArg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                            const char* name, bool* out) {
  return RequiredBoolValue(env, info[index], name, out);
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

/// @brief Positional form of @ref Int32Value: an absent, undefined or null
///        argument reads as @p fallback, anything else goes through the strict
///        int read.
/// @details The strict sibling of OptionalIntArg -- that one refuses only what
///   the int cannot hold, this one also refuses a wrong type and a fraction.
inline bool Int32Arg(Napi::Env env, const Napi::CallbackInfo& info, size_t index, const char* name,
                     int fallback, int* out) {
  if (env.IsExceptionPending() || out == nullptr) return false;
  const Napi::Value value = info[index];
  if (value.IsUndefined() || value.IsNull()) {
    *out = fallback;
    return true;
  }
  return Int32Value(env, value, name, out);
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

/// @brief Read an optional uint32 positional argument whose 0 is the library
///        default (@ref ZeroIsSentinel), refusing a fraction.
/// @details Reports by leaving a pending exception rather than unwinding, which
///   is this family's contract, so the caller still bails on a false return.
inline bool OptionalUint32Arg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                              const char* name, ZeroIsSentinel, uint32_t* out) {
  if (env.IsExceptionPending()) return false;
  const Napi::Value value = info[index];
  if (value.IsUndefined() || value.IsNull()) {
    *out = 0;
    return true;
  }
  if (!RequireNumberValue(env, value, name)) return false;
  if (node_is_fraction(value)) {
    Napi::RangeError::New(env, node_fraction_message(name)).ThrowAsJavaScriptException();
    return false;
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

/// @brief Read an optional float positional argument whose field documents no
///        "unspecified" spelling, refusing a non-finite value as well
///        (@ref node_narrow_finite_float).
inline bool OptionalFiniteFloatArg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                                   const char* name, float fallback, float* out) {
  if (env.IsExceptionPending()) return false;
  const Napi::Value value = info[index];
  if (value.IsUndefined() || value.IsNull()) {
    *out = fallback;
    return true;
  }
  if (!RequireNumberValue(env, value, name)) return false;
  *out = node_narrow_finite_float(env, value, name);
  return true;
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

/// @brief Read an optional positional argument destined for a uint16_t C-ABI
///        field, with the wrap rejection @ref RequiredUint16Value applies.
inline bool OptionalUint16Arg(Napi::Env env, const Napi::CallbackInfo& info, size_t index,
                              const char* name, uint16_t fallback, uint16_t* out) {
  if (env.IsExceptionPending()) return false;
  const Napi::Value value = info[index];
  if (value.IsUndefined() || value.IsNull()) {
    *out = fallback;
    return true;
  }
  return RequiredUint16Value(env, value, name, out);
}

/// @brief The size_t domain both readers below enforce, written once: a finite
///        non-negative integer representable as both a JS number and a native
///        size_t. Expects @p value to have been type-checked already.
inline bool node_read_size_t_domain(Napi::Env env, const Napi::Value& value, const char* name,
                                    size_t* out) {
  const double number = value.As<Napi::Number>().DoubleValue();
  constexpr double kMaxSafeInteger = 9007199254740991.0;  // Number.MAX_SAFE_INTEGER
  const double max_size_t = static_cast<double>(std::numeric_limits<size_t>::max());
  if (!std::isfinite(number) || std::trunc(number) != number || number < 0.0 ||
      number > kMaxSafeInteger || number > max_size_t) {
    Napi::RangeError::New(
        env, std::string(name) +
                 " must be a finite non-negative integer no greater than Number.MAX_SAFE_INTEGER "
                 "or the native size_t maximum")
        .ThrowAsJavaScriptException();
    return false;
  }

  *out = static_cast<size_t>(number);
  return true;
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
  return node_read_size_t_domain(env, info[index], name, out);
}

/// @brief Object-key form of @ref NonNegativeSizeTArg: undefined/null reads as
///        @p fallback, any other non-number is a TypeError.
inline bool NonNegativeSizeTProperty(Napi::Env env, const Napi::Object& obj, const char* key,
                                     size_t fallback, size_t* out) {
  if (env.IsExceptionPending() || out == nullptr) return false;
  const Napi::Value value = obj.Get(key);
  if (value.IsUndefined() || value.IsNull()) {
    *out = fallback;
    return true;
  }
  if (!value.IsNumber()) {
    Napi::TypeError::New(env, std::string(key) + " must be a number").ThrowAsJavaScriptException();
    return false;
  }
  return node_read_size_t_domain(env, value, key, out);
}

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_OPTIONS_H_
