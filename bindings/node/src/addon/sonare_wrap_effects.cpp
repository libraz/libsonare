#include <algorithm>
#include <cctype>
#include <cstring>
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

namespace {

int repair_int_option(const Napi::Object& object, const char* key, int fallback) {
  Napi::Value value = object.Get(key);
  return value.IsNumber() ? value.As<Napi::Number>().Int32Value() : fallback;
}

float repair_float_option(const Napi::Object& object, const char* key, float fallback) {
  Napi::Value value = object.Get(key);
  return value.IsNumber() ? value.As<Napi::Number>().FloatValue() : fallback;
}

bool repair_bool_option(const Napi::Object& object, const char* key, bool fallback) {
  Napi::Value value = object.Get(key);
  return value.IsBoolean() ? value.As<Napi::Boolean>().Value() : fallback;
}

sonare::mastering::repair::DenoiseMode parse_denoise_mode(
    const Napi::Object& options, sonare::mastering::repair::DenoiseMode fallback) {
  Napi::Value value = options.Get("mode");
  if (value.IsUndefined() || value.IsNull()) return fallback;
  if (!value.IsString()) throw std::runtime_error("denoise mode must be a string");
  std::string s = value.As<Napi::String>().Utf8Value();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "logmmse" || s == "log_mmse" || s == "lsa") {
    return sonare::mastering::repair::DenoiseMode::LogMmse;
  }
  if (s == "mmsestsa" || s == "mmse_stsa" || s == "stsa") {
    return sonare::mastering::repair::DenoiseMode::MmseStsa;
  }
  if (s == "spectralsubtraction" || s == "spectral_subtraction" || s == "ss") {
    return sonare::mastering::repair::DenoiseMode::SpectralSubtraction;
  }
  throw std::runtime_error("unknown denoise mode: " + value.As<Napi::String>().Utf8Value());
}

sonare::mastering::repair::DenoiseNoiseEstimator parse_denoise_noise_estimator(
    const Napi::Object& options, sonare::mastering::repair::DenoiseNoiseEstimator fallback) {
  Napi::Value value = options.Get("noiseEstimator");
  if (value.IsUndefined() || value.IsNull()) return fallback;
  if (!value.IsString()) throw std::runtime_error("denoise noise estimator must be a string");
  std::string s = value.As<Napi::String>().Utf8Value();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "quantile") return sonare::mastering::repair::DenoiseNoiseEstimator::Quantile;
  if (s == "mcra") return sonare::mastering::repair::DenoiseNoiseEstimator::Mcra;
  if (s == "imcra") return sonare::mastering::repair::DenoiseNoiseEstimator::Imcra;
  throw std::runtime_error("unknown denoise noise estimator: " +
                           value.As<Napi::String>().Utf8Value());
}

}  // namespace

namespace {

sonare::mastering::dynamics::DetectorMode parse_compressor_detector(
    Napi::Env env, const Napi::Object& options,
    sonare::mastering::dynamics::DetectorMode fallback) {
  Napi::Value value = options.Get("detector");
  if (value.IsUndefined() || value.IsNull()) return fallback;
  if (value.IsNumber()) {
    int mode = value.As<Napi::Number>().Int32Value();
    switch (mode) {
      case 0:
        return sonare::mastering::dynamics::DetectorMode::Peak;
      case 1:
        return sonare::mastering::dynamics::DetectorMode::Rms;
      case 2:
        return sonare::mastering::dynamics::DetectorMode::LogRms;
      default:
        throw std::runtime_error("unknown compressor detector mode");
    }
  }
  if (value.IsString()) {
    std::string s = value.As<Napi::String>().Utf8Value();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "peak") return sonare::mastering::dynamics::DetectorMode::Peak;
    if (s == "rms") return sonare::mastering::dynamics::DetectorMode::Rms;
    if (s == "log_rms" || s == "logrms") return sonare::mastering::dynamics::DetectorMode::LogRms;
    throw std::runtime_error("unknown compressor detector mode: " +
                             value.As<Napi::String>().Utf8Value());
  }
  Napi::TypeError::New(env, "detector must be a string or number").ThrowAsJavaScriptException();
  return fallback;
}

template <typename Processor>
std::vector<float> run_dynamics_offline(Processor& processor, const float* samples, size_t length,
                                        int sample_rate, int& latency_samples_out) {
  std::vector<float> buffer(samples, samples + length);
  processor.prepare(static_cast<double>(sample_rate), static_cast<int>(buffer.size()));
  float* channels[] = {buffer.data()};
  processor.process(channels, 1, static_cast<int>(buffer.size()));
  latency_samples_out = processor.latency_samples();
  return buffer;
}

