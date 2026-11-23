#include <cstring>
#include <limits>
#include <vector>

#include "c_api/inverse_input_domains.h"
#include "core/audio.h"
#include "core/convert.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "feature/chroma.h"
#include "feature/cqt.h"
#include "feature/inverse.h"
#include "feature/mel_spectrogram.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "feature/vqt.h"
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
    std::memcpy(sonare_node::ElementData(magnitude), result.magnitude,
                magnitude_count * sizeof(float));
  }
  out.Set("magnitude", magnitude);

  auto frequencies = Napi::Float32Array::New(env, static_cast<size_t>(result.n_bins));
  if (result.n_bins > 0 && result.frequencies != nullptr) {
    std::memcpy(sonare_node::ElementData(frequencies), result.frequencies,
                static_cast<size_t>(result.n_bins) * sizeof(float));
  }
  out.Set("frequencies", frequencies);
  out.Set("gamma", Napi::Number::New(env, result.gamma));
  return out;
}

using CqtFn = SonareError (*)(const float*, size_t, int, size_t, float, int, int, float,
                              SonareCqtResult*);

Napi::Value CqtLike(const Napi::CallbackInfo& info, CqtFn fn) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();

  auto typed = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();
  int hop_length{};
  if (!RequiredDomainArg(env, info, 2, "hopLength", sonare::kCqtHopLengthDomain, &hop_length)) {
    return env.Undefined();
  }
  float fmin{};
  if (!RequiredFloatValue(env, info[3], "fmin", &fmin)) return env.Undefined();
  int n_bins{};
  if (!RequiredDomainArg(env, info, 4, "nBins", sonare::kCqtCountDomain, &n_bins)) {
    return env.Undefined();
  }
  int bins_per_octave{};
  if (!RequiredDomainArg(env, info, 5, "binsPerOctave", sonare::kCqtCountDomain,
                         &bins_per_octave)) {
    return env.Undefined();
  }
  float filter_scale{};
  if (!OptionalFloatArg(env, info, 6, "filterScale", 1.0f, &filter_scale)) return env.Undefined();

  SonareCqtResult result{};
  SonareError err = fn(sonare_node::ElementData(typed), typed.ElementLength(), sr, hop_length, fmin,
                       n_bins, bins_per_octave, filter_scale, &result);
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

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();

  auto typed = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();
  int hop_length{};
  if (!OptionalDomainArg(env, info, 2, "hopLength", sonare::constants::kDefaultHopLength,
                         sonare::kCqtHopLengthDomain, &hop_length)) {
    return env.Undefined();
  }
  float fmin{};
  if (!OptionalFloatArg(env, info, 3, "fmin", sonare::constants::kC1Hz, &fmin))
    return env.Undefined();
  int n_bins{};
  if (!OptionalDomainArg(env, info, 4, "nBins", 84, sonare::kCqtCountDomain, &n_bins)) {
    return env.Undefined();
  }
  int bins_per_octave{};
  if (!OptionalDomainArg(env, info, 5, "binsPerOctave", 12, sonare::kCqtCountDomain,
                         &bins_per_octave)) {
    return env.Undefined();
  }
  float filter_scale{};
  if (!OptionalFloatArg(env, info, 6, "filterScale", 1.0f, &filter_scale)) return env.Undefined();
  // Absent is the C ABI's NaN, the ERB-derived automatic bandwidth.
  float gamma{};
  if (!OptionalDomainArg(env, info, 7, "gamma", std::numeric_limits<float>::quiet_NaN(),
                         sonare::kVqtGammaDomain, &gamma))
    return env.Undefined();

  SonareCqtResult result{};
  SonareError err =
      sonare_vqt(sonare_node::ElementData(typed), typed.ElementLength(), sr, hop_length, fmin,
                 n_bins, bins_per_octave, filter_scale, gamma, &result);
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
  if (!RequireFloat32Array(info, 0, "magnitude must be a Float32Array")) return env.Undefined();
  auto magnitude = info[0].As<Napi::Float32Array>();
  int n_bins{};
  if (!RequiredIntArg(env, info, 1, "nBins", &n_bins)) return env.Undefined();
  int n_frames{};
  if (!RequiredIntArg(env, info, 2, "nFrames", &n_frames)) return env.Undefined();
  int sample_rate{};
  if (!OptionalIntArg(env, info, 3, "sampleRate", sonare::constants::kDefaultSampleRate,
                      &sample_rate))
    return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 4, "hopLength", sonare::constants::kDefaultHopLength, &hop_length))
    return env.Undefined();
  float fmin{};
  if (!OptionalFloatArg(env, info, 5, "fminHz", sonare::constants::kC1Hz, &fmin))
    return env.Undefined();
  int bins_per_octave{};
  if (!OptionalIntArg(env, info, 6, "binsPerOctave", 12, &bins_per_octave)) return env.Undefined();
  int n_iter{};
  if (!OptionalDomainArg(env, info, 7, "nIter", sonare::constants::kDefaultGriffinLimIterations,
                         sonare_c_detail::kGriffinLimIterationsDomain, &n_iter)) {
    return env.Undefined();
  }
  float* output = nullptr;
  size_t output_length = 0;
  const SonareError error = sonare_cqt_to_audio(
      sonare_node::ElementData(magnitude), magnitude.ElementLength(), n_bins, n_frames, sample_rate,
      hop_length, fmin, bins_per_octave, n_iter, &output, &output_length);
  if (error != SONARE_OK) return CheckCResult(env, error);
  return TakeCFloats(env, output, output_length);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::VqtToAudio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "magnitude must be a Float32Array")) return env.Undefined();
  auto magnitude = info[0].As<Napi::Float32Array>();
  int n_bins{};
  if (!RequiredIntArg(env, info, 1, "nBins", &n_bins)) return env.Undefined();
  int n_frames{};
  if (!RequiredIntArg(env, info, 2, "nFrames", &n_frames)) return env.Undefined();
  int sample_rate{};
  if (!OptionalIntArg(env, info, 3, "sampleRate", sonare::constants::kDefaultSampleRate,
                      &sample_rate))
    return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 4, "hopLength", sonare::constants::kDefaultHopLength, &hop_length))
    return env.Undefined();
  float fmin{};
  if (!OptionalFloatArg(env, info, 5, "fminHz", sonare::constants::kC1Hz, &fmin))
    return env.Undefined();
  int bins_per_octave{};
  if (!OptionalIntArg(env, info, 6, "binsPerOctave", 12, &bins_per_octave)) return env.Undefined();
  // Absent is the C ABI's NaN, the ERB-derived automatic bandwidth.
  float gamma{};
  if (!OptionalDomainArg(env, info, 7, "gamma", std::numeric_limits<float>::quiet_NaN(),
                         sonare::kVqtGammaDomain, &gamma))
    return env.Undefined();
  int n_iter{};
  if (!OptionalDomainArg(env, info, 8, "nIter", sonare::constants::kDefaultGriffinLimIterations,
                         sonare_c_detail::kGriffinLimIterationsDomain, &n_iter)) {
    return env.Undefined();
  }
  float* output = nullptr;
  size_t output_length = 0;
  const SonareError error = sonare_vqt_to_audio(
      sonare_node::ElementData(magnitude), magnitude.ElementLength(), n_bins, n_frames, sample_rate,
      hop_length, fmin, bins_per_octave, gamma, n_iter, &output, &output_length);
  if (error != SONARE_OK) return CheckCResult(env, error);
  return TakeCFloats(env, output, output_length);
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

  if (!RequireFloat32Array(info, 0, "mel must be a Float32Array")) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  int n_mels{};
  if (!RequiredIntArg(env, info, 1, "nMels", &n_mels)) return env.Undefined();
  int n_frames{};
  if (!RequiredIntArg(env, info, 2, "nFrames", &n_frames)) return env.Undefined();
  if (!ValidateMatrixDims(env, n_mels, n_frames, typed.ElementLength())) {
    return env.Undefined();
  }
  int sr{};
  if (!OptionalDomainArg(env, info, 3, "sampleRate", sonare::constants::kDefaultSampleRate,
                         sonare::kInverseSampleRateDomain, &sr)) {
    return env.Undefined();
  }
  int n_fft{};
  if (!OptionalDomainArg(env, info, 4, "nFft", sonare::constants::kDefaultNFft,
                         sonare::kMelToStftFftSizeDomain, &n_fft)) {
    return env.Undefined();
  }
  float fmin{};
  if (!OptionalFloatArg(env, info, 5, "fmin", 0.0f, &fmin)) return env.Undefined();
  float fmax{};
  if (!OptionalFloatArg(env, info, 6, "fmax", 0.0f, &fmax)) return env.Undefined();
  sonare::validate_mel_bounds(fmin, fmax);
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

  std::vector<float> stft =
      sonare::mel_to_stft(sonare_node::ElementData(typed), n_mels, n_frames, config, sr);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nBins", Napi::Number::New(env, n_fft / 2 + 1));
  out.Set("nFrames", Napi::Number::New(env, n_frames));
  out.Set("magnitude", Float32ArrayFrom(env, stft));
  return out;
  SONARE_NODE_CATCH(env)
}

