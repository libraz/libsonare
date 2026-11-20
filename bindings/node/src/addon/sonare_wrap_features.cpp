#include <cstdint>
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

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args)) return env.Undefined();

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  sonare::StftConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;

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

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args)) return env.Undefined();

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  sonare::StftConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;

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

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args)) return env.Undefined();
  int n_mels{};
  if (!OptionalIntArg(env, info, 4, "nMels", 128, &n_mels)) return env.Undefined();
  float fmin{};
  if (!OptionalFloatArg(env, info, 5, "fmin", 0.0f, &fmin)) return env.Undefined();
  float fmax{};
  if (!OptionalFloatArg(env, info, 6, "fmax", 0.0f, &fmax)) return env.Undefined();
  bool htk{};
  if (!OptionalBoolArg(env, info, 7, "htk", false, &htk)) return env.Undefined();
  sonare::validate_mel_bounds(fmin, fmax);

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  sonare::MelConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;
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
  out.Set("nFft", Napi::Number::New(env, config.n_fft));
  out.Set("fmin", Napi::Number::New(env, config.fmin));
  out.Set("fmax", Napi::Number::New(env, config.resolved_fmax(mel.sample_rate())));
  out.Set("htk", Napi::Boolean::New(env, config.htk));
  out.Set("isDb", Napi::Boolean::New(env, false));

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

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args)) return env.Undefined();
  int n_mels{};
  if (!OptionalIntArg(env, info, 4, "nMels", 128, &n_mels)) return env.Undefined();
  int n_mfcc{};
  if (!OptionalIntArg(env, info, 5, "nMfcc", sonare::kMfccDefaultCount, &n_mfcc)) {
    return env.Undefined();
  }
  float fmin{};
  if (!OptionalFloatArg(env, info, 6, "fmin", 0.0f, &fmin)) return env.Undefined();
  float fmax{};
  if (!OptionalFloatArg(env, info, 7, "fmax", 0.0f, &fmax)) return env.Undefined();
  bool htk{};
  if (!OptionalBoolArg(env, info, 8, "htk", false, &htk)) return env.Undefined();
  float lifter{};
  if (!OptionalFloatArg(env, info, 9, "lifter", 0.0f, &lifter)) return env.Undefined();
  sonare::validate_mel_bounds(fmin, fmax);

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  sonare::MelConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  sonare::MelSpectrogram mel = sonare::MelSpectrogram::compute(audio, config);
  std::vector<float> mfcc_coeffs = mel.mfcc(n_mfcc, lifter);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nMfcc", Napi::Number::New(env, n_mfcc));
  out.Set("nFrames", Napi::Number::New(env, mel.n_frames()));
  out.Set("sampleRate", Napi::Number::New(env, mel.sample_rate()));
  out.Set("hopLength", Napi::Number::New(env, mel.hop_length()));
  out.Set("nFft", Napi::Number::New(env, config.n_fft));
  out.Set("nMels", Napi::Number::New(env, mel.n_mels()));
  out.Set("fmin", Napi::Number::New(env, config.fmin));
  out.Set("fmax", Napi::Number::New(env, config.resolved_fmax(mel.sample_rate())));
  out.Set("htk", Napi::Boolean::New(env, config.htk));
  out.Set("isDb", Napi::Boolean::New(env, false));
  out.Set("lifter", Napi::Number::New(env, lifter));
  out.Set("coefficients", VecToFloat32(env, mfcc_coeffs));

  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MelDelta(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "features must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  const auto typed = info[0].As<Napi::Float32Array>();
  int n_features{};
  if (!RequiredIntArg(env, info, 1, "nFeatures", &n_features)) return env.Undefined();
  int n_frames{};
  if (!RequiredIntArg(env, info, 2, "nFrames", &n_frames)) return env.Undefined();
  int width{};
  if (!OptionalIntArg(env, info, 3, "width", 9, &width)) return env.Undefined();
  if (n_features <= 0 || n_frames <= 0 ||
      static_cast<size_t>(n_features) * static_cast<size_t>(n_frames) != typed.ElementLength()) {
    Napi::RangeError::New(env, "feature matrix length must equal nFeatures * nFrames")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return VecToFloat32(env,
                      sonare::MelSpectrogram::delta(typed.Data(), n_features, n_frames, width));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Piptrack(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  SONARE_NODE_TRY
  const auto typed = info[0].As<Napi::Float32Array>();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args)) return env.Undefined();
  float fmin{};
  if (!OptionalFloatArg(env, info, 4, "fmin", 150.0f, &fmin)) return env.Undefined();
  float fmax{};
  if (!OptionalFloatArg(env, info, 5, "fmax", 4000.0f, &fmax)) return env.Undefined();
  float threshold{};
  if (!OptionalFloatArg(env, info, 6, "threshold", 0.1f, &threshold)) return env.Undefined();
  sonare::validate_offline_audio_input("samples", typed.Data(), typed.ElementLength(),
                                       args.sample_rate);
  const sonare::PiptrackResult result = sonare::piptrack(
      sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), args.sample_rate),
      args.frame_length, args.hop_length, fmin, fmax, threshold);
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
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  SONARE_NODE_TRY
  const auto typed = info[0].As<Napi::Float32Array>();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args)) return env.Undefined();
  float ref_power{};
  if (!OptionalFloatArg(env, info, 4, "refPower", 1e-6f, &ref_power)) return env.Undefined();
  bool fill_nan = false;
  if (!OptionalBoolArg(env, info, 5, "fillNan", false, &fill_nan)) {
    return env.Undefined();
  }
  sonare::validate_offline_audio_input("samples", typed.Data(), typed.ElementLength(),
                                       args.sample_rate);
  if (!std::isfinite(ref_power) || ref_power < 0.0f) {
    Napi::RangeError::New(env, "refPower must be finite and at least 0")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  sonare::StftConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;
  const sonare::ReassignedSpectrogram result = sonare::reassigned_spectrogram(
      sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), args.sample_rate), config,
      ref_power, fill_nan);
  const int n_bins = args.frame_length / 2 + 1;
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

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args)) return env.Undefined();

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  sonare::ChromaConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;

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
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", sonare::constants::kDefaultSampleRate, &sr))
    return env.Undefined();
  int hop_length{};
  if (!OptionalIntArg(env, info, 2, "hopLength", sonare::constants::kDefaultHopLength, &hop_length))
    return env.Undefined();
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

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args)) return env.Undefined();

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  sonare::StftConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> centroid = sonare::spectral_centroid(spec, args.sample_rate);

  return VecToFloat32(env, centroid);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectralBandwidth(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args)) return env.Undefined();
  float p{};
  if (!RequiredFloatArg(env, info, 4, "p", &p)) return env.Undefined();

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  sonare::StftConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> bandwidth = sonare::spectral_bandwidth(spec, args.sample_rate, p);

  return VecToFloat32(env, bandwidth);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectralRolloff(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args)) return env.Undefined();
  float roll_percent{};
  if (!RequiredFloatArg(env, info, 4, "rollPercent", &roll_percent)) return env.Undefined();

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  sonare::StftConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> rolloff = sonare::spectral_rolloff(spec, args.sample_rate, roll_percent);

  return VecToFloat32(env, rolloff);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectralFlatness(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args)) return env.Undefined();

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  sonare::StftConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> flatness = sonare::spectral_flatness(spec);

  return VecToFloat32(env, flatness);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectralFlux(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  SONARE_NODE_TRY
  const auto typed = info[0].As<Napi::Float32Array>();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args)) return env.Undefined();
  int lag{};
  if (!RequiredIntArg(env, info, 4, "lag", &lag)) return env.Undefined();
  sonare::validate_offline_audio_input("samples", typed.Data(), typed.ElementLength(),
                                       args.sample_rate);
  sonare::StftConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;
  return VecToFloat32(
      env, sonare::spectral_flux(sonare::Spectrogram::compute(
                                     sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(),
                                                                args.sample_rate),
                                     config),
                                 lag));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ZeroCrossingRate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args, "frameLength")) return env.Undefined();

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  std::vector<float> zcr = sonare::zero_crossing_rate(audio, args.frame_length, args.hop_length);

  return VecToFloat32(env, zcr);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RmsEnergy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args, "frameLength")) return env.Undefined();

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  std::vector<float> rms = sonare::rms_energy(audio, args.frame_length, args.hop_length);

  return VecToFloat32(env, rms);
  SONARE_NODE_CATCH(env)
}

