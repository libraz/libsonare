#include <string>
#include <vector>

#include "core/audio.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "mastering/api/chain.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "sonare_wrap.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

Napi::Value SonareWrap::Hpss(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  int kernel_harmonic =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 31;
  int kernel_percussive =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 31;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);

  sonare::HpssConfig config;
  config.kernel_size_harmonic = kernel_harmonic;
  config.kernel_size_percussive = kernel_percussive;

  sonare::HpssAudioResult result = sonare::hpss(audio, config);

  Napi::Object out = Napi::Object::New(env);

  std::vector<float> harmonic_vec(result.harmonic.data(),
                                  result.harmonic.data() + result.harmonic.size());
  out.Set("harmonic", VecToFloat32(env, harmonic_vec));

  std::vector<float> percussive_vec(result.percussive.data(),
                                    result.percussive.data() + result.percussive.size());
  out.Set("percussive", VecToFloat32(env, percussive_vec));

  out.Set("sampleRate", Napi::Number::New(env, result.harmonic.sample_rate()));

  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Harmonic(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::harmonic(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Percussive(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::percussive(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::TimeStretch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, rate)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float rate = info[2].As<Napi::Number>().FloatValue();

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::time_stretch(audio, rate);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PitchShift(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, semitones)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float semitones = info[2].As<Napi::Number>().FloatValue();

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::pitch_shift(audio, semitones);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Normalize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float target_db =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 0.0f;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::normalize(audio, target_db);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Mastering(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();

  sonare::mastering::maximizer::LoudnessOptimizeConfig config;
  config.target_lufs =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : -14.0f;
  config.ceiling_db =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().FloatValue() : -1.0f;
  config.true_peak_oversample =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 4;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  auto result = sonare::mastering::maximizer::loudness_optimize(audio, config);
  std::vector<float> out_vec(result.audio.data(), result.audio.data() + result.audio.size());

  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", VecToFloat32(env, out_vec));
  out.Set("sampleRate", Napi::Number::New(env, result.audio.sample_rate()));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  return out;
  SONARE_NODE_CATCH(env)
}

namespace {

std::vector<sonare::mastering::api::Param> ParamsFromObject(const Napi::Object& object) {
  std::vector<sonare::mastering::api::Param> params;
  Napi::Array names = object.GetPropertyNames();
  for (uint32_t index = 0; index < names.Length(); ++index) {
    Napi::Value key_value = names.Get(index);
    Napi::Value value = object.Get(key_value);
    if (key_value.IsString() && value.IsNumber()) {
      params.push_back(
          {key_value.As<Napi::String>().Utf8Value(), value.As<Napi::Number>().DoubleValue()});
    } else if (key_value.IsString() && value.IsBoolean()) {
      params.push_back({key_value.As<Napi::String>().Utf8Value(),
                        value.As<Napi::Boolean>().Value() ? 1.0 : 0.0});
    }
  }
  return params;
}

}  // namespace

Napi::Value SonareWrap::MasteringProcess(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[0].IsString() || !IsFloat32Array(info[1]) || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (processorName, Float32Array, sampleRate, params?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto name = info[0].As<Napi::String>().Utf8Value();
  auto typed = info[1].As<Napi::Float32Array>();
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 4 && info[3].IsObject()) {
    params = ParamsFromObject(info[3].As<Napi::Object>());
  }
  auto result = sonare::mastering::api::apply_named_processor(
      name, typed.Data(), typed.ElementLength(), info[2].As<Napi::Number>().Int32Value(), params);
  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", VecToFloat32(env, result.samples));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  out.Set("latencySamples", Napi::Number::New(env, result.latency_samples));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringProcessStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !info[0].IsString() || !IsFloat32Array(info[1]) ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (processorName, left, right, sampleRate, params?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto name = info[0].As<Napi::String>().Utf8Value();
  auto left = info[1].As<Napi::Float32Array>();
  auto right = info[2].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::TypeError::New(env, "left and right channel lengths must match")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 5 && info[4].IsObject()) {
    params = ParamsFromObject(info[4].As<Napi::Object>());
  }
  auto result = sonare::mastering::api::apply_named_processor_stereo(
      name, left.Data(), right.Data(), left.ElementLength(),
      info[3].As<Napi::Number>().Int32Value(), params);
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", VecToFloat32(env, result.left));
  out.Set("right", VecToFloat32(env, result.right));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  out.Set("latencySamples", Napi::Number::New(env, result.latency_samples));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringChain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, config?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 3 && info[2].IsObject()) {
    params = ParamsFromObject(info[2].As<Napi::Object>());
  }
  auto result = sonare::mastering::api::run_chain_mono_params(
      params.data(), params.size(), typed.Data(), typed.ElementLength(),
      info[1].As<Napi::Number>().Int32Value());
  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", VecToFloat32(env, result.samples));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  Napi::Array stages = Napi::Array::New(env, result.stages.size());
  for (size_t i = 0; i < result.stages.size(); ++i) {
    stages.Set(static_cast<uint32_t>(i), Napi::String::New(env, result.stages[i]));
  }
  out.Set("stages", stages);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringChainStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1]) ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (left, right, sampleRate, config?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::TypeError::New(env, "left and right channel lengths must match")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 4 && info[3].IsObject()) {
    params = ParamsFromObject(info[3].As<Napi::Object>());
  }
  auto result = sonare::mastering::api::run_chain_stereo_params(
      params.data(), params.size(), left.Data(), right.Data(), left.ElementLength(),
      info[2].As<Napi::Number>().Int32Value());
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", VecToFloat32(env, result.left));
  out.Set("right", VecToFloat32(env, result.right));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  Napi::Array stages = Napi::Array::New(env, result.stages.size());
  for (size_t i = 0; i < result.stages.size(); ++i) {
    stages.Set(static_cast<uint32_t>(i), Napi::String::New(env, result.stages[i]));
  }
  out.Set("stages", stages);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringPresetNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  auto names = sonare::mastering::api::preset_names();
  Napi::Array out = Napi::Array::New(env, names.size());
  for (size_t index = 0; index < names.size(); ++index) {
    out.Set(index, names[index]);
  }
  return out;
}

