#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "analysis/music_analyzer.h"
#include "core/audio.h"
#include "sonare_wrap.h"
#include "sonare_wrap_key_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

namespace {

int int_option(const Napi::Object& object, const char* key, int fallback) {
  Napi::Value value = object.Get(key);
  return value.IsNumber() ? value.As<Napi::Number>().Int32Value() : fallback;
}

float float_option(const Napi::Object& object, const char* key, float fallback) {
  Napi::Value value = object.Get(key);
  return value.IsNumber() ? value.As<Napi::Number>().FloatValue() : fallback;
}

bool bool_option(const Napi::Object& object, const char* key, bool fallback) {
  Napi::Value value = object.Get(key);
  return value.IsBoolean() ? value.As<Napi::Boolean>().Value() : fallback;
}

}  // namespace

Napi::Value SonareWrap::DetectBpm(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = 22050;
  if (info.Length() >= 2 && info[1].IsNumber()) {
    sample_rate = info[1].As<Napi::Number>().Int32Value();
  }

  float bpm = 0.0f;
  SonareError err = sonare_detect_bpm(data, length, sample_rate, &bpm);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return Napi::Number::New(env, static_cast<double>(bpm));
}

Napi::Value SonareWrap::DetectKey(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = 22050;
  if (info.Length() >= 2 && info[1].IsNumber()) {
    sample_rate = info[1].As<Napi::Number>().Int32Value();
  }

  int n_fft = 4096;
  int hop_length = 512;
  bool use_hpss = false;
  bool loudness_weighted = false;
  float high_pass_hz = 0.0f;
  std::vector<SonareMode> modes;
  SonareKeyProfileType profile = SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER;
  std::string genre_hint;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    n_fft = int_option(options, "nFft", n_fft);
    hop_length = int_option(options, "hopLength", hop_length);
    use_hpss = bool_option(options, "useHpss", use_hpss);
    loudness_weighted = bool_option(options, "loudnessWeighted", loudness_weighted);
    high_pass_hz = float_option(options, "highPassHz", high_pass_hz);
    modes = node_modes_option(options);
    profile = node_profile_from_value(options.Get("profile"));
    Napi::Value genre = options.Get("genreHint");
    if (genre.IsString()) genre_hint = genre.As<Napi::String>().Utf8Value();
  }

  SonareKey key{};
  SonareError err = sonare_detect_key_with_extended_options(
      data, length, sample_rate, n_fft, hop_length, use_hpss ? 1 : 0, loudness_weighted ? 1 : 0,
      high_pass_hz, modes.empty() ? nullptr : modes.data(), modes.size(), profile,
      genre_hint.empty() ? nullptr : genre_hint.c_str(), &key);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return KeyToObject(env, key.root, key.mode, key.confidence);
}

Napi::Value SonareWrap::DetectKeyCandidates(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = 22050;
  if (info.Length() >= 2 && info[1].IsNumber()) {
    sample_rate = info[1].As<Napi::Number>().Int32Value();
  }

  int n_fft = 4096;
  int hop_length = 512;
  bool use_hpss = false;
  bool loudness_weighted = false;
  float high_pass_hz = 0.0f;
  std::vector<SonareMode> modes;
  SonareKeyProfileType profile = SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER;
  std::string genre_hint;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    n_fft = int_option(options, "nFft", n_fft);
    hop_length = int_option(options, "hopLength", hop_length);
    use_hpss = bool_option(options, "useHpss", use_hpss);
    loudness_weighted = bool_option(options, "loudnessWeighted", loudness_weighted);
    high_pass_hz = float_option(options, "highPassHz", high_pass_hz);
    modes = node_modes_option(options);
    profile = node_profile_from_value(options.Get("profile"));
    Napi::Value genre = options.Get("genreHint");
    if (genre.IsString()) genre_hint = genre.As<Napi::String>().Utf8Value();
  }

  SonareKeyCandidate* candidates = nullptr;
  size_t count = 0;
  SonareError err = sonare_detect_key_candidates_with_extended_options(
      data, length, sample_rate, n_fft, hop_length, use_hpss ? 1 : 0, loudness_weighted ? 1 : 0,
      high_pass_hz, modes.empty() ? nullptr : modes.data(), modes.size(), profile,
      genre_hint.empty() ? nullptr : genre_hint.c_str(), &candidates, &count);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array out = Napi::Array::New(env, count);
  for (size_t i = 0; i < count; ++i) {
    Napi::Object candidate = Napi::Object::New(env);
    candidate.Set("key", KeyToObject(env, candidates[i].key.root, candidates[i].key.mode,
                                     candidates[i].key.confidence));
    candidate.Set("correlation",
                  Napi::Number::New(env, static_cast<double>(candidates[i].correlation)));
    out.Set(i, candidate);
  }
  sonare_free_key_candidates(candidates);
  return out;
}

