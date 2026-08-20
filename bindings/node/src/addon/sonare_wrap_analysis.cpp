#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "analysis/music_analyzer.h"
#include "core/audio.h"
#include "sonare_wrap.h"
#include "sonare_wrap_key_options.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

Napi::Value SonareWrap::DetectBpm(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = node_arg_int(info, 1, 22050);

  float bpm = 0.0f;
  SonareError err = sonare_detect_bpm(data, length, sample_rate, &bpm);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  return Napi::Number::New(env, static_cast<double>(bpm));
}

Napi::Value SonareWrap::DetectKey(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = node_arg_int(info, 1, 22050);

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
    n_fft = node_int_option(options, "nFft", n_fft);
    hop_length = node_int_option(options, "hopLength", hop_length);
    use_hpss = node_bool_option(options, "useHpss", use_hpss);
    loudness_weighted = node_bool_option(options, "loudnessWeighted", loudness_weighted);
    high_pass_hz = node_float_option(options, "highPassHz", high_pass_hz);
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
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return KeyToObject(env, key.root, key.mode, key.confidence);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DetectKeyCandidates(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = node_arg_int(info, 1, 22050);

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
    n_fft = node_int_option(options, "nFft", n_fft);
    hop_length = node_int_option(options, "hopLength", hop_length);
    use_hpss = node_bool_option(options, "useHpss", use_hpss);
    loudness_weighted = node_bool_option(options, "loudnessWeighted", loudness_weighted);
    high_pass_hz = node_float_option(options, "highPassHz", high_pass_hz);
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
    sonare_node::ThrowSonareError(env, err);
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DetectBeats(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = node_arg_int(info, 1, 22050);

  float* times = nullptr;
  size_t count = 0;
  SonareError err = sonare_detect_beats(data, length, sample_rate, &times, &count);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
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

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = node_arg_int(info, 1, 22050);

  float* times = nullptr;
  size_t count = 0;
  SonareError err = sonare_detect_downbeats(data, length, sample_rate, &times, &count);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  auto result = Napi::Float32Array::New(env, count);
  if (count > 0 && times != nullptr) {
    std::memcpy(result.Data(), times, count * sizeof(float));
    sonare_free_floats(times);
  }
  return result;
}

Napi::Value SonareWrap::EstimateMeter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "estimateMeter: beatTimes must be a Float32Array") ||
      !RequireFloat32Array(info, 1, "estimateMeter: beatStrengths must be a Float32Array")) {
    return env.Undefined();
  }

  auto beat_times = info[0].As<Napi::Float32Array>();
  auto beat_strengths = info[1].As<Napi::Float32Array>();
  // The C ABI carries a single beat count for both arrays, so a mismatch cannot
  // reach the core guard that rejects it: passing the shorter length would read
  // a prefix nobody asked about, and the longer one would run off a buffer.
  if (beat_times.ElementLength() != beat_strengths.ElementLength()) {
    Napi::RangeError::New(
        env, "estimateMeter: beatTimes and beatStrengths must be the same length (got " +
                 std::to_string(beat_times.ElementLength()) + " and " +
                 std::to_string(beat_strengths.ElementLength()) + ")")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SonareMeterOptions options = sonare_meter_options_default();
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object bag = info[2].As<Napi::Object>();
    options.denominator = node_int_option(bag, "denominator", options.denominator);
    options.downbeat_weight = node_float_option(bag, "downbeatWeight", options.downbeat_weight);
    options.measure_weight = node_float_option(bag, "measureWeight", options.measure_weight);
    options.subdivision_weight =
        node_float_option(bag, "subdivisionWeight", options.subdivision_weight);
    options.compound_subdivision_threshold = node_float_option(
        bag, "compoundSubdivisionThreshold", options.compound_subdivision_threshold);
    if (!ReadMeterCandidateNumerators(bag, "candidateNumerators", options.candidate_numerators,
                                      &options.candidate_numerator_count)) {
      return env.Undefined();
    }
  }

  char* json_str = nullptr;
  SonareError err = sonare_estimate_meter_json(beat_times.Data(), beat_strengths.Data(),
                                               beat_times.ElementLength(), &options, &json_str);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return ParseJsonObjectAndFree(env, json_str, "Failed to parse meter estimation JSON");
}

