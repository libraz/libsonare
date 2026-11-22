#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "core/spectrum.h"
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

/// @brief Read a feature call's sample rate, frame size, and hop length, the frame and hop
///        against the core's domains for that call.
/// @details The frame argument is named by the public entry point: STFT-based
///   functions use @c nFft, while frame-statistics functions use @c frameLength.
inline bool ReadFrameAnalysisArgs(Napi::Env env, const Napi::CallbackInfo& info,
                                  FrameAnalysisArgs* out,
                                  const sonare::util::check::NumericDomain& frame_domain,
                                  const sonare::util::check::NumericDomain& hop_domain,
                                  const char* frame_name = "nFft") {
  if (!OptionalIntArg(env, info, 1, "sampleRate", sonare::constants::kDefaultSampleRate,
                      &out->sample_rate))
    return false;
  if (!OptionalDomainArg(env, info, 2, frame_name, sonare::constants::kDefaultNFft, frame_domain,
                         &out->frame_length))
    return false;
  return OptionalDomainArg(env, info, 3, "hopLength", sonare::constants::kDefaultHopLength,
                           hop_domain, &out->hop_length);
}

/// @brief Read an STFT-backed call's sample rate, nFft and hop length.
/// @details nFft is read for its type only: the core's STFT check states its whole rule (an even
///   size within the STFT bound), which a domain read here would state without the evenness.
inline bool ReadStftFrameArgs(Napi::Env env, const Napi::CallbackInfo& info,
                              FrameAnalysisArgs* out) {
  if (!OptionalIntArg(env, info, 1, "sampleRate", sonare::constants::kDefaultSampleRate,
                      &out->sample_rate))
    return false;
  if (!OptionalIntArg(env, info, 2, "nFft", sonare::constants::kDefaultNFft, &out->frame_length))
    return false;
  return OptionalDomainArg(env, info, 3, "hopLength", sonare::constants::kDefaultHopLength,
                           sonare::kStftHopDomain, &out->hop_length);
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
  throw Napi::TypeError::New(value.Env(), "mode must be 'autocorrelation' or 'cosine'");
}

}  // namespace sonare_node::features
