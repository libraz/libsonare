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

namespace {

Napi::Value CqtResultToObject(Napi::Env env, const SonareCqtResult& result) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("nBins", Napi::Number::New(env, result.n_bins));
  out.Set("nFrames", Napi::Number::New(env, result.n_frames));
  out.Set("hopLength", Napi::Number::New(env, result.hop_length));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));

  const size_t magnitude_count =
      static_cast<size_t>(result.n_bins) * static_cast<size_t>(result.n_frames);
  auto magnitude = Napi::Float32Array::New(env, magnitude_count);
  if (magnitude_count > 0 && result.magnitude != nullptr) {
    std::memcpy(magnitude.Data(), result.magnitude, magnitude_count * sizeof(float));
  }
  out.Set("magnitude", magnitude);

  auto frequencies = Napi::Float32Array::New(env, static_cast<size_t>(result.n_bins));
  if (result.n_bins > 0 && result.frequencies != nullptr) {
    std::memcpy(frequencies.Data(), result.frequencies,
                static_cast<size_t>(result.n_bins) * sizeof(float));
  }
  out.Set("frequencies", frequencies);
  return out;
}

using CqtFn = SonareError (*)(const float*, size_t, int, int, float, int, int, SonareCqtResult*);

Napi::Value CqtLike(const Napi::CallbackInfo& info, CqtFn fn) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  const int hop_length =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 512;
  const float fmin = info.Length() >= 4 && info[3].IsNumber()
                         ? info[3].As<Napi::Number>().FloatValue()
                         : sonare::constants::kC1Hz;
  const int n_bins =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 84;
  const int bins_per_octave =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 12;

  SonareCqtResult result{};
  SonareError err = fn(typed.Data(), typed.ElementLength(), sr, hop_length, fmin, n_bins,
                       bins_per_octave, &result);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Value out = CqtResultToObject(env, result);
  sonare_free_cqt_result(&result);
  return out;
}

}  // namespace

Napi::Value SonareWrap::Cqt(const Napi::CallbackInfo& info) { return CqtLike(info, sonare_cqt); }

Napi::Value SonareWrap::PseudoCqt(const Napi::CallbackInfo& info) {
  return CqtLike(info, sonare_pseudo_cqt);
}

Napi::Value SonareWrap::HybridCqt(const Napi::CallbackInfo& info) {
  return CqtLike(info, sonare_hybrid_cqt);
}

Napi::Value SonareWrap::Vqt(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  const int hop_length =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 512;
  const float fmin = info.Length() >= 4 && info[3].IsNumber()
                         ? info[3].As<Napi::Number>().FloatValue()
                         : sonare::constants::kC1Hz;
  const int n_bins =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 84;
  const int bins_per_octave =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 12;
  const float gamma =
      info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().FloatValue() : 0.0f;

  SonareCqtResult result{};
  SonareError err = sonare_vqt(typed.Data(), typed.ElementLength(), sr, hop_length, fmin, n_bins,
                               bins_per_octave, gamma, &result);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Value out = CqtResultToObject(env, result);
  sonare_free_cqt_result(&result);
  return out;
}

// ============================================================================
// Features - Inverse reconstruction (Mel/MFCC -> spectrogram -> audio)
// ============================================================================
//
// These mirror feature::mel_to_stft / mel_to_audio / mfcc_to_mel /
// mfcc_to_audio (src/feature/inverse.h) and match the WASM surface
// (melToStft / melToAudio in src/wasm/bindings.cpp). The Mel matrix is a
// row-major [n_mels x n_frames] power spectrogram; the MFCC matrix is a
// row-major [n_mfcc x n_frames] coefficient matrix.

