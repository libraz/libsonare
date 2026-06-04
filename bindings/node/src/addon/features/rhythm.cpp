#include <cstring>
#include <string>
#include <vector>

#include "core/audio.h"
#include "core/convert.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "feature/chroma.h"
#include "feature/inverse.h"
#include "feature/mel_spectrogram.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "features/common.h"
#include "sonare_wrap.h"
#include "sonare_wrap_utils.h"
#include "util/constants.h"

using namespace sonare_node;
using namespace sonare_node::features;

Napi::Value SonareWrap::Tonnetz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (chromagram, nChroma, nFrames)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  const int n_chroma = info[1].As<Napi::Number>().Int32Value();
  const int n_frames = info[2].As<Napi::Number>().Int32Value();
  if (!ValidateMatrixDims(env, "tonnetz", n_chroma, n_frames, arr.ElementLength())) {
    return env.Undefined();
  }
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_tonnetz(arr.Data(), n_chroma, n_frames, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::Tempogram(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected onset envelope Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int hop =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 512;
  int win =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 384;
  int mode = SONARE_TEMPOGRAM_AUTOCORRELATION;
  try {
    mode = info.Length() >= 5 ? TempogramModeFromValue(info[4]) : SONARE_TEMPOGRAM_AUTOCORRELATION;
  } catch (const Napi::Error& err) {
    err.ThrowAsJavaScriptException();
    return env.Undefined();
  }
  float* out = nullptr;
  size_t count = 0;
  int n_frames = 0;
  SonareError err = sonare_tempogram_with_mode(arr.Data(), arr.ElementLength(), sr, hop, win, 1, 1,
                                               mode, &out, &count, &n_frames);
  if (err != SONARE_OK) return CheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("nFrames", n_frames);
  result.Set("winLength", win);
  result.Set("data", FloatResult(env, out, count));
  return result;
}

Napi::Value SonareWrap::CyclicTempogram(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected onset envelope Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int hop =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 512;
  int win =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 384;
  float bpm_min =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().FloatValue() : 60.0f;
  int n_bins =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 60;
  float* out = nullptr;
  size_t count = 0;
  int n_frames = 0;
  SonareError err = sonare_cyclic_tempogram(arr.Data(), arr.ElementLength(), sr, hop, win, bpm_min,
                                            n_bins, &out, &count, &n_frames);
  if (err != SONARE_OK) return CheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("nFrames", n_frames);
  result.Set("nBins", n_bins);
  result.Set("data", FloatResult(env, out, count));
  return result;
}

Napi::Value SonareWrap::Plp(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected onset envelope Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int hop =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 512;
  float tempo_min =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().FloatValue() : 30.0f;
  float tempo_max =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().FloatValue() : 300.0f;
  int win =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 384;
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_plp(arr.Data(), arr.ElementLength(), sr, hop, tempo_min, tempo_max, win, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::OnsetEnvelope(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected audio Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  int n_mels =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 128;
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_onset_strength(arr.Data(), arr.ElementLength(), sr, n_fft, hop, n_mels, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::OnsetStrengthMulti(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected audio Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  int n_mels =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 128;
  int n_bands =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 6;
  float* out = nullptr;
  size_t count = 0;
  int n_frames = 0;
  SonareError err = sonare_onset_strength_multi(arr.Data(), arr.ElementLength(), sr, n_fft, hop,
                                                n_mels, n_bands, &out, &count, &n_frames);
  if (err != SONARE_OK) return CheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("nBands", Napi::Number::New(env, n_bands));
  result.Set("nFrames", Napi::Number::New(env, n_frames));
  result.Set("data", FloatResult(env, out, count));
  return result;
}

Napi::Value SonareWrap::FourierTempogram(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected onset envelope Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int hop =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 512;
  int win =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 384;
  float* out = nullptr;
  size_t count = 0;
  int n_frames = 0;
  SonareError err = sonare_fourier_tempogram(arr.Data(), arr.ElementLength(), sr, hop, win, 1, 1,
                                             &out, &count, &n_frames);
  if (err != SONARE_OK) return CheckCResult(env, err);
  int n_bins = n_frames > 0 ? static_cast<int>(count / static_cast<size_t>(n_frames)) : 0;
  Napi::Object result = Napi::Object::New(env);
  result.Set("nBins", Napi::Number::New(env, n_bins));
  result.Set("nFrames", Napi::Number::New(env, n_frames));
  result.Set("data", FloatResult(env, out, count));
  return result;
}

Napi::Value SonareWrap::TempogramRatio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected tempogram Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int win =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 384;
  int sr =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 22050;
  int hop =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  std::vector<float> factors;
  if (info.Length() >= 5 && !info[4].IsUndefined() && !info[4].IsNull()) {
    factors = FloatVectorFromValue(info[4]);
  }
  const float* factors_ptr = factors.empty() ? nullptr : factors.data();
  size_t n_factors = factors.size();
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_tempogram_ratio(arr.Data(), arr.ElementLength(), win, sr, hop,
                                           factors_ptr, n_factors, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::NnlsChroma(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected audio Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  float* out = nullptr;
  size_t count = 0;
  int n_frames = 0;
  SonareError err =
      sonare_nnls_chroma(arr.Data(), arr.ElementLength(), sr, &out, &count, &n_frames);
  if (err != SONARE_OK) return CheckCResult(env, err);
  int n_chroma = n_frames > 0 ? static_cast<int>(count / static_cast<size_t>(n_frames)) : 12;
  Napi::Object result = Napi::Object::New(env);
  result.Set("nChroma", Napi::Number::New(env, n_chroma));
  result.Set("nFrames", Napi::Number::New(env, n_frames));
  result.Set("data", FloatResult(env, out, count));
  return result;
}

// ============================================================================
// Core - Resample
// ============================================================================

Napi::Value SonareWrap::Resample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, srcSr, targetSr)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int src_sr = info[1].As<Napi::Number>().Int32Value();
  int target_sr = info[2].As<Napi::Number>().Int32Value();

  std::vector<float> result = sonare::resample(data, length, src_sr, target_sr);
  return VecToFloat32(env, result);
  SONARE_NODE_CATCH(env)
}
