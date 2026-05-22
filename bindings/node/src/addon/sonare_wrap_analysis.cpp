#include <cstring>
#include <string>

#include "sonare_wrap.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

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

  SonareKey key{};
  SonareError err = sonare_detect_key(data, length, sample_rate, &key);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return KeyToObject(env, key.root, key.mode, key.confidence);
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

  SonareChordAnalysisResult analysis{};
  SonareError err = sonare_detect_chords(data, length, sample_rate, min_duration, smoothing_window,
                                         threshold, use_triads_only ? 1 : 0, n_fft, hop_length,
                                         use_beat_sync ? 1 : 0, &analysis);
  if (err != SONARE_OK) {
    Napi::Error::New(env, ErrorMessageForCode(err)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array chords = Napi::Array::New(env, analysis.chord_count);
  for (size_t i = 0; i < analysis.chord_count; ++i) {
    Napi::Object chord = Napi::Object::New(env);
    std::string root = PitchClassNameLocal(analysis.chords[i].root);
    std::string quality = ChordQualityName(analysis.chords[i].quality);
    chord.Set("root", Napi::String::New(env, root));
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

Napi::Value SonareWrap::Version(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return Napi::String::New(env, sonare_version());
}

Napi::Value SonareWrap::HasFfmpegSupport(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return Napi::Boolean::New(env, sonare_has_ffmpeg_support() != 0);
}