Napi::Value SonareWrap::MasterAudio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[0].IsString() || !IsFloat32Array(info[1]) || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (presetName, Float32Array, sampleRate, overrides?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::string preset_name = info[0].As<Napi::String>().Utf8Value();
  auto typed = info[1].As<Napi::Float32Array>();
  std::vector<sonare::mastering::api::Param> overrides;
  if (info.Length() >= 4 && info[3].IsObject()) {
    overrides = ParamsFromObject(info[3].As<Napi::Object>());
  }
  auto preset = sonare::mastering::api::preset_from_string(preset_name);
  auto result = sonare::mastering::api::master_audio_mono(
      preset, typed.Data(), typed.ElementLength(), info[2].As<Napi::Number>().Int32Value(),
      overrides.data(), overrides.size());
  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", VecToFloat32(env, result.samples));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  Napi::Array stages = Napi::Array::New(env, result.stages.size());
  for (size_t i = 0; i < result.stages.size(); ++i) {
    stages.Set(static_cast<uint32_t>(i), Napi::String::New(env, result.stages[i]));
  }
  out.Set("stages", stages);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasterAudioStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !info[0].IsString() || !IsFloat32Array(info[1]) ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (presetName, left, right, sampleRate, overrides?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::string preset_name = info[0].As<Napi::String>().Utf8Value();
  auto left = info[1].As<Napi::Float32Array>();
  auto right = info[2].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::TypeError::New(env, "left and right channel lengths must match")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<sonare::mastering::api::Param> overrides;
  if (info.Length() >= 5 && info[4].IsObject()) {
    overrides = ParamsFromObject(info[4].As<Napi::Object>());
  }
  auto preset = sonare::mastering::api::preset_from_string(preset_name);
  auto result = sonare::mastering::api::master_audio_stereo(
      preset, left.Data(), right.Data(), left.ElementLength(),
      info[3].As<Napi::Number>().Int32Value(), overrides.data(), overrides.size());
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", VecToFloat32(env, result.left));
  out.Set("right", VecToFloat32(env, result.right));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  Napi::Array stages = Napi::Array::New(env, result.stages.size());
  for (size_t i = 0; i < result.stages.size(); ++i) {
    stages.Set(static_cast<uint32_t>(i), Napi::String::New(env, result.stages[i]));
  }
  out.Set("stages", stages);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringChainWithProgress(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsObject() ||
      !info[3].IsFunction()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, config, onProgress)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  auto params = ParamsFromObject(info[2].As<Napi::Object>());
  Napi::Function js_cb = info[3].As<Napi::Function>();
  auto config = sonare::mastering::api::parse_chain_config_params(params.data(), params.size());
  sonare::mastering::api::MasteringChain chain(std::move(config));
  // process_mono is synchronous, so js_cb (referenced by info[3]) outlives the call.
  chain.set_progress_callback([&js_cb, &env](float progress, const char* stage) {
    js_cb.Call({Napi::Number::New(env, progress), Napi::String::New(env, stage ? stage : "")});
  });
  auto result = chain.process_mono(typed.Data(), typed.ElementLength(),
                                   info[1].As<Napi::Number>().Int32Value());
  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", VecToFloat32(env, result.samples));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  Napi::Array stages = Napi::Array::New(env, result.stages.size());
  for (size_t i = 0; i < result.stages.size(); ++i) {
    stages.Set(static_cast<uint32_t>(i), Napi::String::New(env, result.stages[i]));
  }
  out.Set("stages", stages);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringChainStereoWithProgress(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 5 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1]) ||
      !info[2].IsNumber() || !info[3].IsObject() || !info[4].IsFunction()) {
    Napi::TypeError::New(env, "Expected (left, right, sampleRate, config, onProgress)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::TypeError::New(env, "left and right channel lengths must match")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto params = ParamsFromObject(info[3].As<Napi::Object>());
  Napi::Function js_cb = info[4].As<Napi::Function>();
  auto config = sonare::mastering::api::parse_chain_config_params(params.data(), params.size());
  sonare::mastering::api::MasteringChain chain(std::move(config));
  chain.set_progress_callback([&js_cb, &env](float progress, const char* stage) {
    js_cb.Call({Napi::Number::New(env, progress), Napi::String::New(env, stage ? stage : "")});
  });
  auto result = chain.process_stereo(left.Data(), right.Data(), left.ElementLength(),
                                     info[2].As<Napi::Number>().Int32Value());
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", VecToFloat32(env, result.left));
  out.Set("right", VecToFloat32(env, result.right));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  Napi::Array stages = Napi::Array::New(env, result.stages.size());
  for (size_t i = 0; i < result.stages.size(); ++i) {
    stages.Set(static_cast<uint32_t>(i), Napi::String::New(env, result.stages[i]));
  }
  out.Set("stages", stages);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasterAudioWithProgress(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 5 || !info[0].IsString() || !IsFloat32Array(info[1]) || !info[2].IsNumber() ||
      !info[3].IsObject() || !info[4].IsFunction()) {
    Napi::TypeError::New(env,
                         "Expected (presetName, Float32Array, sampleRate, overrides, onProgress)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::string preset_name = info[0].As<Napi::String>().Utf8Value();
  auto typed = info[1].As<Napi::Float32Array>();
  auto overrides = ParamsFromObject(info[3].As<Napi::Object>());
  Napi::Function js_cb = info[4].As<Napi::Function>();
  auto preset = sonare::mastering::api::preset_from_string(preset_name);
  auto config = sonare::mastering::api::preset_config(preset);
  if (!overrides.empty()) {
    sonare::mastering::api::apply_chain_config_overrides(config, overrides.data(),
                                                         overrides.size());
  }
  sonare::mastering::api::MasteringChain chain(std::move(config));
  chain.set_progress_callback([&js_cb, &env](float progress, const char* stage) {
    js_cb.Call({Napi::Number::New(env, progress), Napi::String::New(env, stage ? stage : "")});
  });
  auto result = chain.process_mono(typed.Data(), typed.ElementLength(),
                                   info[2].As<Napi::Number>().Int32Value());
  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", VecToFloat32(env, result.samples));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  Napi::Array stages = Napi::Array::New(env, result.stages.size());
  for (size_t i = 0; i < result.stages.size(); ++i) {
    stages.Set(static_cast<uint32_t>(i), Napi::String::New(env, result.stages[i]));
  }
  out.Set("stages", stages);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasterAudioStereoWithProgress(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 6 || !info[0].IsString() || !IsFloat32Array(info[1]) ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber() || !info[4].IsObject() ||
      !info[5].IsFunction()) {
    Napi::TypeError::New(env,
                         "Expected (presetName, left, right, sampleRate, overrides, onProgress)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::string preset_name = info[0].As<Napi::String>().Utf8Value();
  auto left = info[1].As<Napi::Float32Array>();
  auto right = info[2].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::TypeError::New(env, "left and right channel lengths must match")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto overrides = ParamsFromObject(info[4].As<Napi::Object>());
  Napi::Function js_cb = info[5].As<Napi::Function>();
  auto preset = sonare::mastering::api::preset_from_string(preset_name);
  auto config = sonare::mastering::api::preset_config(preset);
  if (!overrides.empty()) {
    sonare::mastering::api::apply_chain_config_overrides(config, overrides.data(),
                                                         overrides.size());
  }
  sonare::mastering::api::MasteringChain chain(std::move(config));
  chain.set_progress_callback([&js_cb, &env](float progress, const char* stage) {
    js_cb.Call({Napi::Number::New(env, progress), Napi::String::New(env, stage ? stage : "")});
  });
  auto result = chain.process_stereo(left.Data(), right.Data(), left.ElementLength(),
                                     info[3].As<Napi::Number>().Int32Value());
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", VecToFloat32(env, result.left));
  out.Set("right", VecToFloat32(env, result.right));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  Napi::Array stages = Napi::Array::New(env, result.stages.size());
  for (size_t i = 0; i < result.stages.size(); ++i) {
    stages.Set(static_cast<uint32_t>(i), Napi::String::New(env, result.stages[i]));
  }
  out.Set("stages", stages);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringProcessorNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  auto names = sonare::mastering::api::processor_names();
  Napi::Array out = Napi::Array::New(env, names.size());
  for (size_t index = 0; index < names.size(); ++index) {
    out.Set(index, names[index]);
  }
  return out;
}

Napi::Value SonareWrap::MasteringPairProcessorNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  auto names = sonare::mastering::api::pair_processor_names();
  Napi::Array out = Napi::Array::New(env, names.size());
  for (size_t index = 0; index < names.size(); ++index) {
    out.Set(index, names[index]);
  }
  return out;
}

Napi::Value SonareWrap::MasteringPairAnalysisNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  auto names = sonare::mastering::api::pair_analysis_names();
  Napi::Array out = Napi::Array::New(env, names.size());
  for (size_t index = 0; index < names.size(); ++index) {
    out.Set(index, names[index]);
  }
  return out;
}

Napi::Value SonareWrap::MasteringStereoAnalysisNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  auto names = sonare::mastering::api::stereo_analysis_names();
  Napi::Array out = Napi::Array::New(env, names.size());
  for (size_t index = 0; index < names.size(); ++index) {
    out.Set(index, names[index]);
  }
  return out;
}

Napi::Value SonareWrap::MasteringPairProcess(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !info[0].IsString() || !IsFloat32Array(info[1]) ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (processorName, source, reference, sampleRate, params?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto source = info[1].As<Napi::Float32Array>();
  auto reference = info[2].As<Napi::Float32Array>();
  if (source.ElementLength() != reference.ElementLength()) {
    Napi::TypeError::New(env, "source and reference lengths must match")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 5 && info[4].IsObject())
    params = ParamsFromObject(info[4].As<Napi::Object>());
  auto result = sonare::mastering::api::apply_named_pair_processor(
      info[0].As<Napi::String>().Utf8Value(), source.Data(), reference.Data(),
      source.ElementLength(), info[3].As<Napi::Number>().Int32Value(), params);
  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", VecToFloat32(env, result.samples));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("inputLufs", Napi::Number::New(env, result.input_lufs));
  out.Set("outputLufs", Napi::Number::New(env, result.output_lufs));
  out.Set("appliedGainDb", Napi::Number::New(env, result.applied_gain_db));
  out.Set("latencySamples", Napi::Number::New(env, result.latency_samples));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringPairAnalyze(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !info[0].IsString() || !IsFloat32Array(info[1]) ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (analysisName, source, reference, sampleRate, params?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto source = info[1].As<Napi::Float32Array>();
  auto reference = info[2].As<Napi::Float32Array>();
  if (source.ElementLength() != reference.ElementLength()) {
    Napi::TypeError::New(env, "source and reference lengths must match")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 5 && info[4].IsObject())
    params = ParamsFromObject(info[4].As<Napi::Object>());
  auto json = sonare::mastering::api::analyze_named_pair(
      info[0].As<Napi::String>().Utf8Value(), source.Data(), reference.Data(),
      source.ElementLength(), info[3].As<Napi::Number>().Int32Value(), params);
  return Napi::String::New(env, json);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringStereoAnalyze(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !info[0].IsString() || !IsFloat32Array(info[1]) ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (analysisName, left, right, sampleRate, params?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto left = info[1].As<Napi::Float32Array>();
  auto right = info[2].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::TypeError::New(env, "left and right lengths must match").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 5 && info[4].IsObject())
    params = ParamsFromObject(info[4].As<Napi::Object>());
  auto json = sonare::mastering::api::analyze_named_stereo(
      info[0].As<Napi::String>().Utf8Value(), left.Data(), right.Data(), left.ElementLength(),
      info[3].As<Napi::Number>().Int32Value(), params);
  return Napi::String::New(env, json);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Trim(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float threshold_db =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : -60.0f;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::trim(audio, threshold_db);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}
