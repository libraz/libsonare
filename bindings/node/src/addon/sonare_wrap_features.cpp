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
#include "feature/onset.h"
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

Napi::Object SegmentMatrixResult(Napi::Env env, SonareSegmentMatrix* result) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("rows", Napi::Number::New(env, result->rows));
  out.Set("cols", Napi::Number::New(env, result->cols));
  auto values = Napi::Float32Array::New(env, static_cast<size_t>(result->rows) * result->cols);
  if (result->values != nullptr) {
    std::memcpy(values.Data(), result->values, values.ElementLength() * sizeof(float));
  }
  out.Set("values", values);
  sonare_free_segment_matrix(result);
  return out;
}

Napi::Int32Array SegmentIndicesResult(Napi::Env env, SonareSegmentIndices* result) {
  auto values = Napi::Int32Array::New(env, result->count);
  if (result->values != nullptr) {
    std::memcpy(values.Data(), result->values, result->count * sizeof(int));
  }
  sonare_free_segment_indices(result);
  return values;
}

bool SegmentMatrixInput(Napi::Env env, const char* name, const Napi::Float32Array& values, int rows,
                        int cols) {
  if (rows <= 0 || cols <= 0) {
    Napi::RangeError::New(env, std::string(name) + ": matrix dimensions must be positive")
        .ThrowAsJavaScriptException();
    return false;
  }
  return ValidateMatrixDims(env, name, rows, cols, values.ElementLength());
}

}  // namespace

Napi::Value SonareWrap::Stft(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_arg_int(info, 1, 22050);
  int n_fft = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);

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
  int sr = node_arg_int(info, 1, 22050);
  int n_fft = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);

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
  int sr = node_arg_int(info, 1, 22050);
  int n_fft = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);
  int n_mels = node_arg_int(info, 4, 128);
  float fmin = node_arg_float(info, 5, 0.0f);
  float fmax = node_arg_float(info, 6, 0.0f);
  bool htk = node_arg_bool(info, 7, false);

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
  int sr = node_arg_int(info, 1, 22050);
  int n_fft = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);
  int n_mels = node_arg_int(info, 4, 128);
  int n_mfcc = node_arg_int(info, 5, 20);
  float fmin = node_arg_float(info, 6, 0.0f);
  float fmax = node_arg_float(info, 7, 0.0f);
  bool htk = node_arg_bool(info, 8, false);
  float lifter = node_arg_float(info, 9, 0.0f);

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
  const int n_features = node_arg_int(info, 1, 0);
  const int n_frames = node_arg_int(info, 2, 0);
  const int width = node_arg_int(info, 3, 9);
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
  const int sample_rate = node_arg_int(info, 1, 22050);
  const int n_fft = node_arg_int(info, 2, 2048);
  const int hop_length = node_arg_int(info, 3, 512);
  const float fmin = node_arg_float(info, 4, 150.0f);
  const float fmax = node_arg_float(info, 5, 4000.0f);
  const float threshold = node_arg_float(info, 6, 0.1f);
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
  const int sample_rate = node_arg_int(info, 1, 22050);
  const int n_fft = node_arg_int(info, 2, 2048);
  const int hop_length = node_arg_int(info, 3, 512);
  const float ref_power = node_arg_float(info, 4, 1e-6f);
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

Napi::Value SonareWrap::SegmentCrossSimilarity(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected X Float32Array") ||
      !RequireFloat32Array(info, 3, "Expected Y Float32Array"))
    return env.Undefined();
  const auto x = info[0].As<Napi::Float32Array>();
  const int x_rows = node_arg_int(info, 1, 0);
  const int x_cols = node_arg_int(info, 2, 0);
  const auto y = info[3].As<Napi::Float32Array>();
  const int y_rows = node_arg_int(info, 4, 0);
  const int y_cols = node_arg_int(info, 5, 0);
  const int k = node_arg_int(info, 6, 0);
  const std::string metric =
      info.Length() > 7 && info[7].IsString() ? info[7].As<Napi::String>().Utf8Value() : "cosine";
  const std::string mode = info.Length() > 8 && info[8].IsString()
                               ? info[8].As<Napi::String>().Utf8Value()
                               : "connectivity";
  if (x_rows <= 0 || x_cols <= 0 || y_rows != x_rows || y_cols <= 0 || k < 0 ||
      x.ElementLength() != static_cast<size_t>(x_rows) * x_cols ||
      y.ElementLength() != static_cast<size_t>(y_rows) * y_cols) {
    Napi::TypeError::New(env, "segmentCrossSimilarity: invalid matrix dimensions")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareSegmentMatrix result{};
  const SonareError err = sonare_segment_cross_similarity(
      x.Data(), x_rows, x_cols, y.Data(), y_rows, y_cols, k, metric.c_str(), mode.c_str(), &result);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return SegmentMatrixResult(env, &result);
}

