#include <cstring>
#include <string>
#include <vector>

#include "core/audio.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "feature/chroma.h"
#include "feature/inverse.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "features/common.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;
using namespace sonare_node::features;

Napi::Value SonareWrap::Stft(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();

  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nBins", Napi::Number::New(env, spec.n_bins()));
  out.Set("nFrames", Napi::Number::New(env, spec.n_frames()));
  out.Set("nFft", Napi::Number::New(env, spec.n_fft()));
  out.Set("hopLength", Napi::Number::New(env, spec.hop_length()));
  out.Set("sampleRate", Napi::Number::New(env, spec.sample_rate()));
  out.Set("magnitude", VecToFloat32(env, spec.magnitude()));
  out.Set("power", VecToFloat32(env, spec.power()));

  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::StftDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();

  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nBins", Napi::Number::New(env, spec.n_bins()));
  out.Set("nFrames", Napi::Number::New(env, spec.n_frames()));
  out.Set("db", VecToFloat32(env, spec.to_db()));

  return out;
  SONARE_NODE_CATCH(env)
}

// ============================================================================
// Features - Mel
// ============================================================================

Napi::Value SonareWrap::MelSpectrogramFn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();
  int n_mels{};
  if (!OptionalIntArg(env, info, 4, "nMels", 128, &n_mels)) return env.Undefined();
  float fmin{};
  if (!OptionalFloatArg(env, info, 5, "fmin", 0.0f, &fmin)) return env.Undefined();
  float fmax{};
  if (!OptionalFloatArg(env, info, 6, "fmax", 0.0f, &fmax)) return env.Undefined();
  bool htk{};
  if (!OptionalBoolArg(env, info, 7, "htk", false, &htk)) return env.Undefined();

  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  sonare::MelSpectrogram mel = sonare::MelSpectrogram::compute(audio, config);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nMels", Napi::Number::New(env, mel.n_mels()));
  out.Set("nFrames", Napi::Number::New(env, mel.n_frames()));
  out.Set("sampleRate", Napi::Number::New(env, mel.sample_rate()));
  out.Set("hopLength", Napi::Number::New(env, mel.hop_length()));

  // Power values
  std::vector<float> power_vec(mel.power_data(), mel.power_data() + mel.n_mels() * mel.n_frames());
  out.Set("power", VecToFloat32(env, power_vec));

  // dB values
  out.Set("db", VecToFloat32(env, mel.to_db()));

  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Mfcc(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();
  int n_mels{};
  if (!OptionalIntArg(env, info, 4, "nMels", 128, &n_mels)) return env.Undefined();
  int n_mfcc{};
  if (!OptionalIntArg(env, info, 5, "nMfcc", 20, &n_mfcc)) return env.Undefined();
  float fmin{};
  if (!OptionalFloatArg(env, info, 6, "fmin", 0.0f, &fmin)) return env.Undefined();
  float fmax{};
  if (!OptionalFloatArg(env, info, 7, "fmax", 0.0f, &fmax)) return env.Undefined();
  bool htk{};
  if (!OptionalBoolArg(env, info, 8, "htk", false, &htk)) return env.Undefined();
  float lifter{};
  if (!OptionalFloatArg(env, info, 9, "lifter", 0.0f, &lifter)) return env.Undefined();

  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  sonare::MelSpectrogram mel = sonare::MelSpectrogram::compute(audio, config);
  std::vector<float> mfcc_coeffs = mel.mfcc(n_mfcc, lifter);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nMfcc", Napi::Number::New(env, n_mfcc));
  out.Set("nFrames", Napi::Number::New(env, mel.n_frames()));
  out.Set("coefficients", VecToFloat32(env, mfcc_coeffs));

  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MelDelta(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected Float32Array feature matrix")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  const auto typed = info[0].As<Napi::Float32Array>();
  int n_features{};
  if (!OptionalIntArg(env, info, 1, "nFeatures", 0, &n_features)) return env.Undefined();
  int n_frames{};
  if (!OptionalIntArg(env, info, 2, "nFrames", 0, &n_frames)) return env.Undefined();
  int width{};
  if (!OptionalIntArg(env, info, 3, "width", 9, &width)) return env.Undefined();
  if (n_features <= 0 || n_frames <= 0 ||
      static_cast<size_t>(n_features) * static_cast<size_t>(n_frames) != typed.ElementLength()) {
    Napi::TypeError::New(env, "feature matrix length must equal nFeatures * nFrames")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return VecToFloat32(env,
                      sonare::MelSpectrogram::delta(typed.Data(), n_features, n_frames, width));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Piptrack(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) return env.Undefined();
  SONARE_NODE_TRY
  const auto typed = info[0].As<Napi::Float32Array>();
  int sample_rate{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sample_rate)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();
  float fmin{};
  if (!OptionalFloatArg(env, info, 4, "fmin", 150.0f, &fmin)) return env.Undefined();
  float fmax{};
  if (!OptionalFloatArg(env, info, 5, "fmax", 4000.0f, &fmax)) return env.Undefined();
  float threshold{};
  if (!OptionalFloatArg(env, info, 6, "threshold", 0.1f, &threshold)) return env.Undefined();
  sonare::validate_offline_audio_input(typed.Data(), typed.ElementLength(), sample_rate);
  const sonare::PiptrackResult result =
      sonare::piptrack(sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sample_rate),
                       n_fft, hop_length, fmin, fmax, threshold);
  Napi::Object out = Napi::Object::New(env);
  out.Set("nBins", Napi::Number::New(env, result.n_bins));
  out.Set("nFrames", Napi::Number::New(env, result.n_frames));
  out.Set("pitches", VecToFloat32(env, result.pitches));
  out.Set("magnitudes", VecToFloat32(env, result.magnitudes));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ReassignedSpectrogram(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) return env.Undefined();
  SONARE_NODE_TRY
  const auto typed = info[0].As<Napi::Float32Array>();
  int sample_rate{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sample_rate)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();
  float ref_power{};
  if (!OptionalFloatArg(env, info, 4, "refPower", 1e-6f, &ref_power)) return env.Undefined();
  const bool fill_nan =
      info.Length() >= 6 && info[5].IsBoolean() && info[5].As<Napi::Boolean>().Value();
  sonare::validate_offline_audio_input(typed.Data(), typed.ElementLength(), sample_rate);
  if (!std::isfinite(ref_power) || ref_power < 0.0f) {
    Napi::RangeError::New(env, "refPower must be finite and non-negative")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  sonare::StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  const sonare::ReassignedSpectrogram result = sonare::reassigned_spectrogram(
      sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sample_rate), config,
      ref_power, fill_nan);
  const int n_bins = n_fft / 2 + 1;
  const int n_frames = n_bins > 0 ? static_cast<int>(result.magnitude.size() / n_bins) : 0;
  Napi::Object out = Napi::Object::New(env);
  out.Set("nBins", Napi::Number::New(env, n_bins));
  out.Set("nFrames", Napi::Number::New(env, n_frames));
  out.Set("magnitude", VecToFloat32(env, result.magnitude));
  out.Set("times", VecToFloat32(env, result.times));
  out.Set("frequencies", VecToFloat32(env, result.frequencies));
  return out;
  SONARE_NODE_CATCH(env)
}

// ============================================================================
// Features - Chroma
// ============================================================================

Napi::Value SonareWrap::ChromaFn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();

  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::ChromaConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  sonare::Chroma chroma = sonare::Chroma::compute(audio, config);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nChroma", Napi::Number::New(env, chroma.n_chroma()));
  out.Set("nFrames", Napi::Number::New(env, chroma.n_frames()));
  out.Set("sampleRate", Napi::Number::New(env, chroma.sample_rate()));
  out.Set("hopLength", Napi::Number::New(env, chroma.hop_length()));

  std::vector<float> features_vec(chroma.data(),
                                  chroma.data() + chroma.n_chroma() * chroma.n_frames());
  out.Set("features", VecToFloat32(env, features_vec));

  // Mean energy per pitch class
  auto mean = chroma.mean_energy();
  Napi::Array mean_arr = Napi::Array::New(env, 12);
  for (int i = 0; i < 12; ++i) {
    mean_arr.Set(static_cast<uint32_t>(i), Napi::Number::New(env, mean[i]));
  }
  out.Set("meanEnergy", mean_arr);

  return out;
  SONARE_NODE_CATCH(env)
}

