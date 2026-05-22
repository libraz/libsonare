/// @file bindings.cpp
/// @brief Embind bindings for WebAssembly.

#ifdef __EMSCRIPTEN__

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "analysis/beat_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/meter/lufs.h"
#include "analysis/music_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "analysis/section_analyzer.h"
#include "analysis/timbre_analyzer.h"
#include "core/audio.h"
#include "core/convert.h"
#include "core/db_convert.h"
#include "core/pcen.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/preemphasis.h"
#include "effects/silence.h"
#include "effects/time_stretch.h"
#include "feature/chroma.h"
#include "feature/mel_spectrogram.h"
#include "feature/pitch.h"
#include "feature/rhythm.h"
#include "feature/spectral.h"
#include "feature/tonnetz.h"
#include "mastering/api/chain.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/common/processor_base.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/eq/tilt.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/tape.h"
#include "mastering/spectral/air_band.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/mono_maker.h"
#include "quick.h"
#include "sonare.h"
#include "streaming/stream_analyzer.h"
#include "util/frame.h"
#include "util/padding.h"
#include "util/peak.h"
#include "util/vector_normalize.h"

using namespace emscripten;
using namespace sonare;

// ============================================================================
// Helper functions
// ============================================================================

val vectorToFloat32Array(const std::vector<float>& vec) {
  val result = val::global("Float32Array").new_(vec.size());
  for (size_t i = 0; i < vec.size(); ++i) {
    result.set(i, vec[i]);
  }
  return result;
}

val vectorToInt32Array(const std::vector<int>& vec) {
  val result = val::global("Int32Array").new_(vec.size());
  for (size_t i = 0; i < vec.size(); ++i) {
    result.set(i, vec[i]);
  }
  return result;
}

std::vector<float> float32ArrayToVector(val arr) { return vecFromJSArray<float>(arr); }

std::vector<mastering::api::Param> masteringParamsFromObject(val object) {
  std::vector<mastering::api::Param> params;
  if (object.isNull() || object.isUndefined()) {
    return params;
  }
  val keys = val::global("Object").call<val>("keys", object);
  const int length = keys["length"].as<int>();
  params.reserve(static_cast<size_t>(length));
  for (int index = 0; index < length; ++index) {
    std::string key = keys[index].as<std::string>();
    val value = object[key];
    if (value.typeOf().as<std::string>() == "number") {
      params.push_back({key, value.as<double>()});
    } else if (value.typeOf().as<std::string>() == "boolean") {
      params.push_back({key, value.as<bool>() ? 1.0 : 0.0});
    }
  }
  return params;
}

bool hasProperty(val object, const char* key) {
  if (object.isNull() || object.isUndefined()) {
    return false;
  }
  return !object[key].isUndefined() && !object[key].isNull();
}

val objectProperty(val object, const char* key) {
  if (!hasProperty(object, key)) {
    return val::undefined();
  }
  return object[key];
}

float floatProperty(val object, const char* key, float default_value) {
  val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : value.as<float>();
}

int intProperty(val object, const char* key, int default_value) {
  val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : value.as<int>();
}

bool boolProperty(val object, const char* key, bool default_value) {
  val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : value.as<bool>();
}

void processMono(mastering::common::ProcessorBase& processor, std::vector<float>& samples,
                 int sample_rate) {
  if (samples.empty()) {
    return;
  }
  processor.prepare(sample_rate, static_cast<int>(samples.size()));
  float* channels[] = {samples.data()};
  processor.process(channels, 1, static_cast<int>(samples.size()));
}

void processStereo(mastering::common::ProcessorBase& processor, std::vector<float>& left,
                   std::vector<float>& right, int sample_rate) {
  if (left.empty()) {
    return;
  }
  if (left.size() != right.size()) {
    throw std::invalid_argument("stereo channel lengths must match");
  }
  processor.prepare(sample_rate, static_cast<int>(left.size()));
  float* channels[] = {left.data(), right.data()};
  processor.process(channels, 2, static_cast<int>(left.size()));
}

std::vector<float> monoMix(const std::vector<float>& left, const std::vector<float>& right) {
  if (left.size() != right.size()) {
    throw std::invalid_argument("stereo channel lengths must match");
  }
  std::vector<float> mono(left.size());
  for (size_t i = 0; i < left.size(); ++i) {
    mono[i] = 0.5f * (left[i] + right[i]);
  }
  return mono;
}

float integratedLufs(const std::vector<float>& samples, int sample_rate) {
  Audio audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
  return analysis::meter::lufs(audio).integrated_lufs;
}

void applyGainDb(std::vector<float>& left, std::vector<float>& right, float gain_db) {
  const float gain = std::pow(10.0f, gain_db / 20.0f);
  for (size_t i = 0; i < left.size(); ++i) {
    left[i] *= gain;
    right[i] *= gain;
  }
}

/// @brief Converts AnalysisResult to JavaScript object.
/// @param result Analysis result
/// @return JavaScript object with all analysis data
val analysisResultToVal(const AnalysisResult& result) {
  val out = val::object();

  // BPM
  out.set("bpm", result.bpm);
  out.set("bpmConfidence", result.bpm_confidence);

  // Key
  val key = val::object();
  key.set("root", static_cast<int>(result.key.root));
  key.set("mode", static_cast<int>(result.key.mode));
  key.set("confidence", result.key.confidence);
  key.set("name", result.key.to_string());
  key.set("shortName", result.key.to_short_string());
  out.set("key", key);

  // Time signature
  val timeSig = val::object();
  timeSig.set("numerator", result.time_signature.numerator);
  timeSig.set("denominator", result.time_signature.denominator);
  timeSig.set("confidence", result.time_signature.confidence);
  out.set("timeSignature", timeSig);

  // Beats
  val beats = val::array();
  for (size_t i = 0; i < result.beats.size(); ++i) {
    val beat = val::object();
    beat.set("time", result.beats[i].time);
    beat.set("strength", result.beats[i].strength);
    beats.call<void>("push", beat);
  }
  out.set("beats", beats);

  // Chords
  val chords = val::array();
  for (size_t i = 0; i < result.chords.size(); ++i) {
    val chord = val::object();
    chord.set("root", static_cast<int>(result.chords[i].root));
    chord.set("quality", static_cast<int>(result.chords[i].quality));
    chord.set("start", result.chords[i].start);
    chord.set("end", result.chords[i].end);
    chord.set("confidence", result.chords[i].confidence);
    chord.set("name", result.chords[i].to_string());
    chords.call<void>("push", chord);
  }
  out.set("chords", chords);

  // Sections
  val sections = val::array();
  for (size_t i = 0; i < result.sections.size(); ++i) {
    val section = val::object();
    section.set("type", static_cast<int>(result.sections[i].type));
    section.set("start", result.sections[i].start);
    section.set("end", result.sections[i].end);
    section.set("energyLevel", result.sections[i].energy_level);
    section.set("confidence", result.sections[i].confidence);
    section.set("name", result.sections[i].type_string());
    sections.call<void>("push", section);
  }
  out.set("sections", sections);

  // Timbre
  val timbre = val::object();
  timbre.set("brightness", result.timbre.brightness);
  timbre.set("warmth", result.timbre.warmth);
  timbre.set("density", result.timbre.density);
  timbre.set("roughness", result.timbre.roughness);
  timbre.set("complexity", result.timbre.complexity);
  out.set("timbre", timbre);

  // Dynamics
  val dynamics = val::object();
  dynamics.set("dynamicRangeDb", result.dynamics.dynamic_range_db);
  dynamics.set("loudnessRangeDb", result.dynamics.loudness_range_db);
  dynamics.set("crestFactor", result.dynamics.crest_factor);
  dynamics.set("isCompressed", result.dynamics.is_compressed);
  out.set("dynamics", dynamics);

  // Rhythm
  val rhythm = val::object();
  rhythm.set("syncopation", result.rhythm.syncopation);
  rhythm.set("grooveType", result.rhythm.groove_type);
  rhythm.set("patternRegularity", result.rhythm.pattern_regularity);
  out.set("rhythm", rhythm);

  // Form
  out.set("form", result.form);

  return out;
}

// ============================================================================
// Quick API (high-level)
// ============================================================================

