#pragma once

#include <cstdint>
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

inline void ThrowIfError(Napi::Env env, SonareError err) {
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
  }
}

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

// BoolProperty / DoubleProperty / FloatProperty / Int64Property / IntProperty
// come from sonare_wrap_options.h (namespace sonare_node); re-exported here so
// the project TUs that `using namespace sonare_node::project` keep finding them.
using sonare_node::BoolProperty;
using sonare_node::DoubleProperty;
using sonare_node::FloatProperty;
using sonare_node::Int64Property;
using sonare_node::IntProperty;

}  // namespace sonare_node::project