Napi::Value SonareWrap::DetectBeats(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = 22050;
  if (info.Length() >= 2 && info[1].IsNumber()) {
    sample_rate = info[1].As<Napi::Number>().Int32Value();
  }

  float* times = nullptr;
  size_t count = 0;
  SonareError err = sonare_detect_beats(data, length, sample_rate, &times, &count);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto result = Napi::Float32Array::New(env, count);
  if (count > 0 && times != nullptr) {
    std::memcpy(result.Data(), times, count * sizeof(float));
    sonare_free_floats(times);
  }
  return result;
}

Napi::Value SonareWrap::DetectDownbeats(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = 22050;
  if (info.Length() >= 2 && info[1].IsNumber()) {
    sample_rate = info[1].As<Napi::Number>().Int32Value();
  }

  float* times = nullptr;
  size_t count = 0;
  SonareError err = sonare_detect_downbeats(data, length, sample_rate, &times, &count);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto result = Napi::Float32Array::New(env, count);
  if (count > 0 && times != nullptr) {
    std::memcpy(result.Data(), times, count * sizeof(float));
    sonare_free_floats(times);
  }
  return result;
}

Napi::Value SonareWrap::DetectOnsets(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = 22050;
  if (info.Length() >= 2 && info[1].IsNumber()) {
    sample_rate = info[1].As<Napi::Number>().Int32Value();
  }

  float* times = nullptr;
  size_t count = 0;
  SonareError err = sonare_detect_onsets(data, length, sample_rate, &times, &count);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto result = Napi::Float32Array::New(env, count);
  if (count > 0 && times != nullptr) {
    std::memcpy(result.Data(), times, count * sizeof(float));
    sonare_free_floats(times);
  }
  return result;
}

Napi::Value SonareWrap::Analyze(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = 22050;
  if (info.Length() >= 2 && info[1].IsNumber()) {
    sample_rate = info[1].As<Napi::Number>().Int32Value();
  }

  return FullAnalysisJsonToObject(env, data, length, sample_rate);
}

namespace {

// Off-main-thread analyze. The worker copies the input Float32Array into its
// own std::vector<float> (so the JS thread is free to release the typed array
// view) and runs sonare_analyze_json inside Execute() to get a JSON string.
// On completion the JSON string is parsed on the main thread (required because
// JSON.parse touches the V8 heap) and the Promise resolves with the full result.
class AnalyzeAsyncWorker : public Napi::AsyncWorker {
 public:
  AnalyzeAsyncWorker(Napi::Env env, std::vector<float> samples, int sample_rate)
      : Napi::AsyncWorker(env),
        deferred_(Napi::Promise::Deferred::New(env)),
        samples_(std::move(samples)),
        sample_rate_(sample_rate) {}

  void Execute() override {
    char* json_ptr = nullptr;
    SonareError err =
        sonare_analyze_json(samples_.data(), samples_.size(), sample_rate_, &json_ptr);
    if (err != SONARE_OK) {
      SetError(ErrorMessageForCode(err));
      return;
    }
    if (json_ptr != nullptr) {
      json_string_ = std::string(json_ptr);
      sonare_free_string(json_ptr);
    }
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    Napi::Env env = Env();

    // JSON.parse on the main thread (V8 is not thread-safe).
    Napi::Object json_global = env.Global().Get("JSON").As<Napi::Object>();
    Napi::Function json_parse = json_global.Get("parse").As<Napi::Function>();
    Napi::Value parsed = json_parse.Call({Napi::String::New(env, json_string_)});

    if (env.IsExceptionPending() || !parsed.IsObject()) {
      if (!env.IsExceptionPending()) {
        deferred_.Reject(Napi::Error::New(env, "Failed to parse analysis JSON").Value());
      } else {
        deferred_.Reject(env.GetAndClearPendingException().Value());
      }
      return;
    }

    Napi::Object result = parsed.As<Napi::Object>();

    // Inject legacy beatTimes Float32Array.
    Napi::Value beats_val = result.Get("beats");
    if (beats_val.IsArray()) {
      Napi::Array beats_arr = beats_val.As<Napi::Array>();
      uint32_t n = beats_arr.Length();
      auto beat_times = Napi::Float32Array::New(env, n);
      for (uint32_t i = 0; i < n; ++i) {
        Napi::Value beat = beats_arr.Get(i);
        if (beat.IsObject()) {
          Napi::Value t = beat.As<Napi::Object>().Get("time");
          beat_times[i] =
              t.IsNumber() ? static_cast<float>(t.As<Napi::Number>().DoubleValue()) : 0.0f;
        }
      }
      result.Set("beatTimes", beat_times);
    } else {
      result.Set("beatTimes", Napi::Float32Array::New(env, 0));
    }

    deferred_.Resolve(result);
  }