float js_detect_bpm(val samples, int sample_rate) {
  std::vector<float> data = vecFromJSArray<float>(samples);
  return quick::detect_bpm(data.data(), data.size(), sample_rate);
}

val js_detect_key(val samples, int sample_rate) {
  std::vector<float> data = vecFromJSArray<float>(samples);
  Key key = quick::detect_key(data.data(), data.size(), sample_rate);

  val result = val::object();
  result.set("root", static_cast<int>(key.root));
  result.set("mode", static_cast<int>(key.mode));
  result.set("confidence", key.confidence);
  result.set("name", key.to_string());
  result.set("shortName", key.to_short_string());
  return result;
}

val js_detect_onsets(val samples, int sample_rate) {
  std::vector<float> data = vecFromJSArray<float>(samples);
  std::vector<float> onsets = quick::detect_onsets(data.data(), data.size(), sample_rate);
  return vectorToFloat32Array(onsets);
}

val js_detect_beats(val samples, int sample_rate) {
  std::vector<float> data = vecFromJSArray<float>(samples);
  std::vector<float> beats = quick::detect_beats(data.data(), data.size(), sample_rate);
  return vectorToFloat32Array(beats);
}

val js_analyze(val samples, int sample_rate) {
  std::vector<float> data = vecFromJSArray<float>(samples);
  AnalysisResult result = quick::analyze(data.data(), data.size(), sample_rate);
  return analysisResultToVal(result);
}

// Analyze with progress callback
val js_analyze_with_progress(val samples, int sample_rate, val progress_callback) {
  std::vector<float> data = vecFromJSArray<float>(samples);

  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  MusicAnalyzer analyzer(audio);

  // Set progress callback if provided
  if (!progress_callback.isNull() && !progress_callback.isUndefined()) {
    analyzer.set_progress_callback([progress_callback](float progress, const char* stage) {
      progress_callback(progress, std::string(stage));
    });
  }

  AnalysisResult result = analyzer.analyze();
  return analysisResultToVal(result);
}

// ============================================================================
// Effects
// ============================================================================

// HPSS - Harmonic/Percussive Source Separation
val js_hpss(val samples, int sample_rate, int kernel_harmonic, int kernel_percussive) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  HpssConfig config;
  config.kernel_size_harmonic = kernel_harmonic;
  config.kernel_size_percussive = kernel_percussive;

  HpssAudioResult result = hpss(audio, config);

  val out = val::object();

  // Harmonic audio
  std::vector<float> harmonic_vec(result.harmonic.data(),
                                  result.harmonic.data() + result.harmonic.size());
  out.set("harmonic", vectorToFloat32Array(harmonic_vec));

  // Percussive audio
  std::vector<float> percussive_vec(result.percussive.data(),
                                    result.percussive.data() + result.percussive.size());
  out.set("percussive", vectorToFloat32Array(percussive_vec));

  out.set("sampleRate", result.harmonic.sample_rate());

  return out;
}

