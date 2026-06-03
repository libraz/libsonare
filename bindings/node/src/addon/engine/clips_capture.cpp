#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "engine/common.h"
#include "sonare_wrap_engine.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::engine;

Napi::Value RealtimeEngineWrap::SetClips(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "expected an array of clips").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array input = info[0].As<Napi::Array>();
  std::vector<std::vector<std::vector<float>>> storage;
  std::vector<std::vector<const float*>> ptr_storage;
  std::vector<SonareEngineClip> clips;
  storage.reserve(input.Length());
  ptr_storage.reserve(input.Length());
  clips.reserve(input.Length());

  for (uint32_t i = 0; i < input.Length(); ++i) {
    Napi::Object obj = input.Get(i).As<Napi::Object>();
    Napi::Array channels = obj.Get("channels").As<Napi::Array>();
    if (channels.Length() == 0) {
      Napi::TypeError::New(env, "clip channels must not be empty").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    storage.emplace_back();
    ptr_storage.emplace_back();
    auto& clip_storage = storage.back();
    auto& clip_ptrs = ptr_storage.back();
    clip_storage.reserve(channels.Length());
    clip_ptrs.reserve(channels.Length());
    size_t num_samples = 0;
    for (uint32_t ch = 0; ch < channels.Length(); ++ch) {
      Napi::Value value = channels.Get(ch);
      if (!sonare_node::IsFloat32Array(value)) {
        Napi::TypeError::New(env, "clip channel must be a Float32Array")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      Napi::Float32Array channel = value.As<Napi::Float32Array>();
      if (ch == 0) {
        num_samples = channel.ElementLength();
      } else if (channel.ElementLength() != num_samples) {
        Napi::TypeError::New(env, "all clip channels must have the same length")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      clip_storage.emplace_back(channel.Data(), channel.Data() + channel.ElementLength());
      clip_ptrs.push_back(clip_storage.back().data());
    }

    SonareEngineClip clip{};
    clip.id = obj.Get("id").As<Napi::Number>().Uint32Value();
    clip.channels = clip_ptrs.data();
    clip.num_channels = static_cast<int>(clip_ptrs.size());
    clip.num_samples = static_cast<int64_t>(num_samples);
    clip.start_ppq = obj.Get("startPpq").As<Napi::Number>().DoubleValue();
    clip.clip_offset_samples =
        obj.Has("clipOffsetSamples") && !obj.Get("clipOffsetSamples").IsUndefined()
            ? obj.Get("clipOffsetSamples").As<Napi::Number>().Int64Value()
            : 0;
    clip.length_samples = obj.Has("lengthSamples") && !obj.Get("lengthSamples").IsUndefined()
                              ? obj.Get("lengthSamples").As<Napi::Number>().Int64Value()
                              : static_cast<int64_t>(num_samples);
    clip.loop = obj.Has("loop") && !obj.Get("loop").IsUndefined()
                    ? (obj.Get("loop").As<Napi::Boolean>().Value() ? 1 : 0)
                    : 0;
    clip.gain = obj.Has("gain") && !obj.Get("gain").IsUndefined()
                    ? obj.Get("gain").As<Napi::Number>().FloatValue()
                    : 1.0f;
    clip.fade_in_samples = obj.Has("fadeInSamples") && !obj.Get("fadeInSamples").IsUndefined()
                               ? obj.Get("fadeInSamples").As<Napi::Number>().Int64Value()
                               : 0;
    clip.fade_out_samples = obj.Has("fadeOutSamples") && !obj.Get("fadeOutSamples").IsUndefined()
                                ? obj.Get("fadeOutSamples").As<Napi::Number>().Int64Value()
                                : 0;
    clips.push_back(clip);
  }

  ThrowIfError(env, sonare_engine_set_clips(engine_, clips.data(), clips.size()));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::ClipCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t count = 0;
  ThrowIfError(env, sonare_engine_clip_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Value RealtimeEngineWrap::SetCaptureBuffer(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "expected an array of Float32Array channels")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array channels = info[0].As<Napi::Array>();
  if (channels.Length() == 0) {
    Napi::TypeError::New(env, "capture channels must not be empty").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::vector<Napi::Reference<Napi::Float32Array>> refs;
  std::vector<float*> ptrs;
  refs.reserve(channels.Length());
  ptrs.reserve(channels.Length());
  int64_t frames = 0;
  for (uint32_t ch = 0; ch < channels.Length(); ++ch) {
    Napi::Value value = channels.Get(ch);
    if (!sonare_node::IsFloat32Array(value)) {
      Napi::TypeError::New(env, "capture channel must be a Float32Array")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Float32Array channel = value.As<Napi::Float32Array>();
    if (ch == 0) {
      frames = static_cast<int64_t>(channel.ElementLength());
      if (frames <= 0) {
        Napi::TypeError::New(env, "capture channels must not be empty")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
    } else if (static_cast<int64_t>(channel.ElementLength()) != frames) {
      Napi::TypeError::New(env, "all capture channels must have the same length")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    refs.push_back(Napi::Persistent(channel));
    ptrs.push_back(channel.Data());
  }

  SonareEngineCaptureBuffer buffer{};
  buffer.channels = ptrs.data();
  buffer.num_channels = static_cast<int>(ptrs.size());
  buffer.capacity_frames = frames;
  ThrowIfError(env, sonare_engine_set_capture_buffer(engine_, &buffer));
  if (env.IsExceptionPending()) return env.Undefined();
  capture_refs_ = std::move(refs);
  capture_ptrs_ = std::move(ptrs);
  capture_capacity_frames_ = frames;
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::ArmCapture(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const bool armed =
      info.Length() <= 0 || info[0].IsUndefined() ? true : info[0].As<Napi::Boolean>().Value();
  ThrowIfError(env, sonare_engine_arm_capture(engine_, armed ? 1 : 0));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetCapturePunch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int64_t start_sample = OptionalInt64(info, 0, 0);
  const int64_t end_sample = OptionalInt64(info, 1, 0);
  const bool enabled =
      info.Length() <= 2 || info[2].IsUndefined() ? true : info[2].As<Napi::Boolean>().Value();
  ThrowIfError(env,
               sonare_engine_set_capture_punch(engine_, start_sample, end_sample, enabled ? 1 : 0));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::ResetCapture(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_engine_reset_capture(engine_));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::CaptureStatus(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareEngineCaptureStatus status{};
  ThrowIfError(env, sonare_engine_capture_status(engine_, &status));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("capturedFrames", Napi::Number::New(env, static_cast<double>(status.captured_frames)));
  out.Set("overflowCount", Napi::Number::New(env, status.overflow_count));
  out.Set("armed", Napi::Boolean::New(env, status.armed != 0));
  out.Set("punchEnabled", Napi::Boolean::New(env, status.punch_enabled != 0));
  return out;
}

Napi::Value RealtimeEngineWrap::CapturedAudio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (engine_ == nullptr) {
    Napi::Error::New(env, "RealtimeEngine is destroyed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareEngineCaptureStatus status{};
  ThrowIfError(env, sonare_engine_capture_status(engine_, &status));
  if (env.IsExceptionPending()) return env.Undefined();

  // Clamp the captured frame count to the JS-supplied buffer capacity so that we
  // never read past the Float32Arrays handed to setCaptureBuffer().
  int64_t frames = status.captured_frames;
  if (frames < 0) frames = 0;
  if (frames > capture_capacity_frames_) frames = capture_capacity_frames_;

  Napi::Array out = Napi::Array::New(env, capture_refs_.size());
  for (size_t ch = 0; ch < capture_refs_.size(); ++ch) {
    Napi::Float32Array source = capture_refs_[ch].Value();
    const size_t count = static_cast<size_t>(frames);
    auto channel = Napi::Float32Array::New(env, count);
    if (count > 0) {
      std::memcpy(channel.Data(), source.Data(), count * sizeof(float));
    }
    out.Set(static_cast<uint32_t>(ch), channel);
  }
  return out;
}
