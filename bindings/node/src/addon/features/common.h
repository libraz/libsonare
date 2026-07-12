#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "sonare_wrap.h"
#include "sonare_wrap_utils.h"

namespace sonare_node::features {

inline Napi::Float32Array FloatResult(Napi::Env env, float* data, size_t count) {
  auto out = Napi::Float32Array::New(env, count);
  if (count > 0 && data != nullptr) {
    std::memcpy(out.Data(), data, count * sizeof(float));
  }
  sonare_free_floats(data);
  return out;
}

inline Napi::Int32Array IntResult(Napi::Env env, int* data, size_t count) {
  auto out = Napi::Int32Array::New(env, count);
  if (count > 0 && data != nullptr) {
    std::memcpy(out.Data(), data, count * sizeof(int));
  }
  sonare_free_ints(data);
  return out;
}

inline Napi::Value CheckCResult(Napi::Env env, SonareError err) {
  sonare_node::ThrowIfError(env, err);
  return env.Undefined();
}

// IntVectorFromValue / FloatVectorFromValue come from sonare_wrap_utils.h
// (namespace sonare_node); the features TUs `using namespace sonare_node`, so
// the unqualified names still resolve.
using sonare_node::FloatVectorFromValue;
using sonare_node::IntVectorFromValue;

inline int TempogramModeFromValue(const Napi::Value& value) {
  if (value.IsUndefined() || value.IsNull()) return SONARE_TEMPOGRAM_AUTOCORRELATION;
  if (value.IsNumber()) {
    const int mode = value.As<Napi::Number>().Int32Value();
    if (mode == SONARE_TEMPOGRAM_AUTOCORRELATION || mode == SONARE_TEMPOGRAM_COSINE) return mode;
  }
  if (value.IsString()) {
    const std::string mode = value.As<Napi::String>().Utf8Value();
    if (mode == "autocorrelation" || mode == "auto" || mode == "ac") {
      return SONARE_TEMPOGRAM_AUTOCORRELATION;
    }
    if (mode == "cosine") return SONARE_TEMPOGRAM_COSINE;
  }
  throw Napi::TypeError::New(value.Env(), "Expected tempogram mode 'autocorrelation' or 'cosine'");
}

}  // namespace sonare_node::features