// Get harmonic component only
val js_harmonic(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  Audio result = harmonic(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// Get percussive component only
val js_percussive(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  Audio result = percussive(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// Time stretch
val js_time_stretch(val samples, int sample_rate, float rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  Audio result = time_stretch(audio, rate);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// Pitch shift
val js_pitch_shift(val samples, int sample_rate, float semitones) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  Audio result = pitch_shift(audio, semitones);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// Normalize
val js_normalize(val samples, int sample_rate, float target_db) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  Audio result = normalize(audio, target_db);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// Trim silence
val js_trim(val samples, int sample_rate, float threshold_db) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  Audio result = trim(audio, threshold_db);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

val js_mastering(val samples, int sample_rate, float target_lufs, float ceiling_db,
                 int true_peak_oversample) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  mastering::maximizer::LoudnessOptimizeConfig config;
  config.target_lufs = target_lufs;
  config.ceiling_db = ceiling_db;
  config.true_peak_oversample = true_peak_oversample;

  auto result = mastering::maximizer::loudness_optimize(audio, config);
  std::vector<float> out_vec(result.audio.data(), result.audio.data() + result.audio.size());

  val out = val::object();
  out.set("samples", vectorToFloat32Array(out_vec));
  out.set("sampleRate", result.audio.sample_rate());
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  return out;
}

// ---------------------------------------------------------------------------
// Helpers: build a MasteringChainConfig from the nested JS config object that
// js_mastering_chain / js_mastering_chain_stereo receive.
// ---------------------------------------------------------------------------

mastering::api::MasteringChainConfig masteringChainConfigFromVal(val config) {
  mastering::api::MasteringChainConfig out;

  val repair = objectProperty(config, "repair");
  if (boolProperty(repair, "denoise", false)) {
    out.repair.denoise.enabled = true;
    out.repair.denoise.config.n_fft = intProperty(repair, "nFft", out.repair.denoise.config.n_fft);
    out.repair.denoise.config.hop_length =
        intProperty(repair, "hopLength", out.repair.denoise.config.hop_length);
    out.repair.denoise.config.dd_alpha =
        floatProperty(repair, "ddAlpha", out.repair.denoise.config.dd_alpha);
    out.repair.denoise.config.gain_floor =
        floatProperty(repair, "gainFloor", out.repair.denoise.config.gain_floor);
  }
  if (hasProperty(repair, "declick")) {
    val declick = objectProperty(repair, "declick");
    out.repair.declick.enabled = true;
    auto& dc = out.repair.declick.config;
    dc.threshold = floatProperty(declick, "threshold", dc.threshold);
    dc.neighbor_ratio = floatProperty(declick, "neighborRatio", dc.neighbor_ratio);
    dc.max_click_samples = static_cast<size_t>(
        intProperty(declick, "maxClickSamples", static_cast<int>(dc.max_click_samples)));
    dc.lpc_order = intProperty(declick, "lpcOrder", dc.lpc_order);
    dc.residual_ratio = floatProperty(declick, "residualRatio", dc.residual_ratio);
  }
  if (hasProperty(repair, "dereverb")) {
    val dereverb = objectProperty(repair, "dereverb");
    out.repair.dereverb.enabled = true;
    auto& rc = out.repair.dereverb.config;
    rc.threshold = floatProperty(dereverb, "threshold", rc.threshold);
    rc.attenuation = floatProperty(dereverb, "attenuation", rc.attenuation);
    rc.n_fft = intProperty(dereverb, "nFft", rc.n_fft);
    rc.hop_length = intProperty(dereverb, "hopLength", rc.hop_length);
    rc.t60_sec = floatProperty(dereverb, "t60Sec", rc.t60_sec);
    rc.late_delay_ms = floatProperty(dereverb, "lateDelayMs", rc.late_delay_ms);
    rc.over_subtraction = floatProperty(dereverb, "overSubtraction", rc.over_subtraction);
    rc.spectral_floor = floatProperty(dereverb, "spectralFloor", rc.spectral_floor);
    rc.wpe_enabled = boolProperty(dereverb, "wpeEnabled", rc.wpe_enabled);
    rc.wpe_iterations = intProperty(dereverb, "wpeIterations", rc.wpe_iterations);
    rc.wpe_taps = intProperty(dereverb, "wpeTaps", rc.wpe_taps);
    rc.wpe_strength = floatProperty(dereverb, "wpeStrength", rc.wpe_strength);
  }

  val eq = objectProperty(config, "eq");
  if (hasProperty(eq, "tiltDb") || hasProperty(eq, "pivotHz")) {
    out.eq.tilt.enabled = true;
    out.eq.tilt.tilt_db = floatProperty(eq, "tiltDb", 0.0f);
    out.eq.tilt.pivot_hz = floatProperty(eq, "pivotHz", 1000.0f);
  }

  val dynamics = objectProperty(config, "dynamics");
  if (hasProperty(dynamics, "compressor")) {
    val compressor = objectProperty(dynamics, "compressor");
    out.dynamics.compressor.enabled = true;
    auto& cc = out.dynamics.compressor.config;
    cc.threshold_db = floatProperty(compressor, "thresholdDb", cc.threshold_db);
    cc.ratio = floatProperty(compressor, "ratio", cc.ratio);
    cc.attack_ms = floatProperty(compressor, "attackMs", cc.attack_ms);
    cc.release_ms = floatProperty(compressor, "releaseMs", cc.release_ms);
    cc.knee_db = floatProperty(compressor, "kneeDb", cc.knee_db);
    cc.makeup_gain_db = floatProperty(compressor, "makeupGainDb", cc.makeup_gain_db);
    cc.auto_makeup = boolProperty(compressor, "autoMakeup", cc.auto_makeup);
  }
  if (hasProperty(dynamics, "deesser")) {
    val deesser = objectProperty(dynamics, "deesser");
    out.dynamics.deesser.enabled = true;
    auto& dc = out.dynamics.deesser.config;
    dc.frequency_hz = floatProperty(deesser, "frequencyHz", dc.frequency_hz);
    dc.threshold_db = floatProperty(deesser, "thresholdDb", dc.threshold_db);
    dc.ratio = floatProperty(deesser, "ratio", dc.ratio);
    dc.attack_ms = floatProperty(deesser, "attackMs", dc.attack_ms);
    dc.release_ms = floatProperty(deesser, "releaseMs", dc.release_ms);
    dc.range_db = floatProperty(deesser, "rangeDb", dc.range_db);
  }
  if (hasProperty(dynamics, "transientShaper")) {
    val ts = objectProperty(dynamics, "transientShaper");
    out.dynamics.transient_shaper.enabled = true;
    auto& tc = out.dynamics.transient_shaper.config;
    tc.attack_gain_db = floatProperty(ts, "attackGainDb", tc.attack_gain_db);
    tc.sustain_gain_db = floatProperty(ts, "sustainGainDb", tc.sustain_gain_db);
    tc.fast_attack_ms = floatProperty(ts, "fastAttackMs", tc.fast_attack_ms);
    tc.fast_release_ms = floatProperty(ts, "fastReleaseMs", tc.fast_release_ms);
    tc.slow_attack_ms = floatProperty(ts, "slowAttackMs", tc.slow_attack_ms);
    tc.slow_release_ms = floatProperty(ts, "slowReleaseMs", tc.slow_release_ms);
    tc.sensitivity = floatProperty(ts, "sensitivity", tc.sensitivity);
    tc.max_gain_db = floatProperty(ts, "maxGainDb", tc.max_gain_db);
    tc.gain_smoothing_ms = floatProperty(ts, "gainSmoothingMs", tc.gain_smoothing_ms);
    tc.lookahead_ms = floatProperty(ts, "lookaheadMs", tc.lookahead_ms);
  }
  if (hasProperty(dynamics, "multibandComp")) {
    val mb = objectProperty(dynamics, "multibandComp");
    out.dynamics.multiband_comp.enabled = true;
    auto& mc = out.dynamics.multiband_comp.config;
    // Default config has 2 cutoffs ([120,2000]) and 3 bands; we update in place.
    if (hasProperty(mb, "lowCutoffHz")) {
      mc.crossover.cutoffs_hz[0] = floatProperty(mb, "lowCutoffHz", mc.crossover.cutoffs_hz[0]);
    }
    if (hasProperty(mb, "highCutoffHz")) {
      mc.crossover.cutoffs_hz[1] = floatProperty(mb, "highCutoffHz", mc.crossover.cutoffs_hz[1]);
    }
    auto apply_band = [&](int idx, const char* prefix_threshold, const char* prefix_ratio,
                          const char* prefix_attack, const char* prefix_release) {
      auto& band = mc.bands[idx];
      band.threshold_db = floatProperty(mb, prefix_threshold, band.threshold_db);
      band.ratio = floatProperty(mb, prefix_ratio, band.ratio);
      band.attack_ms = floatProperty(mb, prefix_attack, band.attack_ms);
      band.release_ms = floatProperty(mb, prefix_release, band.release_ms);
    };
    apply_band(0, "lowThresholdDb", "lowRatio", "lowAttackMs", "lowReleaseMs");
    apply_band(1, "midThresholdDb", "midRatio", "midAttackMs", "midReleaseMs");
    apply_band(2, "highThresholdDb", "highRatio", "highAttackMs", "highReleaseMs");
  }

  val saturation = objectProperty(config, "saturation");
  if (hasProperty(saturation, "tape")) {
    val tape = objectProperty(saturation, "tape");
    out.saturation.tape.enabled = true;
    auto& tc = out.saturation.tape.config;
    tc.drive_db = floatProperty(tape, "driveDb", tc.drive_db);
    tc.saturation = floatProperty(tape, "saturation", tc.saturation);
    tc.hysteresis = floatProperty(tape, "hysteresis", tc.hysteresis);
    tc.output_gain_db = floatProperty(tape, "outputGainDb", tc.output_gain_db);
    tc.speed_ips = floatProperty(tape, "speedIps", tc.speed_ips);
    tc.head_bump_db = floatProperty(tape, "headBumpDb", tc.head_bump_db);
    tc.bias = floatProperty(tape, "bias", tc.bias);
    tc.gap_loss = floatProperty(tape, "gapLoss", tc.gap_loss);
  }
  if (hasProperty(saturation, "exciter")) {
    val exciter = objectProperty(saturation, "exciter");
    out.saturation.exciter.enabled = true;
    auto& ec = out.saturation.exciter.config;
    ec.frequency_hz = floatProperty(exciter, "frequencyHz", ec.frequency_hz);
    ec.drive_db = floatProperty(exciter, "driveDb", ec.drive_db);
    ec.amount = floatProperty(exciter, "amount", ec.amount);
    ec.q = floatProperty(exciter, "q", ec.q);
    ec.even_odd_mix = floatProperty(exciter, "evenOddMix", ec.even_odd_mix);
  }

  val spectral = objectProperty(config, "spectral");
  if (hasProperty(spectral, "airBand")) {
    val air_band = objectProperty(spectral, "airBand");
    out.spectral.air_band.enabled = true;
    auto& ac = out.spectral.air_band.config;
    ac.amount = floatProperty(air_band, "amount", ac.amount);
    ac.shelf_frequency_hz = floatProperty(air_band, "shelfFrequencyHz", ac.shelf_frequency_hz);
    ac.dynamic_threshold_db =
        floatProperty(air_band, "dynamicThresholdDb", ac.dynamic_threshold_db);
    ac.dynamic_range_db = floatProperty(air_band, "dynamicRangeDb", ac.dynamic_range_db);
  }

  val stereo = objectProperty(config, "stereo");
  if (hasProperty(stereo, "imager")) {
    val imager = objectProperty(stereo, "imager");
    out.stereo.imager.enabled = true;
    auto& ic = out.stereo.imager.config;
    ic.width = floatProperty(imager, "width", ic.width);
    ic.output_gain_db = floatProperty(imager, "outputGainDb", ic.output_gain_db);
    ic.decorrelation_amount = floatProperty(imager, "decorrelationAmount", ic.decorrelation_amount);
    ic.preserve_energy = boolProperty(imager, "preserveEnergy", ic.preserve_energy);
  }
  if (hasProperty(stereo, "monoMaker")) {
    val mono_maker = objectProperty(stereo, "monoMaker");
    out.stereo.mono_maker.enabled = true;
    out.stereo.mono_maker.config.amount =
        floatProperty(mono_maker, "amount", out.stereo.mono_maker.config.amount);
  }

  val maximizer = objectProperty(config, "maximizer");
  if (hasProperty(maximizer, "truePeakLimiter")) {
    val limiter = objectProperty(maximizer, "truePeakLimiter");
    out.maximizer.true_peak_limiter.enabled = true;
    auto& lc = out.maximizer.true_peak_limiter.config;
    lc.ceiling_db = floatProperty(limiter, "ceilingDb", lc.ceiling_db);
    lc.lookahead_ms = floatProperty(limiter, "lookaheadMs", lc.lookahead_ms);
    lc.release_ms = floatProperty(limiter, "releaseMs", lc.release_ms);
    lc.oversample_factor = intProperty(limiter, "oversampleFactor", lc.oversample_factor);
    lc.apply_gain_at_input_rate =
        boolProperty(limiter, "applyGainAtInputRate", lc.apply_gain_at_input_rate);
  }

  val loudness = objectProperty(config, "loudness");
  if (!loudness.isUndefined() && !loudness.isNull()) {
    out.loudness.enabled = true;
    out.loudness.target_lufs = floatProperty(loudness, "targetLufs", out.loudness.target_lufs);
    out.loudness.ceiling_db = floatProperty(loudness, "ceilingDb", out.loudness.ceiling_db);
    out.loudness.true_peak_oversample =
        intProperty(loudness, "truePeakOversample", out.loudness.true_peak_oversample);
    out.loudness.release_ms = floatProperty(loudness, "releaseMs", out.loudness.release_ms);
    out.loudness.apply_gain_at_input_rate =
        boolProperty(loudness, "applyGainAtInputRate", out.loudness.apply_gain_at_input_rate);
  }

  return out;
}

val js_mastering_chain(val samples, int sample_rate, val config) {
  std::vector<float> data = float32ArrayToVector(samples);
  mastering::api::MasteringChain chain(masteringChainConfigFromVal(config));
  auto result = chain.process_mono(data.data(), data.size(), sample_rate);

  val out = val::object();
  out.set("samples", vectorToFloat32Array(result.samples));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  val stages = val::array();
  for (const auto& s : result.stages) {
    stages.call<void>("push", s);
  }
  out.set("stages", stages);
  return out;
}

val js_mastering_chain_stereo(val left_samples, val right_samples, int sample_rate, val config) {
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  if (left.size() != right.size()) {
    throw std::invalid_argument("stereo channel lengths must match");
  }

  mastering::api::MasteringChain chain(masteringChainConfigFromVal(config));
  auto result = chain.process_stereo(left.data(), right.data(), left.size(), sample_rate);

  val out = val::object();
  out.set("left", vectorToFloat32Array(result.left));
  out.set("right", vectorToFloat32Array(result.right));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  val stages = val::array();
  for (const auto& s : result.stages) {
    stages.call<void>("push", s);
  }
  out.set("stages", stages);
  return out;
}

// Mastering chain (mono) with progress callback
val js_mastering_chain_with_progress(val samples, int sample_rate, val config,
                                     val progress_callback) {
  std::vector<float> data = float32ArrayToVector(samples);
  mastering::api::MasteringChain chain(masteringChainConfigFromVal(config));
  if (!progress_callback.isNull() && !progress_callback.isUndefined()) {
    chain.set_progress_callback([progress_callback](float progress, const char* stage) {
      progress_callback(progress, std::string(stage));
    });
  }
  auto result = chain.process_mono(data.data(), data.size(), sample_rate);

  val out = val::object();
  out.set("samples", vectorToFloat32Array(result.samples));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  val stages = val::array();
  for (const auto& s : result.stages) {
    stages.call<void>("push", s);
  }
  out.set("stages", stages);
  return out;
}

// Mastering chain (stereo) with progress callback
val js_mastering_chain_stereo_with_progress(val left_samples, val right_samples, int sample_rate,
                                            val config, val progress_callback) {
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  if (left.size() != right.size()) {
    throw std::invalid_argument("stereo channel lengths must match");
  }

  mastering::api::MasteringChain chain(masteringChainConfigFromVal(config));
  if (!progress_callback.isNull() && !progress_callback.isUndefined()) {
    chain.set_progress_callback([progress_callback](float progress, const char* stage) {
      progress_callback(progress, std::string(stage));
    });
  }
  auto result = chain.process_stereo(left.data(), right.data(), left.size(), sample_rate);

  val out = val::object();
  out.set("left", vectorToFloat32Array(result.left));
  out.set("right", vectorToFloat32Array(result.right));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  val stages = val::array();
  for (const auto& s : result.stages) {
    stages.call<void>("push", s);
  }
  out.set("stages", stages);
  return out;
}

// ---------------------------------------------------------------------------
// StreamingMasteringChain wrapper (block-by-block streaming).
// Construct via createStreamingMasteringChain(config) factory. Throws if the
// configuration enables non-streaming stages (repair.denoise, loudness).
// ---------------------------------------------------------------------------

class StreamingMasteringChainWrapper {
 public:
  explicit StreamingMasteringChainWrapper(val config)
      : chain_(masteringChainConfigFromVal(config)) {}

  void prepare(double sample_rate, int max_block_size, int num_channels) {
    chain_.prepare(sample_rate, max_block_size, num_channels);
  }

  val processMono(val samples) {
    std::vector<float> block = float32ArrayToVector(samples);
    if (!block.empty()) {
      float* channels[] = {block.data()};
      chain_.process_block(channels, 1, static_cast<int>(block.size()));
    }
    return vectorToFloat32Array(block);
  }

  val processStereo(val left_samples, val right_samples) {
    std::vector<float> left = float32ArrayToVector(left_samples);
    std::vector<float> right = float32ArrayToVector(right_samples);
    if (left.size() != right.size()) {
      throw std::invalid_argument("stereo channel lengths must match");
    }
    if (!left.empty()) {
      float* channels[] = {left.data(), right.data()};
      chain_.process_block(channels, 2, static_cast<int>(left.size()));
    }
    val out = val::object();
    out.set("left", vectorToFloat32Array(left));
    out.set("right", vectorToFloat32Array(right));
    return out;
  }

  void reset() { chain_.reset(); }

  int latencySamples() const { return chain_.latency_samples(); }

  val stageNames() const {
    val out = val::array();
    for (const auto& name : chain_.stage_names()) {
      out.call<void>("push", name);
    }
    return out;
  }

 private:
  mastering::api::StreamingMasteringChain chain_;
};

StreamingMasteringChainWrapper* createStreamingMasteringChain(val config) {
  return new StreamingMasteringChainWrapper(config);
}

val js_mastering_processor_names() {
  val out = val::array();
  auto names = mastering::api::processor_names();
  for (size_t index = 0; index < names.size(); ++index) {
    out.call<void>("push", names[index]);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Mastering presets (high-level master_audio API).
// TODO: Support flat / nested overrides objects. For now only null/undefined
// overrides are accepted; a non-null overrides argument throws.
// ---------------------------------------------------------------------------

val js_mastering_preset_names() {
  val out = val::array();
  auto names = mastering::api::preset_names();
  for (const auto& name : names) {
    out.call<void>("push", name);
  }
  return out;
}

val js_master_audio(std::string preset_name, val samples, int sample_rate, val overrides) {
  if (!overrides.isNull() && !overrides.isUndefined()) {
    // TODO: convert overrides val into a flat Param vector and pass it to
    // mastering::api::master_audio_mono(); see masteringChainConfigFromVal for
    // the existing dispatch shape.
    throw std::invalid_argument("masterAudio overrides are not yet supported; pass null");
  }
  std::vector<float> data = float32ArrayToVector(samples);
  auto preset = mastering::api::preset_from_string(preset_name);
  auto result = mastering::api::master_audio_mono(preset, data.data(), data.size(), sample_rate);

  val out = val::object();
  out.set("samples", vectorToFloat32Array(result.samples));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  val stages = val::array();
  for (const auto& s : result.stages) {
    stages.call<void>("push", s);
  }
  out.set("stages", stages);
  return out;
}

val js_master_audio_stereo(std::string preset_name, val left_samples, val right_samples,
                           int sample_rate, val overrides) {
  if (!overrides.isNull() && !overrides.isUndefined()) {
    // TODO: see js_master_audio above.
    throw std::invalid_argument("masterAudioStereo overrides are not yet supported; pass null");
  }
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  if (left.size() != right.size()) {
    throw std::invalid_argument("stereo channel lengths must match");
  }
  auto preset = mastering::api::preset_from_string(preset_name);
  auto result = mastering::api::master_audio_stereo(preset, left.data(), right.data(), left.size(),
                                                    sample_rate);

  val out = val::object();
  out.set("left", vectorToFloat32Array(result.left));
  out.set("right", vectorToFloat32Array(result.right));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  val stages = val::array();
  for (const auto& s : result.stages) {
    stages.call<void>("push", s);
  }
  out.set("stages", stages);
  return out;
}

val js_mastering_pair_processor_names() {
  val out = val::array();
  auto names = mastering::api::pair_processor_names();
  for (size_t index = 0; index < names.size(); ++index) {
    out.call<void>("push", names[index]);
  }
  return out;
}

val js_mastering_pair_analysis_names() {
  val out = val::array();
  auto names = mastering::api::pair_analysis_names();
  for (size_t index = 0; index < names.size(); ++index) {
    out.call<void>("push", names[index]);
  }
  return out;
}

val js_mastering_stereo_analysis_names() {
  val out = val::array();
  auto names = mastering::api::stereo_analysis_names();
  for (size_t index = 0; index < names.size(); ++index) {
    out.call<void>("push", names[index]);
  }
  return out;
}

val js_mastering_process(std::string processor_name, val samples, int sample_rate, val params) {
  std::vector<float> data = float32ArrayToVector(samples);
  auto result = mastering::api::apply_named_processor(
      processor_name, data.data(), data.size(), sample_rate, masteringParamsFromObject(params));
  val out = val::object();
  out.set("samples", vectorToFloat32Array(result.samples));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  out.set("latencySamples", result.latency_samples);
  return out;
}

val js_mastering_process_stereo(std::string processor_name, val left_samples, val right_samples,
                                int sample_rate, val params) {
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  auto result = mastering::api::apply_named_processor_stereo(processor_name, left.data(),
                                                             right.data(), left.size(), sample_rate,
                                                             masteringParamsFromObject(params));
  val out = val::object();
  out.set("left", vectorToFloat32Array(result.left));
  out.set("right", vectorToFloat32Array(result.right));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  out.set("latencySamples", result.latency_samples);
  return out;
}

val js_mastering_pair_process(std::string processor_name, val source_samples, val reference_samples,
                              int sample_rate, val params) {
  std::vector<float> source = float32ArrayToVector(source_samples);
  std::vector<float> reference = float32ArrayToVector(reference_samples);
  auto result = mastering::api::apply_named_pair_processor(
      processor_name, source.data(), reference.data(), source.size(), sample_rate,
      masteringParamsFromObject(params));
  val out = val::object();
  out.set("samples", vectorToFloat32Array(result.samples));
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  out.set("latencySamples", result.latency_samples);
  return out;
}

std::string js_mastering_pair_analyze(std::string analysis_name, val source_samples,
                                      val reference_samples, int sample_rate, val params) {
  std::vector<float> source = float32ArrayToVector(source_samples);
  std::vector<float> reference = float32ArrayToVector(reference_samples);
  return mastering::api::analyze_named_pair(analysis_name, source.data(), reference.data(),
                                            source.size(), sample_rate,
                                            masteringParamsFromObject(params));
}

std::string js_mastering_stereo_analyze(std::string analysis_name, val left_samples,
                                        val right_samples, int sample_rate, val params) {
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  return mastering::api::analyze_named_stereo(analysis_name, left.data(), right.data(), left.size(),
                                              sample_rate, masteringParamsFromObject(params));
}

// ============================================================================
// Features - Spectrogram
// ============================================================================

val js_stft(val samples, int sample_rate, int n_fft, int hop_length) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);

  val out = val::object();
  out.set("nBins", spec.n_bins());
  out.set("nFrames", spec.n_frames());
  out.set("nFft", spec.n_fft());
  out.set("hopLength", spec.hop_length());
  out.set("sampleRate", spec.sample_rate());
  out.set("magnitude", vectorToFloat32Array(spec.magnitude()));
  out.set("power", vectorToFloat32Array(spec.power()));

  return out;
}

val js_stft_db(val samples, int sample_rate, int n_fft, int hop_length) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);

  val out = val::object();
  out.set("nBins", spec.n_bins());
  out.set("nFrames", spec.n_frames());
  out.set("db", vectorToFloat32Array(spec.to_db()));

  return out;
}

// ============================================================================
// Features - Mel Spectrogram
// ============================================================================

val js_mel_spectrogram(val samples, int sample_rate, int n_fft, int hop_length, int n_mels) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;

  MelSpectrogram mel = MelSpectrogram::compute(audio, config);

  val out = val::object();
  out.set("nMels", mel.n_mels());
  out.set("nFrames", mel.n_frames());
  out.set("sampleRate", mel.sample_rate());
  out.set("hopLength", mel.hop_length());

  // Power values
  std::vector<float> power_vec(mel.power_data(), mel.power_data() + mel.n_mels() * mel.n_frames());
  out.set("power", vectorToFloat32Array(power_vec));

  // dB values
  out.set("db", vectorToFloat32Array(mel.to_db()));

  return out;
}

val js_mfcc(val samples, int sample_rate, int n_fft, int hop_length, int n_mels, int n_mfcc) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;

  MelSpectrogram mel = MelSpectrogram::compute(audio, config);
  std::vector<float> mfcc = mel.mfcc(n_mfcc);

  val out = val::object();
  out.set("nMfcc", n_mfcc);
  out.set("nFrames", mel.n_frames());
  out.set("coefficients", vectorToFloat32Array(mfcc));

  return out;
}