Napi::Object make_dynamics_result(Napi::Env env, const std::vector<float>& samples,
                                  int latency_samples) {
  auto typed = Napi::Float32Array::New(env, samples.size());
  if (!samples.empty()) {
    std::memcpy(typed.Data(), samples.data(), samples.size() * sizeof(float));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", typed);
  out.Set("latencySamples", Napi::Number::New(env, latency_samples));
  return out;
}

}  // namespace

Napi::Value SonareWrap::MasteringDynamicsCompressor(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::dynamics::CompressorConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold_db = node_float_option(options, "thresholdDb", config.threshold_db);
    config.ratio = node_float_option(options, "ratio", config.ratio);
    config.attack_ms = node_float_option(options, "attackMs", config.attack_ms);
    config.release_ms = node_float_option(options, "releaseMs", config.release_ms);
    config.knee_db = node_float_option(options, "kneeDb", config.knee_db);
    config.makeup_gain_db = node_float_option(options, "makeupGainDb", config.makeup_gain_db);
    config.auto_makeup = node_bool_option(options, "autoMakeup", config.auto_makeup);
    config.detector = parse_compressor_detector(env, options, config.detector);
    config.sidechain_hpf_enabled =
        node_bool_option(options, "sidechainHpfEnabled", config.sidechain_hpf_enabled);
    config.sidechain_hpf_hz = node_float_option(options, "sidechainHpfHz", config.sidechain_hpf_hz);
    config.pdr_time_ms = node_float_option(options, "pdrTimeMs", config.pdr_time_ms);
    config.pdr_release_scale =
        node_float_option(options, "pdrReleaseScale", config.pdr_release_scale);
  }
  sonare::mastering::dynamics::Compressor processor(config);
  int latency = 0;
  std::vector<float> out =
      run_dynamics_offline(processor, typed.Data(), typed.ElementLength(), sr, latency);
  return make_dynamics_result(env, out, latency);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringDynamicsGate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::dynamics::GateConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold_db = node_float_option(options, "thresholdDb", config.threshold_db);
    config.attack_ms = node_float_option(options, "attackMs", config.attack_ms);
    config.release_ms = node_float_option(options, "releaseMs", config.release_ms);
    config.range_db = node_float_option(options, "rangeDb", config.range_db);
    config.hold_ms = node_float_option(options, "holdMs", config.hold_ms);
    config.close_threshold_db =
        node_float_option(options, "closeThresholdDb", config.close_threshold_db);
    config.key_hpf_hz = node_float_option(options, "keyHpfHz", config.key_hpf_hz);
  }
  sonare::mastering::dynamics::Gate processor(config);
  int latency = 0;
  std::vector<float> out =
      run_dynamics_offline(processor, typed.Data(), typed.ElementLength(), sr, latency);
  return make_dynamics_result(env, out, latency);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringDynamicsTransientShaper(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::dynamics::TransientShaperConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.attack_gain_db = node_float_option(options, "attackGainDb", config.attack_gain_db);
    config.sustain_gain_db = node_float_option(options, "sustainGainDb", config.sustain_gain_db);
    config.fast_attack_ms = node_float_option(options, "fastAttackMs", config.fast_attack_ms);
    config.fast_release_ms = node_float_option(options, "fastReleaseMs", config.fast_release_ms);
    config.slow_attack_ms = node_float_option(options, "slowAttackMs", config.slow_attack_ms);
    config.slow_release_ms = node_float_option(options, "slowReleaseMs", config.slow_release_ms);
    config.sensitivity = node_float_option(options, "sensitivity", config.sensitivity);
    config.max_gain_db = node_float_option(options, "maxGainDb", config.max_gain_db);
    config.gain_smoothing_ms =
        node_float_option(options, "gainSmoothingMs", config.gain_smoothing_ms);
    config.lookahead_ms = node_float_option(options, "lookaheadMs", config.lookahead_ms);
  }
  sonare::mastering::dynamics::TransientShaper processor(config);
  int latency = 0;
  std::vector<float> out =
      run_dynamics_offline(processor, typed.Data(), typed.ElementLength(), sr, latency);
  return make_dynamics_result(env, out, latency);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDeclick(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::repair::DeclickConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold = repair_float_option(options, "threshold", config.threshold);
    config.neighbor_ratio = repair_float_option(options, "neighborRatio", config.neighbor_ratio);
    if (options.Has("maxClickSamples")) {
      const int max_click_samples =
          repair_int_option(options, "maxClickSamples", static_cast<int>(config.max_click_samples));
      if (max_click_samples <= 0) {
        Napi::RangeError::New(env, "maxClickSamples must be positive").ThrowAsJavaScriptException();
        return env.Undefined();
      }
      config.max_click_samples = static_cast<size_t>(max_click_samples);
    }
    config.lpc_order = repair_int_option(options, "lpcOrder", config.lpc_order);
    config.residual_ratio = repair_float_option(options, "residualRatio", config.residual_ratio);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::declick(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDenoiseClassical(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::repair::DenoiseClassicalConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.mode = parse_denoise_mode(options, config.mode);
    config.noise_estimator = parse_denoise_noise_estimator(options, config.noise_estimator);
    config.n_fft = repair_int_option(options, "nFft", config.n_fft);
    config.hop_length = repair_int_option(options, "hopLength", config.hop_length);
    config.dd_alpha = repair_float_option(options, "ddAlpha", config.dd_alpha);
    config.gain_floor = repair_float_option(options, "gainFloor", config.gain_floor);
    config.over_subtraction =
        repair_float_option(options, "overSubtraction", config.over_subtraction);
    config.spectral_floor = repair_float_option(options, "spectralFloor", config.spectral_floor);
    config.noise_estimation_quantile =
        repair_float_option(options, "noiseEstimationQuantile", config.noise_estimation_quantile);
    config.speech_presence_gain =
        repair_bool_option(options, "speechPresenceGain", config.speech_presence_gain);
    config.gain_smoothing = repair_bool_option(options, "gainSmoothing", config.gain_smoothing);
  }
  if (config.n_fft <= 0 || (config.n_fft & (config.n_fft - 1)) != 0) {
    Napi::RangeError::New(env, "nFft must be a positive power of two").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (config.hop_length <= 0) {
    Napi::RangeError::New(env, "hopLength must be positive").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::denoise_classical(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

namespace {

sonare::mastering::repair::DecrackleMode parse_decrackle_mode(
    const Napi::Object& options, sonare::mastering::repair::DecrackleMode fallback) {
  Napi::Value value = options.Get("mode");
  if (value.IsUndefined() || value.IsNull()) return fallback;
  if (!value.IsString()) throw std::runtime_error("decrackle mode must be a string");
  std::string s = value.As<Napi::String>().Utf8Value();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "median") return sonare::mastering::repair::DecrackleMode::Median;
  if (s == "waveletshrinkage" || s == "wavelet_shrinkage" || s == "wavelet") {
    return sonare::mastering::repair::DecrackleMode::WaveletShrinkage;
  }
  throw std::runtime_error("unknown decrackle mode: " + value.As<Napi::String>().Utf8Value());
}

sonare::mastering::repair::TrimSilenceMode parse_trim_silence_mode(
    const Napi::Object& options, sonare::mastering::repair::TrimSilenceMode fallback) {
  Napi::Value value = options.Get("mode");
  if (value.IsUndefined() || value.IsNull()) return fallback;
  if (!value.IsString()) throw std::runtime_error("trim silence mode must be a string");
  std::string s = value.As<Napi::String>().Utf8Value();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "peak") return sonare::mastering::repair::TrimSilenceMode::Peak;
  if (s == "lufsgated" || s == "lufs_gated" || s == "lufs") {
    return sonare::mastering::repair::TrimSilenceMode::LufsGated;
  }
  throw std::runtime_error("unknown trim silence mode: " + value.As<Napi::String>().Utf8Value());
}

}  // namespace

Napi::Value SonareWrap::MasteringRepairDeclip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::repair::DeclipConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.clip_threshold = repair_float_option(options, "clipThreshold", config.clip_threshold);
    config.lpc_order = repair_int_option(options, "lpcOrder", config.lpc_order);
    config.iterations = repair_int_option(options, "iterations", config.iterations);
    config.lpc_blend = repair_float_option(options, "lpcBlend", config.lpc_blend);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::declip(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDecrackle(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::repair::DecrackleConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold = repair_float_option(options, "threshold", config.threshold);
    config.mode = parse_decrackle_mode(options, config.mode);
    config.levels = repair_int_option(options, "levels", config.levels);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::decrackle(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDehum(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::repair::DehumConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.fundamental_hz = repair_float_option(options, "fundamentalHz", config.fundamental_hz);
    config.harmonics = repair_int_option(options, "harmonics", config.harmonics);
    config.q = repair_float_option(options, "q", config.q);
    config.adaptive = repair_bool_option(options, "adaptive", config.adaptive);
    config.search_range_hz = repair_float_option(options, "searchRangeHz", config.search_range_hz);
    config.adaptation = repair_float_option(options, "adaptation", config.adaptation);
    config.frame_size = repair_int_option(options, "frameSize", config.frame_size);
    config.pll_bandwidth = repair_float_option(options, "pllBandwidth", config.pll_bandwidth);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::dehum(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDereverbClassical(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::repair::DereverbClassicalConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold = repair_float_option(options, "threshold", config.threshold);
    config.attenuation = repair_float_option(options, "attenuation", config.attenuation);
    config.n_fft = repair_int_option(options, "nFft", config.n_fft);
    config.hop_length = repair_int_option(options, "hopLength", config.hop_length);
    config.t60_sec = repair_float_option(options, "t60Sec", config.t60_sec);
    config.late_delay_ms = repair_float_option(options, "lateDelayMs", config.late_delay_ms);
    config.over_subtraction =
        repair_float_option(options, "overSubtraction", config.over_subtraction);
    config.spectral_floor = repair_float_option(options, "spectralFloor", config.spectral_floor);
    config.wpe_enabled = repair_bool_option(options, "wpeEnabled", config.wpe_enabled);
    config.wpe_iterations = repair_int_option(options, "wpeIterations", config.wpe_iterations);
    config.wpe_taps = repair_int_option(options, "wpeTaps", config.wpe_taps);
    config.wpe_strength = repair_float_option(options, "wpeStrength", config.wpe_strength);
  }
  if (config.n_fft <= 0 || (config.n_fft & (config.n_fft - 1)) != 0) {
    Napi::RangeError::New(env, "nFft must be a positive power of two").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (config.hop_length <= 0 || config.hop_length > config.n_fft) {
    Napi::RangeError::New(env, "hopLength must be in (0, nFft]").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::dereverb_classical(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairTrimSilence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::repair::TrimSilenceConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold = repair_float_option(options, "threshold", config.threshold);
    if (options.Has("paddingSamples")) {
      const int padding_samples =
          repair_int_option(options, "paddingSamples", static_cast<int>(config.padding_samples));
      if (padding_samples < 0) {
        Napi::RangeError::New(env, "paddingSamples must be non-negative")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      config.padding_samples = static_cast<size_t>(padding_samples);
    }
    config.mode = parse_trim_silence_mode(options, config.mode);
    config.gate_lufs = repair_float_option(options, "gateLufs", config.gate_lufs);
    config.window_ms = repair_float_option(options, "windowMs", config.window_ms);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::trim_silence(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
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

// ============================================================================
// Effects - librosa.decompose / effects.remix / hpss-with-residual /
// phase-vocoder, wired through the flat C ABI (sonare_c_effects.h).
// ============================================================================

namespace {

// Copy a heap C buffer into a Float32Array and free the C allocation.
Napi::Float32Array EffectsFloatResult(Napi::Env env, float* data, size_t count) {
  auto out = Napi::Float32Array::New(env, count);
  if (count > 0 && data != nullptr) {
    std::memcpy(out.Data(), data, count * sizeof(float));
  }
  sonare_free_floats(data);
  return out;
}

Napi::Value EffectsCheckCResult(Napi::Env env, SonareError err) {
  Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
  return env.Undefined();
}

std::vector<int> EffectsIntVectorFromValue(const Napi::Value& value) {
  if (value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_int32_array) {
    auto arr = value.As<Napi::Int32Array>();
    return std::vector<int>(arr.Data(), arr.Data() + arr.ElementLength());
  }
  if (value.IsArray()) {
    auto arr = value.As<Napi::Array>();
    std::vector<int> out(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      out[i] = arr.Get(i).As<Napi::Number>().Int32Value();
    }
    return out;
  }
  throw Napi::TypeError::New(value.Env(), "Expected Int32Array or number[]");
}

}  // namespace

Napi::Value SonareWrap::VoiceCharacterPresetId(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (presetOrdinal)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int ordinal = info[0].As<Napi::Number>().Int32Value();
  const char* id =
      sonare_voice_character_preset_id(static_cast<SonareVoiceCharacterPreset>(ordinal));
  if (id == nullptr || id[0] == '\0') return env.Null();
  return Napi::String::New(env, id);
}

Napi::Value SonareWrap::RealtimeVoiceChangerPresetConfig(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (presetOrdinal)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int ordinal = info[0].As<Napi::Number>().Int32Value();
  SonareRealtimeVoiceChangerConfig config{};
  SonareError err = sonare_realtime_voice_changer_preset_config(
      static_cast<SonareVoiceCharacterPreset>(ordinal), &config);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
  Napi::Object out = Napi::Object::New(env);
  out.Set("inputGainDb", Napi::Number::New(env, config.input_gain_db));
  out.Set("outputGainDb", Napi::Number::New(env, config.output_gain_db));
  out.Set("wetMix", Napi::Number::New(env, config.wet_mix));
  out.Set("retuneSemitones", Napi::Number::New(env, config.retune_semitones));
  out.Set("retuneMix", Napi::Number::New(env, config.retune_mix));
  out.Set("retuneGrainSize", Napi::Number::New(env, config.retune_grain_size));
  out.Set("formantFactor", Napi::Number::New(env, config.formant_factor));
  out.Set("formantAmount", Napi::Number::New(env, config.formant_amount));
  out.Set("formantBody", Napi::Number::New(env, config.formant_body));
  out.Set("formantBrightness", Napi::Number::New(env, config.formant_brightness));
  out.Set("formantNasal", Napi::Number::New(env, config.formant_nasal));
  out.Set("eqHighpassHz", Napi::Number::New(env, config.eq_highpass_hz));
  out.Set("eqBodyDb", Napi::Number::New(env, config.eq_body_db));
  out.Set("eqPresenceDb", Napi::Number::New(env, config.eq_presence_db));
  out.Set("eqAirDb", Napi::Number::New(env, config.eq_air_db));
  out.Set("gateThresholdDb", Napi::Number::New(env, config.gate_threshold_db));
  out.Set("gateAttackMs", Napi::Number::New(env, config.gate_attack_ms));
  out.Set("gateReleaseMs", Napi::Number::New(env, config.gate_release_ms));
  out.Set("gateRangeDb", Napi::Number::New(env, config.gate_range_db));
  out.Set("compressorThresholdDb", Napi::Number::New(env, config.compressor_threshold_db));
  out.Set("compressorRatio", Napi::Number::New(env, config.compressor_ratio));
  out.Set("compressorAttackMs", Napi::Number::New(env, config.compressor_attack_ms));
  out.Set("compressorReleaseMs", Napi::Number::New(env, config.compressor_release_ms));
  out.Set("compressorMakeupGainDb", Napi::Number::New(env, config.compressor_makeup_gain_db));
  out.Set("deesserFrequencyHz", Napi::Number::New(env, config.deesser_frequency_hz));
  out.Set("deesserThresholdDb", Napi::Number::New(env, config.deesser_threshold_db));
  out.Set("deesserRatio", Napi::Number::New(env, config.deesser_ratio));
  out.Set("deesserRangeDb", Napi::Number::New(env, config.deesser_range_db));
  out.Set("reverbMix", Napi::Number::New(env, config.reverb_mix));
  out.Set("reverbTimeMs", Napi::Number::New(env, config.reverb_time_ms));
  out.Set("reverbDamping", Napi::Number::New(env, config.reverb_damping));
  out.Set("reverbSeed", Napi::Number::New(env, config.reverb_seed));
  out.Set("limiterCeilingDb", Napi::Number::New(env, config.limiter_ceiling_db));
  out.Set("limiterReleaseMs", Napi::Number::New(env, config.limiter_release_ms));
  out.Set("limiterEnableIspLimiter",
          Napi::Boolean::New(env, config.limiter_enable_isp_limiter != 0));
  out.Set("limiterIspCeilingDbtp", Napi::Number::New(env, config.limiter_isp_ceiling_dbtp));
  return out;
}

Napi::Value SonareWrap::Decompose(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber() ||
      !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, nFeatures, nFrames, nComponents, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int n_features = info[1].As<Napi::Number>().Int32Value();
  int n_frames = info[2].As<Napi::Number>().Int32Value();
  int n_components = info[3].As<Napi::Number>().Int32Value();
  if (!ValidateMatrixDims(env, "decompose", n_features, n_frames, arr.ElementLength())) {
    return env.Undefined();
  }
  int n_iter =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 50;
  float beta =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().FloatValue() : 2.0f;
  float* out_w = nullptr;
  size_t out_w_length = 0;
  float* out_h = nullptr;
  size_t out_h_length = 0;
  SonareError err = sonare_decompose(arr.Data(), n_features, n_frames, n_components, n_iter, beta,
                                     &out_w, &out_w_length, &out_h, &out_h_length);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
  Napi::Object w = Napi::Object::New(env);
  w.Set("rows", Napi::Number::New(env, n_features));
  w.Set("cols", Napi::Number::New(env, n_components));
  w.Set("data", EffectsFloatResult(env, out_w, out_w_length));
  Napi::Object h = Napi::Object::New(env);
  h.Set("rows", Napi::Number::New(env, n_components));
  h.Set("cols", Napi::Number::New(env, n_frames));
  h.Set("data", EffectsFloatResult(env, out_h, out_h_length));
  Napi::Object result = Napi::Object::New(env);
  result.Set("w", w);
  result.Set("h", h);
  return result;
}

Napi::Value SonareWrap::NnFilter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, nFeatures, nFrames, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int n_features = info[1].As<Napi::Number>().Int32Value();
  int n_frames = info[2].As<Napi::Number>().Int32Value();
  if (!ValidateMatrixDims(env, "nnFilter", n_features, n_frames, arr.ElementLength())) {
    return env.Undefined();
  }
  std::string aggregate =
      info.Length() >= 4 && info[3].IsString() ? info[3].As<Napi::String>().Utf8Value() : "mean";
  int k = info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 7;
  int width =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 1;
  float* out = nullptr;
  size_t out_length = 0;
  SonareError err = sonare_nn_filter(arr.Data(), n_features, n_frames, aggregate.c_str(), k, width,
                                     &out, &out_length);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("rows", Napi::Number::New(env, n_features));
  result.Set("cols", Napi::Number::New(env, n_frames));
  result.Set("data", EffectsFloatResult(env, out, out_length));
  return result;
}

Napi::Value SonareWrap::Remix(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected (Float32Array, intervals, sampleRate?, alignZeros?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  auto arr = info[0].As<Napi::Float32Array>();
  std::vector<int> intervals = EffectsIntVectorFromValue(info[1]);
  int sr =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 22050;
  int align_zeros =
      info.Length() >= 4 && info[3].IsBoolean() && info[3].As<Napi::Boolean>().Value() ? 1 : 0;
  float* out = nullptr;
  size_t out_length = 0;
  SonareError err = sonare_remix(arr.Data(), arr.ElementLength(), sr, intervals.data(),
                                 intervals.size() / 2, align_zeros, &out, &out_length);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
  return EffectsFloatResult(env, out, out_length);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::HpssWithResidual(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr = info[1].As<Napi::Number>().Int32Value();
  int kernel_harmonic =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 31;
  int kernel_percussive =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 31;
  float* out_harmonic = nullptr;
  float* out_percussive = nullptr;
  float* out_residual = nullptr;
  size_t out_length = 0;
  int out_sample_rate = 0;
  SonareError err = sonare_hpss_with_residual(arr.Data(), arr.ElementLength(), sr, kernel_harmonic,
                                              kernel_percussive, &out_harmonic, &out_percussive,
                                              &out_residual, &out_length, &out_sample_rate);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("harmonic", EffectsFloatResult(env, out_harmonic, out_length));
  result.Set("percussive", EffectsFloatResult(env, out_percussive, out_length));
  result.Set("residual", EffectsFloatResult(env, out_residual, out_length));
  result.Set("sampleRate", Napi::Number::New(env, out_sample_rate));
  return result;
}

Napi::Value SonareWrap::PhaseVocoder(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, rate, nFft?, hopLength?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr = info[1].As<Napi::Number>().Int32Value();
  float rate = info[2].As<Napi::Number>().FloatValue();
  int n_fft =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 512;
  float* out = nullptr;
  size_t out_length = 0;
  SonareError err = sonare_phase_vocoder(arr.Data(), arr.ElementLength(), sr, rate, n_fft,
                                         hop_length, &out, &out_length);
  if (err != SONARE_OK) return EffectsCheckCResult(env, err);
  return EffectsFloatResult(env, out, out_length);
}
