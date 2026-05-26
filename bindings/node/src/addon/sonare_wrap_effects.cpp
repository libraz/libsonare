#include <cctype>
#include <stdexcept>
#include <string>
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
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/streaming_preview.h"
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

Napi::Value SonareWrap::PitchCorrectToMidi(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 4 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber() ||
      !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, currentMidi, targetMidi)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float current_midi = info[2].As<Napi::Number>().FloatValue();
  float target_midi = info[3].As<Napi::Number>().FloatValue();

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::editing::pitch_editor::PitchCorrector corrector;
  sonare::editing::pitch_editor::F0Track track;
  track.sample_rate = sr;
  track.hop_length = 512;
  track.f0_hz = {sonare::editing::pitch_editor::PitchCorrector::midi_to_hz(current_midi)};
  track.voiced = {true};
  track.voiced_prob = {1.0f};
  sonare::Audio result = corrector.correct_to_midi(audio, track, target_midi);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::NoteStretch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 5 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber() ||
      !info[3].IsNumber() || !info[4].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, sampleRate, onsetSample, offsetSample, "
                         "stretchRatio)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  int onset_sample = info[2].As<Napi::Number>().Int32Value();
  int offset_sample = info[3].As<Napi::Number>().Int32Value();
  float stretch_ratio = info[4].As<Napi::Number>().FloatValue();

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::editing::pitch_editor::NoteRegion region;
  region.onset_sample = onset_sample;
  region.offset_sample = offset_sample;
  sonare::editing::pitch_editor::NoteEditor editor;
  sonare::Audio result = editor.stretch_note(audio, region, stretch_ratio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::VoiceChange(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 4 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber() ||
      !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, pitchSemitones, formantFactor)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float pitch_semitones = info[2].As<Napi::Number>().FloatValue();
  float formant_factor = info[3].As<Napi::Number>().FloatValue();

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::editing::voice_changer::VoiceChangerConfig config;
  config.pitch_semitones = pitch_semitones;
  config.formant_factor = formant_factor;
  sonare::editing::voice_changer::VoiceChanger changer(config);
  sonare::Audio result = changer.process(audio);
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

namespace {

int PanModeValue(const Napi::Value& value) {
  if (value.IsNumber()) {
    return value.As<Napi::Number>().Int32Value();
  }
  if (!value.IsString()) {
    return SONARE_PAN_MODE_BALANCE;
  }
  std::string mode = value.As<Napi::String>().Utf8Value();
  for (char& ch : mode) {
    if (ch == '_') ch = '-';
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (mode == "stereo-pan" || mode == "stereopan" || mode == "pan") {
    return SONARE_PAN_MODE_STEREO_PAN;
  }
  if (mode == "dual-pan" || mode == "dualpan") {
    return SONARE_PAN_MODE_DUAL_PAN;
  }
  return SONARE_PAN_MODE_BALANCE;
}

Napi::Value OptionAt(Napi::Env env, const Napi::Object& options, const char* key, size_t index) {
  if (!options.Has(key)) {
    return env.Undefined();
  }
  Napi::Value value = options.Get(key);
  if (value.IsArray()) {
    return value.As<Napi::Array>().Get(index);
  }
  return value;
}

Napi::Object MixMeterToObject(Napi::Env env, const SonareMixMeterSnapshot& snapshot) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("peakDbL", snapshot.peak_db_l);
  out.Set("peakDbR", snapshot.peak_db_r);
  out.Set("rmsDbL", snapshot.rms_db_l);
  out.Set("rmsDbR", snapshot.rms_db_r);
  out.Set("correlation", snapshot.correlation);
  out.Set("monoCompatWidth", snapshot.mono_compat_width);
  out.Set("monoCompatPeak", snapshot.mono_compat_peak);
  out.Set("monoCompatSideRms", snapshot.mono_compat_side_rms);
  out.Set("likelyMonoCompatible", snapshot.likely_mono_compatible != 0);
  out.Set("momentaryLufs", snapshot.momentary_lufs);
  out.Set("shortTermLufs", snapshot.short_term_lufs);
  out.Set("integratedLufs", snapshot.integrated_lufs);
  out.Set("gainReductionDb", snapshot.gain_reduction_db);
  out.Set("truePeakDbL", snapshot.true_peak_db_l);
  out.Set("truePeakDbR", snapshot.true_peak_db_r);
  out.Set("maxTruePeakDb", snapshot.max_true_peak_db);
  out.Set("seq", Napi::Number::New(env, static_cast<double>(snapshot.seq)));
  return out;
}

}  // namespace

Napi::Value SonareWrap::MixingScenePresetNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const char* raw = sonare_mixing_scene_preset_names();
  Napi::Array out = Napi::Array::New(env);
  if (raw == nullptr || raw[0] == '\0') {
    return out;
  }
  std::string names(raw);
  size_t index = 0;
  size_t start = 0;
  while (start <= names.size()) {
    const size_t end = names.find('\n', start);
    out.Set(index++,
            names.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

Napi::Value SonareWrap::MixingScenePresetJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected preset name").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  char* json = nullptr;
  SonareError err =
      sonare_mixing_scene_preset_json(info[0].As<Napi::String>().Utf8Value().c_str(), &json);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string result = json != nullptr ? json : "";
  sonare_free_string(json);
  return Napi::String::New(env, result);
}

Napi::Value SonareWrap::MixStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[0].IsArray() || !info[1].IsArray() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (leftChannels, rightChannels, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array left_input = info[0].As<Napi::Array>();
  Napi::Array right_input = info[1].As<Napi::Array>();
  const size_t count = left_input.Length();
  if (count == 0 || right_input.Length() != count) {
    Napi::TypeError::New(env, "leftChannels and rightChannels must have the same non-zero length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::vector<Napi::Float32Array> left_arrays;
  std::vector<Napi::Float32Array> right_arrays;
  std::vector<const float*> left_ptrs;
  std::vector<const float*> right_ptrs;
  left_arrays.reserve(count);
  right_arrays.reserve(count);
  left_ptrs.reserve(count);
  right_ptrs.reserve(count);

  size_t length = 0;
  for (size_t index = 0; index < count; ++index) {
    Napi::Value left_value = left_input.Get(index);
    Napi::Value right_value = right_input.Get(index);
    if (!IsFloat32Array(left_value) || !IsFloat32Array(right_value)) {
      Napi::TypeError::New(env, "all channels must be Float32Array").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    left_arrays.push_back(left_value.As<Napi::Float32Array>());
    right_arrays.push_back(right_value.As<Napi::Float32Array>());
    if (left_arrays.back().ElementLength() != right_arrays.back().ElementLength()) {
      Napi::TypeError::New(env, "left and right channel lengths must match")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    if (index == 0) {
      length = left_arrays.back().ElementLength();
    } else if (left_arrays.back().ElementLength() != length) {
      Napi::TypeError::New(env, "all strips must have the same length")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    left_ptrs.push_back(left_arrays.back().Data());
    right_ptrs.push_back(right_arrays.back().Data());
  }

  const int sample_rate = info[2].As<Napi::Number>().Int32Value();
  Napi::Object options = info.Length() >= 4 && info[3].IsObject() ? info[3].As<Napi::Object>()
                                                                  : Napi::Object::New(env);

  SonareMixer* mixer =
      sonare_mixer_create(sample_rate, static_cast<int>(std::max<size_t>(1, length)));
  if (mixer == nullptr) {
    Napi::Error::New(env, "failed to create mixer").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::vector<SonareStrip*> strips;
  std::vector<float> out_left(length, 0.0f);
  std::vector<float> out_right(length, 0.0f);
  try {
    for (size_t index = 0; index < count; ++index) {
      SonareStrip* strip = sonare_mixer_add_strip(mixer, ("strip" + std::to_string(index)).c_str());
      if (strip == nullptr) {
        throw std::runtime_error("failed to add mixer strip");
      }
      strips.push_back(strip);
      Napi::Value inputTrim = OptionAt(env, options, "inputTrimDb", index);
      if (inputTrim.IsNumber()) {
        SonareError err =
            sonare_strip_set_input_trim_db(strip, inputTrim.As<Napi::Number>().FloatValue());
        if (err != SONARE_OK) throw std::runtime_error(ErrorMessageForCode(err));
      }
      Napi::Value fader = OptionAt(env, options, "faderDb", index);
      if (fader.IsNumber()) {
        SonareError err = sonare_strip_set_fader_db(strip, fader.As<Napi::Number>().FloatValue());
        if (err != SONARE_OK) throw std::runtime_error(ErrorMessageForCode(err));
      }
      Napi::Value pan = OptionAt(env, options, "pan", index);
      if (pan.IsNumber()) {
        Napi::Value mode = OptionAt(env, options, "panMode", index);
        SonareError err =
            sonare_strip_set_pan(strip, pan.As<Napi::Number>().FloatValue(), PanModeValue(mode));
        if (err != SONARE_OK) throw std::runtime_error(ErrorMessageForCode(err));
      }
      Napi::Value width = OptionAt(env, options, "width", index);
      if (width.IsNumber()) {
        SonareError err = sonare_strip_set_width(strip, width.As<Napi::Number>().FloatValue());
        if (err != SONARE_OK) throw std::runtime_error(ErrorMessageForCode(err));
      }
      Napi::Value muted = OptionAt(env, options, "muted", index);
      if (muted.IsBoolean()) {
        SonareError err = sonare_strip_set_muted(strip, muted.As<Napi::Boolean>().Value() ? 1 : 0);
        if (err != SONARE_OK) throw std::runtime_error(ErrorMessageForCode(err));
      }
    }

    SonareError err = sonare_mixer_process_stereo(mixer, left_ptrs.data(), right_ptrs.data(), count,
                                                  out_left.data(), out_right.data(), length);
    if (err != SONARE_OK) throw std::runtime_error(ErrorMessageForCode(err));

    Napi::Array meters = Napi::Array::New(env, strips.size());
    for (size_t index = 0; index < strips.size(); ++index) {
      SonareMixMeterSnapshot snapshot{};
      err = sonare_strip_meter(strips[index], &snapshot);
      if (err != SONARE_OK) throw std::runtime_error(ErrorMessageForCode(err));
      meters.Set(index, MixMeterToObject(env, snapshot));
    }

    Napi::Object out = Napi::Object::New(env);
    out.Set("left", VecToFloat32(env, out_left));
    out.Set("right", VecToFloat32(env, out_right));
    out.Set("sampleRate", sample_rate);
    out.Set("meters", meters);
    sonare_mixer_destroy(mixer);
    return out;
  } catch (const std::exception& e) {
    sonare_mixer_destroy(mixer);
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return env.Undefined();
  }
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