  void OnError(const Napi::Error& error) override {
    Napi::HandleScope scope(Env());
    deferred_.Reject(error.Value());
  }

  Napi::Promise GetPromise() { return deferred_.Promise(); }

 private:
  Napi::Promise::Deferred deferred_;
  std::vector<float> samples_;
  int sample_rate_;
  std::string json_string_;
};

}  // namespace

Napi::Value SonareWrap::AnalyzeAsync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate?)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  std::vector<float> samples(typed.Data(), typed.Data() + typed.ElementLength());
  int sample_rate = 22050;
  if (info.Length() >= 2 && info[1].IsNumber()) {
    sample_rate = info[1].As<Napi::Number>().Int32Value();
  }
  auto* worker = new AnalyzeAsyncWorker(env, std::move(samples), sample_rate);
  Napi::Promise promise = worker->GetPromise();
  worker->Queue();
  return promise;
}

Napi::Value SonareWrap::AnalyzeBpm(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  float bpm_min =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 30.0f;
  float bpm_max =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().FloatValue() : 300.0f;
  float start_bpm =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().FloatValue() : 120.0f;
  int n_fft =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().Int32Value() : 512;
  int max_candidates =
      info.Length() >= 8 && info[7].IsNumber() ? info[7].As<Napi::Number>().Int32Value() : 5;

  SonareBpmAnalysisResult analysis{};
  SonareError err = sonare_analyze_bpm(data, length, sample_rate, bpm_min, bpm_max, start_bpm,
                                       n_fft, hop_length, max_candidates, &analysis);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("bpm", Napi::Number::New(env, analysis.bpm));
  result.Set("confidence", Napi::Number::New(env, analysis.confidence));

  Napi::Array candidates = Napi::Array::New(env, analysis.candidate_count);
  for (size_t i = 0; i < analysis.candidate_count; ++i) {
    Napi::Object candidate = Napi::Object::New(env);
    candidate.Set("bpm", Napi::Number::New(env, analysis.candidates[i].bpm));
    candidate.Set("confidence", Napi::Number::New(env, analysis.candidates[i].confidence));
    candidates.Set(static_cast<uint32_t>(i), candidate);
  }
  result.Set("candidates", candidates);

  auto autocorrelation = Napi::Float32Array::New(env, analysis.autocorrelation_count);
  if (analysis.autocorrelation_count > 0 && analysis.autocorrelation != nullptr) {
    std::memcpy(autocorrelation.Data(), analysis.autocorrelation,
                analysis.autocorrelation_count * sizeof(float));
  }
  result.Set("autocorrelation", autocorrelation);

  auto tempogram = Napi::Float32Array::New(env, analysis.tempogram_count);
  if (analysis.tempogram_count > 0 && analysis.tempogram != nullptr) {
    std::memcpy(tempogram.Data(), analysis.tempogram, analysis.tempogram_count * sizeof(float));
  }
  result.Set("tempogram", tempogram);

  sonare_free_bpm_analysis_result(&analysis);
  return result;
}