// melToAudio(mel, nMels, nFrames, sampleRate?, nFft?, hopLength?, fmin?, fmax?, nIter?, htk?)
// -> Float32Array (reconstructed audio)
Napi::Value SonareWrap::MelToAudio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "mel must be a Float32Array")) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  int n_mels{};
  if (!RequiredIntArg(env, info, 1, "nMels", &n_mels)) return env.Undefined();
  int n_frames{};
  if (!RequiredIntArg(env, info, 2, "nFrames", &n_frames)) return env.Undefined();
  if (!ValidateMatrixDims(env, n_mels, n_frames, typed.ElementLength())) {
    return env.Undefined();
  }
  int sr{};
  if (!OptionalDomainArg(env, info, 3, "sampleRate", sonare::constants::kDefaultSampleRate,
                         sonare::kInverseAudioSampleRateDomain, &sr)) {
    return env.Undefined();
  }
  int n_fft{};
  if (!OptionalIntArg(env, info, 4, "nFft", sonare::constants::kDefaultNFft, &n_fft)) {
    return env.Undefined();
  }
  int hop_length{};
  if (!OptionalResynthesisHopArg(env, info, 5, &hop_length)) return env.Undefined();
  float fmin{};
  if (!OptionalFloatArg(env, info, 6, "fmin", 0.0f, &fmin)) return env.Undefined();
  float fmax{};
  if (!OptionalFloatArg(env, info, 7, "fmax", 0.0f, &fmax)) return env.Undefined();
  sonare::validate_mel_bounds(fmin, fmax);
  int n_iter{};
  if (!OptionalIntArg(env, info, 8, "nIter", sonare::constants::kDefaultGriffinLimIterations,
                      &n_iter))
    return env.Undefined();
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

  sonare::Audio result =
      sonare::mel_to_audio(sonare_node::ElementData(typed), n_mels, n_frames, config, n_iter, sr);
  return Float32ArrayFrom(env, result.data(), result.size());
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::GriffinLim(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "magnitude must be a Float32Array")) return env.Undefined();
  SONARE_NODE_TRY
  const auto magnitude = info[0].As<Napi::Float32Array>();
  int n_bins{};
  if (!RequiredIntArg(env, info, 1, "nBins", &n_bins)) return env.Undefined();
  int n_frames{};
  if (!RequiredIntArg(env, info, 2, "nFrames", &n_frames)) return env.Undefined();
  if (!ValidateMatrixDims(env, n_bins, n_frames, magnitude.ElementLength())) {
    return env.Undefined();
  }
  int sample_rate{};
  if (!RequiredIntArg(env, info, 3, "sampleRate", &sample_rate)) return env.Undefined();
  int n_fft{};
  if (!RequiredIntArg(env, info, 4, "nFft", &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalResynthesisHopArg(env, info, 5, &hop_length)) return env.Undefined();
  int n_iter{};
  if (!OptionalDomainArg(env, info, 6, "nIter", sonare::constants::kDefaultGriffinLimIterations,
                         sonare_c_detail::kGriffinLimIterationsDomain, &n_iter)) {
    return env.Undefined();
  }
  float momentum{};
  if (!OptionalFloatArg(env, info, 7, "momentum", 0.99f, &momentum)) return env.Undefined();
  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err = sonare_griffin_lim(
      sonare_node::ElementData(magnitude), magnitude.ElementLength(), n_bins, n_frames, n_fft,
      hop_length, sample_rate, n_iter, momentum, &out, &out_length);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return TakeCFloats(env, out, out_length);
  SONARE_NODE_CATCH(env)
}

