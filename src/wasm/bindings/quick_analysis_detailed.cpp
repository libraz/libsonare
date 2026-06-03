/// @file quick_analysis_detailed.cpp
/// @brief Embind bindings for detailed per-domain analysis APIs.

#ifdef __EMSCRIPTEN__

#include "common.h"

// ============================================================================
// Detailed per-domain analyzers (BPM / Rhythm / Dynamics / Timbre).
// Public detect_key_candidates (default config); has_ffmpeg_support.
// These match the Node binding surface so feature parity holds across
// languages.
// ============================================================================

val js_analyze_bpm(val samples, int sample_rate, float bpm_min, float bpm_max, float start_bpm,
                   int n_fft, int hop_length, int max_candidates) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  BpmConfig config;
  config.bpm_min = bpm_min;
  config.bpm_max = bpm_max;
  config.start_bpm = start_bpm;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  BpmAnalyzer analyzer(audio, config);

  val out = val::object();
  out.set("bpm", analyzer.bpm());
  out.set("confidence", analyzer.confidence());

  auto candidates = analyzer.candidates(max_candidates);
  val cands = val::array();
  for (const auto& c : candidates) {
    val entry = val::object();
    entry.set("bpm", c.bpm);
    entry.set("confidence", c.confidence);
    cands.call<void>("push", entry);
  }
  out.set("candidates", cands);

  const auto& autocorr = analyzer.autocorrelation();
  out.set("autocorrelation",
          vectorToFloat32Array(std::vector<float>(autocorr.begin(), autocorr.end())));
  const auto& tempogram = analyzer.tempogram();
  out.set("tempogram",
          vectorToFloat32Array(std::vector<float>(tempogram.begin(), tempogram.end())));
  return out;
}

val js_analyze_rhythm(val samples, int sample_rate, float bpm_min, float bpm_max, float start_bpm,
                      int n_fft, int hop_length) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  RhythmConfig config;
  config.bpm_min = bpm_min;
  config.bpm_max = bpm_max;
  config.start_bpm = start_bpm;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  RhythmAnalyzer analyzer(audio, config);

  const auto& features = analyzer.features();
  val time_sig = val::object();
  time_sig.set("numerator", features.time_signature.numerator);
  time_sig.set("denominator", features.time_signature.denominator);
  time_sig.set("confidence", features.time_signature.confidence);

  val out = val::object();
  out.set("timeSignature", time_sig);
  out.set("syncopation", features.syncopation);
  out.set("grooveType", features.groove_type);
  out.set("patternRegularity", features.pattern_regularity);
  out.set("tempoStability", features.tempo_stability);
  out.set("bpm", analyzer.bpm());

  const auto& intervals = analyzer.beat_intervals();
  out.set("beatIntervals", vectorToFloat32Array(intervals));
  return out;
}

val js_analyze_dynamics(val samples, int sample_rate, float window_sec, int hop_length,
                        float compression_threshold) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  DynamicsConfig config;
  config.window_sec = window_sec;
  config.hop_length = hop_length;
  config.compression_threshold = compression_threshold;
  DynamicsAnalyzer analyzer(audio, config);

  const auto& dyn = analyzer.dynamics();
  val out = val::object();
  out.set("dynamicRangeDb", dyn.dynamic_range_db);
  out.set("peakDb", dyn.peak_db);
  out.set("rmsDb", dyn.rms_db);
  out.set("crestFactor", dyn.crest_factor);
  out.set("loudnessRangeDb", dyn.loudness_range_db);
  out.set("isCompressed", dyn.is_compressed);

  const auto& curve = analyzer.loudness_curve();
  out.set("loudnessTimes", vectorToFloat32Array(curve.times));
  out.set("loudnessRmsDb", vectorToFloat32Array(curve.rms_db));
  return out;
}

val js_analyze_timbre(val samples, int sample_rate, int n_fft, int hop_length, int n_mels,
                      int n_mfcc, float window_sec) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  TimbreConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.n_mfcc = n_mfcc;
  config.window_sec = window_sec;
  TimbreAnalyzer analyzer(audio, config);

  const auto& t = analyzer.timbre();
  val out = val::object();
  out.set("brightness", t.brightness);
  out.set("warmth", t.warmth);
  out.set("density", t.density);
  out.set("roughness", t.roughness);
  out.set("complexity", t.complexity);

  out.set("spectralCentroid", vectorToFloat32Array(analyzer.spectral_centroid()));
  out.set("spectralFlatness", vectorToFloat32Array(analyzer.spectral_flatness()));
  out.set("spectralRolloff", vectorToFloat32Array(analyzer.spectral_rolloff()));

  const auto& over_time = analyzer.timbre_over_time();
  val ot_array = val::array();
  for (const auto& entry : over_time) {
    val obj = val::object();
    obj.set("brightness", entry.brightness);
    obj.set("warmth", entry.warmth);
    obj.set("density", entry.density);
    obj.set("roughness", entry.roughness);
    obj.set("complexity", entry.complexity);
    ot_array.call<void>("push", obj);
  }
  out.set("timbreOverTime", ot_array);
  return out;
}

val js_detect_key_candidates_default(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  const auto candidates =
      quick::detect_key_candidates(data.data(), data.size(), sample_rate, KeyConfig{});
  val out = val::array();
  for (const auto& cand : candidates) {
    val entry = val::object();
    val key = val::object();
    key.set("root", static_cast<int>(cand.key.root));
    key.set("mode", static_cast<int>(cand.key.mode));
    key.set("confidence", cand.key.confidence);
    key.set("name", cand.key.to_string());
    key.set("shortName", cand.key.to_short_string());
    entry.set("key", key);
    entry.set("correlation", cand.correlation);
    out.call<void>("push", entry);
  }
  return out;
}

bool js_has_ffmpeg_support() {
#ifdef SONARE_WITH_FFMPEG
  return true;
#else
  return false;
#endif
}

void registerQuickDetailedAnalysisBindings() {
  function("analyzeBpm", &js_analyze_bpm);
  function("analyzeRhythm", &js_analyze_rhythm);
  function("analyzeDynamics", &js_analyze_dynamics);
  function("analyzeTimbre", &js_analyze_timbre);
  function("detectKeyCandidates", &js_detect_key_candidates_default);
  function("hasFfmpegSupport", &js_has_ffmpeg_support);
}

#endif  // __EMSCRIPTEN__
