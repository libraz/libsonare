#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "sonare_wrap_options.h"
#include "sonare_wrap_project.h"
#include "sonare_wrap_utils.h"

namespace sonare_node::project {

// Derived from the canonical C macro (via sonare_c.h) so this addon can never
// drift from SONARE_PROJECT_ABI_VERSION. A runtime mismatch means the loaded
// native binary lays out the flat project PODs differently than this addon
// expects (or arrangement support was compiled out -> runtime version 0).
constexpr uint32_t kExpectedProjectAbiVersion = SONARE_PROJECT_ABI_VERSION;

inline double NumberArg(const Napi::CallbackInfo& info, size_t index, double fallback) {
  if (info.Length() <= index || info[index].IsUndefined()) {
    return fallback;
  }
  return info[index].As<Napi::Number>().DoubleValue();
}

inline uint32_t Uint32Arg(const Napi::CallbackInfo& info, size_t index, uint32_t fallback) {
  if (info.Length() <= index || info[index].IsUndefined()) {
    return fallback;
  }
  return info[index].As<Napi::Number>().Uint32Value();
}

/// @brief Read an optional positional argument as an int, rejecting anything a
///        float-to-integer cast cannot represent. An absent, undefined or null
///        argument reads as @p fallback; a non-number is a TypeError and a
///        non-finite, fractional or out-of-int-range number a RangeError. Both
///        rejections leave @p out untouched and return false.
///
/// @p NumberArg is the double-valued reader this narrows: casting its result to
/// int is undefined behaviour for NaN, an infinity, or a magnitude past INT_MAX,
/// so an argument headed for an int C-ABI parameter is checked here first.
inline bool Int32Arg(Napi::Env env, const Napi::CallbackInfo& info, size_t index, const char* name,
                     int fallback, int* out) {
  if (env.IsExceptionPending() || out == nullptr) return false;
  if (info.Length() <= index || info[index].IsUndefined() || info[index].IsNull()) {
    *out = fallback;
    return true;
  }
  if (!info[index].IsNumber()) {
    Napi::TypeError::New(env, std::string(name) + " must be a number").ThrowAsJavaScriptException();
    return false;
  }

  const double value = info[index].As<Napi::Number>().DoubleValue();
  constexpr double kMinInt = static_cast<double>(std::numeric_limits<int>::min());
  constexpr double kMaxInt = static_cast<double>(std::numeric_limits<int>::max());
  if (!std::isfinite(value) || std::trunc(value) != value || value < kMinInt || value > kMaxInt) {
    Napi::RangeError::New(
        env, std::string(name) + " must be a finite integer within the native int range")
        .ThrowAsJavaScriptException();
    return false;
  }

  *out = static_cast<int>(value);
  return true;
}

// The property readers, the required-field readers and NonNegativeSizeTArg come
// from sonare_wrap_options.h (namespace sonare_node); re-exported here so the
// project TUs that `using namespace sonare_node::project` keep finding them.
using sonare_node::BoolProperty;
using sonare_node::DoubleProperty;
using sonare_node::FloatProperty;
using sonare_node::Int64Property;
using sonare_node::IntProperty;
using sonare_node::NonNegativeSizeTArg;
using sonare_node::OptionalMidiByteArg;
using sonare_node::RequiredDoubleProperty;
using sonare_node::RequiredDoubleValue;
using sonare_node::RequiredFloatProperty;
using sonare_node::RequiredIntProperty;
using sonare_node::RequiredStringProperty;
using sonare_node::RequiredUint32Property;
using sonare_node::RequiredUint32Value;
using sonare_node::RequireNumberValue;
using sonare_node::ThrowIfError;
using sonare_node::Uint32Property;

}  // namespace sonare_node::project