// mfccToMel(mfcc, nMfcc, nFrames, nMels?)
// -> { nMels, nFrames, power: Float32Array } (Mel power)
Napi::Value SonareWrap::MfccToMel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "mfcc must be a Float32Array")) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  int n_mfcc{};
  if (!RequiredIntArg(env, info, 1, "nMfcc", &n_mfcc)) return env.Undefined();
  int n_frames{};
  if (!RequiredIntArg(env, info, 2, "nFrames", &n_frames)) return env.Undefined();
  if (!ValidateMatrixDims(env, n_mfcc, n_frames, typed.ElementLength())) {
    return env.Undefined();
  }
  int n_mels{};
  if (!OptionalIntArg(env, info, 3, "nMels", 128, &n_mels)) return env.Undefined();
  float lifter{};
  if (!OptionalFloatArg(env, info, 4, "lifter", 0.0f, &lifter)) return env.Undefined();

  std::vector<float> mel =
      sonare::mfcc_to_mel(sonare_node::ElementData(typed), n_mfcc, n_frames, n_mels, lifter);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nMels", Napi::Number::New(env, n_mels));
  out.Set("nFrames", Napi::Number::New(env, n_frames));
  out.Set("power", Float32ArrayFrom(env, mel));
  return out;
  SONARE_NODE_CATCH(env)
}

