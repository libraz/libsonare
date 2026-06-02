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
#include "sonare_wrap.h"
#include "sonare_wrap_utils.h"
#include "util/constants.h"

using namespace sonare_node;

namespace {

Napi::Float32Array FloatResult(Napi::Env env, float* data, size_t count) {
  auto out = Napi::Float32Array::New(env, count);
  if (count > 0 && data != nullptr) {
    std::memcpy(out.Data(), data, count * sizeof(float));
  }
  sonare_free_floats(data);
  return out;
}

Napi::Int32Array IntResult(Napi::Env env, int* data, size_t count) {
  auto out = Napi::Int32Array::New(env, count);
  if (count > 0 && data != nullptr) {
    std::memcpy(out.Data(), data, count * sizeof(int));
  }
  sonare_free_ints(data);
  return out;
}

Napi::Value CheckCResult(Napi::Env env, SonareError err) {
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

std::vector<int> IntVectorFromValue(const Napi::Value& value) {
  if (value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_int32_array) {
    auto arr = value.As<Napi::Int32Array>();
    return std::vector<int>(arr.Data(), arr.Data() + arr.ElementLength());
  }
  if (value.IsArray()) {
    auto arr = value.As<Napi::Array>();
    std::vector<int> out(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      out[i] = arr.Get(i).As<Napi::Number>().Int32Value();
    }
    return out;
  }
  throw Napi::TypeError::New(value.Env(), "Expected Int32Array or number[]");
}

std::vector<float> FloatVectorFromValue(const Napi::Value& value) {
  if (value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_float32_array) {
    auto arr = value.As<Napi::Float32Array>();
    return std::vector<float>(arr.Data(), arr.Data() + arr.ElementLength());
  }
  if (value.IsArray()) {
    auto arr = value.As<Napi::Array>();
    std::vector<float> out(arr.Length());
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      out[i] = arr.Get(i).As<Napi::Number>().FloatValue();
    }
    return out;
  }
  throw Napi::TypeError::New(value.Env(), "Expected Float32Array or number[]");
}

int TempogramModeFromValue(const Napi::Value& value) {
  if (value.IsUndefined() || value.IsNull()) return SONARE_TEMPOGRAM_AUTOCORRELATION;
  if (value.IsNumber()) {
    const int mode = value.As<Napi::Number>().Int32Value();
    if (mode == SONARE_TEMPOGRAM_AUTOCORRELATION || mode == SONARE_TEMPOGRAM_COSINE) return mode;
  }
  if (value.IsString()) {
    const std::string mode = value.As<Napi::String>().Utf8Value();
    if (mode == "autocorrelation" || mode == "auto" || mode == "ac") {
      return SONARE_TEMPOGRAM_AUTOCORRELATION;
    }
    if (mode == "cosine") return SONARE_TEMPOGRAM_COSINE;
  }
  throw Napi::TypeError::New(value.Env(), "Expected tempogram mode 'autocorrelation' or 'cosine'");
}

}  // namespace

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

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;

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

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;

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

Napi::Value SonareWrap::TrimSilence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float top_db =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().FloatValue() : 60.0f;
  int frame_length =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  float* out = nullptr;
  size_t count = 0;
  int start = 0;
  int end = 0;
  SonareError err = sonare_trim_silence(arr.Data(), arr.ElementLength(), top_db, frame_length,
                                        hop_length, &out, &count, &start, &end);
  if (err != SONARE_OK) return CheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("audio", FloatResult(env, out, count));
  result.Set("startSample", start);
  result.Set("endSample", end);
  return result;
}

Napi::Value SonareWrap::SplitSilence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float top_db =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().FloatValue() : 60.0f;
  int frame_length =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  int* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_split_silence(arr.Data(), arr.ElementLength(), top_db, frame_length,
                                         hop_length, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return IntResult(env, out, count);
}

Napi::Value SonareWrap::FrameSignal(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (samples, frameLength, hopLength)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float* out = nullptr;
  size_t count = 0;
  int n_frames = 0;
  SonareError err =
      sonare_frame_signal(arr.Data(), arr.ElementLength(), info[1].As<Napi::Number>().Int32Value(),
                          info[2].As<Napi::Number>().Int32Value(), &out, &count, &n_frames);
  if (err != SONARE_OK) return CheckCResult(env, err);
  Napi::Object result = Napi::Object::New(env);
  result.Set("nFrames", n_frames);
  result.Set("frames", FloatResult(env, out, count));
  return result;
}

