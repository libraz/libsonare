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

  SonareAnalysisResult analysis{};
  SonareError err = sonare_analyze(data, length, sample_rate, &analysis);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object result = AnalysisToObject(env, analysis);
  sonare_free_result(&analysis);

  return result;
}

namespace {

// Off-main-thread analyze. The worker copies the input Float32Array into its
// own std::vector<float> (so the JS thread is free to release the typed array
// view) and runs sonare_analyze inside Execute(). On completion the analysis
// is marshalled into a JS object on the main thread and the Promise resolves.
class AnalyzeAsyncWorker : public Napi::AsyncWorker {
 public:
  AnalyzeAsyncWorker(Napi::Env env, std::vector<float> samples, int sample_rate)
      : Napi::AsyncWorker(env),
        deferred_(Napi::Promise::Deferred::New(env)),
        samples_(std::move(samples)),
        sample_rate_(sample_rate) {}

  void Execute() override {
    SonareError err = sonare_analyze(samples_.data(), samples_.size(), sample_rate_, &analysis_);
    if (err != SONARE_OK) {
      SetError(ErrorMessageForCode(err));
    }
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    Napi::Object obj = AnalysisToObject(Env(), analysis_);
    sonare_free_result(&analysis_);
    deferred_.Resolve(obj);
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
  SonareAnalysisResult analysis_{};
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

Napi::Value SonareWrap::HasFfmpegSupport(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return Napi::Boolean::New(env, sonare_has_ffmpeg_support() != 0);
}

Napi::Value SonareWrap::Lufs(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;

  SonareLufsResult lufs{};
  SonareError err = sonare_lufs(typed.Data(), typed.ElementLength(), sr, &lufs);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("integratedLufs", Napi::Number::New(env, lufs.integrated_lufs));
  result.Set("momentaryLufs", Napi::Number::New(env, lufs.momentary_lufs));
  result.Set("shortTermLufs", Napi::Number::New(env, lufs.short_term_lufs));
  result.Set("loudnessRange", Napi::Number::New(env, lufs.loudness_range));
  return result;
}

Napi::Value SonareWrap::MomentaryLufs(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;

  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_momentary_lufs(typed.Data(), typed.ElementLength(), sr, &out, &count);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto result = Napi::Float32Array::New(env, count);
  if (count > 0 && out != nullptr) {
    std::memcpy(result.Data(), out, count * sizeof(float));
  }
  sonare_free_floats(out);
  return result;
}

Napi::Value SonareWrap::ShortTermLufs(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;

  float* out = nullptr;
  size_t count = 0;
  SonareError err = sonare_short_term_lufs(typed.Data(), typed.ElementLength(), sr, &out, &count);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto result = Napi::Float32Array::New(env, count);
  if (count > 0 && out != nullptr) {
    std::memcpy(result.Data(), out, count * sizeof(float));
  }
  sonare_free_floats(out);
  return result;
}

Napi::Value SonareWrap::LufsInterleaved(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, channels, sampleRate?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int channels = info[1].As<Napi::Number>().Int32Value();
  int sr =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 22050;
  if (channels <= 0) {
    Napi::TypeError::New(env, "channels must be > 0").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (typed.ElementLength() % static_cast<size_t>(channels) != 0) {
    Napi::TypeError::New(env, "interleaved length must be a multiple of channels")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  size_t frames = typed.ElementLength() / static_cast<size_t>(channels);

  SonareLufsResult lufs{};
  SonareError err = sonare_lufs_interleaved(typed.Data(), frames, channels, sr, &lufs);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("integratedLufs", Napi::Number::New(env, lufs.integrated_lufs));
  result.Set("momentaryLufs", Napi::Number::New(env, lufs.momentary_lufs));
  result.Set("shortTermLufs", Napi::Number::New(env, lufs.short_term_lufs));
  result.Set("loudnessRange", Napi::Number::New(env, lufs.loudness_range));
  return result;
}

Napi::Value SonareWrap::Ebur128LoudnessRange(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;

  float out_lra = 0.0f;
  SonareError err =
      sonare_ebur128_loudness_range(typed.Data(), typed.ElementLength(), sr, &out_lra);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, out_lra);
}

namespace {

// Shared shape for the scalar offline meters (peak/rms/crest/dc).
using MeteringScalarFn = SonareError (*)(const float*, size_t, int, float*);

Napi::Value MeteringScalar(const Napi::CallbackInfo& info, MeteringScalarFn fn,
                           const char* fn_label) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, std::string(fn_label) + ": expected Float32Array argument")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  float out_value = 0.0f;
  SonareError err = fn(typed.Data(), typed.ElementLength(), sr, &out_value);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, out_value);
}

}  // namespace

Napi::Value SonareWrap::MeteringPeakDb(const Napi::CallbackInfo& info) {
  return MeteringScalar(info, &sonare_metering_peak_db, "meteringPeakDb");
}

Napi::Value SonareWrap::MeteringRmsDb(const Napi::CallbackInfo& info) {
  return MeteringScalar(info, &sonare_metering_rms_db, "meteringRmsDb");
}

Napi::Value SonareWrap::MeteringCrestFactorDb(const Napi::CallbackInfo& info) {
  return MeteringScalar(info, &sonare_metering_crest_factor_db, "meteringCrestFactorDb");
}

Napi::Value SonareWrap::MeteringDcOffset(const Napi::CallbackInfo& info) {
  return MeteringScalar(info, &sonare_metering_dc_offset, "meteringDcOffset");
}

Napi::Value SonareWrap::MeteringTruePeakDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "meteringTruePeakDb: expected Float32Array argument")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int oversample =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 4;
  float out_value = 0.0f;
  SonareError err =
      sonare_metering_true_peak_db(typed.Data(), typed.ElementLength(), sr, oversample, &out_value);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, out_value);
}