// mfccToAudio(mfcc, nMfcc, nFrames, nMels?, sampleRate?, nFft?, hopLength?, fmin?, fmax?, nIter?,
// htk?)
// -> Float32Array (reconstructed audio)
Napi::Value SonareWrap::MfccToAudio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "mfcc must be a Float32Array")) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  int n_mfcc{};
  if (!RequiredIntArg(env, info, 1, "nMfcc", &n_mfcc)) return env.Undefined();
  int n_frames{};
  if (!RequiredIntArg(env, info, 2, "nFrames", &n_frames)) return env.Undefined();
  if (!ValidateMatrixDims(env, n_mfcc, n_frames, typed.ElementLength())) {
    return env.Undefined();
  }
  int n_mels{};
  if (!OptionalIntArg(env, info, 3, "nMels", 128, &n_mels)) return env.Undefined();
  int sr{};
  if (!OptionalDomainArg(env, info, 4, "sampleRate", sonare::constants::kDefaultSampleRate,
                         sonare::kInverseAudioSampleRateDomain, &sr)) {
    return env.Undefined();
  }
  int n_fft{};
  if (!OptionalIntArg(env, info, 5, "nFft", sonare::constants::kDefaultNFft, &n_fft)) {
    return env.Undefined();
  }
  int hop_length{};
  if (!OptionalResynthesisHopArg(env, info, 6, &hop_length)) return env.Undefined();
  float fmin{};
  if (!OptionalFloatArg(env, info, 7, "fmin", 0.0f, &fmin)) return env.Undefined();
  float fmax{};
  if (!OptionalFloatArg(env, info, 8, "fmax", 0.0f, &fmax)) return env.Undefined();
  sonare::validate_mel_bounds(fmin, fmax);
  int n_iter{};
  if (!OptionalIntArg(env, info, 9, "nIter", sonare::constants::kDefaultGriffinLimIterations,
                      &n_iter))
    return env.Undefined();
  bool htk = false;
  if (!OptionalBoolArg(env, info, 10, "htk", false, &htk)) {
    return env.Undefined();
  }
  float lifter{};
  if (!OptionalFloatArg(env, info, 11, "lifter", 0.0f, &lifter)) return env.Undefined();

  sonare::MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  sonare::Audio result = sonare::mfcc_to_audio(sonare_node::ElementData(typed), n_mfcc, n_frames,
                                               config, n_iter, sr, lifter);
  return Float32ArrayFrom(env, result.data(), result.size());
  SONARE_NODE_CATCH(env)
}

// ============================================================================
// Features - Spectral contrast / poly features / zero crossings / tuning
// ============================================================================

