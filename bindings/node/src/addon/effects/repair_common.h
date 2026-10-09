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

#include "sonare_wrap_utils.h"

namespace sonare_node::repair_detail {

/// @brief Argument check shared by the mono entries, which all take
///        (Float32Array, sampleRate, options?).
inline bool CheckMonoArgs(Napi::Env env, const Napi::CallbackInfo& info) {
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

/// @brief Frees the two heap-owned channels of a stereo repair result.
template <typename Result>
class StereoResultGuard {
 public:
  explicit StereoResultGuard(Result* result) : result_(result) {}
  StereoResultGuard(const StereoResultGuard&) = delete;
  StereoResultGuard& operator=(const StereoResultGuard&) = delete;
  ~StereoResultGuard() {
    sonare_free_floats(result_->left);
    sonare_free_floats(result_->right);
  }

 private:
  Result* result_;
};

/// @brief Copy the two heap-owned channels into the common `{ left, right }` result shape.
/// @details Repair-specific reports and ranges are appended by each caller after this common
///   channel copy. Keeping the copy here makes the result length and null/empty handling identical
///   across every stereo repair entry point.
template <typename Result>
inline Napi::Object EmitStereoResult(Napi::Env env, const Result& result) {
  auto left = Napi::Float32Array::New(env, result.length);
  auto right = Napi::Float32Array::New(env, result.length);
  if (result.length > 0) {
    std::memcpy(left.Data(), result.left, result.length * sizeof(float));
    std::memcpy(right.Data(), result.right, result.length * sizeof(float));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left);
  out.Set("right", right);
  return out;
}

/// @brief Input and output planes of one channel-linked repair call.
/// @details The linked C entries write caller-owned output planes in place, so
///   the Float32Arrays handed back to JS are allocated up front and passed in
///   rather than copied out of a library allocation.
struct LinkedPlanes {
  std::vector<Napi::Float32Array> inputs;
  std::vector<const float*> in_ptrs;
  std::vector<Napi::Float32Array> outputs;
  std::vector<float*> out_ptrs;
  size_t length = 0;
};

/// @brief Reads N Float32Array channels and allocates the matching output planes.
/// @details Refuses what the C form cannot express: a non-Float32Array element,
///   and a length disagreement, which the single @c length argument has no way to
///   carry. An empty list goes through to the C entry, which owns that rejection.
inline bool ReadLinkedPlanes(Napi::Env env, const Napi::Value& value, const char* fn_name,
                             LinkedPlanes* planes) {
  Napi::Array channels = value.As<Napi::Array>();
  const size_t count = channels.Length();
  planes->inputs.reserve(count);
  planes->in_ptrs.reserve(count);
  planes->outputs.reserve(count);
  planes->out_ptrs.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    Napi::Value channel = channels.Get(index);
    if (!IsFloat32Array(channel)) {
      Napi::TypeError::New(env, std::string(fn_name) + ": every channel must be a Float32Array")
          .ThrowAsJavaScriptException();
      return false;
    }
    planes->inputs.push_back(channel.As<Napi::Float32Array>());
    const size_t length = planes->inputs.back().ElementLength();
    if (index == 0) {
      planes->length = length;
    } else if (length != planes->length) {
      Napi::RangeError::New(env, std::string(fn_name) + ": every channel must have the same length")
          .ThrowAsJavaScriptException();
      return false;
    }
    planes->in_ptrs.push_back(planes->inputs.back().Data());
    planes->outputs.push_back(Napi::Float32Array::New(env, planes->length));
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

/// @brief Argument check shared by the channel-linked entries, which both take
///        (Float32Array[], sampleRate, options?).
inline bool CheckLinkedArgs(Napi::Env env, const Napi::CallbackInfo& info) {
  if (info.Length() < 2 || !info[0].IsArray() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array[] channels, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

}  // namespace sonare_node::repair_detail

#endif  // SONARE_NODE_EFFECTS_REPAIR_COMMON_H_
