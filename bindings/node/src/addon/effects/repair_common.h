#ifndef SONARE_NODE_EFFECTS_REPAIR_COMMON_H_
#define SONARE_NODE_EFFECTS_REPAIR_COMMON_H_

/// @file
/// @brief Argument checks and ownership helpers shared by effect entry points.
/// @details These leave internal linkage only because the repair families
///   live in separate translation units; nothing outside effects/ uses them.

#include <napi.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "core/audio.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

namespace sonare_node::repair_detail {

/// @brief The C releaser of each stereo repair result, picked by overload.
inline void FreeStereoResult(SonareDeclickStereoResult* r) { sonare_free_declick_stereo_result(r); }
inline void FreeStereoResult(SonareDenoiseStereoResult* r) { sonare_free_denoise_stereo_result(r); }
inline void FreeStereoResult(SonareDeclipStereoResult* r) { sonare_free_declip_stereo_result(r); }
inline void FreeStereoResult(SonareDecrackleStereoResult* r) {
  sonare_free_decrackle_stereo_result(r);
}
inline void FreeLinkedResult(SonareDenoiseLinkedResult* r) { sonare_free_denoise_linked_result(r); }
inline void FreeLinkedResult(SonareDereverbLinkedResult* r) {
  sonare_free_dereverb_linked_result(r);
}

/// @brief Releases a linked result when it leaves scope.
template <typename Result>
class LinkedResultGuard {
 public:
  explicit LinkedResultGuard(Result* result) : result_(result) {}
  LinkedResultGuard(const LinkedResultGuard&) = delete;
  LinkedResultGuard& operator=(const LinkedResultGuard&) = delete;
  ~LinkedResultGuard() { FreeLinkedResult(result_); }

 private:
  Result* result_;
};

inline void FreeStereoResult(SonareDehumStereoResult* r) { sonare_free_dehum_stereo_result(r); }
inline void FreeStereoResult(SonareDereverbStereoResult* r) {
  sonare_free_dereverb_stereo_result(r);
}
inline void FreeStereoResult(SonareTrimSilenceStereoResult* r) {
  sonare_free_trim_silence_stereo_result(r);
}

/// @brief Releases a stereo repair result through its C releaser.
template <typename Result>
class StereoResultGuard {
 public:
  explicit StereoResultGuard(Result* result) : result_(result) {}
  StereoResultGuard(const StereoResultGuard&) = delete;
  StereoResultGuard& operator=(const StereoResultGuard&) = delete;
  ~StereoResultGuard() { FreeStereoResult(result_); }

 private:
  Result* result_;
};

/// @brief Copy the two heap-owned channels into the common `{ left, right }` result shape.
/// @details Repair-specific reports and ranges are appended by each caller after this common
///   channel copy. Keeping the copy here makes the result length and null/empty handling identical
///   across every stereo repair entry point.
template <typename Result>
inline Napi::Object EmitStereoResult(Napi::Env env, const Result& result) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", Float32ArrayFrom(env, result.left, result.length));
  out.Set("right", Float32ArrayFrom(env, result.right, result.length));
  return out;
}

/// @brief Pack a mono repair result into the common `{ samples, report }` shape.
inline Napi::Object EmitMonoResult(Napi::Env env, const sonare::Audio& audio,
                                   const Napi::Object& report) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", Float32ArrayFrom(env, audio.data(), audio.size()));
  out.Set("report", report);
  return out;
}

/// @brief Input and output planes of one channel-linked repair call.
/// @details The linked C entries write caller-owned output planes in place, so
///   the Float32Arrays handed back to JS are allocated up front and passed in
///   rather than copied out of a library allocation.
struct LinkedPlanes {
  ChannelInputs in;
  std::vector<Napi::Float32Array> outputs;
  std::vector<float*> out_ptrs;
};

/// @brief Reads the channels through ReadChannelInputs and allocates the matching output planes.
inline bool ReadLinkedPlanes(Napi::Env env, const Napi::Value& value, LinkedPlanes* planes) {
  if (!ReadChannelInputs(env, value, &planes->in)) return false;
  const size_t count = planes->in.arrays.size();
  planes->outputs.reserve(count);
  planes->out_ptrs.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    planes->outputs.push_back(Napi::Float32Array::New(env, planes->in.length));
    planes->out_ptrs.push_back(planes->outputs.back().Data());
  }
  return true;
}

/// @brief Packs the output planes into a JS array, in input order.
inline Napi::Array EmitLinkedChannels(Napi::Env env, const LinkedPlanes& planes) {
  Napi::Array out = Napi::Array::New(env, planes.outputs.size());
  for (size_t index = 0; index < planes.outputs.size(); ++index) {
    out.Set(static_cast<uint32_t>(index), planes.outputs[index]);
  }
  return out;
}

/// @brief Packs the planes of a library-allocated linked result into JS arrays, in channel order.
inline Napi::Array EmitLinkedResultChannels(Napi::Env env, float** channels, size_t num_channels,
                                            size_t length) {
  return Float32ArraysFrom(env, channels, num_channels, length);
}

/// @brief Argument check shared by the channel-linked entries, which both take
///        (Float32Array[], sampleRate, options?).
inline bool CheckLinkedArgs(Napi::Env env, const Napi::CallbackInfo& info) {
  Napi::Array channels;
  if (!RequiredPlaneListValue(env, info[0], "channels", &channels)) return false;
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

}  // namespace sonare_node::repair_detail

#endif  // SONARE_NODE_EFFECTS_REPAIR_COMMON_H_
