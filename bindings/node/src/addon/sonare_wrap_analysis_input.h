#ifndef SONARE_NODE_SONARE_WRAP_ANALYSIS_INPUT_H_
#define SONARE_NODE_SONARE_WRAP_ANALYSIS_INPUT_H_

// The (samples, sampleRate?) leading arguments of the mono analysis entry points.

#include <napi.h>

#include <cstddef>

#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {

struct MonoAnalysisInput {
  Napi::Float32Array samples;
  const float* data = nullptr;
  size_t length = 0;
  int sample_rate = 0;
};

inline bool ReadMonoAnalysisInput(const Napi::CallbackInfo& info, MonoAnalysisInput* out,
                                  int default_sample_rate) {
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return false;
  out->samples = info[0].As<Napi::Float32Array>();
  out->data = out->samples.Data();
  out->length = out->samples.ElementLength();
  if (!OptionalIntArg(info.Env(), info, 1, "sampleRate", default_sample_rate, &out->sample_rate))
    return false;
  return true;
}

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_ANALYSIS_INPUT_H_