// ============================================================================
// Features - Chroma
// ============================================================================

val js_chroma(val samples, int sample_rate, int n_fft, int hop_length) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  ChromaConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Chroma chroma = Chroma::compute(audio, config);

  val out = val::object();
  out.set("nChroma", chroma.n_chroma());
  out.set("nFrames", chroma.n_frames());
  out.set("sampleRate", chroma.sample_rate());
  out.set("hopLength", chroma.hop_length());

  std::vector<float> features_vec(chroma.data(),
                                  chroma.data() + chroma.n_chroma() * chroma.n_frames());
  out.set("features", vectorToFloat32Array(features_vec));

  // Mean energy per pitch class
  auto mean = chroma.mean_energy();
  val mean_arr = val::array();
  for (int i = 0; i < 12; ++i) {
    mean_arr.call<void>("push", mean[i]);
  }
  out.set("meanEnergy", mean_arr);

  return out;
}

// ============================================================================
// Features - Spectral
// ============================================================================

val js_spectral_centroid(val samples, int sample_rate, int n_fft, int hop_length) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> centroid = spectral_centroid(spec, sample_rate);

  return vectorToFloat32Array(centroid);
}

val js_spectral_bandwidth(val samples, int sample_rate, int n_fft, int hop_length) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> bandwidth = spectral_bandwidth(spec, sample_rate);

  return vectorToFloat32Array(bandwidth);
}