Napi::Value SonareWrap::DetectOnsets(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = node_arg_int(info, 1, 22050);

  SonareOnsetDetectConfig config{};
  config.n_fft = 2048;
  config.hop_length = 512;
  config.threshold = 0.0f;
  config.pre_max = 1;
  config.post_max = 1;
  config.pre_avg = 3;
  config.post_avg = 4;
  config.delta = 0.06f;
  config.wait = 1;
  config.backtrack = 0;
  config.backtrack_range = 10;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.n_fft = node_int_option(options, "nFft", config.n_fft);
    config.hop_length = node_int_option(options, "hopLength", config.hop_length);
    config.threshold = node_float_option(options, "threshold", config.threshold);
    config.pre_max = node_int_option(options, "preMax", config.pre_max);
    config.post_max = node_int_option(options, "postMax", config.post_max);
    config.pre_avg = node_int_option(options, "preAvg", config.pre_avg);
    config.post_avg = node_int_option(options, "postAvg", config.post_avg);
    config.delta = node_float_option(options, "delta", config.delta);
    config.wait = node_int_option(options, "wait", config.wait);
    config.backtrack = node_bool_option(options, "backtrack", false) ? 1 : 0;
    config.backtrack_range = node_int_option(options, "backtrackRange", config.backtrack_range);
  }

  float* times = nullptr;
  size_t count = 0;
  SonareError err = sonare_detect_onsets_ex(data, length, sample_rate, &config, &times, &count);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
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

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();

  int sample_rate = node_arg_int(info, 1, 22050);
  SonareMusicAnalyzeOptions options{};
  const bool has_options = info.Length() >= 3 && ReadMusicAnalyzeOptions(info[2], &options);
  if (env.IsExceptionPending()) return env.Undefined();
  return FullAnalysisJsonToObject(env, data, length, sample_rate, has_options ? &options : nullptr);
}

namespace {

// Off-main-thread analyze. The worker copies the input Float32Array into its
// own std::vector<float> (so the JS thread is free to release the typed array
// view) and runs sonare_analyze_json inside Execute() to get a JSON string.
// On completion the JSON string is parsed on the main thread (required because
// JSON.parse touches the V8 heap) and the Promise resolves with the full result.
class AnalyzeAsyncWorker : public Napi::AsyncWorker {
 public:
  AnalyzeAsyncWorker(Napi::Env env, std::vector<float> samples, int sample_rate,
                     SonareMusicAnalyzeOptions options, bool has_options)
      : Napi::AsyncWorker(env),
        deferred_(Napi::Promise::Deferred::New(env)),
        samples_(std::move(samples)),
        sample_rate_(sample_rate),
        options_(options),
        has_options_(has_options) {}

  void Execute() override {
    char* json_ptr = nullptr;
    SonareError err = has_options_ ? sonare_analyze_json_ex(samples_.data(), samples_.size(),
                                                            sample_rate_, &options_, &json_ptr)
                                   : sonare_analyze_json(samples_.data(), samples_.size(),
                                                         sample_rate_, &json_ptr);
    if (err != SONARE_OK) {
      error_code_ = err;
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

    Napi::Error enrich_error;
    if (!EnrichFullAnalysisObject(env, result, &enrich_error)) {
      deferred_.Reject(enrich_error.Value());
      return;
    }

    deferred_.Resolve(result);
  }

  void OnError(const Napi::Error& error) override {
    Napi::HandleScope scope(Env());
    sonare_node::DecorateSonareError(Env(), error.Value(), error_code_);
    deferred_.Reject(error.Value());
  }

  Napi::Promise GetPromise() { return deferred_.Promise(); }

 private:
  Napi::Promise::Deferred deferred_;
  std::vector<float> samples_;
  int sample_rate_;
  SonareMusicAnalyzeOptions options_{};
  bool has_options_ = false;
  std::string json_string_;
  SonareError error_code_ = SONARE_ERROR_UNKNOWN;
};

}  // namespace

Napi::Value SonareWrap::AnalyzeAsync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    auto deferred = Napi::Promise::Deferred::New(env);
    deferred.Reject(Napi::TypeError::New(env, "Expected (Float32Array, sampleRate?)").Value());
    return deferred.Promise();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  std::vector<float> samples(typed.Data(), typed.Data() + typed.ElementLength());
  int sample_rate = node_arg_int(info, 1, 22050);
  SonareMusicAnalyzeOptions options{};
  const bool has_options = info.Length() >= 3 && ReadMusicAnalyzeOptions(info[2], &options);
  if (env.IsExceptionPending()) {
    // The async form reports bad input as a rejected Promise, so the pending
    // exception is cleared before it can collide with the Promise plumbing.
    Napi::Error error = env.GetAndClearPendingException();
    auto deferred = Napi::Promise::Deferred::New(env);
    deferred.Reject(error.Value());
    return deferred.Promise();
  }
  auto* worker = new AnalyzeAsyncWorker(env, std::move(samples), sample_rate, options, has_options);
  Napi::Promise promise = worker->GetPromise();
  worker->Queue();
  return promise;
}

