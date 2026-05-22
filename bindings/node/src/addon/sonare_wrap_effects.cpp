#include "sonare_wrap.h"
#include "sonare_wrap_utils.h"

#include <string>
#include <vector>

#include "core/audio.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "effects/tts.h"

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
  int kernel_harmonic = info.Length() >= 3 && info[2].IsNumber()
                            ? info[2].As<Napi::Number>().Int32Value()
                            : 31;
  int kernel_percussive = info.Length() >= 4 && info[3].IsNumber()
                              ? info[3].As<Napi::Number>().Int32Value()
                              : 31;

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
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate)")
        .ThrowAsJavaScriptException();
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
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate)")
        .ThrowAsJavaScriptException();
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

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !info[2].IsNumber()) {
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

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !info[2].IsNumber()) {
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
  float target_db = info.Length() >= 3 && info[2].IsNumber()
                        ? info[2].As<Napi::Number>().FloatValue()
                        : 0.0f;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::normalize(audio, target_db);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
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
  float threshold_db = info.Length() >= 3 && info[2].IsNumber()
                           ? info[2].As<Napi::Number>().FloatValue()
                           : -60.0f;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::trim(audio, threshold_db);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::AnalyzeTtsQuality(const Napi::CallbackInfo& info) {
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
  float silence_threshold_db = info.Length() >= 3 && info[2].IsNumber()
                                   ? info[2].As<Napi::Number>().FloatValue()
                                   : -45.0f;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::TtsQualityResult result = sonare::analyze_tts_quality(audio, silence_threshold_db);

  Napi::Object out = Napi::Object::New(env);
  out.Set("durationSec", Napi::Number::New(env, result.duration_sec));
  out.Set("peakDb", Napi::Number::New(env, result.peak_db));
  out.Set("rmsDb", Napi::Number::New(env, result.rms_db));
  out.Set("silenceRatio", Napi::Number::New(env, result.silence_ratio));
  out.Set("clippingRatio", Napi::Number::New(env, result.clipping_ratio));
  out.Set("leadingSilenceSec", Napi::Number::New(env, result.leading_silence_sec));
  out.Set("trailingSilenceSec", Napi::Number::New(env, result.trailing_silence_sec));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PrepareTts(const Napi::CallbackInfo& info) {
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
  float target_rms_db = info.Length() >= 3 && info[2].IsNumber()
                            ? info[2].As<Napi::Number>().FloatValue()
                            : -20.0f;
  float silence_threshold_db = info.Length() >= 4 && info[3].IsNumber()
                                   ? info[3].As<Napi::Number>().FloatValue()
                                   : -45.0f;
  float peak_limit_db = info.Length() >= 5 && info[4].IsNumber()
                            ? info[4].As<Napi::Number>().FloatValue()
                            : -1.0f;
  float fade_sec = info.Length() >= 6 && info[5].IsNumber()
                       ? info[5].As<Napi::Number>().FloatValue()
                       : 0.005f;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result =
      sonare::prepare_tts(audio, target_rms_db, silence_threshold_db, peak_limit_db, fade_sec);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::CompressPauses(const Napi::CallbackInfo& info) {
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
  float max_pause_sec = info.Length() >= 3 && info[2].IsNumber()
                            ? info[2].As<Napi::Number>().FloatValue()
                            : 0.6f;
  float silence_threshold_db = info.Length() >= 4 && info[3].IsNumber()
                                   ? info[3].As<Napi::Number>().FloatValue()
                                   : -45.0f;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::compress_pauses(audio, max_pause_sec, silence_threshold_db);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}
