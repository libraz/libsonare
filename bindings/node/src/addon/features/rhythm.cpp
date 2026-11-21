#include "feature/rhythm.h"

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
#include "feature/nnls_chroma.h"
#include "feature/onset.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "features/common.h"
#include "filters/mel.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"
#include "util/constants.h"

using namespace sonare_node;
using namespace sonare_node::features;

Napi::Value SonareWrap::Tonnetz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "chromagram must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "nChroma must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "nFrames must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  const int n_chroma = node_narrow_int(env, info[1], "nChroma");
  const int n_frames = node_narrow_int(env, info[2], "nFrames");
  if (!ValidateMatrixDims(env, n_chroma, n_frames, arr.ElementLength())) {
    return env.Undefined();
  }
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_tonnetz(arr.Data(), n_chroma, n_frames, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Tempogram(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "onsetEnvelope must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();
  int hop{};
  if (!OptionalDomainArg(env, info, 2, "hopLength", 512, sonare::kRhythmCountDomain, &hop)) {
    return env.Undefined();
  }
  int win{};
  if (!OptionalDomainArg(env, info, 3, "winLength", 384, sonare::kTempogramWinLengthDomain, &win)) {
    return env.Undefined();
  }
  const int mode =
      info.Length() >= 5 ? TempogramModeFromValue(info[4]) : SONARE_TEMPOGRAM_AUTOCORRELATION;
  bool center = true;
  if (!OptionalBoolArg(env, info, 5, "center", true, &center)) {
    return env.Undefined();
  }
  bool norm = true;
  if (!OptionalBoolArg(env, info, 6, "norm", true, &norm)) {
    return env.Undefined();
  }
  float* out = nullptr;
  size_t count = 0;
  int n_frames = 0;
  SonareError err = sonare_tempogram_with_mode(arr.Data(), arr.ElementLength(), sr, hop, win,
                                               center, norm, mode, &out, &count, &n_frames);
  if (err != SONARE_OK) return CheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("nFrames", n_frames);
  result.Set("winLength", win);
  result.Set("data", FloatResult(env, out, count));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::CyclicTempogram(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "onsetEnvelope must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredDomainArg(env, info, 1, "sampleRate", sonare::kRhythmCountDomain, &sr)) {
    return env.Undefined();
  }
  int hop{};
  if (!OptionalDomainArg(env, info, 2, "hopLength", 512, sonare::kRhythmCountDomain, &hop)) {
    return env.Undefined();
  }
  int win{};
  if (!OptionalDomainArg(env, info, 3, "winLength", 384, sonare::kTempogramWinLengthDomain, &win)) {
    return env.Undefined();
  }
  float bpm_min = sonare::kCyclicTempogramBpmMin;
  if (!OptionalFiniteFloatArg(env, info, 4, "bpmMin", bpm_min, &bpm_min)) return env.Undefined();
  int n_bins{};
  if (!OptionalDomainArg(env, info, 5, "nBins", sonare::kCyclicTempogramBins,
                         sonare::kRhythmCountDomain, &n_bins)) {
    return env.Undefined();
  }
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Plp(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "onsetEnvelope must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!OptionalDomainArg(env, info, 1, "sampleRate", 22050, sonare::kRhythmCountDomain, &sr)) {
    return env.Undefined();
  }
  int hop{};
  if (!OptionalDomainArg(env, info, 2, "hopLength", 512, sonare::kRhythmCountDomain, &hop)) {
    return env.Undefined();
  }
  const sonare::PlpConfig plp_defaults;
  float bpm_min{};
  if (!OptionalFiniteFloatArg(env, info, 3, "bpmMin", plp_defaults.tempo_min, &bpm_min))
    return env.Undefined();
  float bpm_max{};
  if (!OptionalFiniteFloatArg(env, info, 4, "bpmMax", plp_defaults.tempo_max, &bpm_max))
    return env.Undefined();
  int win{};
  if (!OptionalDomainArg(env, info, 5, "winLength", 384, sonare::kTempogramWinLengthDomain, &win)) {
    return env.Undefined();
  }
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_plp(arr.Data(), arr.ElementLength(), sr, hop, bpm_min, bpm_max, win, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::OnsetEnvelope(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop{};
  if (!OptionalDomainArg(env, info, 3, "hopLength", 512, sonare::kStftHopDomain, &hop)) {
    return env.Undefined();
  }
  int n_mels{};
  if (!OptionalDomainArg(env, info, 4, "nMels", 128, sonare::kMelBandsDomain, &n_mels)) {
    return env.Undefined();
  }
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_onset_strength(arr.Data(), arr.ElementLength(), sr, n_fft, hop, n_mels, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::OnsetStrengthMulti(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop{};
  if (!OptionalDomainArg(env, info, 3, "hopLength", 512, sonare::kStftHopDomain, &hop)) {
    return env.Undefined();
  }
  int n_mels{};
  if (!OptionalDomainArg(env, info, 4, "nMels", 128, sonare::kMelBandsDomain, &n_mels)) {
    return env.Undefined();
  }
  int n_bands{};
  if (!OptionalDomainArg(env, info, 5, "nBands", sonare::kOnsetStrengthMultiDefaultBands,
                         sonare::kOnsetCountDomain, &n_bands)) {
    return env.Undefined();
  }
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::FourierTempogram(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "onsetEnvelope must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();
  int hop{};
  if (!OptionalDomainArg(env, info, 2, "hopLength", 512, sonare::kRhythmCountDomain, &hop)) {
    return env.Undefined();
  }
  int win{};
  if (!OptionalDomainArg(env, info, 3, "winLength", 384, sonare::kTempogramWinLengthDomain, &win)) {
    return env.Undefined();
  }
  bool center = true;
  if (!OptionalBoolArg(env, info, 4, "center", true, &center)) {
    return env.Undefined();
  }
  bool norm = true;
  if (!OptionalBoolArg(env, info, 5, "norm", true, &norm)) {
    return env.Undefined();
  }
  float* out = nullptr;
  size_t count = 0;
  int n_frames = 0;
  SonareError err = sonare_fourier_tempogram(arr.Data(), arr.ElementLength(), sr, hop, win, center,
                                             norm, &out, &count, &n_frames);
  if (err != SONARE_OK) return CheckCResult(env, err);
  int n_bins = n_frames > 0 ? static_cast<int>(count / static_cast<size_t>(n_frames)) : 0;
  Napi::Object result = Napi::Object::New(env);
  result.Set("nBins", Napi::Number::New(env, n_bins));
  result.Set("nFrames", Napi::Number::New(env, n_frames));
  result.Set("data", FloatResult(env, out, count));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::TempogramRatio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "tempogram must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int win{};
  if (!RequiredDomainArg(env, info, 1, "win", sonare::kRhythmCountDomain, &win)) {
    return env.Undefined();
  }
  int sr{};
  if (!RequiredIntArg(env, info, 2, "sampleRate", &sr)) return env.Undefined();
  int hop{};
  if (!RequiredDomainArg(env, info, 3, "hopLength", sonare::kRhythmCountDomain, &hop)) {
    return env.Undefined();
  }
  std::vector<float> factors;
  if (info.Length() >= 5 && !info[4].IsUndefined() && !info[4].IsNull()) {
    factors = FloatVectorFromValue(info[4], "factors");
  }
  const float* factors_ptr = factors.empty() ? nullptr : factors.data();
  size_t n_factors = factors.size();
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_tempogram_ratio(arr.Data(), arr.ElementLength(), win, sr, hop,
                                           factors_ptr, n_factors, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::NnlsChroma(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();
  float* out = nullptr;
  size_t count = 0;
  int n_frames = 0;
  bool enable_blend = true;
  if (!OptionalBoolArg(env, info, 2, "enableBlend", true, &enable_blend)) {
    return env.Undefined();
  }
  float blend_weight{};
  if (!OptionalFiniteFloatArg(env, info, 3, "blendWeight", 0.55f, &blend_weight))
    return env.Undefined();
  int blend_n_fft{};
  if (!OptionalIntArg(env, info, 4, "stftBlendNFft", 4096, &blend_n_fft)) return env.Undefined();
  int hop_length{};
  if (!RequiredDomainArg(env, info, 5, "hopLength", sonare::kNnlsHopLengthDomain, &hop_length)) {
    return env.Undefined();
  }
  SonareNnlsChromaOptions nnls_options;
  sonare_nnls_chroma_options_init(&nnls_options);
  nnls_options.enable_stft_blend = enable_blend ? 1 : 0;
  nnls_options.stft_blend_weight = blend_weight;
  nnls_options.stft_blend_n_fft = blend_n_fft;
  nnls_options.hop_length = hop_length;
  SonareError err = sonare_nnls_chroma(arr.Data(), arr.ElementLength(), sr, &nnls_options, &out,
                                       &count, &n_frames);
  if (err != SONARE_OK) return CheckCResult(env, err);
  int n_chroma = n_frames > 0 ? static_cast<int>(count / static_cast<size_t>(n_frames)) : 12;
  Napi::Object result = Napi::Object::New(env);
  result.Set("nChroma", Napi::Number::New(env, n_chroma));
  result.Set("nFrames", Napi::Number::New(env, n_frames));
  result.Set("data", FloatResult(env, out, count));
  return result;
  SONARE_NODE_CATCH(env)
}

// ============================================================================
// Core - Resample
// ============================================================================

Napi::Value SonareWrap::Resample(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "srcSr must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "targetSr must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int src_sr = node_narrow_int(env, info[1], "srcSr");
  int target_sr = node_narrow_int(env, info[2], "targetSr");

  std::vector<float> result = sonare::resample_checked(data, length, src_sr, target_sr);
  return VecToFloat32(env, result);
  SONARE_NODE_CATCH(env)
}
