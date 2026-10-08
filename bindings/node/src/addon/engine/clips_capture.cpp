#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "engine/common.h"
#include "sonare_wrap_engine.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_project_timeline.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::engine;

namespace {

SonareEngineCaptureSource ParseCaptureSource(Napi::Env env, const Napi::Value& value) {
  if (value.IsString()) {
    const std::string source = sonare_node::node_narrow_string(env, value, "source");
    if (source == "output") return SONARE_ENGINE_CAPTURE_SOURCE_OUTPUT;
    if (source == "input") return SONARE_ENGINE_CAPTURE_SOURCE_INPUT;
  } else if (value.IsNumber()) {
    const int source = node_narrow_int(value.Env(), value, "source");
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
    const std::string mode = sonare_node::node_narrow_string(env, value, "warpMode");
    if (mode == "off") return SONARE_ENGINE_WARP_MODE_OFF;
    if (mode == "repitch") return SONARE_ENGINE_WARP_MODE_REPITCH;
    if (mode == "tempo-sync") return SONARE_ENGINE_WARP_MODE_TEMPO_SYNC;
    if (mode == "time-stretch") return SONARE_ENGINE_WARP_MODE_TIME_STRETCH;
  } else if (value.IsNumber()) {
    const int mode = node_narrow_int(value.Env(), value, "mode");
    if (mode == SONARE_ENGINE_WARP_MODE_OFF || mode == SONARE_ENGINE_WARP_MODE_REPITCH ||
        mode == SONARE_ENGINE_WARP_MODE_TEMPO_SYNC ||
        mode == SONARE_ENGINE_WARP_MODE_TIME_STRETCH) {
      return mode;
    }
  }
  Napi::TypeError::New(env, "warpMode must be 'off', 'repitch', 'tempo-sync', or 'time-stretch'")
      .ThrowAsJavaScriptException();
  return SONARE_ENGINE_WARP_MODE_OFF;
}

const char* CaptureSourceName(int source) {
  return source == SONARE_ENGINE_CAPTURE_SOURCE_INPUT ? "input" : "output";
}

SonareClipPageProvider* ProviderById(
    const std::unordered_map<int, SonareClipPageProvider*>& providers, int id) {
  const auto it = providers.find(id);
  return it == providers.end() ? nullptr : it->second;
}

// Reads an optional `sends` array off a track lane or bus object, in the one
// shape both share (SonareEngineTrackSend). Absent/null `sends` leaves
// *out_sends empty and returns true. Returns false with a pending JS exception
// on a malformed entry.
bool ReadOptionalSends(Napi::Env env, const Napi::Object& obj,
                       std::vector<SonareEngineTrackSend>* out_sends) {
  if (!obj.Has("sends") || obj.Get("sends").IsUndefined() || obj.Get("sends").IsNull()) {
    return true;
  }
  if (!obj.Get("sends").IsArray()) {
    Napi::TypeError::New(env, "sends must be an array").ThrowAsJavaScriptException();
    return false;
  }
  Napi::Array sends = obj.Get("sends").As<Napi::Array>();
  out_sends->reserve(sends.Length());
  for (uint32_t send_index = 0; send_index < sends.Length(); ++send_index) {
    if (!sends.Get(send_index).IsObject()) {
      Napi::TypeError::New(env, "send must be an object").ThrowAsJavaScriptException();
      return false;
    }
    Napi::Object send_obj = sends.Get(send_index).As<Napi::Object>();
    SonareEngineTrackSend send{};
    if (!RequiredUint32Property(env, send_obj, "busId", &send.bus_id)) return false;
    send.level_db = FloatProperty(send_obj, "levelDb", 0.0f);
    send.enabled = BoolProperty(send_obj, "enabled", true) ? 1 : 0;
    // Default to post-fader when sendTiming is absent to preserve the
    // historical behavior before the field existed (post-fader is the zero
    // value of SonareSendTiming).
    send.send_timing = IntProperty(send_obj, "sendTiming", SONARE_SEND_TIMING_POST_FADER);
    if (env.IsExceptionPending()) return false;
    out_sends->push_back(send);
  }
  return true;
}

}  // namespace

