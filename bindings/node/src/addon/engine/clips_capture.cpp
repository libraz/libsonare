#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "engine/common.h"
#include "sonare_wrap_engine.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::engine;

namespace {

SonareEngineCaptureSource ParseCaptureSource(Napi::Env env, const Napi::Value& value) {
  if (value.IsString()) {
    const std::string source = value.As<Napi::String>().Utf8Value();
    if (source == "output") return SONARE_ENGINE_CAPTURE_SOURCE_OUTPUT;
    if (source == "input") return SONARE_ENGINE_CAPTURE_SOURCE_INPUT;
  } else if (value.IsNumber()) {
    const int source = value.As<Napi::Number>().Int32Value();
    if (source == SONARE_ENGINE_CAPTURE_SOURCE_OUTPUT) return SONARE_ENGINE_CAPTURE_SOURCE_OUTPUT;
    if (source == SONARE_ENGINE_CAPTURE_SOURCE_INPUT) return SONARE_ENGINE_CAPTURE_SOURCE_INPUT;
  }
  Napi::TypeError::New(env, "capture source must be 'output' or 'input'")
      .ThrowAsJavaScriptException();
  return SONARE_ENGINE_CAPTURE_SOURCE_OUTPUT;
}

int ParseWarpMode(Napi::Env env, const Napi::Value& value) {
  if (value.IsUndefined() || value.IsNull()) return SONARE_ENGINE_WARP_MODE_OFF;
  if (value.IsString()) {
    const std::string mode = value.As<Napi::String>().Utf8Value();
    if (mode == "off") return SONARE_ENGINE_WARP_MODE_OFF;
    if (mode == "repitch") return SONARE_ENGINE_WARP_MODE_REPITCH;
    if (mode == "tempo-sync") return SONARE_ENGINE_WARP_MODE_TEMPO_SYNC;
  } else if (value.IsNumber()) {
    const int mode = value.As<Napi::Number>().Int32Value();
    if (mode == SONARE_ENGINE_WARP_MODE_OFF || mode == SONARE_ENGINE_WARP_MODE_REPITCH ||
        mode == SONARE_ENGINE_WARP_MODE_TEMPO_SYNC) {
      return mode;
    }
  }
  Napi::TypeError::New(env, "warpMode must be 'off', 'repitch', or 'tempo-sync'")
      .ThrowAsJavaScriptException();
  return SONARE_ENGINE_WARP_MODE_OFF;
}

const char* CaptureSourceName(int source) {
  return source == SONARE_ENGINE_CAPTURE_SOURCE_INPUT ? "input" : "output";
}

SonareClipPageProvider* ProviderById(const std::vector<SonareClipPageProvider*>& providers,
                                     int id) {
  if (id <= 0 || static_cast<size_t>(id) > providers.size()) return nullptr;
  return providers[static_cast<size_t>(id - 1)];
}

}  // namespace