Napi::Value SonareWrap::AnalyzeImpulseResponse(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 48000;
  int n_octave_bands =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 6;

  SonareAcousticResult acoustic{};
  SonareError err =
      sonare_analyze_impulse_response(data, length, sample_rate, n_octave_bands, &acoustic);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("rt60", Napi::Number::New(env, acoustic.rt60));
  result.Set("edt", Napi::Number::New(env, acoustic.edt));
  result.Set("c50", Napi::Number::New(env, acoustic.c50));
  result.Set("c80", Napi::Number::New(env, acoustic.c80));
  result.Set("d50", Napi::Number::New(env, acoustic.d50));
  result.Set("confidence", Napi::Number::New(env, acoustic.confidence));
  result.Set("isBlind", Napi::Boolean::New(env, acoustic.is_blind != 0));

  auto rt60_bands = Napi::Float32Array::New(env, acoustic.band_count);
  auto edt_bands = Napi::Float32Array::New(env, acoustic.band_count);
  auto c50_bands = Napi::Float32Array::New(env, acoustic.band_count);
  auto c80_bands = Napi::Float32Array::New(env, acoustic.band_count);
  if (acoustic.band_count > 0) {
    std::memcpy(rt60_bands.Data(), acoustic.rt60_bands, acoustic.band_count * sizeof(float));
    std::memcpy(edt_bands.Data(), acoustic.edt_bands, acoustic.band_count * sizeof(float));
    // Clarity bands may be null in blind mode (not computed); leave arrays zeroed.
    if (acoustic.c50_bands != nullptr) {
      std::memcpy(c50_bands.Data(), acoustic.c50_bands, acoustic.band_count * sizeof(float));
    }
    if (acoustic.c80_bands != nullptr) {
      std::memcpy(c80_bands.Data(), acoustic.c80_bands, acoustic.band_count * sizeof(float));
    }
  }
  result.Set("rt60Bands", rt60_bands);
  result.Set("edtBands", edt_bands);
  result.Set("c50Bands", c50_bands);
  result.Set("c80Bands", c80_bands);

  sonare_free_acoustic_result(&acoustic);
  return result;
}

Napi::Value SonareWrap::DetectAcoustic(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 48000;
  int n_octave_bands =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 6;
  int n_third_octave_subbands =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 24;
  float min_decay_db =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().FloatValue() : 30.0f;
  float noise_floor_margin_db =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().FloatValue() : 10.0f;

  SonareAcousticResult acoustic{};
  SonareError err =
      sonare_detect_acoustic(data, length, sample_rate, n_octave_bands, n_third_octave_subbands,
                             min_decay_db, noise_floor_margin_db, &acoustic);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("rt60", Napi::Number::New(env, acoustic.rt60));
  result.Set("edt", Napi::Number::New(env, acoustic.edt));
  result.Set("c50", Napi::Number::New(env, acoustic.c50));
  result.Set("c80", Napi::Number::New(env, acoustic.c80));
  result.Set("d50", Napi::Number::New(env, acoustic.d50));
  result.Set("confidence", Napi::Number::New(env, acoustic.confidence));
  result.Set("isBlind", Napi::Boolean::New(env, acoustic.is_blind != 0));

  auto rt60_bands = Napi::Float32Array::New(env, acoustic.band_count);
  auto edt_bands = Napi::Float32Array::New(env, acoustic.band_count);
  auto c50_bands = Napi::Float32Array::New(env, acoustic.band_count);
  auto c80_bands = Napi::Float32Array::New(env, acoustic.band_count);
  if (acoustic.band_count > 0) {
    std::memcpy(rt60_bands.Data(), acoustic.rt60_bands, acoustic.band_count * sizeof(float));
    std::memcpy(edt_bands.Data(), acoustic.edt_bands, acoustic.band_count * sizeof(float));
    // Clarity bands may be null in blind mode (not computed); leave arrays zeroed.
    if (acoustic.c50_bands != nullptr) {
      std::memcpy(c50_bands.Data(), acoustic.c50_bands, acoustic.band_count * sizeof(float));
    }
    if (acoustic.c80_bands != nullptr) {
      std::memcpy(c80_bands.Data(), acoustic.c80_bands, acoustic.band_count * sizeof(float));
    }
  }
  result.Set("rt60Bands", rt60_bands);
  result.Set("edtBands", edt_bands);
  result.Set("c50Bands", c50_bands);
  result.Set("c80Bands", c80_bands);

  sonare_free_acoustic_result(&acoustic);
  return result;
}