// ============================================================================
// Features - Pitch
// ============================================================================

Napi::Value SonareWrap::PitchYin(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args, "frameLength")) return env.Undefined();
  sonare::PitchConfig config;
  config.frame_length = args.frame_length;
  config.hop_length = args.hop_length;
  if (!OptionalFloatArg(env, info, 4, "fmin", config.fmin, &config.fmin) ||
      !OptionalFloatArg(env, info, 5, "fmax", config.fmax, &config.fmax) ||
      !OptionalFloatArg(env, info, 6, "threshold", config.threshold, &config.threshold)) {
    return env.Undefined();
  }

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);

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

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args, "frameLength")) return env.Undefined();
  sonare::PitchConfig config;
  config.frame_length = args.frame_length;
  config.hop_length = args.hop_length;
  if (!OptionalFloatArg(env, info, 4, "fmin", config.fmin, &config.fmin) ||
      !OptionalFloatArg(env, info, 5, "fmax", config.fmax, &config.fmax) ||
      !OptionalFloatArg(env, info, 6, "threshold", config.threshold, &config.threshold) ||
      !OptionalBoolArg(env, info, 7, "fillNa", config.fill_na, &config.fill_na)) {
    return env.Undefined();
  }

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);

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
  Napi::Float32Array f0;
  if (!RequiredFloat32ArrayValue(env, info[0], "f0Hz", &f0)) return env.Undefined();
  const bool has_prob = !info[1].IsUndefined() && !info[1].IsNull();
  const bool has_flags = !info[2].IsUndefined() && !info[2].IsNull();
  if (has_prob == has_flags) {
    Napi::RangeError::New(env, "exactly one of voicedProb or voicedFlags must be given")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Float32Array voiced_prob;
  Napi::Uint8Array voiced_flags;
  if (has_prob && !RequiredFloat32ArrayValue(env, info[1], "voicedProb", &voiced_prob)) {
    return env.Undefined();
  }
  if (has_flags && !RequiredUint8ArrayValue(env, info[2], "voicedFlags", &voiced_flags)) {
    return env.Undefined();
  }
  const size_t voicing_length =
      has_prob ? voiced_prob.ElementLength() : voiced_flags.ElementLength();
  if (voicing_length != f0.ElementLength()) {
    Napi::RangeError::New(env, std::string(has_prob ? "voicedProb" : "voicedFlags") +
                                   " length must equal f0Hz length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  float frame_rate{};
  if (!RequiredFiniteFloatValue(env, info[3], "frameRate", &frame_rate)) return env.Undefined();

  std::vector<int32_t> flags_i32;
  if (has_flags) flags_i32.assign(voiced_flags.Data(), voiced_flags.Data() + voicing_length);

  SonareNoteSegmenterConfig config{};
  const Napi::Value config_value = info[4];
  if (!config_value.IsUndefined() && !config_value.IsNull()) {
    if (!config_value.IsObject() || config_value.IsArray()) {
      Napi::TypeError::New(env, "config must be an object").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    const Napi::Object options = config_value.As<Napi::Object>();
    config.struct_version = 2;
    // Analysis options bag: the presence-checked reader family, so an explicit
    // `undefined` reads as the documented default and a wrong-typed value is
    // refused by name rather than silently taking it.
    config.segmentation_threshold_cents =
        FloatProperty(options, "segmentationThresholdCents", config.segmentation_threshold_cents);
    config.min_note_ms = FloatProperty(options, "minNoteMs", config.min_note_ms);
    config.reference_hz = FloatProperty(options, "referenceHz", config.reference_hz);
    config.voiced_threshold = FloatProperty(options, "voicedThreshold", config.voiced_threshold);
  }

  SonareNoteSegmentsResult result{};
  const SonareError error = sonare_note_segments(f0.Data(), has_prob ? voiced_prob.Data() : nullptr,
                                                 has_flags ? flags_i32.data() : nullptr,
                                                 f0.ElementLength(), frame_rate, &config, &result);
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
    row.Set("startSec", Napi::Number::New(env, segment.start_sec));
    row.Set("endSec", Napi::Number::New(env, segment.end_sec));
    row.Set("medianCents", Napi::Number::New(env, segment.median_cents));
    segments.Set(static_cast<uint32_t>(i), row);
  }
  sonare_free_note_segments_result(&result);
  return segments;
  SONARE_NODE_CATCH(env)
}
