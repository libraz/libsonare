#include <cstring>
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
#include "sonare_wrap_options.h"
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
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 2, "hopLength", 512, &hop_length)) return env.Undefined();
  float fmin{};
  if (!OptionalFiniteFloatArg(env, info, 3, "fmin", sonare::constants::kC1Hz, &fmin))
    return env.Undefined();
  int n_bins{};
  if (!OptionalIntArg(env, info, 4, "nBins", 84, &n_bins)) return env.Undefined();
  int bins_per_octave{};
  if (!OptionalIntArg(env, info, 5, "binsPerOctave", 12, &bins_per_octave)) return env.Undefined();

  SonareCqtResult result{};
  SonareError err = fn(typed.Data(), typed.ElementLength(), sr, hop_length, fmin, n_bins,
                       bins_per_octave, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  Napi::Value out = CqtResultToObject(env, result);
  sonare_free_cqt_result(&result);
  return out;
}

}  // namespace

Napi::Value SonareWrap::Cqt(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY return CqtLike(info, sonare_cqt);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PseudoCqt(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return CqtLike(info, sonare_pseudo_cqt);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::HybridCqt(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return CqtLike(info, sonare_hybrid_cqt);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Vqt(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 2, "hopLength", 512, &hop_length)) return env.Undefined();
  float fmin{};
  if (!OptionalFiniteFloatArg(env, info, 3, "fmin", sonare::constants::kC1Hz, &fmin))
    return env.Undefined();
  int n_bins{};
  if (!OptionalIntArg(env, info, 4, "nBins", 84, &n_bins)) return env.Undefined();
  int bins_per_octave{};
  if (!OptionalIntArg(env, info, 5, "binsPerOctave", 12, &bins_per_octave)) return env.Undefined();
  float gamma{};
  if (!OptionalFloatArg(env, info, 6, "gamma", -1.0f, &gamma)) return env.Undefined();

  SonareCqtResult result{};
  SonareError err = sonare_vqt(typed.Data(), typed.ElementLength(), sr, hop_length, fmin, n_bins,
                               bins_per_octave, gamma, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  Napi::Value out = CqtResultToObject(env, result);
  sonare_free_cqt_result(&result);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::CqtToAudio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, nBins, nFrames, sampleRate?, hopLength?, "
                         "fmin?, binsPerOctave?, nIter?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto magnitude = info[0].As<Napi::Float32Array>();
  const int n_bins = node_narrow_int(env, info[1], "nBins");
  const int n_frames = node_narrow_int(env, info[2], "nFrames");
  int sample_rate{};
  if (!OptionalIntArg(env, info, 3, "sampleRate", 22050, &sample_rate)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 4, "hopLength", 512, &hop_length)) return env.Undefined();
  float fmin{};
  if (!OptionalFiniteFloatArg(env, info, 5, "fmin", sonare::constants::kC1Hz, &fmin))
    return env.Undefined();
  int bins_per_octave{};
  if (!OptionalIntArg(env, info, 6, "binsPerOctave", 12, &bins_per_octave)) return env.Undefined();
  int n_iter{};
  if (!OptionalIntArg(env, info, 7, "nIter", 32, &n_iter)) return env.Undefined();
  float* output = nullptr;
  size_t output_length = 0;
  const SonareError error = sonare_cqt_to_audio_checked(
      magnitude.Data(), magnitude.ElementLength(), n_bins, n_frames, sample_rate, hop_length, fmin,
      bins_per_octave, n_iter, &output, &output_length);
  if (error != SONARE_OK) return CheckCResult(env, error);
  std::vector<float> owned(output, output + output_length);
  sonare_free_floats(output);
  return VecToFloat32(env, owned);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::VqtToAudio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, nBins, nFrames, sampleRate?, hopLength?, "
                         "fmin?, binsPerOctave?, gamma?, nIter?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto magnitude = info[0].As<Napi::Float32Array>();
  const int n_bins = node_narrow_int(env, info[1], "nBins");
  const int n_frames = node_narrow_int(env, info[2], "nFrames");
  int sample_rate{};
  if (!OptionalIntArg(env, info, 3, "sampleRate", 22050, &sample_rate)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 4, "hopLength", 512, &hop_length)) return env.Undefined();
  float fmin{};
  if (!OptionalFiniteFloatArg(env, info, 5, "fmin", sonare::constants::kC1Hz, &fmin))
    return env.Undefined();
  int bins_per_octave{};
  if (!OptionalIntArg(env, info, 6, "binsPerOctave", 12, &bins_per_octave)) return env.Undefined();
  float gamma{};
  if (!OptionalFloatArg(env, info, 7, "gamma", -1.0f, &gamma)) return env.Undefined();
  int n_iter{};
  if (!OptionalIntArg(env, info, 8, "nIter", 32, &n_iter)) return env.Undefined();
  float* output = nullptr;
  size_t output_length = 0;
  const SonareError error = sonare_vqt_to_audio_checked(
      magnitude.Data(), magnitude.ElementLength(), n_bins, n_frames, sample_rate, hop_length, fmin,
      bins_per_octave, gamma, n_iter, &output, &output_length);
  if (error != SONARE_OK) return CheckCResult(env, error);
  std::vector<float> owned(output, output + output_length);
  sonare_free_floats(output);
  return VecToFloat32(env, owned);
  SONARE_NODE_CATCH(env)
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
  const int n_mels = node_narrow_int(env, info[1], "nMels");
  const int n_frames = node_narrow_int(env, info[2], "nFrames");
  if (!ValidateMatrixDims(env, "melToStft", n_mels, n_frames, typed.ElementLength())) {
    return env.Undefined();
  }
  int sr{};
  if (!OptionalIntArg(env, info, 3, "sampleRate", 22050, &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 4, "nFft", 2048, &n_fft)) return env.Undefined();
  float fmin{};
  if (!OptionalFiniteFloatArg(env, info, 5, "fmin", 0.0f, &fmin)) return env.Undefined();
  float fmax{};
  if (!OptionalFiniteFloatArg(env, info, 6, "fmax", 0.0f, &fmax)) return env.Undefined();
  bool htk = false;
  if (!OptionalBoolArg(env, info, 7, "htk", false, &htk)) {
    return env.Undefined();
  }

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
  const int n_mels = node_narrow_int(env, info[1], "nMels");
  const int n_frames = node_narrow_int(env, info[2], "nFrames");
  if (!ValidateMatrixDims(env, "melToAudio", n_mels, n_frames, typed.ElementLength())) {
    return env.Undefined();
  }
  int sr{};
  if (!OptionalIntArg(env, info, 3, "sampleRate", 22050, &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 4, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 5, "hopLength", 512, &hop_length)) return env.Undefined();
  float fmin{};
  if (!OptionalFiniteFloatArg(env, info, 6, "fmin", 0.0f, &fmin)) return env.Undefined();
  float fmax{};
  if (!OptionalFiniteFloatArg(env, info, 7, "fmax", 0.0f, &fmax)) return env.Undefined();
  int n_iter{};
  if (!OptionalIntArg(env, info, 8, "nIter", 32, &n_iter)) return env.Undefined();
  bool htk = false;
  if (!OptionalBoolArg(env, info, 9, "htk", false, &htk)) {
    return env.Undefined();
  }

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

Napi::Value SonareWrap::GriffinLim(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(
        env,
        "Expected (magnitude, nBins, nFrames, sampleRate?, nFft?, hopLength?, nIter?, momentum?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  const auto magnitude = info[0].As<Napi::Float32Array>();
  const int n_bins = node_narrow_int(env, info[1], "nBins");
  const int n_frames = node_narrow_int(env, info[2], "nFrames");
  if (!ValidateMatrixDims(env, "griffinLim", n_bins, n_frames, magnitude.ElementLength())) {
    return env.Undefined();
  }
  int sample_rate{};
  if (!OptionalIntArg(env, info, 3, "sampleRate", 22050, &sample_rate)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 4, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 5, "hopLength", 512, &hop_length)) return env.Undefined();
  int n_iter{};
  if (!OptionalIntArg(env, info, 6, "nIter", 32, &n_iter)) return env.Undefined();
  float momentum{};
  if (!OptionalFiniteFloatArg(env, info, 7, "momentum", 0.99f, &momentum)) return env.Undefined();
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_griffin_lim(magnitude.Data(), magnitude.ElementLength(), n_bins, n_frames, n_fft,
                         hop_length, sample_rate, n_iter, momentum, &out, &out_length);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, out_length);
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
  const int n_mfcc = node_narrow_int(env, info[1], "nMfcc");
  const int n_frames = node_narrow_int(env, info[2], "nFrames");
  if (!ValidateMatrixDims(env, "mfccToMel", n_mfcc, n_frames, typed.ElementLength())) {
    return env.Undefined();
  }
  int n_mels{};
  if (!OptionalIntArg(env, info, 3, "nMels", 128, &n_mels)) return env.Undefined();
  float lifter{};
  if (!OptionalFiniteFloatArg(env, info, 4, "lifter", 0.0f, &lifter)) return env.Undefined();

  std::vector<float> mel = sonare::mfcc_to_mel(typed.Data(), n_mfcc, n_frames, n_mels, lifter);

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
  const int n_mfcc = node_narrow_int(env, info[1], "nMfcc");
  const int n_frames = node_narrow_int(env, info[2], "nFrames");
  if (!ValidateMatrixDims(env, "mfccToAudio", n_mfcc, n_frames, typed.ElementLength())) {
    return env.Undefined();
  }
  int n_mels{};
  if (!OptionalIntArg(env, info, 3, "nMels", 128, &n_mels)) return env.Undefined();
  int sr{};
  if (!OptionalIntArg(env, info, 4, "sampleRate", 22050, &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 5, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 6, "hopLength", 512, &hop_length)) return env.Undefined();
  float fmin{};
  if (!OptionalFiniteFloatArg(env, info, 7, "fmin", 0.0f, &fmin)) return env.Undefined();
  float fmax{};
  if (!OptionalFiniteFloatArg(env, info, 8, "fmax", 0.0f, &fmax)) return env.Undefined();
  int n_iter{};
  if (!OptionalIntArg(env, info, 9, "nIter", 32, &n_iter)) return env.Undefined();
  bool htk = false;
  if (!OptionalBoolArg(env, info, 10, "htk", false, &htk)) {
    return env.Undefined();
  }
  float lifter{};
  if (!OptionalFiniteFloatArg(env, info, 11, "lifter", 0.0f, &lifter)) return env.Undefined();

  sonare::MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  sonare::Audio result =
      sonare::mfcc_to_audio(typed.Data(), n_mfcc, n_frames, config, n_iter, sr, lifter);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

// ============================================================================
// Features - Spectral contrast / poly features / zero crossings / tuning
// ============================================================================

Napi::Value SonareWrap::SpectralContrast(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();
  int n_bands{};
  if (!OptionalIntArg(env, info, 4, "nBands", 6, &n_bands)) return env.Undefined();
  float fmin{};
  if (!OptionalFiniteFloatArg(env, info, 5, "fmin", 200.0f, &fmin)) return env.Undefined();
  float quantile{};
  if (!OptionalFiniteFloatArg(env, info, 6, "quantile", 0.02f, &quantile)) return env.Undefined();
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PolyFeatures(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();
  int order{};
  if (!OptionalIntArg(env, info, 4, "order", 1, &order)) return env.Undefined();
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ZeroCrossings(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float threshold{};
  if (!OptionalFiniteFloatArg(env, info, 1, "threshold", 1e-10f, &threshold))
    return env.Undefined();
  bool ref_magnitude = false;
  if (!OptionalBoolArg(env, info, 2, "refMagnitude", false, &ref_magnitude)) {
    return env.Undefined();
  }
  bool pad = true;
  if (!OptionalBoolArg(env, info, 3, "pad", true, &pad)) {
    return env.Undefined();
  }
  bool zero_pos = true;
  if (!OptionalBoolArg(env, info, 4, "zeroPos", true, &zero_pos)) {
    return env.Undefined();
  }
  int* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_zero_crossings(arr.Data(), arr.ElementLength(), threshold, ref_magnitude,
                                          pad, zero_pos, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return IntResult(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PitchTuning(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array of frequencies").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float resolution{};
  if (!OptionalFiniteFloatArg(env, info, 1, "resolution", 0.01f, &resolution))
    return env.Undefined();
  int bins_per_octave{};
  if (!OptionalIntArg(env, info, 2, "binsPerOctave", 12, &bins_per_octave)) return env.Undefined();
  float out_tuning = 0.0f;
  SonareError err = sonare_pitch_tuning(arr.Data(), arr.ElementLength(), resolution,
                                        bins_per_octave, &out_tuning);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return Napi::Number::New(env, out_tuning);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::EstimateTuning(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();
  float resolution{};
  if (!OptionalFiniteFloatArg(env, info, 4, "resolution", 0.01f, &resolution))
    return env.Undefined();
  int bins_per_octave{};
  if (!OptionalIntArg(env, info, 5, "binsPerOctave", 12, &bins_per_octave)) return env.Undefined();
  float out_tuning = 0.0f;
  SonareError err = sonare_estimate_tuning(arr.Data(), arr.ElementLength(), sr, n_fft, hop_length,
                                           resolution, bins_per_octave, &out_tuning);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return Napi::Number::New(env, out_tuning);
  SONARE_NODE_CATCH(env)
}