Napi::Value SonareWrap::AnalyzeBpm(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate = node_arg_int(info, 1, 22050);
  float bpm_min = node_arg_float(info, 2, 30.0f);
  float bpm_max = node_arg_float(info, 3, 300.0f);
  float start_bpm = node_arg_float(info, 4, 120.0f);
  int n_fft = node_arg_int(info, 5, 2048);
  int hop_length = node_arg_int(info, 6, 512);
  int max_candidates = node_arg_int(info, 7, 5);

  SonareBpmAnalysisResult analysis{};
  SonareError err = sonare_analyze_bpm(data, length, sample_rate, bpm_min, bpm_max, start_bpm,
                                       n_fft, hop_length, max_candidates, &analysis);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
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

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate = node_arg_int(info, 1, 48000);
  int n_octave_bands = node_arg_int(info, 2, 6);
  float min_decay_db = node_arg_float(info, 3, 30.0f);

  SonareAcousticResult acoustic{};
  SonareError err = sonare_analyze_impulse_response_ex(data, length, sample_rate, n_octave_bands,
                                                       min_decay_db, &acoustic);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
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

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate = node_arg_int(info, 1, 48000);
  int n_octave_bands = node_arg_int(info, 2, 6);
  int n_third_octave_subbands = node_arg_int(info, 3, 24);
  float min_decay_db = node_arg_float(info, 4, 30.0f);
  float noise_floor_margin_db = node_arg_float(info, 5, 10.0f);

  SonareAcousticResult acoustic{};
  SonareError err =
      sonare_detect_acoustic(data, length, sample_rate, n_octave_bands, n_third_octave_subbands,
                             min_decay_db, noise_floor_margin_db, &acoustic);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
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

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate = node_arg_int(info, 1, 22050);
  float bpm_min = node_arg_float(info, 2, 60.0f);
  float bpm_max = node_arg_float(info, 3, 200.0f);
  float start_bpm = node_arg_float(info, 4, 120.0f);
  int n_fft = node_arg_int(info, 5, 2048);
  int hop_length = node_arg_int(info, 6, 512);

  SonareRhythmResult rhythm{};
  SonareError err = sonare_analyze_rhythm(data, length, sample_rate, bpm_min, bpm_max, start_bpm,
                                          n_fft, hop_length, &rhythm);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
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

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate = node_arg_int(info, 1, 22050);
  float window_sec = node_arg_float(info, 2, 0.4f);
  int hop_length = node_arg_int(info, 3, 512);
  float compression_threshold = node_arg_float(info, 4, 6.0f);

  SonareDynamicsResult dynamics{};
  SonareError err = sonare_analyze_dynamics(data, length, sample_rate, window_sec, hop_length,
                                            compression_threshold, &dynamics);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
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

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate = node_arg_int(info, 1, 22050);
  int n_fft = node_arg_int(info, 2, 2048);
  int hop_length = node_arg_int(info, 3, 512);
  int n_mels = node_arg_int(info, 4, 128);
  int n_mfcc = node_arg_int(info, 5, 13);
  float window_sec = node_arg_float(info, 6, 0.5f);

  SonareTimbreResult timbre{};
  SonareError err = sonare_analyze_timbre(data, length, sample_rate, n_fft, hop_length, n_mels,
                                          n_mfcc, window_sec, &timbre);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
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

namespace {

// The C chord record carries the root, bass and quality but not the rendered
// symbol, so the canonical spelling is recovered by rebuilding the core chord
// and asking it. Formatting the symbol here instead would fork the spelling
// away from the core the moment a quality is added.
//
// Rebuilding relies on the two quality enumerations agreeing value for value,
// which the C API's own root/bass conversion already assumes. These assertions
// make a future divergence a build failure rather than a mislabelled chord.
static_assert(static_cast<int>(sonare::ChordQuality::Major) == SONARE_CHORD_MAJOR, "");
static_assert(static_cast<int>(sonare::ChordQuality::Minor) == SONARE_CHORD_MINOR, "");
static_assert(static_cast<int>(sonare::ChordQuality::Diminished) == SONARE_CHORD_DIMINISHED, "");
static_assert(static_cast<int>(sonare::ChordQuality::Augmented) == SONARE_CHORD_AUGMENTED, "");
static_assert(static_cast<int>(sonare::ChordQuality::Dominant7) == SONARE_CHORD_DOMINANT7, "");
static_assert(static_cast<int>(sonare::ChordQuality::Major7) == SONARE_CHORD_MAJOR7, "");
static_assert(static_cast<int>(sonare::ChordQuality::Minor7) == SONARE_CHORD_MINOR7, "");
static_assert(static_cast<int>(sonare::ChordQuality::Sus2) == SONARE_CHORD_SUS2, "");
static_assert(static_cast<int>(sonare::ChordQuality::Sus4) == SONARE_CHORD_SUS4, "");
static_assert(static_cast<int>(sonare::ChordQuality::Unknown) == SONARE_CHORD_UNKNOWN, "");
static_assert(static_cast<int>(sonare::ChordQuality::Add9) == SONARE_CHORD_ADD9, "");
static_assert(static_cast<int>(sonare::ChordQuality::MinorAdd9) == SONARE_CHORD_MINOR_ADD9, "");
static_assert(static_cast<int>(sonare::ChordQuality::Dim7) == SONARE_CHORD_DIM7, "");
static_assert(static_cast<int>(sonare::ChordQuality::HalfDim7) == SONARE_CHORD_HALF_DIM7, "");
static_assert(static_cast<int>(sonare::ChordQuality::Major9) == SONARE_CHORD_MAJOR9, "");
static_assert(static_cast<int>(sonare::ChordQuality::Dominant9) == SONARE_CHORD_DOMINANT9, "");
static_assert(static_cast<int>(sonare::ChordQuality::Sus2Add4) == SONARE_CHORD_SUS2_ADD4, "");

/// @brief Returns the core's canonical symbol for one C chord record.
std::string ChordSymbol(const SonareChord& chord) {
  sonare::Chord core{};
  core.root = static_cast<sonare::PitchClass>(chord.root);
  core.bass = static_cast<sonare::PitchClass>(chord.bass);
  core.quality = static_cast<sonare::ChordQuality>(chord.quality);
  return core.to_string();
}

}  // namespace

Napi::Value SonareWrap::DetectChords(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sample_rate = node_arg_int(info, 1, 22050);
  float min_duration = node_arg_float(info, 2, 0.3f);
  float smoothing_window = node_arg_float(info, 3, 2.0f);
  float threshold = node_arg_float(info, 4, 0.5f);
  bool use_triads_only = node_arg_bool(info, 5, false);
  int n_fft = node_arg_int(info, 6, 2048);
  int hop_length = node_arg_int(info, 7, 512);
  bool use_beat_sync = node_arg_bool(info, 8, true);
  bool use_hmm = node_arg_bool(info, 9, false);
  int hmm_beam_width = node_arg_int(info, 10, 24);
  bool use_key_context = node_arg_bool(info, 11, false);
  int key_root = node_arg_int(info, 12, 0);
  int key_mode = node_arg_int(info, 13, 0);
  bool detect_inversions = node_arg_bool(info, 14, false);
  int chroma_method = node_arg_int(info, 15, 0);

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
    sonare_node::ThrowSonareError(env, err);
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
    chord.Set("rootName", Napi::String::New(env, root));
    chord.Set("bassName", Napi::String::New(env, bass));
    chord.Set("quality", Napi::String::New(env, quality));
    chord.Set("name", Napi::String::New(env, ChordSymbol(analysis.chords[i])));
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

  if (!RequireFloat32Array(info, 0, "Expected Float32Array argument")) {
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int key_root = node_arg_int(info, 1, 0);
  int key_mode = node_arg_int(info, 2, 0);
  int sample_rate = node_arg_int(info, 3, 22050);
  float min_duration = node_arg_float(info, 4, 0.3f);
  float smoothing_window = node_arg_float(info, 5, 2.0f);
  float threshold = node_arg_float(info, 6, 0.5f);
  bool use_triads_only = node_arg_bool(info, 7, false);
  int n_fft = node_arg_int(info, 8, 2048);
  int hop_length = node_arg_int(info, 9, 512);
  bool use_beat_sync = node_arg_bool(info, 10, true);
  bool use_hmm = node_arg_bool(info, 11, false);
  int hmm_beam_width = node_arg_int(info, 12, 24);
  bool use_key_context = node_arg_bool(info, 13, false);
  bool detect_inversions = node_arg_bool(info, 14, false);
  int chroma_method = node_arg_int(info, 15, 0);

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
    sonare_node::ThrowSonareError(env, err);
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

Napi::Value SonareWrap::Capabilities(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const char* json = sonare_capabilities_json();
  if (json == nullptr) {
    Napi::Error::New(env, "Native capabilities JSON is unavailable").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object json_global = env.Global().Get("JSON").As<Napi::Object>();
  Napi::Function json_parse = json_global.Get("parse").As<Napi::Function>();
  Napi::Value parsed = json_parse.Call(json_global, {Napi::String::New(env, json)});
  if (env.IsExceptionPending() || !parsed.IsObject()) {
    if (!env.IsExceptionPending()) {
      Napi::Error::New(env, "Failed to parse native capabilities JSON")
          .ThrowAsJavaScriptException();
    }
    return env.Undefined();
  }
  return parsed;
}

Napi::Value SonareWrap::HasFfmpegSupport(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return Napi::Boolean::New(env, sonare_has_ffmpeg_support() != 0);
}
