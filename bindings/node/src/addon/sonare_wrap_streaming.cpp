#include "sonare_wrap_streaming.h"

#include <cstring>
#include <string>
#include <vector>

#include "mastering/api/chain.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {

namespace {

// Flatten a nested or already-flat JS config object into dot-notation
// Param entries that sonare::mastering::api::parse_chain_config_params
// understands. Mirrors libsonare/python's _flatten_chain_config.
void FlattenChainConfig(const Napi::Object& object, const std::string& prefix,
                        std::vector<sonare::mastering::api::Param>* out) {
  Napi::Array names = object.GetPropertyNames();
  for (uint32_t index = 0; index < names.Length(); ++index) {
    Napi::Value key_value = names.Get(index);
    if (!key_value.IsString()) continue;
    std::string key = key_value.As<Napi::String>().Utf8Value();
    std::string full_key = prefix.empty() ? key : prefix + "." + key;

    Napi::Value value = object.Get(key_value);
    if (value.IsObject() && !value.IsArray() && !value.IsBuffer() && !value.IsTypedArray() &&
        !value.IsFunction()) {
      FlattenChainConfig(value.As<Napi::Object>(), full_key, out);
    } else if (value.IsNumber()) {
      out->push_back({full_key, value.As<Napi::Number>().DoubleValue()});
    } else if (value.IsBoolean()) {
      out->push_back({full_key, value.As<Napi::Boolean>().Value() ? 1.0 : 0.0});
    }
  }
}

std::vector<sonare::mastering::api::Param> ParseChainConfigFromJs(const Napi::Value& value) {
  std::vector<sonare::mastering::api::Param> params;
  if (!value.IsObject()) return params;
  FlattenChainConfig(value.As<Napi::Object>(), "", &params);
  return params;
}

}  // namespace

Napi::FunctionReference StreamingMasteringChainWrap::constructor_;

Napi::Object StreamingMasteringChainWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "StreamingMasteringChain",
      {
          InstanceMethod<&StreamingMasteringChainWrap::Prepare>("prepare"),
          InstanceMethod<&StreamingMasteringChainWrap::ProcessMono>("processMono"),
          InstanceMethod<&StreamingMasteringChainWrap::ProcessStereo>("processStereo"),
          InstanceMethod<&StreamingMasteringChainWrap::Reset>("reset"),
          InstanceMethod<&StreamingMasteringChainWrap::LatencySamples>("latencySamples"),
          InstanceMethod<&StreamingMasteringChainWrap::StageNames>("stageNames"),
      });

  constructor_ = Napi::Persistent(func);
  constructor_.SuppressDestruct();
  exports.Set("StreamingMasteringChain", func);
  return exports;
}

StreamingMasteringChainWrap::StreamingMasteringChainWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<StreamingMasteringChainWrap>(info) {
  Napi::Env env = info.Env();

  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 1 && info[0].IsObject()) {
    params = ParseChainConfigFromJs(info[0]);
  }

  try {
    auto config = sonare::mastering::api::parse_chain_config_params(params.data(), params.size());
    chain_ = std::make_unique<sonare::mastering::api::StreamingMasteringChain>(std::move(config));
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return;
  }
}

StreamingMasteringChainWrap::~StreamingMasteringChainWrap() = default;

Napi::Value StreamingMasteringChainWrap::Prepare(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!chain_) {
    Napi::Error::New(env, "StreamingMasteringChain is not initialized")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (sampleRate, maxBlockSize, numChannels)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  chain_->prepare(info[0].As<Napi::Number>().DoubleValue(), info[1].As<Napi::Number>().Int32Value(),
                  info[2].As<Napi::Number>().Int32Value());
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::ProcessMono(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!chain_) {
    Napi::Error::New(env, "StreamingMasteringChain is not initialized")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected (Float32Array)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array typed = info[0].As<Napi::Float32Array>();
  size_t length = typed.ElementLength();
  Napi::Float32Array out_arr = Napi::Float32Array::New(env, length);
  if (length > 0) {
    std::memcpy(out_arr.Data(), typed.Data(), length * sizeof(float));
    float* channels[] = {out_arr.Data()};
    chain_->process_block(channels, 1, static_cast<int>(length));
  }
  return out_arr;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::ProcessStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!chain_) {
    Napi::Error::New(env, "StreamingMasteringChain is not initialized")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "Expected (leftFloat32Array, rightFloat32Array)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array left = info[0].As<Napi::Float32Array>();
  Napi::Float32Array right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::TypeError::New(env, "left and right channel lengths must match")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  size_t length = left.ElementLength();
  Napi::Float32Array left_out = Napi::Float32Array::New(env, length);
  Napi::Float32Array right_out = Napi::Float32Array::New(env, length);
  if (length > 0) {
    std::memcpy(left_out.Data(), left.Data(), length * sizeof(float));
    std::memcpy(right_out.Data(), right.Data(), length * sizeof(float));
    float* channels[] = {left_out.Data(), right_out.Data()};
    chain_->process_block(channels, 2, static_cast<int>(length));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::Reset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!chain_) {
    Napi::Error::New(env, "StreamingMasteringChain is not initialized")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  chain_->reset();
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::LatencySamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!chain_) {
    return Napi::Number::New(env, 0);
  }
  return Napi::Number::New(env, chain_->latency_samples());
}

Napi::Value StreamingMasteringChainWrap::StageNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!chain_) {
    return Napi::Array::New(env, 0);
  }
  const auto& names = chain_->stage_names();
  Napi::Array out = Napi::Array::New(env, names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    out.Set(static_cast<uint32_t>(i), Napi::String::New(env, names[i]));
  }
  return out;
}

}  // namespace sonare_node