Napi::Value SonareWrap::SpectralContrast(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto arr = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", sonare::constants::kDefaultNFft, &n_fft))
    return env.Undefined();
  int hop_length{};
  if (!OptionalDomainArg(env, info, 3, "hopLength", sonare::constants::kDefaultHopLength,
                         sonare::kStftHopDomain, &hop_length)) {
    return env.Undefined();
  }
  int n_bands{};
  if (!OptionalDomainArg(env, info, 4, "nBands", 6, sonare::kSpectralCountDomain, &n_bands)) {
    return env.Undefined();
  }
  float fmin{};
  if (!OptionalFloatArg(env, info, 5, "fmin", 200.0f, &fmin)) return env.Undefined();
  float quantile{};
  if (!OptionalFloatArg(env, info, 6, "quantile", 0.02f, &quantile)) return env.Undefined();
  float* out = nullptr;
  size_t count = 0;
  int out_n_subbands = 0;
  int out_n_frames = 0;
  SonareError err = sonare_spectral_contrast(sonare_node::ElementData(arr), arr.ElementLength(), sr,
                                             n_fft, hop_length, n_bands, fmin, quantile, &out,
                                             &count, &out_n_subbands, &out_n_frames);
  if (err != SONARE_OK) return CheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("rows", Napi::Number::New(env, out_n_subbands));
  result.Set("cols", Napi::Number::New(env, out_n_frames));
  result.Set("data", TakeCFloats(env, out, count));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PolyFeatures(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto arr = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", sonare::constants::kDefaultNFft, &n_fft))
    return env.Undefined();
  int hop_length{};
  if (!OptionalDomainArg(env, info, 3, "hopLength", sonare::constants::kDefaultHopLength,
                         sonare::kStftHopDomain, &hop_length)) {
    return env.Undefined();
  }
  int order{};
  if (!RequiredDomainArg(env, info, 4, "order", sonare::kSpectralOrderDomain, &order)) {
    return env.Undefined();
  }
  float* out = nullptr;
  size_t count = 0;
  int out_n_coefficients = 0;
  int out_n_frames = 0;
  SonareError err =
      sonare_poly_features(sonare_node::ElementData(arr), arr.ElementLength(), sr, n_fft,
                           hop_length, order, &out, &count, &out_n_coefficients, &out_n_frames);
  if (err != SONARE_OK) return CheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("rows", Napi::Number::New(env, out_n_coefficients));
  result.Set("cols", Napi::Number::New(env, out_n_frames));
  result.Set("data", TakeCFloats(env, out, count));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ZeroCrossings(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto arr = info[0].As<Napi::Float32Array>();
  float threshold{};
  if (!OptionalFloatArg(env, info, 1, "threshold", 1e-10f, &threshold)) return env.Undefined();
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
  SonareError err = sonare_zero_crossings(sonare_node::ElementData(arr), arr.ElementLength(),
                                          threshold, ref_magnitude, pad, zero_pos, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return TakeCInts(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PitchTuning(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "frequenciesHz must be a Float32Array")) return env.Undefined();
  auto arr = info[0].As<Napi::Float32Array>();
  float resolution{};
  if (!OptionalFloatArg(env, info, 1, "resolution", 0.01f, &resolution)) return env.Undefined();
  int bins_per_octave{};
  if (!OptionalDomainArg(env, info, 2, "binsPerOctave", 12, sonare::kPitchCountDomain,
                         &bins_per_octave)) {
    return env.Undefined();
  }
  float out_tuning = 0.0f;
  SonareError err = sonare_pitch_tuning(sonare_node::ElementData(arr), arr.ElementLength(),
                                        resolution, bins_per_octave, &out_tuning);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return Napi::Number::New(env, out_tuning);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::TuningToReferenceHz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  float tuning_cents{};
  if (!RequiredFloatArg(env, info, 0, "tuningCents", &tuning_cents)) return env.Undefined();
  float a4{};
  if (!OptionalFloatArg(env, info, 1, "a4", sonare::constants::kA4Hz, &a4)) return env.Undefined();
  float out_hz = 0.0f;
  SonareError err = sonare_tuning_to_reference_hz(tuning_cents, a4, &out_hz);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return Napi::Number::New(env, out_hz);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ReferenceHzToTuning(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  float hz{};
  if (!RequiredFloatArg(env, info, 0, "hz", &hz)) return env.Undefined();
  float a4{};
  if (!OptionalFloatArg(env, info, 1, "a4", sonare::constants::kA4Hz, &a4)) return env.Undefined();
  float out_tuning = 0.0f;
  SonareError err = sonare_reference_hz_to_tuning(hz, a4, &out_tuning);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return Napi::Number::New(env, out_tuning);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::EstimateTuning(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  auto arr = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", sonare::constants::kDefaultNFft, &n_fft))
    return env.Undefined();
  int hop_length{};
  if (!OptionalDomainArg(env, info, 3, "hopLength", sonare::constants::kDefaultHopLength,
                         sonare::kStftHopDomain, &hop_length)) {
    return env.Undefined();
  }
  float resolution{};
  if (!OptionalFloatArg(env, info, 4, "resolution", 0.01f, &resolution)) return env.Undefined();
  int bins_per_octave{};
  if (!OptionalDomainArg(env, info, 5, "binsPerOctave", 12, sonare::kPitchCountDomain,
                         &bins_per_octave)) {
    return env.Undefined();
  }
  float out_tuning = 0.0f;
  SonareError err =
      sonare_estimate_tuning(sonare_node::ElementData(arr), arr.ElementLength(), sr, n_fft,
                             hop_length, resolution, bins_per_octave, &out_tuning);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return Napi::Number::New(env, out_tuning);
  SONARE_NODE_CATCH(env)
}
