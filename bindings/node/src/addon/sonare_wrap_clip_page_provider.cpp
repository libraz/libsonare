#include "sonare_wrap_clip_page_provider.h"

#include <cstdint>
#include <string>
#include <vector>

#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using sonare_node::RequiredFloat32ArrayValue;
using sonare_node::RequiredInt64Arg;
using sonare_node::RequiredIntArg;
using sonare_node::RequiredPlaneListValue;
using sonare_node::ThrowIfError;

namespace {

// The class constructor, kept so a clip's pageProvider can be checked with
// InstanceOf before it is unwrapped. Persistent and never released: the addon
// is loaded once per process and the reference dies with it.
Napi::FunctionReference* g_clip_page_provider_constructor = nullptr;

}  // namespace

Napi::Object ClipPageProviderWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "ClipPageProvider",
      {
          InstanceMethod<&ClipPageProviderWrap::Supply>("supply", napi_default_method),
          InstanceMethod<&ClipPageProviderWrap::Clear>("clear", napi_default_method),
          InstanceMethod<&ClipPageProviderWrap::Destroy>("destroy", napi_default_method),
      });
  g_clip_page_provider_constructor = new Napi::FunctionReference();
  *g_clip_page_provider_constructor = Napi::Persistent(func);
  g_clip_page_provider_constructor->SuppressDestruct();
  exports.Set("ClipPageProvider", func);
  return exports;
}

// new ClipPageProvider(numChannels, numSamples, pageFrames)
ClipPageProviderWrap::ClipPageProviderWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<ClipPageProviderWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int num_channels = 0;
  int64_t num_samples = 0;
  int64_t page_frames = 0;
  if (!RequiredIntArg(env, info, 0, "numChannels", &num_channels) ||
      !RequiredInt64Arg(env, info, 1, "numSamples", &num_samples) ||
      !RequiredInt64Arg(env, info, 2, "pageFrames", &page_frames)) {
    return;
  }
  ThrowIfError(
      env, sonare_clip_page_provider_create(num_channels, num_samples, page_frames, &provider_));
  SONARE_NODE_CATCH_VOID(env)
}

ClipPageProviderWrap::~ClipPageProviderWrap() {
  if (provider_ != nullptr) {
    sonare_clip_page_provider_destroy(provider_);
    provider_ = nullptr;
  }
}

bool ClipPageProviderWrap::ReadHandle(Napi::Env env, const Napi::Value& value,
                                      SonareClipPageProvider** out) {
  if (env.IsExceptionPending() || out == nullptr) return false;
  *out = nullptr;
  if (g_clip_page_provider_constructor == nullptr || !value.IsObject() ||
      !value.As<Napi::Object>().InstanceOf(g_clip_page_provider_constructor->Value())) {
    Napi::TypeError::New(env, "pageProvider must be a ClipPageProvider instance")
        .ThrowAsJavaScriptException();
    return false;
  }
  ClipPageProviderWrap* wrap = ClipPageProviderWrap::Unwrap(value.As<Napi::Object>());
  if (wrap == nullptr || wrap->provider_ == nullptr) {
    sonare_node::ThrowHandleClosed(env, "pageProvider");
    return false;
  }
  *out = wrap->provider_;
  return true;
}

// supply(pageIndex, channels): one Float32Array per channel, all the same length.
Napi::Value ClipPageProviderWrap::Supply(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, provider_, "ClipPageProvider");
  int64_t page_index = 0;
  if (!RequiredInt64Arg(env, info, 0, "pageIndex", &page_index)) return env.Undefined();
  Napi::Array channels;
  if (!RequiredPlaneListValue(env, info.Length() > 1 ? info[1] : env.Undefined(), "channels",
                              &channels)) {
    return env.Undefined();
  }
  std::vector<std::vector<float>> storage;
  std::vector<const float*> ptrs;
  storage.reserve(channels.Length());
  ptrs.reserve(channels.Length());
  size_t frames = 0;
  for (uint32_t ch = 0; ch < channels.Length(); ++ch) {
    Napi::Float32Array channel;
    if (!RequiredFloat32ArrayValue(env, channels.Get(ch), "channels[" + std::to_string(ch) + "]",
                                   &channel)) {
      return env.Undefined();
    }
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
  ThrowIfError(env, sonare_clip_page_provider_supply(provider_, page_index, ptrs.data(),
                                                     static_cast<int>(ptrs.size()),
                                                     static_cast<int64_t>(frames)));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

// clear(pageIndex)
Napi::Value ClipPageProviderWrap::Clear(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, provider_, "ClipPageProvider");
  int64_t page_index = 0;
  if (!RequiredInt64Arg(env, info, 0, "pageIndex", &page_index)) return env.Undefined();
  ThrowIfError(env, sonare_clip_page_provider_clear(provider_, page_index));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

// destroy(): idempotent; later supply/clear calls are InvalidState.
void ClipPageProviderWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY(void) info;
  if (provider_ != nullptr) {
    sonare_clip_page_provider_destroy(provider_);
    provider_ = nullptr;
  }
  SONARE_NODE_CATCH_VOID(env)
}
