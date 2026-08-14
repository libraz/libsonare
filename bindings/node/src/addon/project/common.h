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

// BoolProperty / DoubleProperty / FloatProperty / Int64Property / IntProperty
// come from sonare_wrap_options.h (namespace sonare_node); re-exported here so
// the project TUs that `using namespace sonare_node::project` keep finding them.
using sonare_node::BoolProperty;
using sonare_node::DoubleProperty;
using sonare_node::FloatProperty;
using sonare_node::Int64Property;
using sonare_node::IntProperty;
using sonare_node::ThrowIfError;

}  // namespace sonare_node::project
