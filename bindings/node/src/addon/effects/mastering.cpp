#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "editing/pitch_editor/note_editor.h"
#include "editing/pitch_editor/pitch_corrector.h"
#include "editing/voice_changer/voice_changer.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "mastering/api/chain.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/assistant/suggester.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/streaming_preview.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

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

namespace {

// Local copy of VecToFloat32 (the one on SonareWrap is private and the async
// workers are not friends of SonareWrap).
Napi::Float32Array VecToTypedArray(Napi::Env env, const std::vector<float>& vec) {
  Napi::Float32Array array = Napi::Float32Array::New(env, vec.size());
  if (!vec.empty()) {
    std::memcpy(array.Data(), vec.data(), vec.size() * sizeof(float));
  }
  return array;
}

// Helper: serialise a MonoChainResult into a JS object on the main thread.
Napi::Object MonoResultToObject(Napi::Env env,
                                const sonare::mastering::api::MonoChainResult& result) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", VecToTypedArray(env, result.samples));
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
}

// Off-main-thread mono master_audio. Copies samples + overrides into the
// worker so the JS thread can release its Float32Array view.
class MasterAudioAsyncWorker : public Napi::AsyncWorker {
 public:
  MasterAudioAsyncWorker(Napi::Env env, std::string preset_name, std::vector<float> samples,
                         int sample_rate, std::vector<sonare::mastering::api::Param> overrides)
      : Napi::AsyncWorker(env),
        deferred_(Napi::Promise::Deferred::New(env)),
        preset_name_(std::move(preset_name)),
        samples_(std::move(samples)),
        sample_rate_(sample_rate),
        overrides_(std::move(overrides)) {}

  void Execute() override {
    try {
      auto preset = sonare::mastering::api::preset_from_string(preset_name_);
      result_ = sonare::mastering::api::master_audio_mono(preset, samples_.data(), samples_.size(),
                                                          sample_rate_, overrides_.data(),
                                                          overrides_.size());
    } catch (const std::exception& e) {
      SetError(e.what());
    }
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    deferred_.Resolve(MonoResultToObject(Env(), result_));
  }

  void OnError(const Napi::Error& error) override {
    Napi::HandleScope scope(Env());
    deferred_.Reject(error.Value());
  }

  Napi::Promise GetPromise() { return deferred_.Promise(); }

 private:
  Napi::Promise::Deferred deferred_;
  std::string preset_name_;
  std::vector<float> samples_;
  int sample_rate_;
  std::vector<sonare::mastering::api::Param> overrides_;
  sonare::mastering::api::MonoChainResult result_;
};

class MasterAudioStereoAsyncWorker : public Napi::AsyncWorker {
 public:
  MasterAudioStereoAsyncWorker(Napi::Env env, std::string preset_name, std::vector<float> left,
                               std::vector<float> right, int sample_rate,
                               std::vector<sonare::mastering::api::Param> overrides)
      : Napi::AsyncWorker(env),
        deferred_(Napi::Promise::Deferred::New(env)),
        preset_name_(std::move(preset_name)),
        left_(std::move(left)),
        right_(std::move(right)),
        sample_rate_(sample_rate),
        overrides_(std::move(overrides)) {}

  void Execute() override {
    try {
      auto preset = sonare::mastering::api::preset_from_string(preset_name_);
      result_ = sonare::mastering::api::master_audio_stereo(preset, left_.data(), right_.data(),
                                                            left_.size(), sample_rate_,
                                                            overrides_.data(), overrides_.size());
    } catch (const std::exception& e) {
      SetError(e.what());
    }
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    Napi::Object out = Napi::Object::New(Env());
    out.Set("left", VecToTypedArray(Env(), result_.left));
    out.Set("right", VecToTypedArray(Env(), result_.right));
    out.Set("sampleRate", Napi::Number::New(Env(), result_.sample_rate));
    out.Set("inputLufs", Napi::Number::New(Env(), result_.input_lufs));
    out.Set("outputLufs", Napi::Number::New(Env(), result_.output_lufs));
    out.Set("appliedGainDb", Napi::Number::New(Env(), result_.applied_gain_db));
    Napi::Array stages = Napi::Array::New(Env(), result_.stages.size());
    for (size_t i = 0; i < result_.stages.size(); ++i) {
      stages.Set(static_cast<uint32_t>(i), Napi::String::New(Env(), result_.stages[i]));
    }
    out.Set("stages", stages);
    deferred_.Resolve(out);
  }

  void OnError(const Napi::Error& error) override {
    Napi::HandleScope scope(Env());
    deferred_.Reject(error.Value());
  }

  Napi::Promise GetPromise() { return deferred_.Promise(); }

 private:
  Napi::Promise::Deferred deferred_;
  std::string preset_name_;
  std::vector<float> left_;
  std::vector<float> right_;
  int sample_rate_;
  std::vector<sonare::mastering::api::Param> overrides_;
  sonare::mastering::api::StereoChainResult result_;
};

}  // namespace

Napi::Value SonareWrap::MasterAudioAsync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[0].IsString() || !IsFloat32Array(info[1]) || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (presetName, Float32Array, sampleRate, overrides?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string preset_name = info[0].As<Napi::String>().Utf8Value();
  auto typed = info[1].As<Napi::Float32Array>();
  std::vector<float> samples(typed.Data(), typed.Data() + typed.ElementLength());
  int sample_rate = info[2].As<Napi::Number>().Int32Value();
  std::vector<sonare::mastering::api::Param> overrides;
  if (info.Length() >= 4 && info[3].IsObject()) {
    overrides = ParamsFromObject(info[3].As<Napi::Object>());
  }
  auto* worker = new MasterAudioAsyncWorker(env, std::move(preset_name), std::move(samples),
                                            sample_rate, std::move(overrides));
  Napi::Promise promise = worker->GetPromise();
  worker->Queue();
  return promise;
}