namespace {

using ChromaFn = SonareError (*)(const float*, size_t, int, int, int, SonareChromaResult*);
using ChromaExFn = SonareError (*)(const float*, size_t, int, int, int, int, SonareChromaResult*);

Napi::Value ChromaVariant(const Napi::CallbackInfo& info, ChromaFn fn, ChromaExFn ex_fn = nullptr) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 2, "hopLength", 512, &hop_length)) return env.Undefined();
  int n_chroma{};
  if (!OptionalIntArg(env, info, 3, "nChroma", 12, &n_chroma)) return env.Undefined();
  int bins_per_octave{};
  if (!OptionalIntArg(env, info, 4, "binsPerOctave", 36, &bins_per_octave)) return env.Undefined();

  SonareChromaResult result{};
  const SonareError err =
      ex_fn != nullptr ? ex_fn(typed.Data(), typed.ElementLength(), sr, hop_length, n_chroma,
                               bins_per_octave, &result)
                       : fn(typed.Data(), typed.ElementLength(), sr, hop_length, n_chroma, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("nChroma", Napi::Number::New(env, result.n_chroma));
  out.Set("nFrames", Napi::Number::New(env, result.n_frames));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("hopLength", Napi::Number::New(env, result.hop_length));
  const size_t total = static_cast<size_t>(result.n_chroma) * static_cast<size_t>(result.n_frames);
  auto features = Napi::Float32Array::New(env, total);
  if (total > 0 && result.features != nullptr) {
    std::memcpy(features.Data(), result.features, total * sizeof(float));
  }
  out.Set("features", features);
  Napi::Array mean = Napi::Array::New(env, static_cast<size_t>(result.n_chroma));
  for (int i = 0; i < result.n_chroma; ++i) {
    mean.Set(static_cast<uint32_t>(i),
             Napi::Number::New(env, result.mean_energy ? result.mean_energy[i] : 0.0f));
  }
  out.Set("meanEnergy", mean);
  sonare_free_chroma_result(&result);
  return out;
  SONARE_NODE_CATCH(env)
}

}  // namespace