Napi::Value SonareWrap::MeteringDetectClipping(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "meteringDetectClipping: expected Float32Array argument")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  float threshold =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 0.999f;
  size_t min_region = info.Length() >= 4 && info[3].IsNumber()
                          ? static_cast<size_t>(info[3].As<Napi::Number>().Uint32Value())
                          : 1u;
  SonareClippingResult result{};
  SonareError err = sonare_metering_detect_clipping(typed.Data(), typed.ElementLength(), sr,
                                                    threshold, min_region, &result);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Array regions = Napi::Array::New(env, result.region_count);
  for (size_t i = 0; i < result.region_count; ++i) {
    Napi::Object region = Napi::Object::New(env);
    region.Set("startSample",
               Napi::Number::New(env, static_cast<double>(result.regions[i].start_sample)));
    region.Set("endSample",
               Napi::Number::New(env, static_cast<double>(result.regions[i].end_sample)));
    region.Set("length", Napi::Number::New(env, static_cast<double>(result.regions[i].length)));
    region.Set("peak", Napi::Number::New(env, result.regions[i].peak));
    regions.Set(static_cast<uint32_t>(i), region);
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("clippedSamples", Napi::Number::New(env, static_cast<double>(result.clipped_samples)));
  out.Set("clippingRatio", Napi::Number::New(env, result.clipping_ratio));
  out.Set("maxClippedPeak", Napi::Number::New(env, result.max_clipped_peak));
  out.Set("regions", regions);
  sonare_free_clipping_result(&result);
  return out;
}

Napi::Value SonareWrap::MeteringDynamicRange(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "meteringDynamicRange: expected Float32Array argument")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  float window_sec =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 0.0f;
  float hop_sec =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().FloatValue() : 0.0f;
  float low_p =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().FloatValue() : 0.0f;
  float high_p =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().FloatValue() : 0.0f;
  SonareDynamicRangeResult result{};
  SonareError err = sonare_metering_dynamic_range(typed.Data(), typed.ElementLength(), sr,
                                                  window_sec, hop_sec, low_p, high_p, &result);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto windows = Napi::Float32Array::New(env, result.window_count);
  if (result.window_count > 0 && result.window_rms_db != nullptr) {
    std::memcpy(windows.Data(), result.window_rms_db, result.window_count * sizeof(float));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("dynamicRangeDb", Napi::Number::New(env, result.dynamic_range_db));
  out.Set("lowPercentileDb", Napi::Number::New(env, result.low_percentile_db));
  out.Set("highPercentileDb", Napi::Number::New(env, result.high_percentile_db));
  out.Set("windowRmsDb", windows);
  sonare_free_dynamic_range_result(&result);
  return out;
}

