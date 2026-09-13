/// @file common.cpp
/// @brief Shared helpers for the Embind translation units.

#ifdef __EMSCRIPTEN__

#include "common.h"

#include <sonare/sonare_c_project_instruments.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "util/numeric_validation.h"

// ============================================================================
// Helper functions
// ============================================================================

// ---------------------------------------------------------------------------
// Zero-copy / bulk-copy helpers for the JS ↔ C++ Float32Array boundary.
//
// The naïve embind path (`vecFromJSArray<float>` + `result.set(i, vec[i])`)
// performs one JS↔WASM boundary crossing per element, which is O(N) marshalling
// overhead — measurable at hundreds of microseconds per million samples.
//
// These helpers collapse the marshalling to a single bulk memcpy by wrapping
// the C++ buffer in a `Float32Array` view onto the WASM heap and using the
// JS-side `TypedArray.prototype.set(otherTypedArray)` fast path.
// ---------------------------------------------------------------------------

val stringVectorToVal(const std::vector<std::string>& names) {
  val out = val::array();
  for (const std::string& name : names) {
    out.call<void>("push", name);
  }
  return out;
}

val vectorToFloat32Array(const std::vector<float>& vec) {
  const size_t n = vec.size();
  val result = val::global("Float32Array").new_(n);
  if (n == 0) return result;
  // Wrap the C++ vector data as a Float32Array view onto the WASM heap and
  // use JS-side TypedArray.set for a single bulk memcpy across the boundary.
  // The view is non-owning; ownership stays with `vec`. Because `result` is a
  // freshly-allocated, independent Float32Array, the caller owns the copy and
  // we drop the view immediately after the set() call.
  val view = val(typed_memory_view(n, vec.data()));
  result.call<void>("set", view);
  return result;
}

val vectorToInt32Array(const std::vector<int>& vec) {
  const size_t n = vec.size();
  val result = val::global("Int32Array").new_(n);
  if (n == 0) return result;
  val view = val(typed_memory_view(n, vec.data()));
  result.call<void>("set", view);
  return result;
}

// Uint8 sibling, declared in common.h. Defined here with the other
// vectorTo*Array helpers (it used to live in stream_analyzer.cpp, leaving
// project.cpp linking against another TU's definition).
val vectorToUint8Array(const std::vector<uint8_t>& vec) {
  const size_t n = vec.size();
  val result = val::global("Uint8Array").new_(n);
  if (n == 0) return result;
  val view = val(typed_memory_view(n, vec.data()));
  result.call<void>("set", view);
  return result;
}

// Bulk-copy a JS Float32Array (or any array-like with numeric `.length`) into
// a freshly-allocated std::vector<float>. The single boundary crossing is
// `view.set(arr)` inside JS land; the typed_memory_view wraps the destination
// vector's storage so no intermediate buffer is allocated.
std::size_t wasmCountArg(double value, const char* subject) {
  constexpr double kMaxSafeInteger = 9007199254740991.0;
  if (!std::isfinite(value) || value < 0.0 || std::floor(value) != value ||
      value > kMaxSafeInteger) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(subject) + " must be a non-negative safe integer");
  }
  return static_cast<std::size_t>(value);
}

std::size_t wasmArrayLikeLength(const val& arr, const char* subject, const char* length_key) {
  if (arr.isNull() || arr.isUndefined()) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(subject) + " must be an array-like object");
  }
  const val length_value = arr[length_key];
  if (length_value.isUndefined() || length_value.typeOf().as<std::string>() != "number") {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(subject) + " length must be a number");
  }
  const double length = length_value.as<double>();
  constexpr double kMaxSafeInteger = 9007199254740991.0;
  if (!std::isfinite(length) || length < 0.0 || std::floor(length) != length ||
      length > kMaxSafeInteger) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(subject) + " length must be a non-negative safe integer");
  }
  if (length > static_cast<double>(kMaxWasmFloat32Elements)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(subject) + " exceeds the WASM Float32 input budget");
  }
  return static_cast<std::size_t>(length);
}

