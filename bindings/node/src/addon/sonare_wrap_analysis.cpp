#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "analysis/acoustic_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "analysis/timbre_analyzer.h"
#include "core/audio.h"
#include "sonare_wrap.h"
#include "sonare_wrap_analysis_input.h"
#include "sonare_wrap_key_options.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"
#include "util/constants.h"

using namespace sonare_node;

namespace {

struct AnalysisProgressContext {
  Napi::Env env;
  Napi::Function on_progress;
  std::optional<Napi::Function> cancel;
};

// Runs the full analysis synchronously with JS progress/cancel callbacks.
//
// Lending the TypedArray's pointer across the callbacks is safe and deliberately
// not defended a second time: run_offline (src/c_api/sonare_c_internal.h) builds
// the Audio with Audio::from_buffer, which copies, before any callback can fire
// (pinned by "copies the input before the first progress callback" in
// tests/api/sonare_c_core_test.cpp).
Napi::Value AnalyzeWithCallbacks(Napi::Env env, const float* data, size_t length, int sample_rate,
                                 SonareMusicAnalyzeOptions options, Napi::Function on_progress,
                                 std::optional<Napi::Function> cancel) {
  // The C-ABI callbacks run synchronously on this thread, so the stack context stays valid.
  AnalysisProgressContext ctx{env, on_progress, cancel};

  // A JS callback that throws leaves an exception pending; every further N-API
  // call must be skipped, and the analysis is cancelled so the throw surfaces
  // promptly instead of after the whole pipeline has run.
  auto c_progress = [](float progress, const char* stage, void* user_data) {
    auto* c = static_cast<AnalysisProgressContext*>(user_data);
    if (c->env.IsExceptionPending()) return;
    c->on_progress.Call({Napi::Number::New(c->env, static_cast<double>(progress)),
                         Napi::String::New(c->env, stage != nullptr ? stage : "")});
    sonare_node::MarkPendingForeign(c->env);
  };
  auto c_cancel = [](void* user_data) {
    auto* c = static_cast<AnalysisProgressContext*>(user_data);
    if (c->env.IsExceptionPending()) return 1;
    if (!c->cancel) return 0;
    const Napi::Value result = c->cancel->Call({});
    sonare_node::MarkPendingForeign(c->env);
    if (c->env.IsExceptionPending()) return 1;
    return result.IsBoolean() && result.As<Napi::Boolean>().Value() ? 1 : 0;
  };

  char* json_str = nullptr;
  options.progress_callback = c_progress;
  options.progress_user_data = &ctx;
  options.cancel_callback = c_cancel;
  options.cancel_user_data = &ctx;
  SonareError err = sonare_analyze_json(data, length, sample_rate, &options, &json_str);
  // The pending-exception check comes first: throwing a SonareError on top of a
  // callback's exception would abort the process under NAPI_DISABLE_CPP_EXCEPTIONS.
  if (env.IsExceptionPending()) {
    sonare_free_string(json_str);
    return env.Undefined();
  }
  if (err != SONARE_OK) {
    sonare_free_string(json_str);
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Value parsed = ParseJsonObjectAndFree(env, json_str, "Failed to parse analysis JSON");
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object result = parsed.As<Napi::Object>();
  Napi::Error enrich_error;
  if (!EnrichFullAnalysisObject(env, result, &enrich_error)) {
    enrich_error.ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return result;
}

}  // namespace

Napi::Value SonareWrap::DetectBpm(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  MonoAnalysisInput input;
  if (!ReadMonoAnalysisInput(info, &input, sonare::constants::kDefaultSampleRate))
    return env.Undefined();
  const float* data = input.data;
  size_t length = input.length;
  int sample_rate = input.sample_rate;

  const sonare::BpmConfig tempo_defaults;
  float bpm_min{};
  if (!OptionalFloatArg(env, info, 2, "bpmMin", tempo_defaults.bpm_min, &bpm_min))
    return env.Undefined();
  float bpm_max{};
  if (!OptionalFloatArg(env, info, 3, "bpmMax", tempo_defaults.bpm_max, &bpm_max))
    return env.Undefined();

  float bpm = 0.0f;
  SonareError err = sonare_detect_bpm(data, length, sample_rate, bpm_min, bpm_max, &bpm);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  return Napi::Number::New(env, static_cast<double>(bpm));
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DetectKey(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  MonoAnalysisInput input;
  if (!ReadMonoAnalysisInput(info, &input, sonare::constants::kDefaultSampleRate))
    return env.Undefined();
  const float* data = input.data;
  size_t length = input.length;
  int sample_rate = input.sample_rate;

  int n_fft = sonare::constants::kDefaultNFft;
  int hop_length = sonare::constants::kDefaultHopLength;
  bool use_hpss = false;
  bool loudness_weighted = false;
  float chroma_high_pass_hz = 80.0f;
  std::vector<SonareMode> modes;
  SonareKeyProfileType profile = SONARE_KEY_PROFILE_AUTO;
  std::string genre_hint;
  KeyTuningOption tuning;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    n_fft = IntProperty(options, "nFft", n_fft);
    hop_length = DomainProperty<int>(options, "hopLength", hop_length, sonare::kKeyHopLengthDomain);
    use_hpss = BoolProperty(options, "useHpss", use_hpss);
    loudness_weighted = BoolProperty(options, "loudnessWeighted", loudness_weighted);
    chroma_high_pass_hz = FloatProperty(options, "chromaHighPassHz", chroma_high_pass_hz);
    modes = node_modes_option(options);
    profile = node_profile_from_value(options.Get("profile"));
    genre_hint = StringProperty(options, "genreHint", "");
    tuning = node_key_tuning_option(options);
  }

  SonareKey key{};
  float tuning_used = 0.0f;
  SonareDetectKeyOptions key_options;
  sonare_detect_key_options_init(&key_options);
  key_options.n_fft = n_fft;
  key_options.hop_length = hop_length;
  key_options.use_hpss = use_hpss ? 1 : 0;
  key_options.loudness_weighted = loudness_weighted ? 1 : 0;
  key_options.chroma_high_pass_hz = chroma_high_pass_hz;
  key_options.modes = modes.empty() ? nullptr : modes.data();
  key_options.mode_count = modes.size();
  key_options.profile_type = profile;
  key_options.genre_hint = genre_hint.empty() ? nullptr : genre_hint.c_str();
  key_options.tuning_cents = tuning.tuning_cents;
  key_options.tuning_auto = tuning.measure ? 1 : 0;
  SonareError err = sonare_detect_key(data, length, sample_rate, &key_options, &key, &tuning_used);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  Napi::Object result = KeyToObject(env, key.root, key.mode, key.confidence);
  result.Set("tuningCents", Napi::Number::New(env, static_cast<double>(tuning_used)));
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DetectKeyCandidates(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  MonoAnalysisInput input;
  if (!ReadMonoAnalysisInput(info, &input, sonare::constants::kDefaultSampleRate))
    return env.Undefined();
  const float* data = input.data;
  size_t length = input.length;
  int sample_rate = input.sample_rate;

  int n_fft = sonare::constants::kDefaultNFft;
  int hop_length = sonare::constants::kDefaultHopLength;
  bool use_hpss = false;
  bool loudness_weighted = false;
  float chroma_high_pass_hz = 80.0f;
  std::vector<SonareMode> modes;
  SonareKeyProfileType profile = SONARE_KEY_PROFILE_AUTO;
  std::string genre_hint;
  KeyTuningOption tuning;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    n_fft = IntProperty(options, "nFft", n_fft);
    hop_length = DomainProperty<int>(options, "hopLength", hop_length, sonare::kKeyHopLengthDomain);
    use_hpss = BoolProperty(options, "useHpss", use_hpss);
    loudness_weighted = BoolProperty(options, "loudnessWeighted", loudness_weighted);
    chroma_high_pass_hz = FloatProperty(options, "chromaHighPassHz", chroma_high_pass_hz);
    modes = node_modes_option(options);
    profile = node_profile_from_value(options.Get("profile"));
    genre_hint = StringProperty(options, "genreHint", "");
    tuning = node_key_tuning_option(options);
  }

  SonareKeyCandidate* candidates = nullptr;
  size_t count = 0;
  SonareDetectKeyOptions key_options;
  sonare_detect_key_options_init(&key_options);
  key_options.n_fft = n_fft;
  key_options.hop_length = hop_length;
  key_options.use_hpss = use_hpss ? 1 : 0;
  key_options.loudness_weighted = loudness_weighted ? 1 : 0;
  key_options.chroma_high_pass_hz = chroma_high_pass_hz;
  key_options.modes = modes.empty() ? nullptr : modes.data();
  key_options.mode_count = modes.size();
  key_options.profile_type = profile;
  key_options.genre_hint = genre_hint.empty() ? nullptr : genre_hint.c_str();
  key_options.tuning_cents = tuning.tuning_cents;
  key_options.tuning_auto = tuning.measure ? 1 : 0;
  SonareError err = sonare_detect_key_candidates(data, length, sample_rate, &key_options,
                                                 &candidates, &count, nullptr);
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
  SONARE_NODE_TRY

  MonoAnalysisInput input;
  if (!ReadMonoAnalysisInput(info, &input, sonare::constants::kDefaultSampleRate))
    return env.Undefined();
  const float* data = input.data;
  size_t length = input.length;
  int sample_rate = input.sample_rate;

  const sonare::BpmConfig tempo_defaults;
  float bpm_min{};
  if (!OptionalFloatArg(env, info, 2, "bpmMin", tempo_defaults.bpm_min, &bpm_min))
    return env.Undefined();
  float bpm_max{};
  if (!OptionalFloatArg(env, info, 3, "bpmMax", tempo_defaults.bpm_max, &bpm_max))
    return env.Undefined();

  float* times = nullptr;
  size_t count = 0;
  SonareError err =
      sonare_detect_beats(data, length, sample_rate, bpm_min, bpm_max, &times, &count);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  auto result = TakeCFloats(env, times, count);
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DetectDownbeats(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  MonoAnalysisInput input;
  if (!ReadMonoAnalysisInput(info, &input, sonare::constants::kDefaultSampleRate))
    return env.Undefined();
  const float* data = input.data;
  size_t length = input.length;
  int sample_rate = input.sample_rate;

  float* times = nullptr;
  size_t count = 0;
  SonareError err = sonare_detect_downbeats(data, length, sample_rate, &times, &count);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  auto result = TakeCFloats(env, times, count);
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::EstimateMeter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  if (!RequireFloat32Array(info, 0, "beatTimes must be a Float32Array") ||
      !RequireFloat32Array(info, 1, "beatStrengths must be a Float32Array")) {
    return env.Undefined();
  }

  auto beat_times = info[0].As<Napi::Float32Array>();
  auto beat_strengths = info[1].As<Napi::Float32Array>();
  // The C ABI carries a single beat count for both arrays, so a mismatch cannot
  // reach the core guard that rejects it: passing the shorter length would read
  // a prefix nobody asked about, and the longer one would run off a buffer.
  if (beat_times.ElementLength() != beat_strengths.ElementLength()) {
    Napi::RangeError::New(env, "beatTimes and beatStrengths must be the same length (got " +
                                   std::to_string(beat_times.ElementLength()) + " and " +
                                   std::to_string(beat_strengths.ElementLength()) + ")")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SonareMeterOptions options;
  sonare_meter_options_init(&options);
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object bag = info[2].As<Napi::Object>();
    options.denominator = IntProperty(bag, "denominator", options.denominator);
    options.downbeat_weight = FloatProperty(bag, "downbeatWeight", options.downbeat_weight);
    options.measure_weight = FloatProperty(bag, "measureWeight", options.measure_weight);
    options.subdivision_weight =
        FloatProperty(bag, "subdivisionWeight", options.subdivision_weight);
    options.compound_subdivision_threshold =
        FloatProperty(bag, "compoundSubdivisionThreshold", options.compound_subdivision_threshold);
    if (!ReadMeterCandidateNumerators(bag, "candidateNumerators", options.candidate_numerators,
                                      &options.candidate_numerator_count)) {
      return env.Undefined();
    }
  }

  char* json_str = nullptr;
  SonareError err = sonare_estimate_meter_json(sonare_node::ElementData(beat_times),
                                               sonare_node::ElementData(beat_strengths),
                                               beat_times.ElementLength(), &options, &json_str);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return ParseJsonObjectAndFree(env, json_str, "Failed to parse meter estimation JSON");
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DetectOnsets(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  MonoAnalysisInput input;
  if (!ReadMonoAnalysisInput(info, &input, sonare::constants::kDefaultSampleRate))
    return env.Undefined();
  const float* data = input.data;
  size_t length = input.length;
  int sample_rate = input.sample_rate;

  SonareOnsetDetectConfig config{};
  sonare_onset_detect_config_init(&config);
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.n_fft = IntProperty(options, "nFft", config.n_fft);
    config.hop_length = IntProperty(options, "hopLength", config.hop_length);
    config.threshold = FloatProperty(options, "threshold", config.threshold);
    config.pre_max = IntProperty(options, "preMax", config.pre_max);
    config.post_max = IntProperty(options, "postMax", config.post_max);
    config.pre_avg = IntProperty(options, "preAvg", config.pre_avg);
    config.post_avg = IntProperty(options, "postAvg", config.post_avg);
    config.delta = FloatProperty(options, "delta", config.delta);
    config.wait = IntProperty(options, "wait", config.wait);
    config.backtrack = BoolProperty(options, "backtrack", false) ? 1 : 0;
    config.backtrack_range = IntProperty(options, "backtrackRange", config.backtrack_range);
  }
  // The core's rule, so a refusal names the key this caller wrote rather than the C field.
  sonare::OnsetDetectConfig native;
  native.n_fft = config.n_fft;
  native.hop_length = config.hop_length;
  native.threshold = std::isnan(config.threshold) ? 0.0f : config.threshold;
  native.pre_max = config.pre_max;
  native.post_max = config.post_max;
  native.pre_avg = config.pre_avg;
  native.post_avg = config.post_avg;
  native.delta = config.delta;
  native.wait = config.wait;
  native.backtrack_range = config.backtrack_range;
  sonare::validate_onset_detect_config(native, sonare::kOnsetDetectJsFieldNames);

  float* times = nullptr;
  size_t count = 0;
  SonareError err = sonare_detect_onsets(data, length, sample_rate, &config, &times, &count);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  auto result = TakeCFloats(env, times, count);
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Analyze(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  MonoAnalysisInput input;
  if (!ReadMonoAnalysisInput(info, &input, sonare::constants::kDefaultSampleRate))
    return env.Undefined();
  const float* data = input.data;
  size_t length = input.length;
  int sample_rate = input.sample_rate;
  SonareMusicAnalyzeOptions options{};
  const bool has_options = info.Length() >= 3 && ReadMusicAnalyzeOptions(info[2], &options);
  if (env.IsExceptionPending()) return env.Undefined();
  // Trailing onProgress / cancel run the analysis with those callbacks, synchronously.
  std::optional<Napi::Function> on_progress;
  std::optional<Napi::Function> cancel;
  if (!OptionalFunctionArg(env, info, 3, "onProgress", &on_progress) ||
      !OptionalFunctionArg(env, info, 4, "cancel", &cancel)) {
    return env.Undefined();
  }
  if (on_progress || cancel) {
    return AnalyzeWithCallbacks(
        env, data, length, sample_rate, options,
        on_progress ? *on_progress : Napi::Function::New(env, [](const Napi::CallbackInfo&) {}),
        cancel);
  }
  return FullAnalysisJsonToObject(env, data, length, sample_rate, has_options ? &options : nullptr);
  SONARE_NODE_CATCH(env)
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
    SonareError err = sonare_analyze_json(samples_.data(), samples_.size(), sample_rate_,
                                          has_options_ ? &options_ : nullptr, &json_ptr);
    if (err != SONARE_OK) {
      error_code_ = err;
      shape_refusal_ = LastRefusalIsShape();
      refusal_ = sonare_node::ShownLastRefusal();
      refusal_entry_ = sonare_node::LastRefusalEntry();
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
        deferred_.Reject(sonare_node::MakeSonareError(env, SONARE_ERROR_INVALID_FORMAT,
                                                      "Failed to parse analysis JSON")
                             .Value());
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
    sonare_node::DecorateSonareError(Env(), error.Value(), error_code_, shape_refusal_, refusal_,
                                     refusal_entry_);
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
  bool shape_refusal_ = false;
  std::optional<sonare::SonareParameterRefusal> refusal_;
  std::string refusal_entry_;
};

}  // namespace

Napi::Value SonareWrap::AnalyzeAsync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    auto deferred = Napi::Promise::Deferred::New(env);
    deferred.Reject(Napi::TypeError::New(env, "samples must be a Float32Array").Value());
    return deferred.Promise();
  }
  auto typed = info[0].As<Napi::Float32Array>();
  std::vector<float> samples(sonare_node::ElementData(typed),
                             sonare_node::ElementData(typed) + typed.ElementLength());
  int sample_rate{};
  if (!OptionalIntArg(env, info, 1, "sampleRate", sonare::constants::kDefaultSampleRate,
                      &sample_rate)) {
    return sonare_node::RejectedFromPending(env);
  }
  SonareMusicAnalyzeOptions options{};
  // A *Property reader refuses by C++ throw, the array field by a pending
  // exception; SONARE_NODE_CATCH_REJECT and the check below turn both into a rejection.
  const bool has_options = info.Length() >= 3 && ReadMusicAnalyzeOptions(info[2], &options);
  if (env.IsExceptionPending()) return sonare_node::RejectedFromPending(env);
  auto* worker = new AnalyzeAsyncWorker(env, std::move(samples), sample_rate, options, has_options);
  Napi::Promise promise = worker->GetPromise();
  worker->Queue();
  return promise;
  SONARE_NODE_CATCH_REJECT(env)
}

Napi::Value SonareWrap::AnalyzeBpm(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  MonoAnalysisInput input;
  if (!ReadMonoAnalysisInput(info, &input, sonare::constants::kDefaultSampleRate))
    return env.Undefined();
  const float* data = input.data;
  size_t length = input.length;
  int sample_rate = input.sample_rate;
  float bpm_min{};
  if (!OptionalFloatArg(env, info, 2, "bpmMin", sonare::BpmConfig{}.bpm_min, &bpm_min))
    return env.Undefined();
  float bpm_max{};
  if (!OptionalFloatArg(env, info, 3, "bpmMax", sonare::BpmConfig{}.bpm_max, &bpm_max))
    return env.Undefined();
  float start_bpm{};
  if (!OptionalFloatArg(env, info, 4, "startBpm", sonare::BpmConfig{}.start_bpm, &start_bpm))
    return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 5, "nFft", sonare::constants::kDefaultNFft, &n_fft)) {
    return env.Undefined();
  }
  int hop_length{};
  if (!OptionalDomainArg(env, info, 6, "hopLength", sonare::constants::kDefaultHopLength,
                         sonare::kBpmCountDomain, &hop_length)) {
    return env.Undefined();
  }
  int max_candidates{};
  if (!RequiredIntArg(env, info, 7, "maxCandidates", &max_candidates)) return env.Undefined();

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

