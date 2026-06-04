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

Napi::Value SonareWrap::Stft(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;

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

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;

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

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  int n_mels =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 128;
  float fmin =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().FloatValue() : 0.0f;
  float fmax =
      info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().FloatValue() : 0.0f;
  bool htk = info.Length() >= 8 && info[7].IsBoolean() && info[7].As<Napi::Boolean>().Value();

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

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  int n_mels =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 128;
  int n_mfcc =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 20;
  float fmin =
      info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().FloatValue() : 0.0f;
  float fmax =
      info.Length() >= 8 && info[7].IsNumber() ? info[7].As<Napi::Number>().FloatValue() : 0.0f;
  bool htk = info.Length() >= 9 && info[8].IsBoolean() && info[8].As<Napi::Boolean>().Value();

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  sonare::MelSpectrogram mel = sonare::MelSpectrogram::compute(audio, config);
  std::vector<float> mfcc_coeffs = mel.mfcc(n_mfcc);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nMfcc", Napi::Number::New(env, n_mfcc));
  out.Set("nFrames", Napi::Number::New(env, mel.n_frames()));
  out.Set("coefficients", VecToFloat32(env, mfcc_coeffs));

  return out;
  SONARE_NODE_CATCH(env)
}

// ============================================================================
// Features - Chroma
// ============================================================================

Napi::Value SonareWrap::ChromaFn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;

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

// ============================================================================
// Features - Spectral
// ============================================================================

Napi::Value SonareWrap::SpectralCentroid(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;

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

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> bandwidth = sonare::spectral_bandwidth(spec, sr);

  return VecToFloat32(env, bandwidth);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SpectralRolloff(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  float roll_percent =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().FloatValue() : 0.85f;

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

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  sonare::Spectrogram spec = sonare::Spectrogram::compute(audio, config);
  std::vector<float> flatness = sonare::spectral_flatness(spec);

  return VecToFloat32(env, flatness);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ZeroCrossingRate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int frame_length =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  std::vector<float> zcr = sonare::zero_crossing_rate(audio, frame_length, hop_length);

  return VecToFloat32(env, zcr);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RmsEnergy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int frame_length =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;

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

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int frame_length =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  float fmin =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().FloatValue() : 65.0f;
  float fmax =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().FloatValue() : 2093.0f;
  float threshold =
      info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().FloatValue() : 0.3f;
  bool fill_na =
      info.Length() >= 8 && info[7].IsBoolean() ? info[7].As<Napi::Boolean>().Value() : false;

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

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int frame_length =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  float fmin =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().FloatValue() : 65.0f;
  float fmax =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().FloatValue() : 2093.0f;
  float threshold =
      info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().FloatValue() : 0.3f;
  bool fill_na =
      info.Length() >= 8 && info[7].IsBoolean() ? info[7].As<Napi::Boolean>().Value() : false;

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
  int hop =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 512;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 0;
  return Napi::Number::New(env, sonare_frames_to_samples(frames, hop, n_fft));
}

Napi::Value SonareWrap::SamplesToFrames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (samples, hopLength?, nFft?)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int samples = info[0].As<Napi::Number>().Int32Value();
  int hop =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 512;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 0;
  return Napi::Number::New(env, sonare_samples_to_frames(samples, hop, n_fft));
}

Napi::Value SonareWrap::PowerToDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float ref =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().FloatValue() : 1.0f;
  float amin = info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue()
                                                        : sonare::constants::kEpsilon;
  float top_db =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().FloatValue() : 80.0f;
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_power_to_db(arr.Data(), arr.ElementLength(), ref, amin, top_db, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::AmplitudeToDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float ref =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().FloatValue() : 1.0f;
  float amin =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 1e-5f;
  float top_db =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().FloatValue() : 80.0f;
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_amplitude_to_db(arr.Data(), arr.ElementLength(), ref, amin, top_db, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::DbToPower(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float ref =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().FloatValue() : 1.0f;
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_db_to_power(arr.Data(), arr.ElementLength(), ref, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::DbToAmplitude(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float ref =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().FloatValue() : 1.0f;
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_db_to_amplitude(arr.Data(), arr.ElementLength(), ref, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::Preemphasis(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float coef =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().FloatValue() : 0.97f;
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
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float coef =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().FloatValue() : 0.97f;
  bool use_zi = info.Length() >= 3 && info[2].IsNumber();
  float zi = use_zi ? info[2].As<Napi::Number>().FloatValue() : 0.0f;
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_deemphasis(arr.Data(), arr.ElementLength(), coef, zi, use_zi ? 1 : 0, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}