// melToStft(mel, nMels, nFrames, sampleRate?, nFft?, fmin?, fmax?, htk?)
// -> { nBins, nFrames, power: Float32Array }
//
// hop_length is intentionally absent: sonare::mel_to_stft does not consume it
// (the inverse mel projection is per-frame). Keep this signature in sync with
// the WASM / Python / C surfaces.
Napi::Value SonareWrap::MelToStft(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, nMels, nFrames, sampleRate?, nFft?, fmin?, "
                         "fmax?, htk?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int n_mels = info[1].As<Napi::Number>().Int32Value();
  const int n_frames = info[2].As<Napi::Number>().Int32Value();
  if (!ValidateMatrixDims(env, "melToStft", n_mels, n_frames, typed.ElementLength())) {
    return env.Undefined();
  }
  const int sr =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 22050;
  const int n_fft =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 2048;
  const float fmin =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().FloatValue() : 0.0f;
  const float fmax =
      info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().FloatValue() : 0.0f;
  const bool htk =
      info.Length() >= 8 && info[7].IsBoolean() ? info[7].As<Napi::Boolean>().Value() : false;

  sonare::MelConfig config;
  config.n_fft = n_fft;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  std::vector<float> stft = sonare::mel_to_stft(typed.Data(), n_mels, n_frames, config, sr);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nBins", Napi::Number::New(env, n_fft / 2 + 1));
  out.Set("nFrames", Napi::Number::New(env, n_frames));
  out.Set("power", VecToFloat32(env, stft));
  return out;
  SONARE_NODE_CATCH(env)
}

// melToAudio(mel, nMels, nFrames, sampleRate?, nFft?, hopLength?, fmin?, fmax?, nIter?, htk?)
// -> Float32Array (reconstructed audio)
Napi::Value SonareWrap::MelToAudio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, nMels, nFrames, sampleRate?, nFft?, "
                         "hopLength?, fmin?, fmax?, nIter?, htk?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int n_mels = info[1].As<Napi::Number>().Int32Value();
  const int n_frames = info[2].As<Napi::Number>().Int32Value();
  if (!ValidateMatrixDims(env, "melToAudio", n_mels, n_frames, typed.ElementLength())) {
    return env.Undefined();
  }
  const int sr =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 22050;
  const int n_fft =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 2048;
  const int hop_length =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 512;
  const float fmin =
      info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().FloatValue() : 0.0f;
  const float fmax =
      info.Length() >= 8 && info[7].IsNumber() ? info[7].As<Napi::Number>().FloatValue() : 0.0f;
  const int n_iter =
      info.Length() >= 9 && info[8].IsNumber() ? info[8].As<Napi::Number>().Int32Value() : 32;
  const bool htk =
      info.Length() >= 10 && info[9].IsBoolean() ? info[9].As<Napi::Boolean>().Value() : false;

  sonare::MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  sonare::Audio result = sonare::mel_to_audio(typed.Data(), n_mels, n_frames, config, n_iter, sr);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

// mfccToMel(mfcc, nMfcc, nFrames, nMels?)
// -> { nMels, nFrames, power: Float32Array } (Mel power)
Napi::Value SonareWrap::MfccToMel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, nMfcc, nFrames, nMels?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int n_mfcc = info[1].As<Napi::Number>().Int32Value();
  const int n_frames = info[2].As<Napi::Number>().Int32Value();
  if (!ValidateMatrixDims(env, "mfccToMel", n_mfcc, n_frames, typed.ElementLength())) {
    return env.Undefined();
  }
  const int n_mels =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 128;

  std::vector<float> mel = sonare::mfcc_to_mel(typed.Data(), n_mfcc, n_frames, n_mels);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nMels", Napi::Number::New(env, n_mels));
  out.Set("nFrames", Napi::Number::New(env, n_frames));
  out.Set("power", VecToFloat32(env, mel));
  return out;
  SONARE_NODE_CATCH(env)
}

// mfccToAudio(mfcc, nMfcc, nFrames, nMels?, sampleRate?, nFft?, hopLength?, fmin?, fmax?, nIter?,
// htk?)
// -> Float32Array (reconstructed audio)
Napi::Value SonareWrap::MfccToAudio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, nMfcc, nFrames, nMels?, sampleRate?, nFft?, "
                         "hopLength?, fmin?, fmax?, nIter?, htk?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int n_mfcc = info[1].As<Napi::Number>().Int32Value();
  const int n_frames = info[2].As<Napi::Number>().Int32Value();
  if (!ValidateMatrixDims(env, "mfccToAudio", n_mfcc, n_frames, typed.ElementLength())) {
    return env.Undefined();
  }
  const int n_mels =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 128;
  const int sr =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 22050;
  const int n_fft =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 2048;
  const int hop_length =
      info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().Int32Value() : 512;
  const float fmin =
      info.Length() >= 8 && info[7].IsNumber() ? info[7].As<Napi::Number>().FloatValue() : 0.0f;
  const float fmax =
      info.Length() >= 9 && info[8].IsNumber() ? info[8].As<Napi::Number>().FloatValue() : 0.0f;
  const int n_iter =
      info.Length() >= 10 && info[9].IsNumber() ? info[9].As<Napi::Number>().Int32Value() : 32;
  const bool htk =
      info.Length() >= 11 && info[10].IsBoolean() ? info[10].As<Napi::Boolean>().Value() : false;

  sonare::MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  sonare::Audio result = sonare::mfcc_to_audio(typed.Data(), n_mfcc, n_frames, config, n_iter, sr);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