val js_spectral_rolloff(val samples, int sample_rate, int n_fft, int hop_length,
                        float roll_percent) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> rolloff = spectral_rolloff(spec, sample_rate, roll_percent);

  return vectorToFloat32Array(rolloff);
}

val js_spectral_flatness(val samples, int sample_rate, int n_fft, int hop_length) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> flatness = spectral_flatness(spec);

  return vectorToFloat32Array(flatness);
}

val js_zero_crossing_rate(val samples, int sample_rate, int frame_length, int hop_length) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  std::vector<float> zcr = zero_crossing_rate(audio, frame_length, hop_length);
  return vectorToFloat32Array(zcr);
}

val js_rms_energy(val samples, int sample_rate, int frame_length, int hop_length) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  std::vector<float> rms = rms_energy(audio, frame_length, hop_length);
  return vectorToFloat32Array(rms);
}

// ============================================================================
// Features - Pitch
// ============================================================================

val js_pitch_yin(val samples, int sample_rate, int frame_length, int hop_length, float fmin,
                 float fmax, float threshold) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  PitchConfig config;
  config.frame_length = frame_length;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  config.threshold = threshold;

  PitchResult result = yin_track(audio, config);

  val out = val::object();
  out.set("f0", vectorToFloat32Array(result.f0));

  // Convert voiced_prob to Float32Array
  out.set("voicedProb", vectorToFloat32Array(result.voiced_prob));

  // Convert voiced_flag to array of bools.
  // std::vector<bool>::operator[] returns a __bit_reference proxy that embind
  // cannot marshal, so we cast to bool explicitly.
  val voiced_arr = val::array();
  for (size_t i = 0; i < result.voiced_flag.size(); ++i) {
    voiced_arr.call<void>("push", static_cast<bool>(result.voiced_flag[i]));
  }
  out.set("voicedFlag", voiced_arr);

  out.set("nFrames", result.n_frames());
  out.set("medianF0", result.median_f0());
  out.set("meanF0", result.mean_f0());

  return out;
}

val js_pitch_pyin(val samples, int sample_rate, int frame_length, int hop_length, float fmin,
                  float fmax, float threshold) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  PitchConfig config;
  config.frame_length = frame_length;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  config.threshold = threshold;

  PitchResult result = pyin(audio, config);

  val out = val::object();
  out.set("f0", vectorToFloat32Array(result.f0));
  out.set("voicedProb", vectorToFloat32Array(result.voiced_prob));

  val voiced_arr = val::array();
  for (size_t i = 0; i < result.voiced_flag.size(); ++i) {
    voiced_arr.call<void>("push", static_cast<bool>(result.voiced_flag[i]));
  }
  out.set("voicedFlag", voiced_arr);

  out.set("nFrames", result.n_frames());
  out.set("medianF0", result.median_f0());
  out.set("meanF0", result.mean_f0());

  return out;
}

