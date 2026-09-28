#include "sonare_wrap_playback.h"

#include <string>

#include "sonare_wrap_utils.h"

namespace {

Napi::FunctionReference* g_hrtf_set_constructor = nullptr;

}  // namespace

Napi::Object HrtfSetWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(env, "HrtfSet",
                                    {
                                        StaticMethod<&HrtfSetWrap::Default>("default"),
                                        StaticMethod<&HrtfSetWrap::FromBytes>("fromBytes"),
                                        InstanceMethod<&HrtfSetWrap::Destroy>("destroy"),
                                    });

  g_hrtf_set_constructor = new Napi::FunctionReference();
  *g_hrtf_set_constructor = Napi::Persistent(func);
  g_hrtf_set_constructor->SuppressDestruct();

  exports.Set("HrtfSet", func);
  return exports;
}

Napi::Object HrtfSetWrap::Wrap(const Napi::CallbackInfo& info, SonareHrtfSet* handle) {
  // Inject the already-created native handle through an External so the
  // constructor adopts it instead of rejecting a direct `new HrtfSet()`.
  Napi::Env env = info.Env();
  Napi::External<SonareHrtfSet> external = Napi::External<SonareHrtfSet>::New(env, handle);
  return info.This().As<Napi::Function>().New({external});
}

HrtfSetWrap::HrtfSetWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<HrtfSetWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() > 0 && info[0].IsExternal()) {
    set_ = info[0].As<Napi::External<SonareHrtfSet>>().Data();
    return;
  }
  Napi::Error::New(env,
                   "HrtfSet cannot be constructed directly; use HrtfSet.default() or "
                   "HrtfSet.fromBytes()")
      .ThrowAsJavaScriptException();
  return;
  SONARE_NODE_CATCH_VOID(env)
}

HrtfSetWrap::~HrtfSetWrap() {
  sonare_hrtf_set_destroy(set_);
  set_ = nullptr;
}

Napi::Value HrtfSetWrap::Default(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SonareHrtfSet* handle = nullptr;
  sonare_node::ThrowIfError(env, sonare_hrtf_set_create_default(&handle));
  if (env.IsExceptionPending()) return env.Undefined();
  return HrtfSetWrap::Wrap(info, handle);
  SONARE_NODE_CATCH(env)
}

Napi::Value HrtfSetWrap::FromBytes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const uint8_t* data = nullptr;
  size_t size = 0;
  if (info.Length() > 0 && info[0].IsBuffer()) {
    auto buffer = info[0].As<Napi::Buffer<uint8_t>>();
    data = buffer.Data();
    size = buffer.Length();
  } else if (info.Length() > 0 && sonare_node::IsUint8Array(info[0])) {
    auto array = info[0].As<Napi::Uint8Array>();
    data = array.Data();
    size = array.ByteLength();
  } else {
    Napi::TypeError::New(env, "Expected Buffer or Uint8Array argument")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareHrtfSet* handle = nullptr;
  sonare_node::ThrowIfError(env, sonare_hrtf_set_create_from_memory(data, size, &handle));
  if (env.IsExceptionPending()) return env.Undefined();
  return HrtfSetWrap::Wrap(info, handle);
  SONARE_NODE_CATCH(env)
}

Napi::Value HrtfSetWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  sonare_hrtf_set_destroy(set_);
  set_ = nullptr;
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

bool HrtfSetWrap::ReadHandle(Napi::Env env, const Napi::Value& value, const SonareHrtfSet** out) {
  if (env.IsExceptionPending() || out == nullptr) return false;
  *out = nullptr;
  if (value.IsUndefined() || value.IsNull()) return true;
  if (g_hrtf_set_constructor == nullptr || !value.IsObject() ||
      !value.As<Napi::Object>().InstanceOf(g_hrtf_set_constructor->Value())) {
    Napi::TypeError::New(env, "hrtf must be an HrtfSet instance").ThrowAsJavaScriptException();
    return false;
  }
  HrtfSetWrap* wrap = HrtfSetWrap::Unwrap(value.As<Napi::Object>());
  if (wrap == nullptr || wrap->set_ == nullptr) {
    Napi::TypeError::New(env, "hrtf is destroyed").ThrowAsJavaScriptException();
    return false;
  }
  *out = wrap->set_;
  return true;
}