namespace {

bool ParseScaleArgs(const Napi::CallbackInfo& info, int* root, uint16_t* mode_mask,
                    float* reference_midi, float* midi) {
  if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
    return false;
  }
  *root = info[0].As<Napi::Number>().Int32Value();
  int mask_int = info[1].As<Napi::Number>().Int32Value();
  // modeMask is a 12-bit pitch-class set (one bit per semitone). Validate the
  // range explicitly: the narrowing cast to uint16_t would otherwise turn -1
  // into 0xFFFF and any value > 4095 into an unrelated mask.
  if (mask_int < 0 || mask_int > 4095) {
    Napi::RangeError::New(info.Env(), "modeMask must be in [0, 4095]").ThrowAsJavaScriptException();
    return false;
  }
  *mode_mask = static_cast<uint16_t>(mask_int);
  *midi = info[2].As<Napi::Number>().FloatValue();
  *reference_midi =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().FloatValue() : 0.0f;
  return true;
}

}  // namespace

Napi::Value SonareWrap::ScaleQuantizeMidi(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int root = 0;
  uint16_t mask = 0;
  float ref = 0.0f;
  float midi = 0.0f;
  if (!ParseScaleArgs(info, &root, &mask, &ref, &midi)) {
    if (env.IsExceptionPending()) return env.Undefined();
    Napi::TypeError::New(env, "scaleQuantizeMidi: expected (root, modeMask, midi, referenceMidi?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  float out_value = 0.0f;
  SonareError err = sonare_scale_quantize_midi(root, mask, ref, midi, &out_value);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, out_value);
}

Napi::Value SonareWrap::ScaleCorrectionSemitones(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int root = 0;
  uint16_t mask = 0;
  float ref = 0.0f;
  float midi = 0.0f;
  if (!ParseScaleArgs(info, &root, &mask, &ref, &midi)) {
    if (env.IsExceptionPending()) return env.Undefined();
    Napi::TypeError::New(
        env, "scaleCorrectionSemitones: expected (root, modeMask, midi, referenceMidi?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  float out_value = 0.0f;
  SonareError err = sonare_scale_correction_semitones(root, mask, ref, midi, &out_value);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, out_value);
}

Napi::Value SonareWrap::ScalePitchClassEnabled(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "scalePitchClassEnabled: expected (root, modeMask, pitchClass)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int root = info[0].As<Napi::Number>().Int32Value();
  uint16_t mask = static_cast<uint16_t>(info[1].As<Napi::Number>().Int32Value());
  int pitch_class = info[2].As<Napi::Number>().Int32Value();
  int out_enabled = 0;
  SonareError err = sonare_scale_pitch_class_enabled(root, mask, pitch_class, &out_enabled);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Boolean::New(env, out_enabled != 0);
}

namespace {

using StereoScalarFn = SonareError (*)(const float*, const float*, size_t, int, float*);

template <typename T, void (*FreeFn)(T*)>
class CResultGuard {
 public:
  explicit CResultGuard(T* result) : result_(result) {}
  CResultGuard(const CResultGuard&) = delete;
  CResultGuard& operator=(const CResultGuard&) = delete;
  ~CResultGuard() {
    if (result_) FreeFn(result_);
  }

 private:
  T* result_;
};

Napi::Value StereoScalar(const Napi::CallbackInfo& info, StereoScalarFn fn, const char* fn_label) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1])) {
    Napi::TypeError::New(
        env, std::string(fn_label) + ": expected (Float32Array left, Float32Array right)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(env, std::string(fn_label) + ": left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int sr =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 22050;
  float out_value = 0.0f;
  SonareError err = fn(left.Data(), right.Data(), left.ElementLength(), sr, &out_value);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, out_value);
}

}  // namespace

Napi::Value SonareWrap::MeteringStereoCorrelation(const Napi::CallbackInfo& info) {
  return StereoScalar(info, &sonare_metering_stereo_correlation, "meteringStereoCorrelation");
}

Napi::Value SonareWrap::MeteringStereoWidth(const Napi::CallbackInfo& info) {
  return StereoScalar(info, &sonare_metering_stereo_width, "meteringStereoWidth");
}