  auto autocorrelation =
      Float32ArrayFrom(env, analysis.autocorrelation, analysis.autocorrelation_count);
  result.Set("autocorrelation", autocorrelation);

  auto tempogram = Float32ArrayFrom(env, analysis.tempogram, analysis.tempogram_count);
  result.Set("tempogram", tempogram);

  sonare_free_bpm_analysis_result(&analysis);
  return result;
  SONARE_NODE_CATCH(env)
}

namespace {

/// @brief The one JS spelling of a SonareAcousticResult: a scalar or band array that was not
///        measured is an absent member (a null band pointer is the blind path's clarity family,
///        an all-NaN one its decay times), never NaN or an empty or zero-filled array.
Napi::Object AcousticResultToObject(Napi::Env env, const SonareAcousticResult& acoustic) {
  Napi::Object result = Napi::Object::New(env);
  SetMeasured(result, "rt60", acoustic.rt60);
  SetMeasured(result, "edt", acoustic.edt);
  SetMeasured(result, "c50", acoustic.c50);
  SetMeasured(result, "c80", acoustic.c80);
  SetMeasured(result, "d50", acoustic.d50);
  result.Set("confidence", Napi::Number::New(env, acoustic.confidence));
  result.Set("isBlind", Napi::Boolean::New(env, acoustic.is_blind != 0));
  SetMeasuredArray(result, "rt60Bands", acoustic.rt60_bands, acoustic.band_count);
  SetMeasuredArray(result, "edtBands", acoustic.edt_bands, acoustic.band_count);
  SetMeasuredArray(result, "c50Bands", acoustic.c50_bands, acoustic.band_count);
  SetMeasuredArray(result, "c80Bands", acoustic.c80_bands, acoustic.band_count);
  return result;
}

}  // namespace