std::size_t wasmFloat32ArrayLength(const val& arr, const char* subject) {
  return wasmArrayLikeLength(arr, subject);
}

void validateWasmFloat32ElementBudget(std::initializer_list<std::size_t> counts,
                                      const char* subject) {
  std::size_t total = 0;
  for (const std::size_t count : counts) {
    if (!sonare::numeric::checked_add(total, count, &total) || total > kMaxWasmFloat32Elements) {
      throw SonareException(ErrorCode::InvalidParameter,
                            std::string(subject) + " exceeds the WASM Float32 input budget");
    }
  }
}

std::size_t accumulateWasmFloat32ArrayLength(const val& arr, const char* array_subject,
                                             const char* budget_subject,
                                             std::size_t* cumulative_count) {
  if (cumulative_count == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(budget_subject) + " has no cumulative budget counter");
  }
  const std::size_t count = wasmFloat32ArrayLength(arr, array_subject);
  if (!sonare::numeric::checked_add(*cumulative_count, count, cumulative_count) ||
      *cumulative_count > kMaxWasmFloat32Elements) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(budget_subject) + " exceeds the WASM Float32 input budget");
  }
  return count;
}

void validateWasmFloat32ArrayPair(const val& first, const char* first_subject, const val& second,
                                  const char* second_subject, const char* budget_subject,
                                  bool require_matching_lengths) {
  std::size_t cumulative_count = 0;
  const std::size_t first_count =
      accumulateWasmFloat32ArrayLength(first, first_subject, budget_subject, &cumulative_count);
  const std::size_t second_count =
      accumulateWasmFloat32ArrayLength(second, second_subject, budget_subject, &cumulative_count);
  if (require_matching_lengths && first_count != second_count) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(budget_subject) + " channel lengths must match");
  }
}

std::vector<float> float32ArrayToVector(val arr) {
  const size_t n = wasmFloat32ArrayLength(arr);
  std::vector<float> result(n);
  if (n == 0) return result;
  // Build a Float32Array view onto the destination vector's storage. The view
  // is short-lived: we only keep it long enough to invoke set() before the
  // function returns and the view is dropped.
  val view = val(typed_memory_view(n, result.data()));
  view.call<void>("set", arr);
  return result;
}

Audio loadValidatedAudio(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  return Audio::from_buffer(data.data(), data.size(), sample_rate);
}

std::vector<float> float32ArrayWindowToVector(val arr, std::size_t start, std::size_t count) {
  std::vector<float> result(count);
  if (count == 0) return result;
  // Ask the source for the span rather than for everything: a typed array's
  // subarray is a view, so the only copy is the set() below, and slice on a
  // plain array copies the span and nothing else. An array-like offering
  // neither is copied whole and then narrowed -- the same answer, and the one
  // case that still costs the buffer's length rather than the window's.
  val source = val::undefined();
  const double first = static_cast<double>(start);
  const double last = static_cast<double>(start + count);
  if (arr["subarray"].typeOf().as<std::string>() == "function") {
    source = arr.call<val>("subarray", first, last);
  } else if (arr["slice"].typeOf().as<std::string>() == "function") {
    source = arr.call<val>("slice", first, last);
  } else {
    const std::vector<float> whole = float32ArrayToVector(arr);
    std::copy(whole.begin() + static_cast<std::ptrdiff_t>(start),
              whole.begin() + static_cast<std::ptrdiff_t>(start + count), result.begin());
    return result;
  }
  val view = val(typed_memory_view(count, result.data()));
  view.call<void>("set", source);
  return result;
}