// ============================================================================
// Core - Conversion
// ============================================================================

float js_hz_to_mel(float hz) { return hz_to_mel(hz); }
float js_mel_to_hz(float mel) { return mel_to_hz(mel); }
float js_hz_to_midi(float hz) { return hz_to_midi(hz); }
float js_midi_to_hz(float midi) { return midi_to_hz(midi); }
std::string js_hz_to_note(float hz) { return hz_to_note(hz); }
float js_note_to_hz(const std::string& note) { return note_to_hz(note); }
float js_frames_to_time(int frames, int sr, int hop_length) {
  return frames_to_time(frames, sr, hop_length);
}
int js_time_to_frames(float time, int sr, int hop_length) {
  return time_to_frames(time, sr, hop_length);
}
int js_frames_to_samples(int frames, int hop_length, int n_fft) {
  return frames_to_samples(frames, hop_length, n_fft);
}
int js_samples_to_frames(int samples, int hop_length, int n_fft) {
  return samples_to_frames(samples, hop_length, n_fft);
}

val js_power_to_db(val values, float ref, float amin, float top_db) {
  std::vector<float> data = float32ArrayToVector(values);
  return vectorToFloat32Array(power_to_db(data, ref, amin, top_db));
}

val js_amplitude_to_db(val values, float ref, float amin, float top_db) {
  std::vector<float> data = float32ArrayToVector(values);
  return vectorToFloat32Array(amplitude_to_db(data, ref, amin, top_db));
}

val js_db_to_power(val values, float ref) {
  std::vector<float> data = float32ArrayToVector(values);
  return vectorToFloat32Array(db_to_power(data, ref));
}

val js_db_to_amplitude(val values, float ref) {
  std::vector<float> data = float32ArrayToVector(values);
  return vectorToFloat32Array(db_to_amplitude(data, ref));
}

val js_preemphasis(val samples, float coef, val zi) {
  std::vector<float> data = float32ArrayToVector(samples);
  if (zi.isUndefined() || zi.isNull()) {
    return vectorToFloat32Array(preemphasis(data, coef));
  }
  return vectorToFloat32Array(preemphasis(data, coef, zi.as<float>()));
}

val js_deemphasis(val samples, float coef, val zi) {
  std::vector<float> data = float32ArrayToVector(samples);
  if (zi.isUndefined() || zi.isNull()) {
    return vectorToFloat32Array(deemphasis(data, coef));
  }
  return vectorToFloat32Array(deemphasis(data, coef, zi.as<float>()));
}

val js_trim_silence(val samples, float top_db, int frame_length, int hop_length) {
  std::vector<float> data = float32ArrayToVector(samples);
  auto result = trim(data, top_db, frame_length, hop_length);
  val out = val::object();
  out.set("audio", vectorToFloat32Array(result.audio));
  out.set("startSample", result.start_sample);
  out.set("endSample", result.end_sample);
  return out;
}

val js_split_silence(val samples, float top_db, int frame_length, int hop_length) {
  std::vector<float> data = float32ArrayToVector(samples);
  auto ranges = split(data, top_db, frame_length, hop_length);
  std::vector<int> flat;
  flat.reserve(ranges.size() * 2);
  for (const auto& range : ranges) {
    flat.push_back(range.first);
    flat.push_back(range.second);
  }
  return vectorToInt32Array(flat);
}

val js_frame_signal(val samples, int frame_length, int hop_length) {
  std::vector<float> data = float32ArrayToVector(samples);
  val out = val::object();
  out.set("nFrames", frame_count(data.size(), frame_length, hop_length));
  out.set("frames", vectorToFloat32Array(frame(data, frame_length, hop_length)));
  return out;
}

val js_pad_center(val values, int size, float pad_value) {
  std::vector<float> data = float32ArrayToVector(values);
  return vectorToFloat32Array(pad_center(data, static_cast<size_t>(size), pad_value));
}

val js_fix_length(val values, int size, float pad_value) {
  std::vector<float> data = float32ArrayToVector(values);
  return vectorToFloat32Array(fix_length(data, static_cast<size_t>(size), pad_value));
}

std::vector<int> intArrayToVector(val arr) {
  const int length = arr["length"].as<int>();
  std::vector<int> out(static_cast<size_t>(length));
  for (int index = 0; index < length; ++index) {
    out[static_cast<size_t>(index)] = arr[index].as<int>();
  }
  return out;
}

val js_fix_frames(val frames, int x_min, int x_max, bool pad) {
  return vectorToInt32Array(fix_frames(intArrayToVector(frames), x_min, x_max, pad));
}

val js_peak_pick(val values, int pre_max, int post_max, int pre_avg, int post_avg, float delta,
                 int wait) {
  std::vector<float> data = float32ArrayToVector(values);
  return vectorToInt32Array(peak_pick(data, pre_max, post_max, pre_avg, post_avg, delta, wait));
}

val js_vector_normalize(val values, int norm_type, float threshold) {
  std::vector<float> data = float32ArrayToVector(values);
  NormType norm = NormType::Inf;
  if (norm_type == 1) norm = NormType::L1;
  if (norm_type == 2) norm = NormType::L2;
  if (norm_type == 3) norm = NormType::Power;
  return vectorToFloat32Array(normalize(data, norm, threshold));
}

val js_pcen(val values, int n_bins, int n_frames, val options) {
  std::vector<float> data = float32ArrayToVector(values);
  PcenConfig config;
  if (!options.isUndefined() && !options.isNull()) {
    config.sr = intProperty(options, "sampleRate", config.sr);
    config.hop_length = intProperty(options, "hopLength", config.hop_length);
    config.time_constant = floatProperty(options, "timeConstant", config.time_constant);
    config.gain = floatProperty(options, "gain", config.gain);
    config.bias = floatProperty(options, "bias", config.bias);
    config.power = floatProperty(options, "power", config.power);
    config.eps = floatProperty(options, "eps", config.eps);
  }
  return vectorToFloat32Array(pcen(data, n_bins, n_frames, config));
}

val js_tonnetz(val chromagram, int n_chroma, int n_frames) {
  std::vector<float> data = float32ArrayToVector(chromagram);
  return vectorToFloat32Array(tonnetz(data.data(), n_chroma, n_frames));
}

val js_tempogram(val onset_envelope, int sample_rate, int hop_length, int win_length) {
  std::vector<float> data = float32ArrayToVector(onset_envelope);
  TempogramConfig config;
  config.hop_length = hop_length;
  config.win_length = win_length;
  auto result = tempogram(data, sample_rate, config);
  val out = val::object();
  out.set("nFrames", static_cast<int>(data.size()));
  out.set("winLength", win_length);
  out.set("data", vectorToFloat32Array(result));
  return out;
}

val js_plp(val onset_envelope, int sample_rate, int hop_length, float tempo_min, float tempo_max,
           int win_length) {
  std::vector<float> data = float32ArrayToVector(onset_envelope);
  PlpConfig config;
  config.sr = sample_rate;
  config.hop_length = hop_length;
  config.tempo_min = tempo_min;
  config.tempo_max = tempo_max;
  config.win_length = win_length;
  return vectorToFloat32Array(plp(data, config));
}

// ============================================================================
// Core - Resample
// ============================================================================

val js_resample(val samples, int src_sr, int target_sr) {
  std::vector<float> data = float32ArrayToVector(samples);
  std::vector<float> result = resample(data.data(), data.size(), src_sr, target_sr);
  return vectorToFloat32Array(result);
}

// ============================================================================
// Streaming - StreamAnalyzer
// ============================================================================

/// @brief Helper to convert uint8 vector to Uint8Array.
val vectorToUint8Array(const std::vector<uint8_t>& vec) {
  val result = val::global("Uint8Array").new_(vec.size());
  for (size_t i = 0; i < vec.size(); ++i) {
    result.set(i, vec[i]);
  }
  return result;
}