Napi::Value SonareWrap::AnalyzeRhythm(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  float bpm_min =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 60.0f;
  float bpm_max =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().FloatValue() : 200.0f;
  float start_bpm =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().FloatValue() : 120.0f;
  int n_fft =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().Int32Value() : 512;

  SonareRhythmResult rhythm{};
  SonareError err = sonare_analyze_rhythm(data, length, sample_rate, bpm_min, bpm_max, start_bpm,
                                          n_fft, hop_length, &rhythm);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  const char* groove = "straight";
  if (rhythm.groove_type == SONARE_GROOVE_SHUFFLE) groove = "shuffle";
  if (rhythm.groove_type == SONARE_GROOVE_SWING) groove = "swing";

  Napi::Object result = Napi::Object::New(env);
  result.Set("bpm", Napi::Number::New(env, rhythm.bpm));
  Napi::Object time_signature = Napi::Object::New(env);
  time_signature.Set("numerator", Napi::Number::New(env, rhythm.time_signature.numerator));
  time_signature.Set("denominator", Napi::Number::New(env, rhythm.time_signature.denominator));
  time_signature.Set("confidence", Napi::Number::New(env, rhythm.time_signature.confidence));
  result.Set("timeSignature", time_signature);
  result.Set("grooveType", Napi::String::New(env, groove));
  result.Set("syncopation", Napi::Number::New(env, rhythm.syncopation));
  result.Set("patternRegularity", Napi::Number::New(env, rhythm.pattern_regularity));
  result.Set("tempoStability", Napi::Number::New(env, rhythm.tempo_stability));

  auto intervals = Napi::Float32Array::New(env, rhythm.beat_interval_count);
  if (rhythm.beat_interval_count > 0 && rhythm.beat_intervals != nullptr) {
    std::memcpy(intervals.Data(), rhythm.beat_intervals,
                rhythm.beat_interval_count * sizeof(float));
  }
  result.Set("beatIntervals", intervals);

  sonare_free_rhythm_result(&rhythm);
  return result;
}

Napi::Value SonareWrap::AnalyzeDynamics(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  float window_sec =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 0.4f;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  float compression_threshold =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().FloatValue() : 6.0f;

  SonareDynamicsResult dynamics{};
  SonareError err = sonare_analyze_dynamics(data, length, sample_rate, window_sec, hop_length,
                                            compression_threshold, &dynamics);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("dynamicRangeDb", Napi::Number::New(env, dynamics.dynamic_range_db));
  result.Set("peakDb", Napi::Number::New(env, dynamics.peak_db));
  result.Set("rmsDb", Napi::Number::New(env, dynamics.rms_db));
  result.Set("crestFactor", Napi::Number::New(env, dynamics.crest_factor));
  result.Set("loudnessRangeDb", Napi::Number::New(env, dynamics.loudness_range_db));
  result.Set("isCompressed", Napi::Boolean::New(env, dynamics.is_compressed != 0));

  auto times = Napi::Float32Array::New(env, dynamics.loudness_count);
  auto rms_db = Napi::Float32Array::New(env, dynamics.loudness_count);
  if (dynamics.loudness_count > 0) {
    std::memcpy(times.Data(), dynamics.loudness_times, dynamics.loudness_count * sizeof(float));
    std::memcpy(rms_db.Data(), dynamics.loudness_rms_db, dynamics.loudness_count * sizeof(float));
  }
  result.Set("loudnessTimes", times);
  result.Set("loudnessRmsDb", rms_db);

  sonare_free_dynamics_result(&dynamics);
  return result;
}

