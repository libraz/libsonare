#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/audio.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "feature/chroma.h"
#include "feature/cqt.h"
#include "feature/inverse.h"
#include "feature/mel_spectrogram.h"
#include "feature/onset.h"
#include "feature/pitch.h"
#include "feature/spectral.h"
#include "features/common.h"
#include "filters/mel.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"
#include "util/parameter_checks.h"

using namespace sonare_node;
using namespace sonare_node::features;

Napi::Value SonareWrap::Stft(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = sonare_node::ElementData(typed);
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadStftFrameArgs(env, info, &args)) return env.Undefined();
  bool db = false;
  if (!OptionalBoolArg(env, info, 4, "db", false, &db)) return env.Undefined();
  bool phase = false;
  if (!OptionalBoolArg(env, info, 5, "phase", false, &phase)) return env.Undefined();

  SonareStftOptions options{};
  sonare_stft_options_init(&options);
  options.db = db ? 1 : 0;
  options.phase = phase ? 1 : 0;
  SonareStftResult result{};
  std::unique_ptr<SonareStftResult, decltype(&sonare_free_stft_result)> guard(
      &result, sonare_free_stft_result);
  const SonareError err = sonare_stft(data, length, args.sample_rate, args.frame_length,
                                      args.hop_length, &options, &result);
  if (err != SONARE_OK) {
    ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Object out = Napi::Object::New(env);
  const size_t count = static_cast<size_t>(result.n_bins) * static_cast<size_t>(result.n_frames);
  out.Set("nBins", Napi::Number::New(env, result.n_bins));
  out.Set("nFrames", Napi::Number::New(env, result.n_frames));
  out.Set("nFft", Napi::Number::New(env, result.n_fft));
  out.Set("hopLength", Napi::Number::New(env, result.hop_length));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("magnitude", Float32ArrayFrom(env, result.magnitude, count));
  out.Set("power", Float32ArrayFrom(env, result.power, count));
  if (db) out.Set("db", Float32ArrayFrom(env, result.db, count));
  if (phase) out.Set("phase", Float32ArrayFrom(env, result.phase, count));

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
  const float* data = sonare_node::ElementData(typed);
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadStftFrameArgs(env, info, &args)) return env.Undefined();
  int n_mels{};
  if (!OptionalDomainArg(env, info, 4, "nMels", 128, sonare::kMelBandsDomain, &n_mels)) {
    return env.Undefined();
  }
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
  out.Set("power", Float32ArrayFrom(env, power_vec));

  // dB values
  out.Set("db", Float32ArrayFrom(env, mel.to_db()));

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
  const float* data = sonare_node::ElementData(typed);
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadStftFrameArgs(env, info, &args)) return env.Undefined();
  int n_mels{};
  if (!OptionalDomainArg(env, info, 4, "nMels", 128, sonare::kMelBandsDomain, &n_mels)) {
    return env.Undefined();
  }
  int n_mfcc{};
  if (!OptionalDomainArg(env, info, 5, "nMfcc", sonare::kMfccDefaultCount, sonare::kMfccCountDomain,
                         &n_mfcc)) {
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
  out.Set("coefficients", Float32ArrayFrom(env, mfcc_coeffs));

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
  return Float32ArrayFrom(env, sonare::MelSpectrogram::delta(sonare_node::ElementData(typed),
                                                             n_features, n_frames, width));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MelFilterbank(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int sample_rate{};
  if (!RequiredIntArg(env, info, 0, "sampleRate", &sample_rate)) return env.Undefined();
  int n_fft{};
  if (!RequiredIntArg(env, info, 1, "nFft", &n_fft)) return env.Undefined();
  int n_mels{};
  if (!RequiredIntArg(env, info, 2, "nMels", &n_mels)) return env.Undefined();
  SonareMelFilterbankOptions options;
  sonare_mel_filterbank_options_init(&options);
  if (!RequiredDomainFloatArg(env, info, 3, "fminHz", &options.fmin_hz)) {
    return env.Undefined();
  }
  if (!RequiredDomainFloatArg(env, info, 4, "fmaxHz", &options.fmax_hz)) {
    return env.Undefined();
  }
  bool htk = false;
  if (!RequiredBoolArg(env, info, 5, "htk", &htk)) return env.Undefined();
  options.htk = htk ? 1 : 0;
  if (!RequiredIntArg(env, info, 6, "norm", &options.norm)) return env.Undefined();

  float* data = nullptr;
  size_t length = 0;
  int rows = 0;
  int cols = 0;
  const SonareError err =
      sonare_mel_filterbank(sample_rate, n_fft, n_mels, &options, &data, &length, &rows, &cols);
  if (err != SONARE_OK) {
    ThrowSonareErrorFlattening(env, err, "options");
    return env.Undefined();
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("data", TakeCFloats(env, data, length));
  out.Set("nMels", Napi::Number::New(env, rows));
  out.Set("nBins", Napi::Number::New(env, cols));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MelFrequencies(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int n_mels{};
  if (!RequiredIntArg(env, info, 0, "nMels", &n_mels)) return env.Undefined();
  float fmin_hz{};
  if (!RequiredDomainFloatArg(env, info, 1, "fminHz", &fmin_hz)) return env.Undefined();
  float fmax_hz{};
  if (!RequiredDomainFloatArg(env, info, 2, "fmaxHz", &fmax_hz)) return env.Undefined();
  bool htk = false;
  if (!RequiredBoolArg(env, info, 3, "htk", &htk)) return env.Undefined();

  float* data = nullptr;
  size_t length = 0;
  const SonareError err =
      sonare_mel_frequencies(n_mels, fmin_hz, fmax_hz, htk ? 1 : 0, &data, &length);
  if (err != SONARE_OK) {
    ThrowSonareError(env, err);
    return env.Undefined();
  }
  return TakeCFloats(env, data, length);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Piptrack(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  SONARE_NODE_TRY
  const auto typed = info[0].As<Napi::Float32Array>();
  FrameAnalysisArgs args;
  if (!ReadStftFrameArgs(env, info, &args)) return env.Undefined();
  float fmin{};
  if (!OptionalFloatArg(env, info, 4, "fminHz", 150.0f, &fmin)) return env.Undefined();
  float fmax{};
  if (!OptionalFloatArg(env, info, 5, "fmaxHz", 4000.0f, &fmax)) return env.Undefined();
  float threshold{};
  if (!OptionalFloatArg(env, info, 6, "threshold", 0.1f, &threshold)) return env.Undefined();
  sonare::validate_offline_audio_input("samples", sonare_node::ElementData(typed),
                                       typed.ElementLength(), args.sample_rate);
  const sonare::PiptrackResult result = sonare::util::check::with_camel_paths(
      [&] {
        return sonare::piptrack(sonare::Audio::from_buffer(sonare_node::ElementData(typed),
                                                           typed.ElementLength(), args.sample_rate),
                                args.frame_length, args.hop_length, fmin, fmax, threshold);
      },
      sonare::kPiptrackRequestKeys);
  Napi::Object out = Napi::Object::New(env);
  out.Set("nBins", Napi::Number::New(env, result.n_bins));
  out.Set("nFrames", Napi::Number::New(env, result.n_frames));
  out.Set("pitches", Float32ArrayFrom(env, result.pitches));
  out.Set("magnitudes", Float32ArrayFrom(env, result.magnitudes));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ReassignedSpectrogram(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  SONARE_NODE_TRY
  const auto typed = info[0].As<Napi::Float32Array>();
  FrameAnalysisArgs args;
  if (!ReadStftFrameArgs(env, info, &args)) return env.Undefined();
  float ref_power{};
  if (!OptionalFloatArg(env, info, 4, "refPower", 1e-6f, &ref_power)) return env.Undefined();
  bool fill_nan = false;
  if (!OptionalBoolArg(env, info, 5, "fillNan", false, &fill_nan)) {
    return env.Undefined();
  }
  sonare::validate_offline_audio_input("samples", sonare_node::ElementData(typed),
                                       typed.ElementLength(), args.sample_rate);
  if (!std::isfinite(ref_power) || ref_power < 0.0f) {
    Napi::RangeError::New(env, "refPower must be finite and at least 0")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  sonare::StftConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;
  const sonare::ReassignedSpectrogram result = sonare::reassigned_spectrogram(
      sonare::Audio::from_buffer(sonare_node::ElementData(typed), typed.ElementLength(),
                                 args.sample_rate),
      config, ref_power, fill_nan);
  const int n_bins = args.frame_length / 2 + 1;
  const int n_frames = n_bins > 0 ? static_cast<int>(result.magnitude.size() / n_bins) : 0;
  Napi::Object out = Napi::Object::New(env);
  out.Set("nBins", Napi::Number::New(env, n_bins));
  out.Set("nFrames", Napi::Number::New(env, n_frames));
  out.Set("magnitude", Float32ArrayFrom(env, result.magnitude));
  out.Set("times", Float32ArrayFrom(env, result.times));
  out.Set("frequencies", Float32ArrayFrom(env, result.frequencies));
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
  const float* data = sonare_node::ElementData(typed);
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadStftFrameArgs(env, info, &args)) return env.Undefined();

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
  out.Set("features", Float32ArrayFrom(env, features_vec));

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

using ChromaFn = SonareError (*)(const float*, size_t, int, size_t, int, SonareChromaResult*);
using ChromaExFn = SonareError (*)(const float*, size_t, int, size_t, int, int,
                                   SonareChromaResult*);

SonareError ChromaCensWithBins(const float* samples, size_t length, int sample_rate,
                               size_t hop_length, int n_chroma, int bins_per_octave,
                               SonareChromaResult* out) {
  SonareChromaCensOptions options;
  sonare_chroma_cens_options_init(&options);
  options.bins_per_octave = bins_per_octave;
  return sonare_chroma_cens(samples, length, sample_rate, hop_length, n_chroma, &options, out);
}

SonareError ChromaCqtWithBins(const float* samples, size_t length, int sample_rate,
                              size_t hop_length, int n_chroma, int bins_per_octave,
                              SonareChromaResult* out) {
  SonareChromaCqtOptions options;
  sonare_chroma_cqt_options_init(&options);
  options.bins_per_octave = bins_per_octave;
  return sonare_chroma_cqt(samples, length, sample_rate, hop_length, n_chroma, &options, out);
}

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
  if (!OptionalDomainArg(env, info, 2, "hopLength", sonare::constants::kDefaultHopLength,
                         sonare::kCqtHopLengthDomain, &hop_length)) {
    return env.Undefined();
  }
  int n_chroma{};
  if (!OptionalDomainArg(env, info, 3, "nChroma", 12, sonare::kChromaCountDomain, &n_chroma)) {
    return env.Undefined();
  }
  int bins_per_octave{};
  // The bass chroma entry takes no bins-per-octave, so only the extended entries read it as one.
  if (ex_fn != nullptr) {
    if (!OptionalDomainArg(env, info, 4, "binsPerOctave", 36, sonare::kChromaBinsPerOctaveDomain,
                           &bins_per_octave)) {
      return env.Undefined();
    }
  } else if (!OptionalIntArg(env, info, 4, "binsPerOctave", 36, &bins_per_octave)) {
    return env.Undefined();
  }

  SonareChromaResult result{};
  const SonareError err = ex_fn != nullptr
                              ? ex_fn(sonare_node::ElementData(typed), typed.ElementLength(), sr,
                                      hop_length, n_chroma, bins_per_octave, &result)
                              : fn(sonare_node::ElementData(typed), typed.ElementLength(), sr,
                                   hop_length, n_chroma, &result);
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
    std::memcpy(sonare_node::ElementData(features), result.features, total * sizeof(float));
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
  return ChromaVariant(info, nullptr, ChromaCensWithBins);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ChromaCqt(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return ChromaVariant(info, nullptr, ChromaCqtWithBins);
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
  const float* data = sonare_node::ElementData(typed);
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadStftFrameArgs(env, info, &args)) return env.Undefined();

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  sonare::StftConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> centroid = sonare::spectral_centroid(spec, args.sample_rate);

  return Float32ArrayFrom(env, centroid);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectralBandwidth(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = sonare_node::ElementData(typed);
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadStftFrameArgs(env, info, &args)) return env.Undefined();
  float p{};
  if (!RequiredFloatArg(env, info, 4, "p", &p)) return env.Undefined();

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  sonare::StftConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> bandwidth = sonare::spectral_bandwidth(spec, args.sample_rate, p);

  return Float32ArrayFrom(env, bandwidth);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectralRolloff(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = sonare_node::ElementData(typed);
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadStftFrameArgs(env, info, &args)) return env.Undefined();
  float roll_percent{};
  if (!RequiredFloatArg(env, info, 4, "rollPercent", &roll_percent)) return env.Undefined();

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  sonare::StftConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> rolloff = sonare::spectral_rolloff(spec, args.sample_rate, roll_percent);

  return Float32ArrayFrom(env, rolloff);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectralFlatness(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = sonare_node::ElementData(typed);
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadStftFrameArgs(env, info, &args)) return env.Undefined();

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  sonare::StftConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> flatness = sonare::spectral_flatness(spec);

  return Float32ArrayFrom(env, flatness);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectralFlux(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  SONARE_NODE_TRY
  const auto typed = info[0].As<Napi::Float32Array>();
  FrameAnalysisArgs args;
  if (!ReadStftFrameArgs(env, info, &args)) return env.Undefined();
  int lag{};
  if (!RequiredDomainArg(env, info, 4, "lag", sonare::kOnsetCountDomain, &lag)) {
    return env.Undefined();
  }
  sonare::validate_offline_audio_input("samples", sonare_node::ElementData(typed),
                                       typed.ElementLength(), args.sample_rate);
  sonare::StftConfig config;
  config.n_fft = args.frame_length;
  config.hop_length = args.hop_length;
  return Float32ArrayFrom(
      env,
      sonare::spectral_flux(sonare::Spectrogram::compute(
                                sonare::Audio::from_buffer(sonare_node::ElementData(typed),
                                                           typed.ElementLength(), args.sample_rate),
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
  const float* data = sonare_node::ElementData(typed);
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args, sonare::kSpectralCountDomain,
                             sonare::kSpectralCountDomain, "frameLength"))
    return env.Undefined();

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  std::vector<float> zcr = sonare::zero_crossing_rate(audio, args.frame_length, args.hop_length);

  return Float32ArrayFrom(env, zcr);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RmsEnergy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = sonare_node::ElementData(typed);
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args, sonare::kSpectralCountDomain,
                             sonare::kSpectralCountDomain, "frameLength"))
    return env.Undefined();

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);
  std::vector<float> rms = sonare::rms_energy(audio, args.frame_length, args.hop_length);

  return Float32ArrayFrom(env, rms);
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
  const float* data = sonare_node::ElementData(typed);
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args, sonare::kPitchCountDomain, sonare::kPitchCountDomain,
                             "frameLength"))
    return env.Undefined();
  sonare::PitchConfig config;
  config.frame_length = args.frame_length;
  config.hop_length = args.hop_length;
  if (!OptionalFloatArg(env, info, 4, "f0MinHz", config.fmin, &config.fmin) ||
      !OptionalFloatArg(env, info, 5, "f0MaxHz", config.fmax, &config.fmax) ||
      !OptionalFloatArg(env, info, 6, "threshold", config.threshold, &config.threshold)) {
    return env.Undefined();
  }

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);

  sonare::PitchResult result = sonare::util::check::with_camel_paths(
      [&] { return sonare::yin_track(audio, config); }, sonare::kPitchTrackRequestKeys);

  Napi::Object out = Napi::Object::New(env);
  out.Set("f0", Float32ArrayFrom(env, result.f0));
  out.Set("voicedProb", Float32ArrayFrom(env, result.voiced_prob));

  // Convert voiced_flag to array of bools
  Napi::Array voiced_arr = Napi::Array::New(env, result.voiced_flag.size());
  for (size_t i = 0; i < result.voiced_flag.size(); ++i) {
    voiced_arr.Set(static_cast<uint32_t>(i),
                   Napi::Boolean::New(env, static_cast<bool>(result.voiced_flag[i])));
  }
  out.Set("voicedFlag", voiced_arr);

  out.Set("nFrames", Napi::Number::New(env, result.n_frames()));
  SetMeasured(out, "medianF0", static_cast<double>(result.median_f0()));
  SetMeasured(out, "meanF0", static_cast<double>(result.mean_f0()));

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
  const float* data = sonare_node::ElementData(typed);
  size_t length = typed.ElementLength();
  FrameAnalysisArgs args;
  if (!ReadFrameAnalysisArgs(env, info, &args, sonare::kPitchCountDomain, sonare::kPitchCountDomain,
                             "frameLength"))
    return env.Undefined();
  sonare::PitchConfig config;
  config.frame_length = args.frame_length;
  config.hop_length = args.hop_length;
  if (!OptionalFloatArg(env, info, 4, "f0MinHz", config.fmin, &config.fmin) ||
      !OptionalFloatArg(env, info, 5, "f0MaxHz", config.fmax, &config.fmax) ||
      !OptionalFloatArg(env, info, 6, "threshold", config.threshold, &config.threshold) ||
      !OptionalBoolArg(env, info, 7, "fillNa", config.fill_na, &config.fill_na)) {
    return env.Undefined();
  }

  sonare::validate_offline_audio_input("samples", data, length, args.sample_rate);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, args.sample_rate);

  sonare::PitchResult result = sonare::util::check::with_camel_paths(
      [&] { return sonare::pyin(audio, config); }, sonare::kPitchTrackRequestKeys);

  Napi::Object out = Napi::Object::New(env);
  out.Set("f0", Float32ArrayFrom(env, result.f0));
  out.Set("voicedProb", Float32ArrayFrom(env, result.voiced_prob));

  // Convert voiced_flag to array of bools
  Napi::Array voiced_arr = Napi::Array::New(env, result.voiced_flag.size());
  for (size_t i = 0; i < result.voiced_flag.size(); ++i) {
    voiced_arr.Set(static_cast<uint32_t>(i),
                   Napi::Boolean::New(env, static_cast<bool>(result.voiced_flag[i])));
  }
  out.Set("voicedFlag", voiced_arr);

  out.Set("nFrames", Napi::Number::New(env, result.n_frames()));
  SetMeasured(out, "medianF0", static_cast<double>(result.median_f0()));
  SetMeasured(out, "meanF0", static_cast<double>(result.mean_f0()));

  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::NoteSegments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  Napi::Float32Array f0;
  if (!RequiredFloat32ArrayValue(env, info[0], "f0Hz", &f0)) return env.Undefined();
  // Two spellings of one input: both are passed as given, and the C entry refuses both and neither.
  const bool has_prob = !info[1].IsUndefined() && !info[1].IsNull();
  const bool has_flags = !info[2].IsUndefined() && !info[2].IsNull();
  Napi::Float32Array voiced_prob;
  Napi::Uint8Array voiced_flags;
  if (has_prob && !RequiredFloat32ArrayValue(env, info[1], "voicedProb", &voiced_prob)) {
    return env.Undefined();
  }
  if (has_flags && !RequiredUint8ArrayValue(env, info[2], "voiced", &voiced_flags)) {
    return env.Undefined();
  }
  if (has_prob && voiced_prob.ElementLength() != f0.ElementLength()) {
    Napi::RangeError::New(env, "voicedProb must have the same length as f0Hz")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (has_flags && voiced_flags.ElementLength() != f0.ElementLength()) {
    Napi::RangeError::New(env, "voiced must have the same length as f0Hz")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  float frame_rate{};
  if (!RequiredFloatValue(env, info[3], "frameRate", &frame_rate)) return env.Undefined();

  std::vector<int32_t> flags_i32;
  if (has_flags)
    flags_i32.assign(sonare_node::ElementData(voiced_flags),
                     sonare_node::ElementData(voiced_flags) + voiced_flags.ElementLength());

  SonareNoteSegmenterConfig config;
  sonare_note_segmenter_config_init(&config);
  const Napi::Value config_value = info[4];
  if (!config_value.IsUndefined() && !config_value.IsNull()) {
    if (!config_value.IsObject() || config_value.IsArray()) {
      Napi::TypeError::New(env, "config must be an object").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    const Napi::Object options = config_value.As<Napi::Object>();
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
  const SonareError error = sonare_note_segments(
      sonare_node::ElementData(f0), has_prob ? sonare_node::ElementData(voiced_prob) : nullptr,
      has_flags ? sonare_node::ElementData(flags_i32) : nullptr, f0.ElementLength(), frame_rate,
      &config, &result);
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
