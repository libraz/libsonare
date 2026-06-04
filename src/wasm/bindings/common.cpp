/// @file common.cpp
/// @brief Shared helpers for the Embind translation units.

#ifdef __EMSCRIPTEN__

#include "common.h"

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
std::vector<float> float32ArrayToVector(val arr) {
  const size_t n = arr["length"].as<size_t>();
  std::vector<float> result(n);
  if (n == 0) return result;
  // Build a Float32Array view onto the destination vector's storage. The view
  // is short-lived: we only keep it long enough to invoke set() before the
  // function returns and the view is dropped.
  val view = val(typed_memory_view(n, result.data()));
  view.call<void>("set", arr);
  return result;
}

// Int32 sibling of float32ArrayToVector. Used where a JS Int32Array carries
// integer sample indices (e.g. remix interval boundaries) that must not be
// round-tripped through float32 — values above 2^24 lose precision as float.
// The typed_memory_view<int32_t> wraps the destination vector's storage so the
// single boundary crossing (view.set(arr)) copies the raw 32-bit integers.
std::vector<int32_t> int32ArrayToVector(val arr) {
  const size_t n = arr["length"].as<size_t>();
  std::vector<int32_t> result(n);
  if (n == 0) return result;
  val view = val(typed_memory_view(n, result.data()));
  view.call<void>("set", arr);
  return result;
}

std::vector<uint8_t> uint8ArrayToVector(val arr) {
  const size_t n = arr["byteLength"].as<size_t>();
  std::vector<uint8_t> result(n);
  if (n == 0) return result;
  val view = val(typed_memory_view(n, result.data()));
  view.call<void>("set", arr);
  return result;
}

std::vector<mastering::api::Param> masteringParamsFromObject(val object) {
  std::vector<mastering::api::Param> params;
  if (object.isNull() || object.isUndefined()) {
    return params;
  }
  val keys = val::global("Object").call<val>("keys", object);
  const int length = keys["length"].as<int>();
  params.reserve(static_cast<size_t>(length));
  for (int index = 0; index < length; ++index) {
    std::string key = keys[index].as<std::string>();
    val value = object[key];
    if (value.typeOf().as<std::string>() == "number") {
      params.push_back({key, value.as<double>()});
    } else if (value.typeOf().as<std::string>() == "boolean") {
      params.push_back({key, value.as<bool>() ? 1.0 : 0.0});
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
  return value.isUndefined() ? default_value : value.as<float>();
}

int intProperty(val object, const char* key, int default_value) {
  val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : value.as<int>();
}

bool boolProperty(val object, const char* key, bool default_value) {
  val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : value.as<bool>();
}

std::string stringProperty(val object, const char* key, const std::string& default_value) {
  val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : value.as<std::string>();
}

#endif  // __EMSCRIPTEN__