// ============================================================================
// Features - Spectral contrast / poly features / zero crossings / tuning
// ============================================================================

Napi::Value SonareWrap::SpectralContrast(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  int n_bands =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 6;
  float fmin =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().FloatValue() : 200.0f;
  float quantile =
      info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().FloatValue() : 0.02f;
  float* out = nullptr;
  int out_rows = 0;
  int out_cols = 0;
  SonareError err = sonare_spectral_contrast(arr.Data(), arr.ElementLength(), sr, n_fft, hop_length,
                                             n_bands, fmin, quantile, &out, &out_rows, &out_cols);
  if (err != SONARE_OK) return CheckCResult(env, err);
  const size_t count = static_cast<size_t>(out_rows) * static_cast<size_t>(out_cols);
  Napi::Object result = Napi::Object::New(env);
  result.Set("rows", Napi::Number::New(env, out_rows));
  result.Set("cols", Napi::Number::New(env, out_cols));
  result.Set("data", FloatResult(env, out, count));
  return result;
}

Napi::Value SonareWrap::PolyFeatures(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  int order =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 1;
  float* out = nullptr;
  int out_rows = 0;
  int out_cols = 0;
  SonareError err = sonare_poly_features(arr.Data(), arr.ElementLength(), sr, n_fft, hop_length,
                                         order, &out, &out_rows, &out_cols);
  if (err != SONARE_OK) return CheckCResult(env, err);
  const size_t count = static_cast<size_t>(out_rows) * static_cast<size_t>(out_cols);
  Napi::Object result = Napi::Object::New(env);
  result.Set("rows", Napi::Number::New(env, out_rows));
  result.Set("cols", Napi::Number::New(env, out_cols));
  result.Set("data", FloatResult(env, out, count));
  return result;
}

Napi::Value SonareWrap::ZeroCrossings(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float threshold =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().FloatValue() : 1e-10f;
  int ref_magnitude =
      info.Length() >= 3 && info[2].IsBoolean() && info[2].As<Napi::Boolean>().Value() ? 1 : 0;
  int pad =
      info.Length() >= 4 && info[3].IsBoolean() ? (info[3].As<Napi::Boolean>().Value() ? 1 : 0) : 1;
  int zero_pos =
      info.Length() >= 5 && info[4].IsBoolean() ? (info[4].As<Napi::Boolean>().Value() ? 1 : 0) : 1;
  int* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_zero_crossings(arr.Data(), arr.ElementLength(), threshold, ref_magnitude,
                                          pad, zero_pos, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return IntResult(env, out, count);
}

Napi::Value SonareWrap::PitchTuning(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array of frequencies").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float resolution =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().FloatValue() : 0.01f;
  int bins_per_octave =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 12;
  float out_tuning = 0.0f;
  SonareError err = sonare_pitch_tuning(arr.Data(), arr.ElementLength(), resolution,
                                        bins_per_octave, &out_tuning);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return Napi::Number::New(env, out_tuning);
}

Napi::Value SonareWrap::EstimateTuning(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  float resolution =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().FloatValue() : 0.01f;
  int bins_per_octave =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 12;
  float out_tuning = 0.0f;
  SonareError err = sonare_estimate_tuning(arr.Data(), arr.ElementLength(), sr, n_fft, hop_length,
                                           resolution, bins_per_octave, &out_tuning);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return Napi::Number::New(env, out_tuning);
}
