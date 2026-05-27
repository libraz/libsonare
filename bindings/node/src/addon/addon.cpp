#include <napi.h>

#include "sonare_c.h"
#include "sonare_wrap.h"
#include "sonare_wrap_engine.h"

namespace {

Napi::Value EngineAbiVersion(const Napi::CallbackInfo& info) {
  return Napi::Number::New(info.Env(), sonare_engine_abi_version());
}

}  // namespace

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  SonareWrap::Init(env, exports);
  RealtimeEngineWrap::Init(env, exports);
  exports.Set("engineAbiVersion", Napi::Function::New(env, EngineAbiVersion, "engineAbiVersion"));
  return exports;
}

NODE_API_MODULE(sonare, Init)