Napi::Value SonareWrap::ChromaCens(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return ChromaVariant(info, nullptr, sonare_chroma_cens_ex);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ChromaCqt(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return ChromaVariant(info, nullptr, sonare_chroma_cqt_ex);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::BassChroma(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return ChromaVariant(info, sonare_bass_chroma);
  SONARE_NODE_CATCH(env)
}

// ============================================================================
// Features - Spectral
// ============================================================================

Napi::Value SonareWrap::SpectralCentroid(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();

  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> centroid = sonare::spectral_centroid(spec, sr);

  return VecToFloat32(env, centroid);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectralBandwidth(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();
  float p{};
  if (!OptionalFloatArg(env, info, 4, "p", 2.0f, &p)) return env.Undefined();

  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> bandwidth = sonare::spectral_bandwidth(spec, sr, p);

  return VecToFloat32(env, bandwidth);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectralRolloff(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();
  float roll_percent{};
  if (!OptionalFloatArg(env, info, 4, "rollPercent", 0.85f, &roll_percent)) return env.Undefined();

  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> rolloff = sonare::spectral_rolloff(spec, sr, roll_percent);

  return VecToFloat32(env, rolloff);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectralFlatness(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();

  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> flatness = sonare::spectral_flatness(spec);

  return VecToFloat32(env, flatness);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectralFlux(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) return env.Undefined();
  SONARE_NODE_TRY
  const auto typed = info[0].As<Napi::Float32Array>();
  int sample_rate{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sample_rate)) return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 2, "nFft", 2048, &n_fft)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();
  int lag{};
  if (!OptionalIntArg(env, info, 4, "lag", 1, &lag)) return env.Undefined();
  sonare::validate_offline_audio_input(typed.Data(), typed.ElementLength(), sample_rate);
  sonare::StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  return VecToFloat32(
      env,
      sonare::spectral_flux(
          sonare::Spectrogram::compute(
              sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sample_rate), config),
          lag));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ZeroCrossingRate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int frame_length{};
  if (!OptionalIntArg(env, info, 2, "frameLength", 2048, &frame_length)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();

  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  std::vector<float> zcr = sonare::zero_crossing_rate(audio, frame_length, hop_length);

  return VecToFloat32(env, zcr);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RmsEnergy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int frame_length{};
  if (!OptionalIntArg(env, info, 2, "frameLength", 2048, &frame_length)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();

  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  std::vector<float> rms = sonare::rms_energy(audio, frame_length, hop_length);

  return VecToFloat32(env, rms);
  SONARE_NODE_CATCH(env)
}

// ============================================================================
// Features - Pitch
// ============================================================================

Napi::Value SonareWrap::PitchYin(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int frame_length{};
  if (!OptionalIntArg(env, info, 2, "frameLength", 2048, &frame_length)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();
  float fmin{};
  if (!OptionalFloatArg(env, info, 4, "fmin", 65.0f, &fmin)) return env.Undefined();
  float fmax{};
  if (!OptionalFloatArg(env, info, 5, "fmax", 2093.0f, &fmax)) return env.Undefined();
  float threshold{};
  if (!OptionalFloatArg(env, info, 6, "threshold", 0.3f, &threshold)) return env.Undefined();
  bool fill_na{};
  if (!OptionalBoolArg(env, info, 7, "fillNa", false, &fill_na)) return env.Undefined();

  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::PitchConfig config;
  config.frame_length = frame_length;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  config.threshold = threshold;
  config.fill_na = fill_na;

  sonare::PitchResult result = sonare::yin_track(audio, config);

  Napi::Object out = Napi::Object::New(env);
  out.Set("f0", VecToFloat32(env, result.f0));
  out.Set("voicedProb", VecToFloat32(env, result.voiced_prob));

  // Convert voiced_flag to array of bools
  Napi::Array voiced_arr = Napi::Array::New(env, result.voiced_flag.size());
  for (size_t i = 0; i < result.voiced_flag.size(); ++i) {
    voiced_arr.Set(static_cast<uint32_t>(i),
                   Napi::Boolean::New(env, static_cast<bool>(result.voiced_flag[i])));
  }
  out.Set("voicedFlag", voiced_arr);

  out.Set("nFrames", Napi::Number::New(env, result.n_frames()));
  out.Set("medianF0", Napi::Number::New(env, static_cast<double>(result.median_f0())));
  out.Set("meanF0", Napi::Number::New(env, static_cast<double>(result.mean_f0())));

  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PitchPyin(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", 22050, &sr)) return env.Undefined();
  int frame_length{};
  if (!OptionalIntArg(env, info, 2, "frameLength", 2048, &frame_length)) return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 3, "hopLength", 512, &hop_length)) return env.Undefined();
  float fmin{};
  if (!OptionalFloatArg(env, info, 4, "fmin", 65.0f, &fmin)) return env.Undefined();
  float fmax{};
  if (!OptionalFloatArg(env, info, 5, "fmax", 2093.0f, &fmax)) return env.Undefined();
  float threshold{};
  if (!OptionalFloatArg(env, info, 6, "threshold", 0.3f, &threshold)) return env.Undefined();
  bool fill_na{};
  if (!OptionalBoolArg(env, info, 7, "fillNa", false, &fill_na)) return env.Undefined();

  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::PitchConfig config;
  config.frame_length = frame_length;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  config.threshold = threshold;
  config.fill_na = fill_na;

  sonare::PitchResult result = sonare::pyin(audio, config);

  Napi::Object out = Napi::Object::New(env);
  out.Set("f0", VecToFloat32(env, result.f0));
  out.Set("voicedProb", VecToFloat32(env, result.voiced_prob));

  // Convert voiced_flag to array of bools
  Napi::Array voiced_arr = Napi::Array::New(env, result.voiced_flag.size());
  for (size_t i = 0; i < result.voiced_flag.size(); ++i) {
    voiced_arr.Set(static_cast<uint32_t>(i),
                   Napi::Boolean::New(env, static_cast<bool>(result.voiced_flag[i])));
  }
  out.Set("voicedFlag", voiced_arr);

  out.Set("nFrames", Napi::Number::New(env, result.n_frames()));
  out.Set("medianF0", Napi::Number::New(env, static_cast<double>(result.median_f0())));
  out.Set("meanF0", Napi::Number::New(env, static_cast<double>(result.mean_f0())));

  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::NoteSegments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() != 1 || !info[0].IsObject() || info[0].IsArray()) {
    Napi::TypeError::New(env, "noteSegments expects one request object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object request = info[0].As<Napi::Object>();
  const Napi::Value f0_value = request.Get("f0Hz");
  const Napi::Value voiced_value = request.Get("voicedProb");
  const Napi::Value rate_value = request.Get("frameRate");
  if (!sonare_node::IsFloat32Array(f0_value) || !sonare_node::IsFloat32Array(voiced_value) ||
      !rate_value.IsNumber()) {
    Napi::TypeError::New(env, "noteSegments requires f0Hz, voicedProb, and frameRate")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  const Napi::Float32Array f0 = f0_value.As<Napi::Float32Array>();
  const Napi::Float32Array voiced = voiced_value.As<Napi::Float32Array>();
  SonareNoteSegmenterConfig config{};
  const Napi::Value config_value = request.Get("config");
  if (!config_value.IsUndefined() && !config_value.IsNull()) {
    if (!config_value.IsObject() || config_value.IsArray()) {
      Napi::TypeError::New(env, "noteSegments config must be an object")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    const Napi::Object options = config_value.As<Napi::Object>();
    config.struct_version = 2;
    // Analysis options bag: the presence-checked reader family, so an explicit
    // `undefined` reads as the documented default and a wrong-typed value is
    // refused by name rather than silently taking it.
    config.segmentation_threshold_cents =
        FloatProperty(options, "segmentationThresholdCents", 0.0f);
    config.min_note_ms = FloatProperty(options, "minNoteMs", 0.0f);
    config.reference_hz = FloatProperty(options, "referenceHz", 0.0f);
    config.voiced_threshold = FloatProperty(options, "voicedThreshold", 0.0f);
  }

  SonareNoteSegmentsResult result{};
  const SonareError error = sonare_note_segments(
      f0.Data(), f0.ElementLength(), voiced.Data(), voiced.ElementLength(),
      node_narrow_finite_float(env, rate_value, "frameRate"), &config, &result);
  if (error != SONARE_OK) {
    ThrowIfError(env, error);
    return env.Undefined();
  }
  Napi::Array segments = Napi::Array::New(env, result.count);
  for (size_t i = 0; i < result.count; ++i) {
    const SonareNoteSegment& segment = result.segments[i];
    Napi::Object row = Napi::Object::New(env);
    row.Set("frameStart", Napi::Number::New(env, segment.frame_start));
    row.Set("frameEnd", Napi::Number::New(env, segment.frame_end));
    row.Set("startSeconds", Napi::Number::New(env, segment.start_seconds));
    row.Set("endSeconds", Napi::Number::New(env, segment.end_seconds));
    row.Set("medianCents", Napi::Number::New(env, segment.median_cents));
    segments.Set(static_cast<uint32_t>(i), row);
  }
  sonare_free_note_segments(&result);
  return segments;
  SONARE_NODE_CATCH(env)
}