Audio loadValidatedAudioWindow(val samples, int sample_rate, std::size_t scan_offset,
                               std::size_t scan_count) {
  const std::size_t length = wasmFloat32ArrayLength(samples);
  // The extent rules are asked about the whole buffer, as they are on the C ABI;
  // only the finiteness scan -- and the copy that feeds it -- narrows.
  validate_offline_audio_extent(length, sample_rate);
  const std::size_t begin = std::min(scan_offset, length);
  const std::size_t count = std::min(scan_count, length - begin);
  std::vector<float> window = float32ArrayWindowToVector(samples, begin, count);
  // An offset at or past the end leaves no sample to read, which the core
  // answers with a zero-padded frame rather than a refusal.
  if (count != 0) {
    validate_offline_audio_window(window.data(), count, sample_rate, 0, count);
  }
  return Audio::from_buffer(window.data(), window.size(), sample_rate);
}

std::vector<float> loadValidatedInterleaved(val samples, int channels, int sample_rate,
                                            size_t* frames) {
  if (channels <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "channels must be positive");
  }
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  if (data.size() % static_cast<size_t>(channels) != 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "interleaved sample count must be a whole number of frames");
  }
  if (frames != nullptr) *frames = data.size() / static_cast<size_t>(channels);
  return data;
}

// Int32 sibling of float32ArrayToVector. Used where a JS Int32Array carries
// integer sample indices (e.g. remix interval boundaries) that must not be
// round-tripped through float32 — values above 2^24 lose precision as float.
// The typed_memory_view<int32_t> wraps the destination vector's storage so the
// single boundary crossing (view.set(arr)) copies the raw 32-bit integers.
std::vector<int32_t> int32ArrayToVector(val arr) {
  const size_t n = wasmArrayLikeLength(arr, "Int32Array");
  std::vector<int32_t> result(n);
  if (n == 0) return result;
  val view = val(typed_memory_view(n, result.data()));
  view.call<void>("set", arr);
  return result;
}

std::vector<uint8_t> uint8ArrayToVector(val arr) {
  const size_t n = wasmArrayLikeLength(arr, "Uint8Array", "byteLength");
  std::vector<uint8_t> result(n);
  if (n == 0) return result;
  val view = val(typed_memory_view(n, result.data()));
  view.call<void>("set", arr);
  return result;
}

std::vector<mastering::api::Param> masteringParamsFromObject(
    val object, const std::vector<std::string>& skip_keys) {
  std::vector<mastering::api::Param> params;
  if (object.isNull() || object.isUndefined()) {
    return params;
  }
  val keys = val::global("Object").call<val>("keys", object);
  const int length = keys["length"].as<int>();
  params.reserve(static_cast<size_t>(length));
  for (int index = 0; index < length; ++index) {
    std::string key = keys[index].as<std::string>();
    if (std::find(skip_keys.begin(), skip_keys.end(), key) != skip_keys.end()) continue;
    val value = object[key];
    if (value.typeOf().as<std::string>() == "number") {
      params.push_back({key, value.as<double>()});
    } else if (value.typeOf().as<std::string>() == "boolean") {
      params.push_back({key, value.as<bool>() ? 1.0 : 0.0});
    } else {
      throw SonareException(ErrorCode::InvalidParameter,
                            "mastering override '" + key + "' must be a number or boolean");
    }
  }
  return params;
}

bool hasProperty(val object, const char* key) {
  if (object.isNull() || object.isUndefined()) {
    return false;
  }
  return !object[key].isUndefined() && !object[key].isNull();
}

val objectProperty(val object, const char* key) {
  if (!hasProperty(object, key)) {
    return val::undefined();
  }
  return object[key];
}

float floatProperty(val object, const char* key, float default_value) {
  val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : checkedFloatFromVal(value, key);
}

float floatOption(val object, const char* key, float default_value) {
  val value = objectProperty(object, key);
  if (value.isUndefined()) return default_value;
  const float number = value.as<float>();
  return std::isfinite(number) ? number : default_value;
}