Napi::Value RealtimeEngineWrap::SetClips(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "expected an array of clips").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array input = info[0].As<Napi::Array>();
  std::vector<std::vector<std::vector<float>>> storage;
  std::vector<std::vector<const float*>> ptr_storage;
  std::vector<std::vector<SonareEngineWarpAnchor>> warp_storage;
  std::vector<SonareEngineClip> clips;
  storage.reserve(input.Length());
  ptr_storage.reserve(input.Length());
  warp_storage.reserve(input.Length());
  clips.reserve(input.Length());

  for (uint32_t i = 0; i < input.Length(); ++i) {
    Napi::Object obj = input.Get(i).As<Napi::Object>();
    const bool has_page_provider = obj.Has("pageProvider") &&
                                   !obj.Get("pageProvider").IsUndefined() &&
                                   !obj.Get("pageProvider").IsNull();
    Napi::Array channels = Napi::Array::New(env);
    if (!has_page_provider) {
      const Napi::Value channel_value = obj.Get("channels");
      if (!channel_value.IsArray()) {
        Napi::TypeError::New(env, "clip requires non-empty channels or a pageProvider")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      channels = channel_value.As<Napi::Array>();
    }
    if (!has_page_provider && channels.Length() == 0) {
      Napi::TypeError::New(env, "clip requires non-empty channels or a pageProvider")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    storage.emplace_back();
    ptr_storage.emplace_back();
    warp_storage.emplace_back();
    auto& clip_storage = storage.back();
    auto& clip_ptrs = ptr_storage.back();
    auto& clip_warp_anchors = warp_storage.back();
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
    clip.track_id = obj.Has("trackId") && !obj.Get("trackId").IsUndefined()
                        ? obj.Get("trackId").As<Napi::Number>().Uint32Value()
                        : 0;
    if (has_page_provider) {
      const int provider_id = obj.Get("pageProvider").As<Napi::Number>().Int32Value();
      SonareClipPageProvider* provider = ProviderById(clip_page_providers_, provider_id);
      if (!provider) {
        Napi::TypeError::New(env, "pageProvider is not a live ClipPageProvider")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      clip.page_provider = provider;
      clip.channels = nullptr;
      clip.num_channels = 0;
      clip.num_samples = 0;
    } else {
      clip.channels = clip_ptrs.data();
      clip.num_channels = static_cast<int>(clip_ptrs.size());
      clip.num_samples = static_cast<int64_t>(num_samples);
    }
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
    clip.warp_mode =
        obj.Has("warpMode") ? ParseWarpMode(env, obj.Get("warpMode")) : SONARE_ENGINE_WARP_MODE_OFF;
    if (env.IsExceptionPending()) return env.Undefined();
    if (obj.Has("warpAnchors") && !obj.Get("warpAnchors").IsUndefined()) {
      const Napi::Value anchors_value = obj.Get("warpAnchors");
      if (!anchors_value.IsArray()) {
        Napi::TypeError::New(env, "warpAnchors must be an array").ThrowAsJavaScriptException();
        return env.Undefined();
      }
      Napi::Array anchors = anchors_value.As<Napi::Array>();
      clip_warp_anchors.reserve(anchors.Length());
      for (uint32_t anchor_index = 0; anchor_index < anchors.Length(); ++anchor_index) {
        Napi::Object anchor = anchors.Get(anchor_index).As<Napi::Object>();
        SonareEngineWarpAnchor out{};
        out.warp_sample = anchor.Get("warpSample").As<Napi::Number>().DoubleValue();
        out.source_sample = anchor.Get("sourceSample").As<Napi::Number>().DoubleValue();
        clip_warp_anchors.push_back(out);
      }
      clip.warp_anchors = clip_warp_anchors.data();
      clip.warp_anchor_count = clip_warp_anchors.size();
    }
    clips.push_back(clip);
  }

  ThrowIfError(env, sonare_engine_set_clips(engine_, clips.data(), clips.size()));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetTrackLanes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "expected an array of track lanes").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array input = info[0].As<Napi::Array>();
  std::vector<SonareEngineTrackLane> lanes;
  std::vector<std::vector<SonareEngineTrackSend>> send_storage;
  lanes.reserve(input.Length());
  send_storage.reserve(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    Napi::Value value = input.Get(i);
    SonareEngineTrackLane lane{};
    if (value.IsNumber()) {
      lane.track_id = value.As<Napi::Number>().Uint32Value();
    } else if (value.IsObject()) {
      Napi::Object obj = value.As<Napi::Object>();
      lane.track_id = obj.Get("trackId").As<Napi::Number>().Uint32Value();
      if (obj.Has("sends") && !obj.Get("sends").IsUndefined() && !obj.Get("sends").IsNull()) {
        if (!obj.Get("sends").IsArray()) {
          Napi::TypeError::New(env, "track lane sends must be an array")
              .ThrowAsJavaScriptException();
          return env.Undefined();
        }
        Napi::Array sends = obj.Get("sends").As<Napi::Array>();
        std::vector<SonareEngineTrackSend> lane_sends;
        lane_sends.reserve(sends.Length());
        for (uint32_t send_index = 0; send_index < sends.Length(); ++send_index) {
          if (!sends.Get(send_index).IsObject()) {
            Napi::TypeError::New(env, "track lane send must be an object")
                .ThrowAsJavaScriptException();
            return env.Undefined();
          }
          Napi::Object send_obj = sends.Get(send_index).As<Napi::Object>();
          SonareEngineTrackSend send{};
          send.bus_id = send_obj.Get("busId").As<Napi::Number>().Uint32Value();
          send.level_db = send_obj.Has("levelDb") && !send_obj.Get("levelDb").IsUndefined()
                              ? send_obj.Get("levelDb").As<Napi::Number>().FloatValue()
                              : 0.0f;
          send.enabled = !send_obj.Has("enabled") || send_obj.Get("enabled").IsUndefined() ||
                                 send_obj.Get("enabled").As<Napi::Boolean>().Value()
                             ? 1
                             : 0;
          lane_sends.push_back(send);
        }
        send_storage.push_back(std::move(lane_sends));
        lane.sends = send_storage.back().data();
        lane.send_count = send_storage.back().size();
      }
    } else {
      Napi::TypeError::New(env, "track lane must be a number or object")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    lanes.push_back(lane);
  }
  ThrowIfError(env, sonare_engine_set_track_lanes(engine_, lanes.data(), lanes.size()));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetTrackBuses(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "expected an array of track buses").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array input = info[0].As<Napi::Array>();
  std::vector<SonareEngineBus> buses;
  buses.reserve(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    if (!input.Get(i).IsObject()) {
      Napi::TypeError::New(env, "track bus must be an object").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object obj = input.Get(i).As<Napi::Object>();
    SonareEngineBus bus{};
    bus.bus_id = obj.Get("busId").As<Napi::Number>().Uint32Value();
    bus.gain_db = obj.Has("gainDb") && !obj.Get("gainDb").IsUndefined()
                      ? obj.Get("gainDb").As<Napi::Number>().FloatValue()
                      : 0.0f;
    buses.push_back(bus);
  }
  ThrowIfError(env, sonare_engine_set_track_buses(engine_, buses.data(), buses.size()));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetBusStripJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t bus_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  std::string scene_json = info.Length() > 1 ? info[1].As<Napi::String>().Utf8Value() : "";
  ThrowIfError(env, sonare_engine_set_bus_strip_json(engine_, bus_id, scene_json.c_str()));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetTrackStripJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t track_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  std::string scene_json = info.Length() > 1 ? info[1].As<Napi::String>().Utf8Value() : "";
  ThrowIfError(env, sonare_engine_set_track_strip_json(engine_, track_id, scene_json.c_str()));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetTrackStripEqBandJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t track_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  const int band_index = info.Length() > 1 ? info[1].As<Napi::Number>().Int32Value() : -1;
  std::string band_json = info.Length() > 2 ? info[2].As<Napi::String>().Utf8Value() : "";
  ThrowIfError(env, sonare_engine_set_track_strip_eq_band_json(engine_, track_id, band_index,
                                                               band_json.c_str()));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetTrackStripInsertBypassed(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const uint32_t track_id = info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  const unsigned int insert_index =
      info.Length() > 1 ? info[1].As<Napi::Number>().Uint32Value() : 0;
  const bool bypassed = info.Length() > 2 && info[2].As<Napi::Boolean>().Value();
  const bool reset_on_bypass = info.Length() > 3 && info[3].As<Napi::Boolean>().Value();
  ThrowIfError(env,
               sonare_engine_set_track_strip_insert_bypassed(
                   engine_, track_id, insert_index, bypassed ? 1 : 0, reset_on_bypass ? 1 : 0));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetMasterStripJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  std::string scene_json = info.Length() > 0 ? info[0].As<Napi::String>().Utf8Value() : "";
  ThrowIfError(env, sonare_engine_set_master_strip_json(engine_, scene_json.c_str()));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetMasterStripEqBandJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int band_index = info.Length() > 0 ? info[0].As<Napi::Number>().Int32Value() : -1;
  std::string band_json = info.Length() > 1 ? info[1].As<Napi::String>().Utf8Value() : "";
  ThrowIfError(env,
               sonare_engine_set_master_strip_eq_band_json(engine_, band_index, band_json.c_str()));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetMasterStripInsertBypassed(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const unsigned int insert_index =
      info.Length() > 0 ? info[0].As<Napi::Number>().Uint32Value() : 0;
  const bool bypassed = info.Length() > 1 && info[1].As<Napi::Boolean>().Value();
  const bool reset_on_bypass = info.Length() > 2 && info[2].As<Napi::Boolean>().Value();
  ThrowIfError(env, sonare_engine_set_master_strip_insert_bypassed(
                        engine_, insert_index, bypassed ? 1 : 0, reset_on_bypass ? 1 : 0));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::ClipCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  size_t count = 0;
  ThrowIfError(env, sonare_engine_clip_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Value RealtimeEngineWrap::CreateClipPageProvider(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int num_channels = info[0].As<Napi::Number>().Int32Value();
  const int64_t num_samples = info[1].As<Napi::Number>().Int64Value();
  const int64_t page_frames = info[2].As<Napi::Number>().Int64Value();
  SonareClipPageProvider* provider = nullptr;
  ThrowIfError(env,
               sonare_clip_page_provider_create(num_channels, num_samples, page_frames, &provider));
  if (env.IsExceptionPending()) return env.Undefined();
  for (size_t index = 0; index < clip_page_providers_.size(); ++index) {
    if (clip_page_providers_[index] == nullptr) {
      clip_page_providers_[index] = provider;
      return Napi::Number::New(env, static_cast<double>(index + 1));
    }
  }
  clip_page_providers_.push_back(provider);
  return Napi::Number::New(env, static_cast<double>(clip_page_providers_.size()));
}

Napi::Value RealtimeEngineWrap::SupplyClipPage(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareClipPageProvider* provider =
      ProviderById(clip_page_providers_, info[0].As<Napi::Number>().Int32Value());
  if (!provider) {
    Napi::TypeError::New(env, "pageProvider is not live").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int64_t page_index = info[1].As<Napi::Number>().Int64Value();
  Napi::Array channels = info[2].As<Napi::Array>();
  std::vector<std::vector<float>> storage;
  std::vector<const float*> ptrs;
  storage.reserve(channels.Length());
  ptrs.reserve(channels.Length());
  size_t frames = 0;
  for (uint32_t ch = 0; ch < channels.Length(); ++ch) {
    Napi::Value value = channels.Get(ch);
    if (!sonare_node::IsFloat32Array(value)) {
      Napi::TypeError::New(env, "clip page channel must be a Float32Array")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Float32Array channel = value.As<Napi::Float32Array>();
    if (ch == 0) {
      frames = channel.ElementLength();
    } else if (channel.ElementLength() != frames) {
      Napi::TypeError::New(env, "all clip page channels must have the same length")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    storage.emplace_back(channel.Data(), channel.Data() + channel.ElementLength());
    ptrs.push_back(storage.back().data());
  }
  ThrowIfError(env, sonare_clip_page_provider_supply(provider, page_index, ptrs.data(),
                                                     static_cast<int>(ptrs.size()),
                                                     static_cast<int64_t>(frames)));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::ClearClipPage(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareClipPageProvider* provider =
      ProviderById(clip_page_providers_, info[0].As<Napi::Number>().Int32Value());
  if (!provider) {
    Napi::TypeError::New(env, "pageProvider is not live").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ThrowIfError(env,
               sonare_clip_page_provider_clear(provider, info[1].As<Napi::Number>().Int64Value()));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::DestroyClipPageProvider(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int id = info[0].As<Napi::Number>().Int32Value();
  SonareClipPageProvider* provider = ProviderById(clip_page_providers_, id);
  if (!provider) return env.Undefined();
  sonare_clip_page_provider_destroy(provider);
  clip_page_providers_[static_cast<size_t>(id - 1)] = nullptr;
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::PopClipPageRequest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SonareClipPageRequest request{};
  int has_request = 0;
  ThrowIfError(env, sonare_engine_pop_clip_page_request(engine_, &request, &has_request));
  if (!has_request) return env.Null();
  Napi::Object out = Napi::Object::New(env);
  out.Set("clipId", Napi::Number::New(env, request.clip_id));
  out.Set("channel", Napi::Number::New(env, request.channel));
  out.Set("sample", Napi::Number::New(env, static_cast<double>(request.sample)));
  return out;
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

Napi::Value RealtimeEngineWrap::SetCaptureSource(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 0 || info[0].IsUndefined()) {
    Napi::TypeError::New(env, "capture source must be 'output' or 'input'")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const SonareEngineCaptureSource source = ParseCaptureSource(env, info[0]);
  if (env.IsExceptionPending()) return env.Undefined();
  ThrowIfError(env, sonare_engine_set_capture_source(engine_, source));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetRecordOffsetSamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int64_t offset_samples = OptionalInt64(info, 0, 0);
  ThrowIfError(env, sonare_engine_set_record_offset_samples(engine_, offset_samples));
  return env.Undefined();
}

Napi::Value RealtimeEngineWrap::SetInputMonitor(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const bool enabled =
      info.Length() <= 0 || info[0].IsUndefined() ? true : info[0].As<Napi::Boolean>().Value();
  const float gain =
      info.Length() <= 1 || info[1].IsUndefined() ? 1.0f : info[1].As<Napi::Number>().FloatValue();
  ThrowIfError(env, sonare_engine_set_input_monitor(engine_, enabled ? 1 : 0, gain));
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
  out.Set("source", Napi::String::New(env, CaptureSourceName(status.source)));
  out.Set("recordOffsetSamples",
          Napi::Number::New(env, static_cast<double>(status.record_offset_samples)));
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
