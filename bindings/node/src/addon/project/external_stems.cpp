#include <cstdint>
#include <string>
#include <vector>

#include "project/common.h"
#include "sonare_wrap_project.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::project;

namespace {

size_t channel_count(uint32_t layout) {
  if (layout == SONARE_EXTERNAL_STEM_MONO) return 1;
  if (layout == SONARE_EXTERNAL_STEM_STEREO) return 2;
  return 0;
}

}  // namespace

Napi::Value ProjectWrap::ImportExternalStems(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() != 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "importExternalStems expects a request object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const Napi::Object request = info[0].As<Napi::Object>();
  const Napi::Value stems_value = request.Get("stems");
  if (!stems_value.IsArray()) {
    Napi::TypeError::New(env, "importExternalStems request.stems must be an array")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const Napi::Array stem_values = stems_value.As<Napi::Array>();
  std::vector<SonareExternalStemDesc> descriptors;
  std::vector<std::string> names;
  std::vector<std::string> roles;
  std::vector<std::vector<Napi::Float32Array>> buffers;
  std::vector<std::vector<const float*>> planes;
  descriptors.reserve(stem_values.Length());
  names.reserve(stem_values.Length());
  roles.reserve(stem_values.Length());
  buffers.reserve(stem_values.Length());
  planes.reserve(stem_values.Length());

  for (uint32_t i = 0; i < stem_values.Length(); ++i) {
    const Napi::Value value = stem_values.Get(i);
    if (!value.IsObject()) {
      Napi::TypeError::New(env, "external stem must be an object").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    const Napi::Object stem = value.As<Napi::Object>();
    const Napi::Value name = stem.Get("name");
    const Napi::Value samples = stem.Get("planarSamples");
    if (!name.IsString() || !samples.IsArray()) {
      Napi::TypeError::New(env, "external stem needs name and planarSamples")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    const uint32_t layout = static_cast<uint32_t>(IntProperty(stem, "layout", 0));
    const size_t channels = channel_count(layout);
    const Napi::Array source_planes = samples.As<Napi::Array>();
    if (channels == 0 || source_planes.Length() != channels) {
      Napi::TypeError::New(env, "planarSamples must match mono or stereo layout")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    names.push_back(name.As<Napi::String>().Utf8Value());
    const Napi::Value role = stem.Get("role");
    if (role.IsString()) roles.push_back(role.As<Napi::String>().Utf8Value());
    buffers.emplace_back();
    planes.emplace_back();
    auto& stem_buffers = buffers.back();
    auto& stem_planes = planes.back();
    stem_buffers.reserve(channels);
    stem_planes.reserve(channels);
    size_t frame_count = 0;
    for (size_t channel = 0; channel < channels; ++channel) {
      const Napi::Value plane = source_planes.Get(static_cast<uint32_t>(channel));
      if (!sonare_node::IsFloat32Array(plane)) {
        Napi::TypeError::New(env, "each planarSamples entry must be a Float32Array")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      const Napi::Float32Array array = plane.As<Napi::Float32Array>();
      if (channel == 0) {
        frame_count = array.ElementLength();
      } else if (array.ElementLength() != frame_count) {
        Napi::TypeError::New(env, "all planarSamples entries must have equal length")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      stem_buffers.push_back(array);
      stem_planes.push_back(array.Data());
    }
    SonareExternalStemDesc desc{};
    desc.name = names.back().c_str();
    desc.role = role.IsString() ? roles.back().c_str() : nullptr;
    desc.layout = layout;
    desc.planar_samples = stem_planes.data();
    desc.frame_count = static_cast<int64_t>(frame_count);
    desc.start_frame = Int64Property(stem, "startFrame", 0);
    descriptors.push_back(desc);
  }

  const SonareExternalStemImportRequest request_desc{0, IntProperty(request, "sampleRate", 0),
                                                     descriptors.data(), descriptors.size()};
  SonareExternalStemImportResult result{};
  ThrowIfError(env, sonare_project_import_external_stems(project_, &request_desc, &result));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object output = Napi::Object::New(env);
  Napi::Array track_ids = Napi::Array::New(env, result.count);
  Napi::Array clip_ids = Napi::Array::New(env, result.count);
  for (size_t i = 0; i < result.count; ++i) {
    track_ids.Set(static_cast<uint32_t>(i), Napi::Number::New(env, result.track_ids[i]));
    clip_ids.Set(static_cast<uint32_t>(i), Napi::Number::New(env, result.clip_ids[i]));
  }
  sonare_free_external_stem_import_result(&result);
  output.Set("trackIds", track_ids);
  output.Set("clipIds", clip_ids);
  return output;
}