Napi::Value SonareWrap::MeteringVectorscope(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1])) {
    Napi::TypeError::New(env,
                         "meteringVectorscope: expected (Float32Array left, Float32Array right)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(env, "meteringVectorscope: left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int sr =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 22050;
  SonareVectorscopeResult result{};
  SonareError err =
      sonare_metering_vectorscope(left.Data(), right.Data(), left.ElementLength(), sr, &result);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  CResultGuard<SonareVectorscopeResult, sonare_free_vectorscope_result> guard(&result);
  auto mid = Napi::Float32Array::New(env, result.point_count);
  auto side = Napi::Float32Array::New(env, result.point_count);
  for (size_t i = 0; i < result.point_count; ++i) {
    mid[i] = result.points[i].mid;
    side[i] = result.points[i].side;
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("mid", mid);
  out.Set("side", side);
  return out;
}

Napi::Value SonareWrap::MeteringPhaseScope(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1])) {
    Napi::TypeError::New(env,
                         "meteringPhaseScope: expected (Float32Array left, Float32Array right)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(env, "meteringPhaseScope: left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int sr =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 22050;
  SonarePhaseScopeResult result{};
  SonareError err =
      sonare_metering_phase_scope(left.Data(), right.Data(), left.ElementLength(), sr, &result);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  CResultGuard<SonarePhaseScopeResult, sonare_free_phase_scope_result> guard(&result);
  auto mid = Napi::Float32Array::New(env, result.point_count);
  auto side = Napi::Float32Array::New(env, result.point_count);
  auto radius = Napi::Float32Array::New(env, result.point_count);
  auto angle = Napi::Float32Array::New(env, result.point_count);
  for (size_t i = 0; i < result.point_count; ++i) {
    mid[i] = result.points[i].mid;
    side[i] = result.points[i].side;
    radius[i] = result.points[i].radius;
    angle[i] = result.points[i].angle_rad;
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("mid", mid);
  out.Set("side", side);
  out.Set("radius", radius);
  out.Set("angleRad", angle);
  out.Set("correlation", Napi::Number::New(env, result.correlation));
  out.Set("averageAbsAngleRad", Napi::Number::New(env, result.average_abs_angle_rad));
  out.Set("maxRadius", Napi::Number::New(env, result.max_radius));
  return out;
}

Napi::Value SonareWrap::MeteringSpectrum(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "meteringSpectrum: expected Float32Array samples")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  int sr =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  int n_fft = 0;
  int smooth = 0;
  int octave = 0;
  float db_ref = 0.0f;
  float db_amin = 0.0f;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object opts = info[2].As<Napi::Object>();
    n_fft = int_option(opts, "nFft", 0);
    smooth = bool_option(opts, "applyOctaveSmoothing", false) ? 1 : 0;
    octave = int_option(opts, "octaveFraction", 0);
    db_ref = float_option(opts, "dbRef", 0.0f);
    db_amin = float_option(opts, "dbAmin", 0.0f);
  }
  SonareSpectrumResult result{};
  SonareError err = sonare_metering_spectrum(typed.Data(), typed.ElementLength(), sr, n_fft, smooth,
                                             octave, db_ref, db_amin, &result);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  CResultGuard<SonareSpectrumResult, sonare_free_spectrum_result> guard(&result);
  const size_t bytes = result.bin_count * sizeof(float);
  auto freq = Napi::Float32Array::New(env, result.bin_count);
  auto mag = Napi::Float32Array::New(env, result.bin_count);
  auto pwr = Napi::Float32Array::New(env, result.bin_count);
  auto db = Napi::Float32Array::New(env, result.bin_count);
  if (result.bin_count > 0) {
    std::memcpy(freq.Data(), result.frequencies, bytes);
    std::memcpy(mag.Data(), result.magnitude, bytes);
    std::memcpy(pwr.Data(), result.power, bytes);
    std::memcpy(db.Data(), result.db, bytes);
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("frequencies", freq);
  out.Set("magnitude", mag);
  out.Set("power", pwr);
  out.Set("db", db);
  out.Set("nFft", Napi::Number::New(env, result.n_fft));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  return out;
}

namespace {

const char* SectionTypeName(SonareSectionType type) {
  switch (type) {
    case SONARE_SECTION_INTRO:
      return "Intro";
    case SONARE_SECTION_VERSE:
      return "Verse";
    case SONARE_SECTION_PRE_CHORUS:
      return "PreChorus";
    case SONARE_SECTION_CHORUS:
      return "Chorus";
    case SONARE_SECTION_BRIDGE:
      return "Bridge";
    case SONARE_SECTION_INSTRUMENTAL:
      return "Instrumental";
    case SONARE_SECTION_OUTRO:
      return "Outro";
    case SONARE_SECTION_UNKNOWN:
    default:
      return "Unknown";
  }
}

}  // namespace

Napi::Value SonareWrap::AnalyzeWithProgress(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !info[2].IsFunction()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, onProgress)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const int sample_rate = info[1].As<Napi::Number>().Int32Value();
  Napi::Function js_cb = info[2].As<Napi::Function>();

  try {
    // analyze() is synchronous, so js_cb (referenced by info[2]) outlives the call.
    sonare::Audio audio =
        sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sample_rate);
    sonare::MusicAnalyzer analyzer(audio);
    analyzer.set_progress_callback([&js_cb, &env](float progress, const char* stage) {
      js_cb.Call({Napi::Number::New(env, progress), Napi::String::New(env, stage ? stage : "")});
    });
    sonare::AnalysisResult analysis = analyzer.analyze();

    // Reuse the C-ABI result shape so the output matches analyze().
    std::vector<float> beat_times;
    beat_times.reserve(analysis.beats.size());
    for (const auto& beat : analysis.beats) {
      beat_times.push_back(beat.time);
    }

    SonareAnalysisResult c_result{};
    c_result.bpm = analysis.bpm;
    c_result.bpm_confidence = analysis.bpm_confidence;
    c_result.key.root = static_cast<SonarePitchClass>(static_cast<int>(analysis.key.root));
    c_result.key.mode = static_cast<SonareMode>(static_cast<int>(analysis.key.mode));
    c_result.key.confidence = analysis.key.confidence;
    c_result.time_signature.numerator = analysis.time_signature.numerator;
    c_result.time_signature.denominator = analysis.time_signature.denominator;
    c_result.time_signature.confidence = analysis.time_signature.confidence;
    c_result.beat_times = beat_times.empty() ? nullptr : beat_times.data();
    c_result.beat_count = beat_times.size();

    return AnalysisToObject(env, c_result);
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return env.Undefined();
  }
}

