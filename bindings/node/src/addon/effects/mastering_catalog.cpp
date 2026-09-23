/// @file
/// @brief Node bindings for the mastering catalogue queries: preset, platform, processor and insert
/// names.

#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/assistant/platform_targets.h"
#include "mastering/assistant/suggester.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

Napi::Value SonareWrap::MasteringPresetNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  auto names = sonare::mastering::api::preset_names();
  Napi::Array out = Napi::Array::New(env, names.size());
  for (size_t index = 0; index < names.size(); ++index) {
    out.Set(index, names[index]);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringPlatformNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  // Read from the shared delivery-target table, so the names a caller can pass
  // as `targetPlatform` are the names the assistant actually accepts.
  const std::vector<std::string> names = sonare::mastering::assistant::platform_names();
  Napi::Array out = Napi::Array::New(env, names.size());
  for (size_t index = 0; index < names.size(); ++index) {
    out.Set(index, names[index]);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringProcessorNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  auto names = sonare::mastering::api::processor_names();
  Napi::Array out = Napi::Array::New(env, names.size());
  for (size_t index = 0; index < names.size(); ++index) {
    out.Set(index, names[index]);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringPairProcessorNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  auto names = sonare::mastering::api::pair_processor_names();
  Napi::Array out = Napi::Array::New(env, names.size());
  for (size_t index = 0; index < names.size(); ++index) {
    out.Set(index, names[index]);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringPairAnalysisNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  auto names = sonare::mastering::api::pair_analysis_names();
  Napi::Array out = Napi::Array::New(env, names.size());
  for (size_t index = 0; index < names.size(); ++index) {
    out.Set(index, names[index]);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringStereoAnalysisNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  auto names = sonare::mastering::api::stereo_analysis_names();
  Napi::Array out = Napi::Array::New(env, names.size());
  for (size_t index = 0; index < names.size(); ++index) {
    out.Set(index, names[index]);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringInsertNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  // sonare_mastering_insert_names() returns a program-lifetime '\n'-joined
  // const char* (NOT to be freed); split it into a JS string[] like the other
  // *_names getters. An empty string yields an empty array.
  const char* joined = sonare_mastering_insert_names();
  Napi::Array out = Napi::Array::New(env);
  if (joined == nullptr || joined[0] == '\0') {
    return out;
  }
  std::string names(joined);
  uint32_t index = 0;
  size_t start = 0;
  while (start <= names.size()) {
    size_t end = names.find('\n', start);
    if (end == std::string::npos) {
      out.Set(index++, Napi::String::New(env, names.substr(start)));
      break;
    }
    out.Set(index++, Napi::String::New(env, names.substr(start, end - start)));
    start = end + 1;
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringInsertParamNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (name: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // sonare_mastering_insert_param_names(name) returns a thread-local '\n'-joined
  // const char* (NOT to be freed); split it into a JS string[] like the *_names
  // getters. An unknown name yields an empty string -> empty array.
  const std::string name = info[0].As<Napi::String>().Utf8Value();
  const char* joined = sonare_mastering_insert_param_names(name.c_str());
  Napi::Array out = Napi::Array::New(env);
  if (joined == nullptr || joined[0] == '\0') {
    return out;
  }
  std::string names(joined);
  uint32_t index = 0;
  size_t start = 0;
  while (start <= names.size()) {
    size_t end = names.find('\n', start);
    if (end == std::string::npos) {
      out.Set(index++, Napi::String::New(env, names.substr(start)));
      break;
    }
    out.Set(index++, Napi::String::New(env, names.substr(start, end - start)));
    start = end + 1;
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringInsertParamInfo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (name: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // sonare_mastering_insert_param_info(name) returns a thread-local JSON array
  // string (NOT to be freed); "[]" for an unknown name. The TS facade parses it
  // into the typed MasteringInsertParamInfo[].
  const std::string name = info[0].As<Napi::String>().Utf8Value();
  const char* json = sonare_mastering_insert_param_info(name.c_str());
  return Napi::String::New(env, json != nullptr ? json : "[]");
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringInsertTiming(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (name: string, params: string, sampleRate: number)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  const std::string name = info[0].As<Napi::String>().Utf8Value();
  const std::string params = info[1].As<Napi::String>().Utf8Value();
  const int sample_rate = node_narrow_int(env, info[2], "sampleRate");
  int latency_samples = 0;
  int tail_samples = 0;
  const SonareError err = sonare_mastering_insert_timing(name.c_str(), params.c_str(), sample_rate,
                                                         &latency_samples, &tail_samples);
  if (err != SONARE_OK) {
    ThrowSonareError(env, err);
    return env.Undefined();
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("latencySamples", Napi::Number::New(env, latency_samples));
  out.Set("tailSamples", Napi::Number::New(env, tail_samples));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringProcessorCatalog(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  // sonare_mastering_processor_catalog() returns a program-lifetime JSON array
  // string (NOT to be freed). The TS facade parses it into the typed
  // MasteringProcessorCatalogEntry[].
  const char* json = sonare_mastering_processor_catalog();
  return Napi::String::New(env, json != nullptr ? json : "[]");
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::CapabilityCatalog(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  // The aggregate C API owns this program-lifetime JSON string; the TypeScript
  // facade parses it so each language receives the same catalog shape.
  const char* json = sonare_capability_catalog_json();
  if (json == nullptr) {
    Napi::Error::New(env, "Native capability catalog JSON is unavailable")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::String::New(env, json);
  SONARE_NODE_CATCH(env)
}