Napi::Value SonareWrap::AnalyzeTimbre(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  int n_mels =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 128;
  int n_mfcc =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 13;
  float window_sec =
      info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().FloatValue() : 0.5f;

  SonareTimbreResult timbre{};
  SonareError err = sonare_analyze_timbre(data, length, sample_rate, n_fft, hop_length, n_mels,
                                          n_mfcc, window_sec, &timbre);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("brightness", Napi::Number::New(env, timbre.brightness));
  result.Set("warmth", Napi::Number::New(env, timbre.warmth));
  result.Set("density", Napi::Number::New(env, timbre.density));
  result.Set("roughness", Napi::Number::New(env, timbre.roughness));
  result.Set("complexity", Napi::Number::New(env, timbre.complexity));

  auto centroid = Napi::Float32Array::New(env, timbre.spectral_centroid_count);
  if (timbre.spectral_centroid_count > 0 && timbre.spectral_centroid != nullptr) {
    std::memcpy(centroid.Data(), timbre.spectral_centroid,
                timbre.spectral_centroid_count * sizeof(float));
  }
  result.Set("spectralCentroid", centroid);

  auto flatness = Napi::Float32Array::New(env, timbre.spectral_flatness_count);
  if (timbre.spectral_flatness_count > 0 && timbre.spectral_flatness != nullptr) {
    std::memcpy(flatness.Data(), timbre.spectral_flatness,
                timbre.spectral_flatness_count * sizeof(float));
  }
  result.Set("spectralFlatness", flatness);

  auto rolloff = Napi::Float32Array::New(env, timbre.spectral_rolloff_count);
  if (timbre.spectral_rolloff_count > 0 && timbre.spectral_rolloff != nullptr) {
    std::memcpy(rolloff.Data(), timbre.spectral_rolloff,
                timbre.spectral_rolloff_count * sizeof(float));
  }
  result.Set("spectralRolloff", rolloff);

  auto over_time = Napi::Array::New(env, timbre.timbre_over_time_count);
  for (size_t i = 0; i < timbre.timbre_over_time_count; ++i) {
    const SonareTimbreFrame& frame = timbre.timbre_over_time[i];
    Napi::Object entry = Napi::Object::New(env);
    entry.Set("brightness", Napi::Number::New(env, frame.brightness));
    entry.Set("warmth", Napi::Number::New(env, frame.warmth));
    entry.Set("density", Napi::Number::New(env, frame.density));
    entry.Set("roughness", Napi::Number::New(env, frame.roughness));
    entry.Set("complexity", Napi::Number::New(env, frame.complexity));
    over_time.Set(static_cast<uint32_t>(i), entry);
  }
  result.Set("timbreOverTime", over_time);

  sonare_free_timbre_result(&timbre);
  return result;
}