Napi::Value SonareWrap::PadCenter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (values, size, padValue?)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float pad_value =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 0.0f;
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_pad_center(arr.Data(), arr.ElementLength(), info[1].As<Napi::Number>().Int64Value(),
                        pad_value, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::FixLength(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (values, size, padValue?)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  float pad_value =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 0.0f;
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_fix_length(arr.Data(), arr.ElementLength(), info[1].As<Napi::Number>().Int64Value(),
                        pad_value, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::FixFrames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Expected frames").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::vector<int> frames = IntVectorFromValue(info[0]);
  int x_min =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 0;
  int x_max =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : -1;
  bool pad = info.Length() >= 4 && info[3].IsBoolean() ? info[3].As<Napi::Boolean>().Value() : true;
  int* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_fix_frames(frames.data(), frames.size(), x_min, x_max, pad ? 1 : 0, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return IntResult(env, out, count);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::PeakPick(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 7 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected (values, preMax, postMax, preAvg, postAvg, delta, wait)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_peak_pick(
      arr.Data(), arr.ElementLength(), info[1].As<Napi::Number>().Int32Value(),
      info[2].As<Napi::Number>().Int32Value(), info[3].As<Napi::Number>().Int32Value(),
      info[4].As<Napi::Number>().Int32Value(), info[5].As<Napi::Number>().FloatValue(),
      info[6].As<Napi::Number>().Int32Value(), &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return IntResult(env, out, count);
}

Napi::Value SonareWrap::VectorNormalize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int norm_type =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 0;
  float threshold =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 0.0f;
  float* out = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_vector_normalize(arr.Data(), arr.ElementLength(), norm_type, threshold, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

Napi::Value SonareWrap::Pcen(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (values, nBins, nFrames, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto arr = info[0].As<Napi::Float32Array>();
  int sr = 22050, hop = 512;
  float time_constant = 0.4f, gain = 0.98f, bias = 2.0f, power = 0.5f, eps = 1e-6f;
  if (info.Length() >= 4 && info[3].IsObject()) {
    auto opts = info[3].As<Napi::Object>();
    if (opts.Has("sampleRate")) sr = opts.Get("sampleRate").As<Napi::Number>().Int32Value();
    if (opts.Has("hopLength")) hop = opts.Get("hopLength").As<Napi::Number>().Int32Value();
    if (opts.Has("timeConstant"))
      time_constant = opts.Get("timeConstant").As<Napi::Number>().FloatValue();
    if (opts.Has("gain")) gain = opts.Get("gain").As<Napi::Number>().FloatValue();
    if (opts.Has("bias")) bias = opts.Get("bias").As<Napi::Number>().FloatValue();
    if (opts.Has("power")) power = opts.Get("power").As<Napi::Number>().FloatValue();
    if (opts.Has("eps")) eps = opts.Get("eps").As<Napi::Number>().FloatValue();
  }
  const int n_bins = info[1].As<Napi::Number>().Int32Value();
  const int n_frames = info[2].As<Napi::Number>().Int32Value();
  if (!ValidateMatrixDims(env, "pcen", n_bins, n_frames, arr.ElementLength())) {
    return env.Undefined();
  }
  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_pcen(arr.Data(), n_bins, n_frames, sr, hop, time_constant, gain, bias,
                                power, eps, &out, &count);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return FloatResult(env, out, count);
}

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

// ============================================================================
// Features - Constant-Q / Variable-Q transforms
// ============================================================================

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

}  // namespace

Napi::Value SonareWrap::Cqt(const Napi::CallbackInfo& info) {
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
  SonareError err = sonare_cqt(typed.Data(), typed.ElementLength(), sr, hop_length, fmin, n_bins,
                               bins_per_octave, &result);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Value out = CqtResultToObject(env, result);
  sonare_free_cqt_result(&result);
  return out;
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

// melToStft(mel, nMels, nFrames, sampleRate?, nFft?, fmin?, fmax?)
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
                         "fmax?)")
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

  sonare::MelConfig config;
  config.n_fft = n_fft;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;

  std::vector<float> stft = sonare::mel_to_stft(typed.Data(), n_mels, n_frames, config, sr);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nBins", Napi::Number::New(env, n_fft / 2 + 1));
  out.Set("nFrames", Napi::Number::New(env, n_frames));
  out.Set("power", VecToFloat32(env, stft));
  return out;
  SONARE_NODE_CATCH(env)
}

// melToAudio(mel, nMels, nFrames, sampleRate?, nFft?, hopLength?, fmin?, fmax?, nIter?)
// -> Float32Array (reconstructed audio)
Napi::Value SonareWrap::MelToAudio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, nMels, nFrames, sampleRate?, nFft?, "
                         "hopLength?, fmin?, fmax?, nIter?)")
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

  sonare::MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;

  sonare::Audio result = sonare::mel_to_audio(typed.Data(), n_mels, n_frames, config, n_iter, sr);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}

// mfccToMel(mfcc, nMfcc, nFrames, nMels?)
// -> { nMels, nFrames, power: Float32Array } (Mel power, dB scale per inverse.h)
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

// mfccToAudio(mfcc, nMfcc, nFrames, nMels?, sampleRate?, nFft?, hopLength?, fmin?, fmax?, nIter?)
// -> Float32Array (reconstructed audio)
Napi::Value SonareWrap::MfccToAudio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, nMfcc, nFrames, nMels?, sampleRate?, nFft?, "
                         "hopLength?, fmin?, fmax?, nIter?)")
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

  sonare::MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;

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