Napi::Value SonareWrap::AnalyzeSections(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const int sample_rate =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  const int n_fft =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 2048;
  const int hop_length =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 512;
  const float min_section_sec =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().FloatValue() : 4.0f;

  SonareSectionResult result{};
  SonareError err = sonare_analyze_sections(typed.Data(), typed.ElementLength(), sample_rate, n_fft,
                                            hop_length, min_section_sec, &result);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array sections = Napi::Array::New(env, result.section_count);
  for (size_t i = 0; i < result.section_count; ++i) {
    const SonareSection& section = result.sections[i];
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("type", Napi::Number::New(env, static_cast<int>(section.type)));
    obj.Set("name", Napi::String::New(env, SectionTypeName(section.type)));
    obj.Set("start", Napi::Number::New(env, section.start));
    obj.Set("end", Napi::Number::New(env, section.end));
    obj.Set("energyLevel", Napi::Number::New(env, section.energy_level));
    obj.Set("confidence", Napi::Number::New(env, section.confidence));
    sections.Set(static_cast<uint32_t>(i), obj);
  }
  sonare_free_section_result(&result);
  return sections;
}

Napi::Value SonareWrap::AnalyzeMelody(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected Float32Array argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const int sample_rate =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 22050;
  const float fmin =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : 65.0f;
  const float fmax =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().FloatValue() : 2093.0f;
  const int frame_length =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 2048;
  const int hop_length =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 512;
  const float threshold =
      info.Length() >= 7 && info[6].IsNumber() ? info[6].As<Napi::Number>().FloatValue() : 0.1f;

  SonareMelodyResult result{};
  SonareError err = sonare_analyze_melody(typed.Data(), typed.ElementLength(), sample_rate, fmin,
                                          fmax, frame_length, hop_length, threshold, &result);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array points = Napi::Array::New(env, result.point_count);
  for (size_t i = 0; i < result.point_count; ++i) {
    const SonareMelodyPoint& point = result.points[i];
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("time", Napi::Number::New(env, point.time));
    obj.Set("frequency", Napi::Number::New(env, point.frequency));
    obj.Set("confidence", Napi::Number::New(env, point.confidence));
    points.Set(static_cast<uint32_t>(i), obj);
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("points", points);
  out.Set("pitchRangeOctaves", Napi::Number::New(env, result.pitch_range_octaves));
  out.Set("pitchStability", Napi::Number::New(env, result.pitch_stability));
  out.Set("meanFrequency", Napi::Number::New(env, result.mean_frequency));
  out.Set("vibratoRate", Napi::Number::New(env, result.vibrato_rate));
  sonare_free_melody_result(&result);
  return out;
}