/// @brief Helper to convert int16 vector to Int16Array.
val vectorToInt16Array(const std::vector<int16_t>& vec) {
  val result = val::global("Int16Array").new_(vec.size());
  for (size_t i = 0; i < vec.size(); ++i) {
    result.set(i, vec[i]);
  }
  return result;
}

/// @brief JavaScript wrapper for StreamAnalyzer.
class StreamAnalyzerWrapper {
 public:
  StreamAnalyzerWrapper(int sample_rate, int n_fft, int hop_length, int n_mels, bool compute_mel,
                        bool compute_chroma, bool compute_onset, int emit_every_n_frames) {
    StreamConfig config;
    config.sample_rate = sample_rate;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    config.n_mels = n_mels;
    config.compute_mel = compute_mel;
    config.compute_chroma = compute_chroma;
    config.compute_onset = compute_onset;
    config.emit_every_n_frames = emit_every_n_frames;
    config_ = config;
    analyzer_ = std::make_unique<StreamAnalyzer>(config);
  }

  /// @brief Returns the sample rate.
  int sampleRate() const { return config_.sample_rate; }

  void process(val samples) {
    std::vector<float> data = vecFromJSArray<float>(samples);
    analyzer_->process(data.data(), data.size());
  }

  void processWithOffset(val samples, size_t sample_offset) {
    std::vector<float> data = vecFromJSArray<float>(samples);
    analyzer_->process(data.data(), data.size(), sample_offset);
  }

  size_t availableFrames() const { return analyzer_->available_frames(); }

  /// @brief Reads frames in Float32 SOA format.
  val readFramesSoa(size_t max_frames) {
    FrameBuffer buffer;
    analyzer_->read_frames_soa(max_frames, buffer);

    val out = val::object();
    out.set("nFrames", buffer.n_frames);
    out.set("timestamps", vectorToFloat32Array(buffer.timestamps));
    out.set("mel", vectorToFloat32Array(buffer.mel));
    out.set("chroma", vectorToFloat32Array(buffer.chroma));
    out.set("onsetStrength", vectorToFloat32Array(buffer.onset_strength));
    out.set("rmsEnergy", vectorToFloat32Array(buffer.rms_energy));
    out.set("spectralCentroid", vectorToFloat32Array(buffer.spectral_centroid));
    out.set("spectralFlatness", vectorToFloat32Array(buffer.spectral_flatness));
    out.set("chordRoot", vectorToInt32Array(buffer.chord_root));
    out.set("chordQuality", vectorToInt32Array(buffer.chord_quality));
    out.set("chordConfidence", vectorToFloat32Array(buffer.chord_confidence));
    return out;
  }

  /// @brief Reads frames in quantized Uint8 format (4x bandwidth reduction).
  val readFramesU8(size_t max_frames) {
    QuantizedFrameBufferU8 buffer;
    QuantizeConfig qconfig;
    analyzer_->read_frames_quantized_u8(max_frames, buffer, qconfig);

    val out = val::object();
    out.set("nFrames", buffer.n_frames);
    out.set("nMels", buffer.n_mels);
    out.set("timestamps", vectorToFloat32Array(buffer.timestamps));
    out.set("mel", vectorToUint8Array(buffer.mel));
    out.set("chroma", vectorToUint8Array(buffer.chroma));
    out.set("onsetStrength", vectorToUint8Array(buffer.onset_strength));
    out.set("rmsEnergy", vectorToUint8Array(buffer.rms_energy));
    out.set("spectralCentroid", vectorToUint8Array(buffer.spectral_centroid));
    out.set("spectralFlatness", vectorToUint8Array(buffer.spectral_flatness));
    return out;
  }

  /// @brief Reads frames in quantized Int16 format (2x bandwidth reduction).
  val readFramesI16(size_t max_frames) {
    QuantizedFrameBufferI16 buffer;
    QuantizeConfig qconfig;
    analyzer_->read_frames_quantized_i16(max_frames, buffer, qconfig);

    val out = val::object();
    out.set("nFrames", buffer.n_frames);
    out.set("nMels", buffer.n_mels);
    out.set("timestamps", vectorToFloat32Array(buffer.timestamps));
    out.set("mel", vectorToInt16Array(buffer.mel));
    out.set("chroma", vectorToInt16Array(buffer.chroma));
    out.set("onsetStrength", vectorToInt16Array(buffer.onset_strength));
    out.set("rmsEnergy", vectorToInt16Array(buffer.rms_energy));
    out.set("spectralCentroid", vectorToInt16Array(buffer.spectral_centroid));
    out.set("spectralFlatness", vectorToInt16Array(buffer.spectral_flatness));
    return out;
  }

  void reset(size_t base_sample_offset) { analyzer_->reset(base_sample_offset); }

  val stats() {
    AnalyzerStats s = analyzer_->stats();

    val out = val::object();
    out.set("totalFrames", s.total_frames);
    out.set("totalSamples", static_cast<int>(s.total_samples));
    out.set("durationSeconds", s.duration_seconds);

    val estimate = val::object();
    estimate.set("bpm", s.estimate.bpm);
    estimate.set("bpmConfidence", s.estimate.bpm_confidence);
    estimate.set("bpmCandidateCount", s.estimate.bpm_candidate_count);
    estimate.set("key", s.estimate.key);
    estimate.set("keyMinor", s.estimate.key_minor);
    estimate.set("keyConfidence", s.estimate.key_confidence);
    estimate.set("chordRoot", s.estimate.chord_root);
    estimate.set("chordQuality", s.estimate.chord_quality);
    estimate.set("chordConfidence", s.estimate.chord_confidence);

    // Chord progression (time-based)
    val chordProgression = val::array();
    for (const auto& chord : s.estimate.chord_progression) {
      val c = val::object();
      c.set("root", chord.root);
      c.set("quality", chord.quality);
      c.set("startTime", chord.start_time);
      c.set("confidence", chord.confidence);
      chordProgression.call<void>("push", c);
    }
    estimate.set("chordProgression", chordProgression);

    // Bar-synchronized chord progression (requires stable BPM)
    val barChordProgression = val::array();
    for (const auto& chord : s.estimate.bar_chord_progression) {
      val c = val::object();
      c.set("barIndex", chord.bar_index);
      c.set("root", chord.root);
      c.set("quality", chord.quality);
      c.set("startTime", chord.start_time);
      c.set("confidence", chord.confidence);
      barChordProgression.call<void>("push", c);
    }
    estimate.set("barChordProgression", barChordProgression);
    estimate.set("currentBar", s.estimate.current_bar);
    estimate.set("barDuration", s.estimate.bar_duration);

    // Voted chord pattern (computed from repetitions)
    val votedPattern = val::array();
    for (const auto& chord : s.estimate.voted_pattern) {
      val c = val::object();
      c.set("barIndex", chord.bar_index);
      c.set("root", chord.root);
      c.set("quality", chord.quality);
      c.set("confidence", chord.confidence);
      votedPattern.call<void>("push", c);
    }
    estimate.set("votedPattern", votedPattern);
    estimate.set("patternLength", s.estimate.pattern_length);

    // Best matching progression pattern
    estimate.set("detectedPatternName", val(s.estimate.detected_pattern_name));
    estimate.set("detectedPatternScore", val(s.estimate.detected_pattern_score));

    // All pattern scores
    val allPatternScores = val::array();
    for (const auto& ps_pair : s.estimate.all_pattern_scores) {
      val ps = val::object();
      ps.set("name", ps_pair.first);
      ps.set("score", ps_pair.second);
      allPatternScores.call<void>("push", ps);
    }
    estimate.set("allPatternScores", allPatternScores);

    estimate.set("accumulatedSeconds", s.estimate.accumulated_seconds);
    estimate.set("usedFrames", s.estimate.used_frames);
    estimate.set("updated", s.estimate.updated);
    out.set("estimate", estimate);

    return out;
  }

  int frameCount() const { return analyzer_->frame_count(); }
  float currentTime() const { return analyzer_->current_time(); }

  /// @brief Sets the expected total duration for pattern lock timing.
  void setExpectedDuration(float duration_seconds) {
    analyzer_->set_expected_duration(duration_seconds);
  }

  /// @brief Sets normalization gain for loud audio.
  void setNormalizationGain(float gain) { analyzer_->set_normalization_gain(gain); }