Napi::Value SonareWrap::DetectChords(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  float min_duration =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 0.3f;
  float smoothing_window =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().FloatValue() : 2.0f;
  float threshold =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().FloatValue() : 0.5f;
  bool use_triads_only =
      info.Length() >= 6 && info[5].IsBoolean() ? info[5].As<Napi::Boolean>().Value() : false;
  int n_fft =
      info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 8 && info[7].IsNumber() ? info[7].As<Napi::Number>().Int32Value() : 512;
  bool use_beat_sync =
      info.Length() >= 9 && info[8].IsBoolean() ? info[8].As<Napi::Boolean>().Value() : true;
  bool use_hmm =
      info.Length() >= 10 && info[9].IsBoolean() ? info[9].As<Napi::Boolean>().Value() : false;
  int hmm_beam_width =
      info.Length() >= 11 && info[10].IsNumber() ? info[10].As<Napi::Number>().Int32Value() : 24;
  bool use_key_context =
      info.Length() >= 12 && info[11].IsBoolean() ? info[11].As<Napi::Boolean>().Value() : false;
  int key_root =
      info.Length() >= 13 && info[12].IsNumber() ? info[12].As<Napi::Number>().Int32Value() : 0;
  int key_mode =
      info.Length() >= 14 && info[13].IsNumber() ? info[13].As<Napi::Number>().Int32Value() : 0;
  bool detect_inversions =
      info.Length() >= 15 && info[14].IsBoolean() ? info[14].As<Napi::Boolean>().Value() : false;
  int chroma_method =
      info.Length() >= 16 && info[15].IsNumber() ? info[15].As<Napi::Number>().Int32Value() : 0;

  SonareChordAnalysisResult analysis{};
  SonareChordDetectionOptions options{};
  options.min_duration = min_duration;
  options.smoothing_window = smoothing_window;
  options.threshold = threshold;
  options.use_triads_only = use_triads_only ? 1 : 0;
  options.n_fft = n_fft;
  options.hop_length = hop_length;
  options.use_beat_sync = use_beat_sync ? 1 : 0;
  options.use_hmm = use_hmm ? 1 : 0;
  options.hmm_beam_width = hmm_beam_width;
  options.use_key_context = use_key_context ? 1 : 0;
  options.key_root = static_cast<SonarePitchClass>(key_root);
  options.key_mode = static_cast<SonareMode>(key_mode);
  options.detect_inversions = detect_inversions ? 1 : 0;
  options.chroma_method = chroma_method;
  SonareError err = sonare_detect_chords_ex(data, length, sample_rate, &options, &analysis);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array chords = Napi::Array::New(env, analysis.chord_count);
  for (size_t i = 0; i < analysis.chord_count; ++i) {
    Napi::Object chord = Napi::Object::New(env);
    std::string root = PitchClassNameLocal(analysis.chords[i].root);
    std::string bass = PitchClassNameLocal(analysis.chords[i].bass);
    std::string quality = ChordQualityName(analysis.chords[i].quality);
    chord.Set("root", Napi::String::New(env, root));
    chord.Set("bass", Napi::String::New(env, bass));
    chord.Set("quality", Napi::String::New(env, quality));
    chord.Set("start", Napi::Number::New(env, analysis.chords[i].start));
    chord.Set("end", Napi::Number::New(env, analysis.chords[i].end));
    chord.Set("duration",
              Napi::Number::New(env, analysis.chords[i].end - analysis.chords[i].start));
    chord.Set("confidence", Napi::Number::New(env, analysis.chords[i].confidence));
    chords.Set(static_cast<uint32_t>(i), chord);
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("chords", chords);
  sonare_free_chord_analysis_result(&analysis);
  return result;
}

Napi::Value SonareWrap::FunctionalAnalysis(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int key_root =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 0;
  int key_mode =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 0;
  int sample_rate =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 22050;
  float min_duration =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().FloatValue() : 0.3f;
  float smoothing_window =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().FloatValue() : 2.0f;
  float threshold =
      info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().FloatValue() : 0.5f;
  bool use_triads_only =
      info.Length() >= 8 && info[7].IsBoolean() ? info[7].As<Napi::Boolean>().Value() : false;
  int n_fft =
      info.Length() >= 9 && info[8].IsNumber() ? info[8].As<Napi::Number>().Int32Value() : 2048;
  int hop_length =
      info.Length() >= 10 && info[9].IsNumber() ? info[9].As<Napi::Number>().Int32Value() : 512;
  bool use_beat_sync =
      info.Length() >= 11 && info[10].IsBoolean() ? info[10].As<Napi::Boolean>().Value() : true;
  bool use_hmm =
      info.Length() >= 12 && info[11].IsBoolean() ? info[11].As<Napi::Boolean>().Value() : false;
  int hmm_beam_width =
      info.Length() >= 13 && info[12].IsNumber() ? info[12].As<Napi::Number>().Int32Value() : 24;
  bool use_key_context =
      info.Length() >= 14 && info[13].IsBoolean() ? info[13].As<Napi::Boolean>().Value() : false;
  bool detect_inversions =
      info.Length() >= 15 && info[14].IsBoolean() ? info[14].As<Napi::Boolean>().Value() : false;
  int chroma_method =
      info.Length() >= 16 && info[15].IsNumber() ? info[15].As<Napi::Number>().Int32Value() : 0;

  SonareChordDetectionOptions options{};
  options.min_duration = min_duration;
  options.smoothing_window = smoothing_window;
  options.threshold = threshold;
  options.use_triads_only = use_triads_only ? 1 : 0;
  options.n_fft = n_fft;
  options.hop_length = hop_length;
  options.use_beat_sync = use_beat_sync ? 1 : 0;
  options.use_hmm = use_hmm ? 1 : 0;
  options.hmm_beam_width = hmm_beam_width;
  options.use_key_context = use_key_context ? 1 : 0;
  options.key_root = static_cast<SonarePitchClass>(key_root);
  options.key_mode = static_cast<SonareMode>(key_mode);
  options.detect_inversions = detect_inversions ? 1 : 0;
  options.chroma_method = chroma_method;

  SonareStringArray labels{};
  SonareError err = sonare_chord_functional_analysis(data, length, sample_rate, &options,
                                                     static_cast<SonarePitchClass>(key_root),
                                                     static_cast<SonareMode>(key_mode), &labels);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array result = Napi::Array::New(env, labels.count);
  for (size_t i = 0; i < labels.count; ++i) {
    result.Set(static_cast<uint32_t>(i),
               Napi::String::New(env, labels.items[i] != nullptr ? labels.items[i] : ""));
  }
  sonare_free_string_array(&labels);
  return result;
}

Napi::Value SonareWrap::Version(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return Napi::String::New(env, sonare_version());
}

Napi::Value SonareWrap::AbiVersion(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return Napi::Number::New(env, sonare_abi_version());
}

Napi::Value SonareWrap::HasFfmpegSupport(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return Napi::Boolean::New(env, sonare_has_ffmpeg_support() != 0);
}
