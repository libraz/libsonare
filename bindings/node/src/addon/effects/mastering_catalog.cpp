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
  if (joined == nullptr) {
    ThrowLastSonareError(env, "mastering insert names: ", SONARE_ERROR_UNKNOWN);
    return env.Undefined();
  }
  Napi::Array out = Napi::Array::New(env);
  if (joined[0] == '\0') {
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
  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "name must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // sonare_mastering_insert_param_names(name) returns a thread-local '\n'-joined
  // const char* (NOT to be freed); split it into a JS string[] like the *_names
  // getters. An unknown name yields an empty string -> empty array.
  const std::string name = node_narrow_string(env, info[0], "name");
  const char* joined = sonare_mastering_insert_param_names(name.c_str());
  if (joined == nullptr) {
    ThrowLastSonareError(env, "mastering insert param names: ", SONARE_ERROR_UNKNOWN);
    return env.Undefined();
  }
  Napi::Array out = Napi::Array::New(env);
  if (joined[0] == '\0') {
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
  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "name must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // A JSON array ("[]" for an unknown name) the TS facade parses into the typed
  // MasteringInsertParamInfo[]. An omitted sample rate is the rate-less query (C rate 0).
  const std::string name = node_narrow_string(env, info[0], "name");
  int sample_rate = 0;
  const bool at_rate = !(info[1].IsUndefined() || info[1].IsNull());
  if (at_rate && !RequiredIntArg(env, info, 1, "sampleRate", &sample_rate)) {
    return env.Undefined();
  }
  // 0 is the C ABI's rate-less query, so a given 0 is refused here rather than read as absent.
  if (at_rate && sample_rate == 0) {
    Napi::RangeError::New(env, "sampleRate 0 is out of range").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  char* json = nullptr;
  const SonareError err = sonare_mastering_insert_param_info(name.c_str(), sample_rate, &json);
  if (err != SONARE_OK) {
    ThrowSonareError(env, err, "mastering insert param info: ");
    return env.Undefined();
  }
  std::string out(json);
  sonare_free_string(json);
  return Napi::String::New(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringInsertTiming(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "name must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsString()) {
    Napi::TypeError::New(env, "params must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  const std::string name = node_narrow_string(env, info[0], "name");
  const std::string params = node_narrow_string(env, info[1], "params");
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

Napi::Value SonareWrap::MasteringEnumValue(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!(info[0].IsString() || info[0].IsNull())) {
    Napi::TypeError::New(env, "processor must be a string or null").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsString()) {
    Napi::TypeError::New(env, "key must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsString()) {
    Napi::TypeError::New(env, "name must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  const std::string processor =
      info[0].IsNull() ? std::string() : node_narrow_string(env, info[0], "processor");
  const std::string key = node_narrow_string(env, info[1], "key");
  const std::string name = node_narrow_string(env, info[2], "name");
  int is_enum = 0;
  double value = 0.0;
  const SonareError err =
      sonare_mastering_enum_value(processor.c_str(), key.c_str(), name.c_str(), &is_enum, &value);
  if (err != SONARE_OK) {
    ThrowSonareError(env, err);
    return env.Undefined();
  }
  // null: the key is not an enum, which the facade refuses as a wrong type.
  return is_enum != 0 ? Napi::Value(Napi::Number::New(env, value)) : env.Null();
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringProcessorCatalog(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  // The TS facade parses the array into the typed MasteringProcessorCatalogEntry[].
  char* json = nullptr;
  const SonareError err = sonare_mastering_processor_catalog(&json);
  if (err != SONARE_OK) {
    ThrowSonareError(env, err, "mastering processor catalog: ");
    return env.Undefined();
  }
  std::string out(json);
  sonare_free_string(json);
  return Napi::String::New(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringAmpPresetCatalog(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  // The TS facade parses the array into the typed MasteringAmpPresetCatalogEntry[].
  char* json = nullptr;
  const SonareError err = sonare_mastering_amp_preset_catalog(&json);
  if (err != SONARE_OK) {
    ThrowSonareError(env, err, "mastering amp preset catalog: ");
    return env.Undefined();
  }
  std::string out(json);
  sonare_free_string(json);
  return Napi::String::New(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::CapabilityCatalog(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  // The TypeScript facade parses the document so each language receives the same catalog shape.
  char* json = nullptr;
  const SonareError err = sonare_capability_catalog_json(&json);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "capability catalog: ");
    return env.Undefined();
  }
  std::string out(json);
  sonare_free_string(json);
  return Napi::String::New(env, out);
  SONARE_NODE_CATCH(env)
}