Napi::Value SonareWrap::MasterAudioStereoAsync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !info[0].IsString() || !IsFloat32Array(info[1]) ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (presetName, left, right, sampleRate, overrides?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string preset_name = info[0].As<Napi::String>().Utf8Value();
  auto left_typed = info[1].As<Napi::Float32Array>();
  auto right_typed = info[2].As<Napi::Float32Array>();
  if (left_typed.ElementLength() != right_typed.ElementLength()) {
    Napi::TypeError::New(env, "left and right channel lengths must match")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::vector<float> left(left_typed.Data(), left_typed.Data() + left_typed.ElementLength());
  std::vector<float> right(right_typed.Data(), right_typed.Data() + right_typed.ElementLength());
  int sample_rate = info[3].As<Napi::Number>().Int32Value();
  std::vector<sonare::mastering::api::Param> overrides;
  if (info.Length() >= 5 && info[4].IsObject()) {
    overrides = ParamsFromObject(info[4].As<Napi::Object>());
  }
  auto* worker =
      new MasterAudioStereoAsyncWorker(env, std::move(preset_name), std::move(left),
                                       std::move(right), sample_rate, std::move(overrides));
  Napi::Promise promise = worker->GetPromise();
  worker->Queue();
  return promise;
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

Napi::Value SonareWrap::MasteringAssistantSuggest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (samples, sampleRate, params?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto samples = info[0].As<Napi::Float32Array>();
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 3 && info[2].IsObject())
    params = ParamsFromObject(info[2].As<Napi::Object>());
  sonare::mastering::assistant::AssistantConfig config;
  for (const auto& param : params) {
    if (param.key == "targetLufs" || param.key == "target_lufs") {
      config.target_lufs = static_cast<float>(param.value);
    } else if (param.key == "ceilingDb" || param.key == "ceiling_db") {
      config.ceiling_db = static_cast<float>(param.value);
    } else if (param.key == "enableRepair" || param.key == "enable_repair") {
      config.enable_repair = param.value != 0.0;
    } else if (param.key == "preferStreamingSafe" || param.key == "prefer_streaming_safe") {
      config.prefer_streaming_safe = param.value != 0.0;
    } else if (param.key == "speechMonoAmount" || param.key == "speech_mono_amount") {
      config.speech_mono_amount = static_cast<float>(param.value);
    }
  }
  const auto result = sonare::mastering::assistant::suggest_chain(
      samples.Data(), samples.ElementLength(), info[1].As<Napi::Number>().Int32Value(), config);
  return Napi::String::New(env, sonare::mastering::assistant::assistant_result_to_json(result));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringAudioProfile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (samples, sampleRate, params?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto samples = info[0].As<Napi::Float32Array>();
  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 3 && info[2].IsObject())
    params = ParamsFromObject(info[2].As<Napi::Object>());
  sonare::mastering::assistant::AudioProfileConfig config;
  for (const auto& param : params) {
    if (param.key == "nFft" || param.key == "n_fft") {
      config.n_fft = static_cast<int>(param.value);
    } else if (param.key == "hopLength" || param.key == "hop_length") {
      config.hop_length = static_cast<int>(param.value);
    } else if (param.key == "truePeakOversample" || param.key == "true_peak_oversample") {
      config.true_peak_oversample = static_cast<int>(param.value);
    }
  }
  const auto profile = sonare::mastering::assistant::analyze_audio_profile(
      samples.Data(), samples.ElementLength(), info[1].As<Napi::Number>().Int32Value(), config);
  return Napi::String::New(env, sonare::mastering::assistant::audio_profile_to_json(profile));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringStreamingPreview(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (samples, sampleRate, platforms?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto samples = info[0].As<Napi::Float32Array>();
  std::vector<sonare::mastering::maximizer::StreamingPlatform> platforms;
  if (info.Length() >= 3 && info[2].IsArray()) {
    Napi::Array input = info[2].As<Napi::Array>();
    platforms.reserve(input.Length());
    for (uint32_t index = 0; index < input.Length(); ++index) {
      Napi::Value value = input.Get(index);
      if (!value.IsObject()) {
        Napi::TypeError::New(env, "platforms entries must be objects").ThrowAsJavaScriptException();
        return env.Undefined();
      }
      Napi::Object object = value.As<Napi::Object>();
      if (!object.Get("name").IsString() || !object.Get("targetLufs").IsNumber() ||
          !object.Get("ceilingDb").IsNumber()) {
        Napi::TypeError::New(env, "platforms entries require name, targetLufs, ceilingDb")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      platforms.push_back({object.Get("name").As<Napi::String>().Utf8Value(),
                           object.Get("targetLufs").As<Napi::Number>().FloatValue(),
                           object.Get("ceilingDb").As<Napi::Number>().FloatValue()});
    }
  }
  const sonare::Audio audio = sonare::Audio::from_buffer(samples.Data(), samples.ElementLength(),
                                                         info[1].As<Napi::Number>().Int32Value());
  const auto results = platforms.empty()
                           ? sonare::mastering::maximizer::streaming_preview(audio)
                           : sonare::mastering::maximizer::streaming_preview(audio, platforms);
  return Napi::String::New(env, sonare::mastering::maximizer::streaming_preview_to_json(results));
  SONARE_NODE_CATCH(env)
}