int checkedIntFromVal(const val& value, const char* key) {
  // val::as<int>() SATURATES out of range, so 2^31, 2^40, 3e9 and 4294967295 all
  // arrive as INT_MAX and pass every downstream guard that only asks for a
  // positive value -- four distinct caller mistakes becoming one plausible
  // number. The positional embind path escapes this by accident rather than by
  // design: a declared int parameter WRAPS, so the same inputs land non-positive
  // and the existing guards reject them. Rejecting here is what makes the two
  // agree; widening the positional path to match the saturating one would look
  // consistent and remove the only range check this surface has.
  const double number = value.as<double>();
  if (!std::isfinite(number) || number < static_cast<double>(std::numeric_limits<int>::min()) ||
      number > static_cast<double>(std::numeric_limits<int>::max())) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        std::string(key) + " must be a finite number within the 32-bit integer range");
  }
  // The cast truncates, and truncation is the same class of silent value change
  // as the saturation above: 31.5 separates on 31, and anything in (-1, 0) lands
  // on the 0 that most of these fields read as "keep the default". Refusing here
  // rather than per facade is what reaches a caller driving the embind classes
  // directly, which no JS-side check can see.
  if (number != std::trunc(number)) {
    throw SonareException(ErrorCode::InvalidParameter, std::string(key) + " must be an integer");
  }
  return static_cast<int>(number);
}

int intProperty(val object, const char* key, int default_value) {
  val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : checkedIntFromVal(value, key);
}

namespace {

// Shared by the unsigned narrowings below: the range is the only thing that
// differs between them, and every one of them refuses the same two silent value
// changes -- a value outside the target type, and a fractional one.
double checkedUnsignedNumber(const val& value, const char* key, double max) {
  const double number = value.as<double>();
  if (!std::isfinite(number) || number < 0.0 || number > max) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(key) + " must be a finite number within [0, " +
                              std::to_string(static_cast<long long>(max)) + "]");
  }
  if (number != std::trunc(number)) {
    throw SonareException(ErrorCode::InvalidParameter, std::string(key) + " must be an integer");
  }
  return number;
}

}  // namespace

uint32_t checkedUintFromVal(const val& value, const char* key) {
  return static_cast<uint32_t>(
      checkedUnsignedNumber(value, key, static_cast<double>(std::numeric_limits<uint32_t>::max())));
}

uint32_t uintProperty(val object, const char* key, uint32_t default_value) {
  val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : checkedUintFromVal(value, key);
}

uint32_t checkedWordFromVal(const val& value, const char* key) {
  static constexpr double kSignedMin = -2147483648.0;   // -2^31
  static constexpr double kUnsignedMax = 4294967295.0;  // 2^32 - 1
  const double number = value.as<double>();
  if (!std::isfinite(number) || number < kSignedMin || number > kUnsignedMax) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(key) + " must be a finite 32-bit word value");
  }
  if (number != std::trunc(number)) {
    throw SonareException(ErrorCode::InvalidParameter, std::string(key) + " must be an integer");
  }
  return number < 0.0 ? static_cast<uint32_t>(static_cast<int64_t>(number))
                      : static_cast<uint32_t>(number);
}

uint32_t wordProperty(val object, const char* key, uint32_t default_value) {
  val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : checkedWordFromVal(value, key);
}

uint8_t checkedByteFromVal(const val& value, const char* key) {
  return static_cast<uint8_t>(
      checkedUnsignedNumber(value, key, static_cast<double>(std::numeric_limits<uint8_t>::max())));
}

uint8_t byteProperty(val object, const char* key, uint8_t default_value) {
  val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : checkedByteFromVal(value, key);
}

int64_t checkedInt64FromVal(const val& value, const char* key) {
  // Written as a power of two rather than as numeric_limits: INT64_MAX is not
  // representable as a double, and converting it rounds the bound up past the
  // values it is meant to exclude.
  static constexpr double kUpperBound = 9223372036854775808.0;  // 2^63
  const double number = value.as<double>();
  if (!std::isfinite(number) || number < -kUpperBound || number >= kUpperBound) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        std::string(key) + " must be a finite number within the 64-bit integer range");
  }
  if (number != std::trunc(number)) {
    throw SonareException(ErrorCode::InvalidParameter, std::string(key) + " must be an integer");
  }
  return static_cast<int64_t>(number);
}

