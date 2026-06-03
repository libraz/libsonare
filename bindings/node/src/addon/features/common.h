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
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

inline std::vector<int> IntVectorFromValue(const Napi::Value& value) {
  if (value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_int32_array) {
    auto arr = value.As<Napi::Int32Array>();
    return std::vector<int>(arr.Data(), arr.Data() + arr.ElementLength());
  }
  if (value.IsArray()) {
    auto arr = value.As<Napi::Array>();
    std::vector<int> out(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      out[i] = arr.Get(i).As<Napi::Number>().Int32Value();
    }
    return out;
  }
  throw Napi::TypeError::New(value.Env(), "Expected Int32Array or number[]");
}

inline std::vector<float> FloatVectorFromValue(const Napi::Value& value) {
  if (value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_float32_array) {
    auto arr = value.As<Napi::Float32Array>();
    return std::vector<float>(arr.Data(), arr.Data() + arr.ElementLength());
  }
  if (value.IsArray()) {
    auto arr = value.As<Napi::Array>();
    std::vector<float> out(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      out[i] = arr.Get(i).As<Napi::Number>().FloatValue();
    }
    return out;
  }
  throw Napi::TypeError::New(value.Env(), "Expected Float32Array or number[]");
}

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