Napi::Value SonareWrap::SegmentRecurrenceMatrix(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected data Float32Array")) return env.Undefined();
  const auto data = info[0].As<Napi::Float32Array>();
  const int rows = node_arg_int(info, 1, 0);
  const int cols = node_arg_int(info, 2, 0);
  if (!SegmentMatrixInput(env, "segmentRecurrenceMatrix", data, rows, cols)) return env.Undefined();
  const int k = node_arg_int(info, 3, 0);
  const int width = node_arg_int(info, 4, 1);
  const bool sym = node_arg_bool(info, 5, false);
  const std::string metric = info.Length() > 6 && info[6].IsString()
                                 ? info[6].As<Napi::String>().Utf8Value()
                                 : "euclidean";
  const std::string mode = info.Length() > 7 && info[7].IsString()
                               ? info[7].As<Napi::String>().Utf8Value()
                               : "connectivity";
  SonareSegmentMatrix result{};
  const SonareError err = sonare_segment_recurrence_matrix(
      data.Data(), rows, cols, k, width, sym ? 1 : 0, metric.c_str(), mode.c_str(), &result);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return SegmentMatrixResult(env, &result);
}

Napi::Value SonareWrap::SegmentRecurrenceToLag(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected recurrence Float32Array")) return env.Undefined();
  const auto recurrence = info[0].As<Napi::Float32Array>();
  const int n = node_arg_int(info, 1, 0);
  if (!SegmentMatrixInput(env, "segmentRecurrenceToLag", recurrence, n, n)) return env.Undefined();
  SonareSegmentMatrix result{};
  const SonareError err = sonare_segment_recurrence_to_lag(
      recurrence.Data(), n, node_arg_bool(info, 2, false) ? 1 : 0, &result);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return SegmentMatrixResult(env, &result);
}

Napi::Value SonareWrap::SegmentLagToRecurrence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected lag Float32Array")) return env.Undefined();
  const auto lag = info[0].As<Napi::Float32Array>();
  const int rows = node_arg_int(info, 1, 0);
  const int lags = node_arg_int(info, 2, 0);
  if (!SegmentMatrixInput(env, "segmentLagToRecurrence", lag, rows, lags)) return env.Undefined();
  SonareSegmentMatrix result{};
  const SonareError err = sonare_segment_lag_to_recurrence(lag.Data(), rows, lags, &result);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return SegmentMatrixResult(env, &result);
}

Napi::Value SonareWrap::SegmentSubsegment(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected data Float32Array") || info.Length() < 4) {
    return env.Undefined();
  }
  SONARE_NODE_TRY
  const auto data = info[0].As<Napi::Float32Array>();
  const int rows = node_arg_int(info, 1, 0);
  const int cols = node_arg_int(info, 2, 0);
  if (!SegmentMatrixInput(env, "segmentSubsegment", data, rows, cols)) return env.Undefined();
  const std::vector<int> boundaries = IntVectorFromValue(info[3]);
  const int n_segments = node_arg_int(info, 4, 4);
  SonareSegmentIndices result{};
  const SonareError err = sonare_segment_subsegment(data.Data(), rows, cols, boundaries.data(),
                                                    boundaries.size(), n_segments, &result);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return SegmentIndicesResult(env, &result);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SegmentAgglomerative(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected data Float32Array")) return env.Undefined();
  const auto data = info[0].As<Napi::Float32Array>();
  const int rows = node_arg_int(info, 1, 0);
  const int cols = node_arg_int(info, 2, 0);
  if (!SegmentMatrixInput(env, "segmentAgglomerative", data, rows, cols)) return env.Undefined();
  const int k = node_arg_int(info, 3, 0);
  const std::string linkage =
      info.Length() > 4 && info[4].IsString() ? info[4].As<Napi::String>().Utf8Value() : "average";
  SonareSegmentIndices result{};
  const SonareError err =
      sonare_segment_agglomerative(data.Data(), rows, cols, k, linkage.c_str(), &result);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return SegmentIndicesResult(env, &result);
}