int64_t int64Property(val object, const char* key, int64_t default_value) {
  val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : checkedInt64FromVal(value, key);
}

float checkedFloatFromVal(const val& value, const char* key) {
  const double number = value.as<double>();
  if (!std::isfinite(number) ||
      std::abs(number) > static_cast<double>(std::numeric_limits<float>::max())) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        std::string(key) + " must be a finite number within the 32-bit float range");
  }
  return static_cast<float>(number);
}

double checkedDoubleFromVal(const val& value, const char* key) {
  // No narrowing to do -- double is what a JS number already is -- so finiteness
  // is the whole check. Every field reading through this today is also checked
  // by the site that reads it; the check is here so a field added to one of
  // those bags is covered without someone having to repeat it.
  const double number = value.as<double>();
  if (!std::isfinite(number)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(key) + " must be a finite number");
  }
  return number;
}

double doubleProperty(val object, const char* key, double default_value) {
  val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : checkedDoubleFromVal(value, key);
}

int builtinWaveformFromVal(const val& value) {
  // One rejection for both spellings, matching the C ABI. The first invalid
  // ordinal is 4 -- what an off-by-one or a 1-based mirror emits -- so the
  // numeric path has to be checked at the edge of the range, not out at some
  // implausible number.
  static const char* kExpected = "' (expected sine, saw, sawtooth, square, or triangle)";
  const std::string type = value.typeOf().as<std::string>();
  if (type == "string") {
    const std::string name = value.as<std::string>();
    const int mapped = sonare_synth_builtin_waveform_from_name(name.c_str());
    if (mapped < 0) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "Unknown synth waveform: '" + name + kExpected);
    }
    return mapped;
  }
  // A wrong TYPE is out of domain too, and letting it through would put the two
  // surfaces back out of step: the addon's typed read rejects a boolean, while
  // val::as<double>() would coerce true to 1 and render a saw.
  if (type != "number") {
    throw SonareException(ErrorCode::InvalidParameter,
                          "Unknown synth waveform: '" + type + kExpected);
  }
  const int ordinal = checkedIntFromVal(value, "waveform");
  if (ordinal < SONARE_SYNTH_WAVEFORM_SINE || ordinal > SONARE_SYNTH_WAVEFORM_TRIANGLE) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "Unknown synth waveform: '" + std::to_string(ordinal) + kExpected);
  }
  return ordinal;
}

bool boolProperty(val object, const char* key, bool default_value) {
  val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : value.as<bool>();
}

std::string stringProperty(val object, const char* key, const std::string& default_value) {
  val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : value.as<std::string>();
}

std::optional<float> optionalNumber(const val& v) {
  if (v.isUndefined() || v.isNull() || v.typeOf().as<std::string>() != "number") {
    return std::nullopt;
  }
  return v.as<float>();
}

std::optional<bool> optionalBool(const val& v) {
  if (v.isUndefined() || v.isNull() || v.typeOf().as<std::string>() != "boolean") {
    return std::nullopt;
  }
  return v.as<bool>();
}

bool cancelCallbackRequested(const val& callback) {
  if (callback.isNull() || callback.isUndefined()) return false;
  const val outcome = callback();
  return outcome.typeOf().as<std::string>() == "boolean" && outcome.as<bool>();
}

int requireMatchedLength(const val& a, const val& b, const char* subject, bool require_non_zero) {
  const std::size_t a_length = wasmArrayLikeLength(a, subject);
  const std::size_t b_length = wasmArrayLikeLength(b, subject);
  if (a_length != b_length) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(subject) + " must have the same length");
  }
  if (require_non_zero && a_length == 0) {
    throw SonareException(ErrorCode::InvalidParameter, std::string(subject) + " must not be empty");
  }
  return static_cast<int>(a_length);
}

void requireOrdinalInRange(int value, int min, int max, const char* subject) {
  if (value < min || value > max) {
    throw SonareException(ErrorCode::InvalidParameter, std::string(subject) + " is out of range");
  }
}

#endif  // __EMSCRIPTEN__
