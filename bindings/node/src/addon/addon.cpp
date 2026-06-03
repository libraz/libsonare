#include <napi.h>

#include "sonare_c.h"
#include "sonare_wrap.h"
#include "sonare_wrap_engine.h"
#include "sonare_wrap_project.h"

namespace {

Napi::Value EngineAbiVersion(const Napi::CallbackInfo& info) {
  return Napi::Number::New(info.Env(), sonare_engine_abi_version());
}

Napi::Value VoiceChangerAbiVersion(const Napi::CallbackInfo& info) {
  return Napi::Number::New(info.Env(), sonare_voice_changer_abi_version());
}

Napi::Value ProjectAbiVersion(const Napi::CallbackInfo& info) {
  return Napi::Number::New(info.Env(), sonare_project_abi_version());
}

}  // namespace

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  SonareWrap::Init(env, exports);
  RealtimeEngineWrap::Init(env, exports);
  ProjectWrap::Init(env, exports);
  exports.Set("engineAbiVersion", Napi::Function::New(env, EngineAbiVersion, "engineAbiVersion"));
  exports.Set("voiceChangerAbiVersion",
              Napi::Function::New(env, VoiceChangerAbiVersion, "voiceChangerAbiVersion"));
  exports.Set("projectAbiVersion",
              Napi::Function::New(env, ProjectAbiVersion, "projectAbiVersion"));
  return exports;
}

NODE_API_MODULE(sonare, Init)