  /// @brief Sets tuning reference frequency (A4).
  /// @param ref_hz Reference frequency for A4 (default 440 Hz)
  /// @details Use 466.16 if audio is 1 semitone sharp, 415.30 if 1 semitone flat.
  void setTuningRefHz(float ref_hz) { analyzer_->set_tuning_ref_hz(ref_hz); }

 private:
  StreamConfig config_;
  std::unique_ptr<StreamAnalyzer> analyzer_;
};

// ============================================================================
// Version
// ============================================================================

std::string js_version() { return SONARE_VERSION_STRING; }

// ============================================================================
// Embind Registrations
// ============================================================================

EMSCRIPTEN_BINDINGS(sonare) {
  // Enums
  enum_<PitchClass>("PitchClass")
      .value("C", PitchClass::C)
      .value("Cs", PitchClass::Cs)
      .value("D", PitchClass::D)
      .value("Ds", PitchClass::Ds)
      .value("E", PitchClass::E)
      .value("F", PitchClass::F)
      .value("Fs", PitchClass::Fs)
      .value("G", PitchClass::G)
      .value("Gs", PitchClass::Gs)
      .value("A", PitchClass::A)
      .value("As", PitchClass::As)
      .value("B", PitchClass::B);

  enum_<Mode>("Mode").value("Major", Mode::Major).value("Minor", Mode::Minor);

  enum_<ChordQuality>("ChordQuality")
      .value("Major", ChordQuality::Major)
      .value("Minor", ChordQuality::Minor)
      .value("Diminished", ChordQuality::Diminished)
      .value("Augmented", ChordQuality::Augmented)
      .value("Dominant7", ChordQuality::Dominant7)
      .value("Major7", ChordQuality::Major7)
      .value("Minor7", ChordQuality::Minor7)
      .value("Sus2", ChordQuality::Sus2)
      .value("Sus4", ChordQuality::Sus4);

  enum_<SectionType>("SectionType")
      .value("Intro", SectionType::Intro)
      .value("Verse", SectionType::Verse)
      .value("PreChorus", SectionType::PreChorus)
      .value("Chorus", SectionType::Chorus)
      .value("Bridge", SectionType::Bridge)
      .value("Instrumental", SectionType::Instrumental)
      .value("Outro", SectionType::Outro);

  // Quick API (high-level)
  function("detectBpm", &js_detect_bpm);
  function("detectKey", &js_detect_key);
  function("detectOnsets", &js_detect_onsets);
  function("detectBeats", &js_detect_beats);
  function("analyze", &js_analyze);
  function("analyzeWithProgress", &js_analyze_with_progress);
  function("version", &js_version);

  // Effects
  function("hpss", &js_hpss);
  function("harmonic", &js_harmonic);
  function("percussive", &js_percussive);
  function("timeStretch", &js_time_stretch);
  function("pitchShift", &js_pitch_shift);
  function("normalize", &js_normalize);
  function("mastering", &js_mastering);
  function("masteringProcessorNames", &js_mastering_processor_names);
  function("masteringPairProcessorNames", &js_mastering_pair_processor_names);
  function("masteringPairAnalysisNames", &js_mastering_pair_analysis_names);
  function("masteringStereoAnalysisNames", &js_mastering_stereo_analysis_names);
  function("masteringProcess", &js_mastering_process);
  function("masteringProcessStereo", &js_mastering_process_stereo);
  function("masteringPairProcess", &js_mastering_pair_process);
  function("masteringPairAnalyze", &js_mastering_pair_analyze);
  function("masteringStereoAnalyze", &js_mastering_stereo_analyze);
  function("masteringChain", &js_mastering_chain);
  function("masteringChainStereo", &js_mastering_chain_stereo);
  function("masteringChainWithProgress", &js_mastering_chain_with_progress);
  function("masteringChainStereoWithProgress", &js_mastering_chain_stereo_with_progress);
  function("masteringPresetNames", &js_mastering_preset_names);
  function("masterAudio", &js_master_audio);
  function("masterAudioStereo", &js_master_audio_stereo);
  function("trim", &js_trim);

  // Features - Spectrogram
  function("stft", &js_stft);
  function("stftDb", &js_stft_db);

  // Features - Mel Spectrogram
  function("melSpectrogram", &js_mel_spectrogram);
  function("mfcc", &js_mfcc);

  // Features - Chroma
  function("chroma", &js_chroma);

  // Features - Spectral
  function("spectralCentroid", &js_spectral_centroid);
  function("spectralBandwidth", &js_spectral_bandwidth);
  function("spectralRolloff", &js_spectral_rolloff);
  function("spectralFlatness", &js_spectral_flatness);
  function("zeroCrossingRate", &js_zero_crossing_rate);
  function("rmsEnergy", &js_rms_energy);

  // Features - Pitch
  function("pitchYin", &js_pitch_yin);
  function("pitchPyin", &js_pitch_pyin);

  // Core - Conversion
  function("hzToMel", &js_hz_to_mel);
  function("melToHz", &js_mel_to_hz);
  function("hzToMidi", &js_hz_to_midi);
  function("midiToHz", &js_midi_to_hz);
  function("hzToNote", &js_hz_to_note);
  function("noteToHz", &js_note_to_hz);
  function("framesToTime", &js_frames_to_time);
  function("timeToFrames", &js_time_to_frames);
  function("framesToSamples", &js_frames_to_samples);
  function("samplesToFrames", &js_samples_to_frames);
  function("powerToDb", &js_power_to_db);
  function("amplitudeToDb", &js_amplitude_to_db);
  function("dbToPower", &js_db_to_power);
  function("dbToAmplitude", &js_db_to_amplitude);
  function("preemphasis", &js_preemphasis);
  function("deemphasis", &js_deemphasis);
  function("trimSilence", &js_trim_silence);
  function("splitSilence", &js_split_silence);
  function("frameSignal", &js_frame_signal);
  function("padCenter", &js_pad_center);
  function("fixLength", &js_fix_length);
  function("fixFrames", &js_fix_frames);
  function("peakPick", &js_peak_pick);
  function("vectorNormalize", &js_vector_normalize);
  function("pcen", &js_pcen);
  function("tonnetz", &js_tonnetz);
  function("tempogram", &js_tempogram);
  function("plp", &js_plp);

  // Core - Resample
  function("resample", &js_resample);

  // Streaming - StreamingMasteringChain
  class_<StreamingMasteringChainWrapper>("StreamingMasteringChain")
      .function("prepare", &StreamingMasteringChainWrapper::prepare)
      .function("processMono", &StreamingMasteringChainWrapper::processMono)
      .function("processStereo", &StreamingMasteringChainWrapper::processStereo)
      .function("reset", &StreamingMasteringChainWrapper::reset)
      .function("latencySamples", &StreamingMasteringChainWrapper::latencySamples)
      .function("stageNames", &StreamingMasteringChainWrapper::stageNames);
  function("createStreamingMasteringChain", &createStreamingMasteringChain, allow_raw_pointers());

  // Streaming - StreamAnalyzer
  class_<StreamAnalyzerWrapper>("StreamAnalyzer")
      .constructor<int, int, int, int, bool, bool, bool, int>()
      .function("process", &StreamAnalyzerWrapper::process)
      .function("processWithOffset", &StreamAnalyzerWrapper::processWithOffset)
      .function("availableFrames", &StreamAnalyzerWrapper::availableFrames)
      .function("readFramesSoa", &StreamAnalyzerWrapper::readFramesSoa)
      .function("readFramesU8", &StreamAnalyzerWrapper::readFramesU8)
      .function("readFramesI16", &StreamAnalyzerWrapper::readFramesI16)
      .function("reset", &StreamAnalyzerWrapper::reset)
      .function("stats", &StreamAnalyzerWrapper::stats)
      .function("frameCount", &StreamAnalyzerWrapper::frameCount)
      .function("currentTime", &StreamAnalyzerWrapper::currentTime)
      .function("sampleRate", &StreamAnalyzerWrapper::sampleRate)
      .function("setExpectedDuration", &StreamAnalyzerWrapper::setExpectedDuration)
      .function("setNormalizationGain", &StreamAnalyzerWrapper::setNormalizationGain)
      .function("setTuningRefHz", &StreamAnalyzerWrapper::setTuningRefHz);
}

#endif  // __EMSCRIPTEN__