Napi::Value SonareWrap::SegmentPathEnhance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected recurrence Float32Array")) return env.Undefined();
  const auto recurrence = info[0].As<Napi::Float32Array>();
  const int n = node_arg_int(info, 1, 0);
  if (!SegmentMatrixInput(env, "segmentPathEnhance", recurrence, n, n)) return env.Undefined();
  const int win = node_arg_int(info, 2, 0);
  const int max_ratio = node_arg_int(info, 3, 2);
  const int min_ratio = node_arg_int(info, 4, 0);
  const int n_filters = node_arg_int(info, 5, 7);
  SonareSegmentMatrix result{};
  const SonareError err = sonare_segment_path_enhance(recurrence.Data(), n, win, max_ratio,
                                                      min_ratio, n_filters, &result);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return SegmentMatrixResult(env, &result);
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
  int sr = node_arg_int(info, 1, 22050);
  int n_fft = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);

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
  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_arg_int(info, 1, 22050);
  const int hop_length = node_arg_int(info, 2, 512);
  const int n_chroma = node_arg_int(info, 3, 12);
  const int bins_per_octave = node_arg_int(info, 4, 36);

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
}

}  // namespace

Napi::Value SonareWrap::ChromaCens(const Napi::CallbackInfo& info) {
  return ChromaVariant(info, nullptr, sonare_chroma_cens_ex);
}

Napi::Value SonareWrap::ChromaCqt(const Napi::CallbackInfo& info) {
  return ChromaVariant(info, nullptr, sonare_chroma_cqt_ex);
}

Napi::Value SonareWrap::BassChroma(const Napi::CallbackInfo& info) {
  return ChromaVariant(info, sonare_bass_chroma);
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
  int sr = node_arg_int(info, 1, 22050);
  int n_fft = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);

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
  int sr = node_arg_int(info, 1, 22050);
  int n_fft = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);
  float p = node_arg_float(info, 4, 2.0f);

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
  int sr = node_arg_int(info, 1, 22050);
  int n_fft = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);
  float roll_percent = node_arg_float(info, 4, 0.85f);

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
  int sr = node_arg_int(info, 1, 22050);
  int n_fft = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);

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
  const int sample_rate = node_arg_int(info, 1, 22050);
  const int n_fft = node_arg_int(info, 2, 2048);
  const int hop_length = node_arg_int(info, 3, 512);
  const int lag = node_arg_int(info, 4, 1);
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
  int sr = node_arg_int(info, 1, 22050);
  int frame_length = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);

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
  int sr = node_arg_int(info, 1, 22050);
  int frame_length = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);

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
  int sr = node_arg_int(info, 1, 22050);
  int frame_length = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);
  float fmin = node_arg_float(info, 4, 65.0f);
  float fmax = node_arg_float(info, 5, 2093.0f);
  float threshold = node_arg_float(info, 6, 0.3f);
  bool fill_na = node_arg_bool(info, 7, false);

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
  int sr = node_arg_int(info, 1, 22050);
  int frame_length = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);
  float fmin = node_arg_float(info, 4, 65.0f);
  float fmax = node_arg_float(info, 5, 2093.0f);
  float threshold = node_arg_float(info, 6, 0.3f);
  bool fill_na = node_arg_bool(info, 7, false);

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
    config.struct_version = 1;
    config.segmentation_threshold_cents =
        FloatProperty(options, "segmentationThresholdCents", 0.0f);
    config.min_note_ms = FloatProperty(options, "minNoteMs", 0.0f);
    config.reference_hz = FloatProperty(options, "referenceHz", 0.0f);
  }

  SonareNoteSegmentsResult result{};
  const SonareError error =
      sonare_note_segments(f0.Data(), f0.ElementLength(), voiced.Data(), voiced.ElementLength(),
                           rate_value.As<Napi::Number>().FloatValue(), &config, &result);
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
}

// ============================================================================
// Core - Conversion
// ============================================================================

