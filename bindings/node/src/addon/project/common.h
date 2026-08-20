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

// The property readers, the required-field readers and NonNegativeSizeTArg come
// from sonare_wrap_options.h (namespace sonare_node); re-exported here so the
// project TUs that `using namespace sonare_node::project` keep finding them.
using sonare_node::BoolProperty;
using sonare_node::DoubleProperty;
using sonare_node::FloatProperty;
using sonare_node::Int64Property;
using sonare_node::IntProperty;
using sonare_node::NonNegativeSizeTArg;
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