Napi::Value RealtimeEngineWrap::SetClips(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
    Napi::Value entry = input.Get(i);
    if (!entry.IsObject()) {
      Napi::TypeError::New(env, "clip must be an object").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object obj = entry.As<Napi::Object>();
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
        Napi::RangeError::New(env, "all clip channels must have the same length")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      clip_storage.emplace_back(channel.Data(), channel.Data() + channel.ElementLength());
      clip_ptrs.push_back(clip_storage.back().data());
    }

    SonareEngineClip clip{};
    if (!RequiredUint32Property(env, obj, "id", &clip.id)) return env.Undefined();
    clip.track_id = Uint32Property(obj, "trackId", 0);
    if (env.IsExceptionPending()) return env.Undefined();
    if (has_page_provider) {
      int provider_id = 0;
      if (!RequiredIntProperty(env, obj, "pageProvider", &provider_id)) return env.Undefined();
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
    if (!RequiredDoubleProperty(env, obj, "startPpq", &clip.start_ppq)) return env.Undefined();
    clip.clip_offset_samples = Int64Property(obj, "clipOffsetSamples", 0);
    clip.length_samples = Int64Property(obj, "lengthSamples", static_cast<int64_t>(num_samples));
    clip.loop = BoolProperty(obj, "loop", false) ? 1 : 0;
    clip.gain = FloatProperty(obj, "gain", 1.0f);
    clip.fade_in_samples = Int64Property(obj, "fadeInSamples", 0);
    clip.fade_out_samples = Int64Property(obj, "fadeOutSamples", 0);
    // A wrong-typed optional field left one pending JS exception; stop before
    // ParseWarpMode can throw a second one on top of it (a fatal abort).
    if (env.IsExceptionPending()) return env.Undefined();
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
        Napi::Value anchor_value = anchors.Get(anchor_index);
        if (!anchor_value.IsObject()) {
          Napi::TypeError::New(env, "warp anchor must be an object").ThrowAsJavaScriptException();
          return env.Undefined();
        }
        Napi::Object anchor = anchor_value.As<Napi::Object>();
        SonareEngineWarpAnchor out{};
        if (!RequiredDoubleProperty(env, anchor, "warpSample", &out.warp_sample)) {
          return env.Undefined();
        }
        if (!RequiredDoubleProperty(env, anchor, "sourceSample", &out.source_sample)) {
          return env.Undefined();
        }
        clip_warp_anchors.push_back(out);
      }
      clip.warp_anchors = clip_warp_anchors.data();
      clip.warp_anchor_count = clip_warp_anchors.size();
    }
    clips.push_back(clip);
  }

  ThrowIfError(env, sonare_engine_set_clips(engine_, clips.data(), clips.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTrackLanes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
    // Zero-init leaves source_channel_layout at 0 (mono); default to stereo so
    // existing callers that omit it keep the prior stereo behavior.
    lane.source_channel_layout = SONARE_CHANNEL_LAYOUT_STEREO;
    if (value.IsNumber()) {
      lane.track_id = node_narrow_uint32(value.Env(), value, "trackId");
    } else if (value.IsObject()) {
      Napi::Object obj = value.As<Napi::Object>();
      if (!RequiredUint32Property(env, obj, "trackId", &lane.track_id)) return env.Undefined();
      lane.output_bus_id = Uint32Property(obj, "outputBusId", lane.output_bus_id);
      // The full-range value is checked BEFORE narrowing to the uint8_t C-ABI
      // field: narrowing first would let e.g. 257 wrap to 1 (a valid Stereo)
      // and arrive at the C ABI already inside the range its own check
      // accepts.
      const uint32_t source_channel_layout_raw =
          Uint32Property(obj, "sourceChannelLayout", lane.source_channel_layout);
      if (env.IsExceptionPending()) return env.Undefined();
      if (source_channel_layout_raw > 255) {
        Napi::RangeError::New(env, "sourceChannelLayout must be an integer in [0, 255]")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      if (source_channel_layout_raw != SONARE_CHANNEL_LAYOUT_STEREO) {
        Napi::RangeError::New(env,
                              "track lane sourceChannelLayout must be stereo; multichannel lanes "
                              "are not implemented")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      lane.source_channel_layout = static_cast<uint8_t>(source_channel_layout_raw);
      std::vector<SonareEngineTrackSend> lane_sends;
      if (!ReadOptionalSends(env, obj, &lane_sends)) return env.Undefined();
      if (!lane_sends.empty()) {
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
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetLaneSidechain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "expected (trackId, insertIndex, sourceTrackId)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ThrowIfError(
      env,
      sonare_engine_set_lane_sidechain(
          engine_,
          sonare_node::node_narrow_uint32(env, info[0], sonare_node::node_arg_label(0).c_str()),
          sonare_node::node_narrow_uint32(env, info[1], sonare_node::node_arg_label(1).c_str()),
          sonare_node::node_narrow_uint32(env, info[2], sonare_node::node_arg_label(2).c_str())));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ApplyProjectTimeline(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const SonareProjectTimeline* timeline = nullptr;
  if (!ProjectTimelineWrap::ReadHandle(env, info[0], &timeline)) return env.Undefined();
  ThrowIfError(env, sonare_engine_apply_project_timeline(engine_, timeline));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTrackBuses(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() <= 0 || !info[0].IsArray()) {
    Napi::TypeError::New(env, "expected an array of track buses").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array input = info[0].As<Napi::Array>();
  std::vector<SonareEngineBus> buses;
  std::vector<std::vector<SonareEngineTrackSend>> send_storage;
  buses.reserve(input.Length());
  send_storage.reserve(input.Length());
  for (uint32_t i = 0; i < input.Length(); ++i) {
    if (!input.Get(i).IsObject()) {
      Napi::TypeError::New(env, "track bus must be an object").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object obj = input.Get(i).As<Napi::Object>();
    SonareEngineBus bus{};
    if (!RequiredUint32Property(env, obj, "busId", &bus.bus_id)) return env.Undefined();
    bus.gain_db = FloatProperty(obj, "gainDb", 0.0f);
    // Zero-init leaves channel_layout at 0 (mono); default to stereo so existing
    // callers that omit it keep the prior stereo behavior. The full-range
    // value is checked BEFORE narrowing to the uint8_t C-ABI field, the same
    // way the track lane's sourceChannelLayout above does.
    const uint32_t channel_layout_raw =
        Uint32Property(obj, "channelLayout", SONARE_CHANNEL_LAYOUT_STEREO);
    if (env.IsExceptionPending()) return env.Undefined();
    if (channel_layout_raw > 255) {
      Napi::RangeError::New(env, "channelLayout must be an integer in [0, 255]")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    bus.channel_layout = static_cast<uint8_t>(channel_layout_raw);
    // Bus this bus's output sums into instead of the master mix; 0 (the
    // zero-init default) keeps the bus on the master mix.
    bus.output_bus_id = Uint32Property(obj, "outputBusId", bus.output_bus_id);
    if (env.IsExceptionPending()) return env.Undefined();
    std::vector<SonareEngineTrackSend> bus_sends;
    if (!ReadOptionalSends(env, obj, &bus_sends)) return env.Undefined();
    if (!bus_sends.empty()) {
      send_storage.push_back(std::move(bus_sends));
      bus.sends = send_storage.back().data();
      bus.send_count = send_storage.back().size();
    }
    buses.push_back(bus);
  }
  ThrowIfError(env, sonare_engine_set_track_buses(engine_, buses.data(), buses.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetBusSidechain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 4 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber() ||
      !info[3].IsNumber()) {
    Napi::TypeError::New(env, "expected (busId, insertIndex, sourceKind, sourceId)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ThrowIfError(
      env,
      sonare_engine_set_bus_sidechain(
          engine_,
          sonare_node::node_narrow_uint32(env, info[0], sonare_node::node_arg_label(0).c_str()),
          sonare_node::node_narrow_uint32(env, info[1], sonare_node::node_arg_label(1).c_str()),
          sonare_node::node_narrow_int(env, info[2], sonare_node::node_arg_label(2).c_str()),
          sonare_node::node_narrow_uint32(env, info[3], sonare_node::node_arg_label(3).c_str())));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetMasterSidechain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "expected (insertIndex, sourceKind, sourceId)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ThrowIfError(
      env,
      sonare_engine_set_master_sidechain(
          engine_,
          sonare_node::node_narrow_uint32(env, info[0], sonare_node::node_arg_label(0).c_str()),
          sonare_node::node_narrow_int(env, info[1], sonare_node::node_arg_label(1).c_str()),
          sonare_node::node_narrow_uint32(env, info[2], sonare_node::node_arg_label(2).c_str())));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::CanSetLaneSidechain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t track_id = 0;
  uint32_t insert_index = 0;
  uint32_t source_track_id = 0;
  if (!RequiredUint32Arg(env, info, 0, "trackId", &track_id) ||
      !RequiredUint32Arg(env, info, 1, "insertIndex", &insert_index) ||
      !RequiredUint32Arg(env, info, 2, "sourceTrackId", &source_track_id)) {
    return env.Undefined();
  }
  int refusal = 0;
  ThrowIfError(env, sonare_engine_can_set_lane_sidechain(engine_, track_id, insert_index,
                                                         source_track_id, &refusal));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(refusal));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::CanSetBusSidechain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t bus_id = 0;
  uint32_t insert_index = 0;
  int source_kind = 0;
  uint32_t source_id = 0;
  if (!RequiredUint32Arg(env, info, 0, "busId", &bus_id) ||
      !RequiredUint32Arg(env, info, 1, "insertIndex", &insert_index) ||
      !RequiredIntArg(env, info, 2, "sourceKind", &source_kind) ||
      !RequiredUint32Arg(env, info, 3, "sourceId", &source_id)) {
    return env.Undefined();
  }
  int refusal = 0;
  ThrowIfError(env, sonare_engine_can_set_bus_sidechain(engine_, bus_id, insert_index, source_kind,
                                                        source_id, &refusal));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(refusal));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::CanSetMasterSidechain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t insert_index = 0;
  int source_kind = 0;
  uint32_t source_id = 0;
  if (!RequiredUint32Arg(env, info, 0, "insertIndex", &insert_index) ||
      !RequiredIntArg(env, info, 1, "sourceKind", &source_kind) ||
      !RequiredUint32Arg(env, info, 2, "sourceId", &source_id)) {
    return env.Undefined();
  }
  int refusal = 0;
  ThrowIfError(env, sonare_engine_can_set_master_sidechain(engine_, insert_index, source_kind,
                                                           source_id, &refusal));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(refusal));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClipCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  size_t count = 0;
  ThrowIfError(env, sonare_engine_clip_count(engine_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::CreateClipPageProvider(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int num_channels = 0;
  int64_t num_samples = 0;
  int64_t page_frames = 0;
  if (!RequiredIntArg(env, info, 0, "numChannels", &num_channels) ||
      !RequiredInt64Arg(env, info, 1, "numSamples", &num_samples) ||
      !RequiredInt64Arg(env, info, 2, "pageFrames", &page_frames)) {
    return env.Undefined();
  }
  SonareClipPageProvider* provider = nullptr;
  ThrowIfError(env,
               sonare_clip_page_provider_create(num_channels, num_samples, page_frames, &provider));
  if (env.IsExceptionPending()) return env.Undefined();
  const int id = next_clip_page_provider_id_++;
  clip_page_providers_[id] = provider;
  return Napi::Number::New(env, static_cast<double>(id));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SupplyClipPage(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int provider_id = 0;
  int64_t page_index = 0;
  if (!RequiredIntArg(env, info, 0, "providerId", &provider_id) ||
      !RequiredInt64Arg(env, info, 1, "pageIndex", &page_index)) {
    return env.Undefined();
  }
  SonareClipPageProvider* provider = ProviderById(clip_page_providers_, provider_id);
  if (!provider) {
    Napi::TypeError::New(env, "pageProvider is not live").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() <= 2 || !info[2].IsArray()) {
    Napi::TypeError::New(env, "expected an array of Float32Array channels")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
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
      Napi::RangeError::New(env, "all clip page channels must have the same length")
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
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClearClipPage(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int provider_id = 0;
  int64_t page_index = 0;
  if (!RequiredIntArg(env, info, 0, "providerId", &provider_id) ||
      !RequiredInt64Arg(env, info, 1, "pageIndex", &page_index)) {
    return env.Undefined();
  }
  SonareClipPageProvider* provider = ProviderById(clip_page_providers_, provider_id);
  if (!provider) {
    Napi::TypeError::New(env, "pageProvider is not live").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ThrowIfError(env, sonare_clip_page_provider_clear(provider, page_index));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::DestroyClipPageProvider(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int id = 0;
  if (!RequiredIntArg(env, info, 0, "providerId", &id)) return env.Undefined();
  SonareClipPageProvider* provider = ProviderById(clip_page_providers_, id);
  if (!provider) return env.Undefined();
  sonare_clip_page_provider_destroy(provider);
  clip_page_providers_.erase(id);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::PopClipPageRequest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SonareClipPageRequest request{};
  int has_request = 0;
  ThrowIfError(env, sonare_engine_pop_clip_page_request(engine_, &request, &has_request));
  if (!has_request) return env.Null();
  Napi::Object out = Napi::Object::New(env);
  out.Set("clipId", Napi::Number::New(env, request.clip_id));
  out.Set("channel", Napi::Number::New(env, request.channel));
  out.Set("sample", Napi::Number::New(env, static_cast<double>(request.sample)));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetClipPagePrefetchFrames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  double frames = 0.0;
  if (!OptionalDoubleArg(env, info, 0, "frames", 0.0, &frames)) return env.Undefined();
  if (!(frames >= 0.0)) {
    Napi::TypeError::New(env, "clip page prefetch frames must be >= 0")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ThrowIfError(env,
               sonare_engine_set_clip_page_prefetch_frames(engine_, static_cast<int64_t>(frames)));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClipPagePrefetchFrames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int64_t frames = 0;
  ThrowIfError(env, sonare_engine_clip_page_prefetch_frames(engine_, &frames));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(frames));
  SONARE_NODE_CATCH(env)
}

void RealtimeEngineWrap::InstallCaptureBuffers(Napi::Env env,
                                               std::vector<std::vector<float>>&& buffers,
                                               int64_t frames) {
  // Move storage into its final owner before sharing pointers with the C engine.
  // A pointer into the local vector would become invalid if the move assignment
  // cannot steal its allocation.
  capture_buffers_ = std::move(buffers);
  capture_ptrs_.clear();
  capture_ptrs_.reserve(capture_buffers_.size());
  for (auto& buffer : capture_buffers_) capture_ptrs_.push_back(buffer.data());

  SonareEngineCaptureBuffer buffer{};
  buffer.channels = capture_ptrs_.data();
  buffer.num_channels = static_cast<int>(capture_ptrs_.size());
  buffer.capacity_frames = frames;
  ThrowIfError(env, sonare_engine_set_capture_buffer(engine_, &buffer));
  if (env.IsExceptionPending()) return;
  capture_capacity_frames_ = frames;
}

// Canonical form: the addon sizes the capture planes itself. Routing this
// through the channel-array form would make the caller build a full-size JS
// Float32Array per channel purely to be copied in and dropped, doubling the
// peak footprint of a long capture.
Napi::Value RealtimeEngineWrap::SetCaptureBufferExtent(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int num_channels = 0;
  int64_t capacity_frames = 0;
  if (!RequiredIntArg(env, info, 0, "numChannels", &num_channels) ||
      !RequiredInt64Arg(env, info, 1, "capacityFrames", &capacity_frames)) {
    return env.Undefined();
  }
  if (num_channels <= 0 || capacity_frames <= 0) {
    Napi::RangeError::New(env, "capture channel count and capacity must be positive")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::vector<std::vector<float>> buffers(static_cast<size_t>(num_channels));
  // Size each plane in place: constructing one sized plane and copying it per
  // channel would hold a full extra plane live for the length of the loop.
  for (std::vector<float>& plane : buffers) {
    plane.assign(static_cast<size_t>(capacity_frames), 0.0f);
  }
  InstallCaptureBuffers(env, std::move(buffers), capacity_frames);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetCaptureBuffer(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  // Two arities: an array of caller-owned planes, or the addon-owned extent.
  // Anything else lands in the extent form and is reported against its named
  // arguments rather than as a generic "expected an array".
  if (!info[0].IsArray()) return SetCaptureBufferExtent(info);

  Napi::Array channels = info[0].As<Napi::Array>();
  if (channels.Length() == 0) {
    Napi::TypeError::New(env, "capture channels must not be empty").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::vector<std::vector<float>> buffers;
  buffers.reserve(channels.Length());
  int64_t frames = 0;
  for (uint32_t ch = 0; ch < channels.Length(); ++ch) {
    Napi::Value value = channels.Get(ch);
    if (!sonare_node::IsFloat32Array(value)) {
      Napi::TypeError::New(env, "capture channel must be a Float32Array")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Float32Array channel = value.As<Napi::Float32Array>();
    if (channel.Data() == nullptr) {
      Napi::TypeError::New(env, "capture channel must not be detached")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    if (ch == 0) {
      frames = static_cast<int64_t>(channel.ElementLength());
      if (frames <= 0) {
        Napi::TypeError::New(env, "capture channels must not be empty")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
    } else if (static_cast<int64_t>(channel.ElementLength()) != frames) {
      Napi::RangeError::New(env, "all capture channels must have the same length")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    buffers.emplace_back(channel.Data(), channel.Data() + channel.ElementLength());
  }

  InstallCaptureBuffers(env, std::move(buffers), frames);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ArmCapture(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  bool armed = true;
  if (!OptionalBoolArg(env, info, 0, "armed", true, &armed)) return env.Undefined();
  ThrowIfError(env, sonare_engine_arm_capture(engine_, armed ? 1 : 0));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetCapturePunch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int64_t start_sample = 0;
  int64_t end_sample = 0;
  bool enabled = true;
  if (!OptionalInt64Arg(env, info, 0, "startSample", 0, &start_sample) ||
      !OptionalInt64Arg(env, info, 1, "endSample", 0, &end_sample) ||
      !OptionalBoolArg(env, info, 2, "enabled", true, &enabled)) {
    return env.Undefined();
  }
  ThrowIfError(env,
               sonare_engine_set_capture_punch(engine_, start_sample, end_sample, enabled ? 1 : 0));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetCaptureSource(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() <= 0 || info[0].IsUndefined()) {
    Napi::TypeError::New(env, "capture source must be 'output' or 'input'")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const SonareEngineCaptureSource source = ParseCaptureSource(env, info[0]);
  if (env.IsExceptionPending()) return env.Undefined();
  ThrowIfError(env, sonare_engine_set_capture_source(engine_, source));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetRecordOffsetSamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int64_t offset_samples = 0;
  if (!OptionalInt64Arg(env, info, 0, "offsetSamples", 0, &offset_samples)) return env.Undefined();
  ThrowIfError(env, sonare_engine_set_record_offset_samples(engine_, offset_samples));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetInputMonitor(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  bool enabled = true;
  float gain = 1.0f;
  if (!OptionalBoolArg(env, info, 0, "enabled", true, &enabled) ||
      !OptionalFloatArg(env, info, 1, "gain", 1.0f, &gain)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_input_monitor(engine_, enabled ? 1 : 0, gain));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ResetCapture(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  ThrowIfError(env, sonare_engine_reset_capture(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::CaptureStatus(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
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
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::CapturedAudio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (engine_ == nullptr) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE,
                                         "RealtimeEngine is destroyed");
    return env.Undefined();
  }
  SonareEngineCaptureStatus status{};
  ThrowIfError(env, sonare_engine_capture_status(engine_, &status));
  if (env.IsExceptionPending()) return env.Undefined();

  // Clamp the captured frame count to the installed capacity so that we never
  // read past the planes setCaptureBuffer() allocated.
  int64_t frames = status.captured_frames;
  if (frames < 0) frames = 0;
  if (frames > capture_capacity_frames_) frames = capture_capacity_frames_;

  Napi::Array out = Napi::Array::New(env, capture_buffers_.size());
  for (size_t ch = 0; ch < capture_buffers_.size(); ++ch) {
    const std::vector<float>& source = capture_buffers_[ch];
    const size_t count = static_cast<size_t>(frames);
    auto channel = Napi::Float32Array::New(env, count);
    if (count > 0) {
      std::memcpy(channel.Data(), source.data(), count * sizeof(float));
    }
    out.Set(static_cast<uint32_t>(ch), channel);
  }
  return out;
  SONARE_NODE_CATCH(env)
}