Napi::Value SonareWrap::HzToMel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected number argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  float hz = info[0].As<Napi::Number>().FloatValue();
  return Napi::Number::New(env, static_cast<double>(sonare::hz_to_mel(hz)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MelToHz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected number argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  float mel = info[0].As<Napi::Number>().FloatValue();
  return Napi::Number::New(env, static_cast<double>(sonare::mel_to_hz(mel)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::HzToMidi(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected number argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  float hz = info[0].As<Napi::Number>().FloatValue();
  return Napi::Number::New(env, static_cast<double>(sonare::hz_to_midi(hz)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MidiToHz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected number argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  float midi = info[0].As<Napi::Number>().FloatValue();
  return Napi::Number::New(env, static_cast<double>(sonare::midi_to_hz(midi)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::HzToNote(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected number argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  float hz = info[0].As<Napi::Number>().FloatValue();
  return Napi::String::New(env, sonare::hz_to_note(hz));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::NoteToHz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected string argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  std::string note = info[0].As<Napi::String>().Utf8Value();
  return Napi::Number::New(env, static_cast<double>(sonare::note_to_hz(note)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::FramesToTime(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (frames, sr, hopLength)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  int frames = info[0].As<Napi::Number>().Int32Value();
  int sr = info[1].As<Napi::Number>().Int32Value();
  int hop_length = info[2].As<Napi::Number>().Int32Value();

  return Napi::Number::New(env,
                           static_cast<double>(sonare::frames_to_time(frames, sr, hop_length)));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::TimeToFrames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (time, sr, hopLength)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  float time = info[0].As<Napi::Number>().FloatValue();
  int sr = info[1].As<Napi::Number>().Int32Value();
  int hop_length = info[2].As<Napi::Number>().Int32Value();

  return Napi::Number::New(env, sonare::time_to_frames(time, sr, hop_length));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::FramesToSamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (frames, hopLength?, nFft?)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int frames = info[0].As<Napi::Number>().Int32Value();
  int hop = node_arg_int(info, 1, 512);
  int n_fft = node_arg_int(info, 2, 0);
  return Napi::Number::New(env, sonare_frames_to_samples(frames, hop, n_fft));
}

Napi::Value SonareWrap::SamplesToFrames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (samples, hopLength?, nFft?)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int samples = info[0].As<Napi::Number>().Int32Value();
  int hop = node_arg_int(info, 1, 512);
  int n_fft = node_arg_int(info, 2, 0);
  return Napi::Number::New(env, sonare_samples_to_frames(samples, hop, n_fft));
}

Napi::Value SonareWrap::PowerToDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected Float32Array")) {
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float ref = node_arg_float(info, 1, 1.0f);
  float amin = node_arg_float(info, 2, sonare::constants::kEpsilon);
  float top_db = node_arg_float(info, 3, 80.0f);
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_power_to_db(arr.Data(), arr.ElementLength(), ref, amin, top_db, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::AmplitudeToDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected Float32Array")) {
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float ref = node_arg_float(info, 1, 1.0f);
  float amin = node_arg_float(info, 2, 1e-5f);
  float top_db = node_arg_float(info, 3, 80.0f);
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_amplitude_to_db(arr.Data(), arr.ElementLength(), ref, amin, top_db, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::DbToPower(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected Float32Array")) {
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float ref = node_arg_float(info, 1, 1.0f);
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_db_to_power(arr.Data(), arr.ElementLength(), ref, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::DbToAmplitude(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected Float32Array")) {
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float ref = node_arg_float(info, 1, 1.0f);
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_db_to_amplitude(arr.Data(), arr.ElementLength(), ref, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::Preemphasis(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected Float32Array")) {
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float coef = node_arg_float(info, 1, 0.97f);
  bool use_zi = info.Length() >= 3 && info[2].IsNumber();
  float zi = use_zi ? info[2].As<Napi::Number>().FloatValue() : 0.0f;
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_preemphasis(arr.Data(), arr.ElementLength(), coef, zi, use_zi ? 1 : 0, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::Deemphasis(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected Float32Array")) {
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float coef = node_arg_float(info, 1, 0.97f);
  bool use_zi = info.Length() >= 3 && info[2].IsNumber();
  float zi = use_zi ? info[2].As<Napi::Number>().FloatValue() : 0.0f;
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_deemphasis(arr.Data(), arr.ElementLength(), coef, zi, use_zi ? 1 : 0, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}
