#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"
#include "util/constants.h"
namespace sonare_node::features {

/// @brief The common sample-rate and frame geometry arguments used by feature calls.
struct FrameAnalysisArgs {
  int sample_rate{};
  int frame_length{};
  int hop_length{};
};

/// @brief Read a feature call's sample rate, frame size, and hop length.
/// @details The frame argument is named by the public entry point: STFT-based
///   functions use @c nFft, while frame-statistics functions use @c frameLength.
///   Reading the fields in one place keeps their defaults and strict narrowing
///   aligned without changing the order in which argument errors are reported.
inline bool ReadFrameAnalysisArgs(Napi::Env env, const Napi::CallbackInfo& info,
                                  FrameAnalysisArgs* out, const char* frame_name = "nFft") {
  if (!OptionalIntArg(env, info, 1, "sampleRate", sonare::constants::kDefaultSampleRate,
                      &out->sample_rate))
    return false;
  if (!OptionalIntArg(env, info, 2, frame_name, sonare::constants::kDefaultNFft,
                      &out->frame_length))
    return false;
  if (!OptionalIntArg(env, info, 3, "hopLength", sonare::constants::kDefaultHopLength,
                      &out->hop_length))
    return false;
  return true;
}

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
    const int mode = sonare_node::node_narrow_int(value.Env(), value, "mode");
    if (mode == SONARE_TEMPOGRAM_AUTOCORRELATION || mode == SONARE_TEMPOGRAM_COSINE) return mode;
  }
  if (value.IsString()) {
    const std::string mode = sonare_node::node_narrow_string(value.Env(), value, "mode");
    if (mode == "autocorrelation" || mode == "auto" || mode == "ac") {
      return SONARE_TEMPOGRAM_AUTOCORRELATION;
    }
    if (mode == "cosine") return SONARE_TEMPOGRAM_COSINE;
  }
  throw Napi::TypeError::New(value.Env(), "Expected tempogram mode 'autocorrelation' or 'cosine'");
}

}  // namespace sonare_node::features