Napi::Value SonareWrap::AnalyzeImpulseResponse(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  MonoAnalysisInput input;
  if (!ReadMonoAnalysisInput(info, &input,
                             static_cast<int>(sonare::constants::kDefaultDawSampleRate)))
    return env.Undefined();
  const float* data = input.data;
  size_t length = input.length;
  int sample_rate = input.sample_rate;
  int n_octave_bands{};
  if (!OptionalDomainArg(env, info, 2, "nOctaveBands", 6, sonare::kAcousticBandCountDomain,
                         &n_octave_bands)) {
    return env.Undefined();
  }
  float min_decay_db{};
  if (!OptionalDomainArg(env, info, 3, "minDecayDb", 30.0f, sonare::kAcousticMinDecayDbDomain,
                         &min_decay_db)) {
    return env.Undefined();
  }

  SonareAcousticResult acoustic{};
  SonareAnalyzeImpulseResponseOptions impulse_options;
  sonare_analyze_impulse_response_options_init(&impulse_options);
  impulse_options.min_decay_db = min_decay_db;
  SonareError err = sonare_analyze_impulse_response(data, length, sample_rate, n_octave_bands,
                                                    &impulse_options, &acoustic);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Object result = AcousticResultToObject(env, acoustic);

  sonare_free_acoustic_result(&acoustic);
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DetectAcoustic(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  MonoAnalysisInput input;
  if (!ReadMonoAnalysisInput(info, &input,
                             static_cast<int>(sonare::constants::kDefaultDawSampleRate)))
    return env.Undefined();
  const float* data = input.data;
  size_t length = input.length;
  int sample_rate = input.sample_rate;
  int n_octave_bands{};
  if (!OptionalDomainArg(env, info, 2, "nOctaveBands", 6, sonare::kAcousticBandCountDomain,
                         &n_octave_bands)) {
    return env.Undefined();
  }
  int n_third_octave_subbands{};
  if (!OptionalDomainArg(env, info, 3, "nThirdOctaveSubbands", 24, sonare::kAcousticBandCountDomain,
                         &n_third_octave_subbands)) {
    return env.Undefined();
  }
  float min_decay_db{};
  if (!OptionalDomainArg(env, info, 4, "minDecayDb", 30.0f, sonare::kAcousticMinDecayDbDomain,
                         &min_decay_db)) {
    return env.Undefined();
  }
  float noise_floor_margin_db{};
  if (!OptionalDomainArg(env, info, 5, "noiseFloorMarginDb", 10.0f,
                         sonare::kAcousticNoiseFloorMarginDbDomain, &noise_floor_margin_db)) {
    return env.Undefined();
  }

  SonareAcousticResult acoustic{};
  SonareError err =
      sonare_detect_acoustic(data, length, sample_rate, n_octave_bands, n_third_octave_subbands,
                             min_decay_db, noise_floor_margin_db, &acoustic);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Object result = AcousticResultToObject(env, acoustic);

  sonare_free_acoustic_result(&acoustic);
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::AnalyzeRhythm(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  MonoAnalysisInput input;
  if (!ReadMonoAnalysisInput(info, &input, sonare::constants::kDefaultSampleRate))
    return env.Undefined();
  const float* data = input.data;
  size_t length = input.length;
  int sample_rate = input.sample_rate;
  float bpm_min{};
  if (!OptionalFloatArg(env, info, 2, "bpmMin", sonare::RhythmConfig{}.bpm_min, &bpm_min))
    return env.Undefined();
  float bpm_max{};
  if (!OptionalFloatArg(env, info, 3, "bpmMax", sonare::RhythmConfig{}.bpm_max, &bpm_max))
    return env.Undefined();
  float start_bpm{};
  if (!OptionalFloatArg(env, info, 4, "startBpm", sonare::RhythmConfig{}.start_bpm, &start_bpm))
    return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 5, "nFft", sonare::constants::kDefaultNFft, &n_fft)) {
    return env.Undefined();
  }
  int hop_length{};
  if (!OptionalDomainArg(env, info, 6, "hopLength", sonare::constants::kDefaultHopLength,
                         sonare::kBpmCountDomain, &hop_length)) {
    return env.Undefined();
  }

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

  auto intervals = Float32ArrayFrom(env, rhythm.beat_intervals, rhythm.beat_interval_count);
  result.Set("beatIntervals", intervals);

  sonare_free_rhythm_result(&rhythm);
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::AnalyzeDynamics(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  MonoAnalysisInput input;
  if (!ReadMonoAnalysisInput(info, &input, sonare::constants::kDefaultSampleRate))
    return env.Undefined();
  const float* data = input.data;
  size_t length = input.length;
  int sample_rate = input.sample_rate;
  float window_sec{};
  if (!OptionalDomainArg(env, info, 2, "windowSec", 0.4f, sonare::kDynamicsWindowSecDomain,
                         &window_sec)) {
    return env.Undefined();
  }
  int hop_length{};
  if (!OptionalDomainArg(env, info, 3, "hopLength", sonare::constants::kDefaultHopLength,
                         sonare::kDynamicsHopDomain, &hop_length)) {
    return env.Undefined();
  }
  float compression_threshold{};
  if (!OptionalDomainArg(env, info, 4, "compressionThresholdDb", 6.0f,
                         sonare::kDynamicsCompressionThresholdDomain, &compression_threshold)) {
    return env.Undefined();
  }

  SonareDynamicsResult dynamics{};
  SonareError err = sonare_analyze_dynamics(data, length, sample_rate, window_sec, hop_length,
                                            compression_threshold, &dynamics);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  SetMeasured(result, "dynamicRangeDb", dynamics.dynamic_range_db);
  SetMeasured(result, "peakDb", dynamics.peak_db);
  SetMeasured(result, "rmsDb", dynamics.rms_db);
  SetMeasured(result, "crestFactor", dynamics.crest_factor);
  SetMeasured(result, "loudnessRangeLu", dynamics.loudness_range_lu);
  result.Set("isCompressed", Napi::Boolean::New(env, dynamics.is_compressed != 0));

  auto times = Float32ArrayFrom(env, dynamics.loudness_times_sec, dynamics.loudness_count);
  auto rms_db = Float32ArrayFrom(env, dynamics.loudness_rms_db, dynamics.loudness_count);
  result.Set("loudnessTimesSec", times);
  result.Set("loudnessRmsDb", rms_db);

  sonare_free_dynamics_result(&dynamics);
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::AnalyzeTimbre(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  MonoAnalysisInput input;
  if (!ReadMonoAnalysisInput(info, &input, sonare::constants::kDefaultSampleRate))
    return env.Undefined();
  const float* data = input.data;
  size_t length = input.length;
  int sample_rate = input.sample_rate;
  int n_fft{};
  int hop_length{};
  int n_mels{};
  int n_mfcc{};
  float window_sec{};
  if (!OptionalIntArg(env, info, 2, "nFft", sonare::constants::kDefaultNFft, &n_fft) ||
      !OptionalDomainArg(env, info, 3, "hopLength", sonare::constants::kDefaultHopLength,
                         sonare::kTimbreCountDomain, &hop_length) ||
      !OptionalDomainArg(env, info, 4, "nMels", 128, sonare::kTimbreCountDomain, &n_mels) ||
      !OptionalDomainArg(env, info, 5, "nMfcc", 13, sonare::kTimbreCountDomain, &n_mfcc) ||
      !OptionalDomainArg(env, info, 6, "windowSec", 0.5f, sonare::kTimbreWindowSecDomain,
                         &window_sec)) {
    return env.Undefined();
  }

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

  auto centroid = Float32ArrayFrom(env, timbre.spectral_centroid, timbre.spectral_centroid_count);
  result.Set("spectralCentroid", centroid);

  auto flatness = Float32ArrayFrom(env, timbre.spectral_flatness, timbre.spectral_flatness_count);
  result.Set("spectralFlatness", flatness);

  auto rolloff = Float32ArrayFrom(env, timbre.spectral_rolloff, timbre.spectral_rolloff_count);
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
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Version(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return Napi::String::New(env, sonare_version());
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::AbiVersion(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return Napi::Number::New(env, sonare_abi_version());
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SystemInfo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  char* raw = nullptr;
  const SonareError err = sonare_system_info_json(&raw);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  const std::string json(raw);
  sonare_free_string(raw);
  return Napi::String::New(env, json);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Capabilities(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  char* raw = nullptr;
  const SonareError err = sonare_capabilities_json(&raw);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  const std::string json(raw);
  sonare_free_string(raw);

  Napi::Object json_global = env.Global().Get("JSON").As<Napi::Object>();
  Napi::Function json_parse = json_global.Get("parse").As<Napi::Function>();
  Napi::Value parsed = json_parse.Call(json_global, {Napi::String::New(env, json)});
  if (env.IsExceptionPending() || !parsed.IsObject()) {
    if (!env.IsExceptionPending()) {
      sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_FORMAT,
                                           "Failed to parse native capabilities JSON");
    }
    return env.Undefined();
  }
  return parsed;
  SONARE_NODE_CATCH(env)
}
