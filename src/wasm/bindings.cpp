/// @file bindings.cpp
/// @brief Embind bindings for WebAssembly.

#ifdef __EMSCRIPTEN__

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "acoustic/material.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "analysis/acoustic_analyzer.h"
#include "analysis/beat_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/melody_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "analysis/rhythm_analyzer.h"
#include "analysis/room_estimator.h"
#include "analysis/section_analyzer.h"
#include "analysis/timbre_analyzer.h"
#include "automation/parameter.h"
#include "core/audio.h"
#include "core/convert.h"
#include "core/db_convert.h"
#include "core/pcen.h"
#include "core/resample.h"
#include "core/spectrum.h"
#include "editing/pitch_editor/note_editor.h"
#include "editing/pitch_editor/pitch_corrector.h"
#include "editing/pitch_editor/scale_quantizer.h"
#include "editing/voice_changer/realtime_voice_changer.h"
#include "editing/voice_changer/streaming_retune.h"
#include "editing/voice_changer/voice_changer.h"
#include "effects/acoustic/room_morph.h"
#include "effects/decompose.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/phase_vocoder.h"
#include "effects/pitch_shift.h"
#include "effects/preemphasis.h"
#include "effects/remix.h"
#include "effects/silence.h"
#include "effects/time_stretch.h"
#include "engine/realtime_engine.h"
#include "feature/chroma.h"
#include "feature/cqt.h"
#include "feature/inverse.h"
#include "feature/mel_spectrogram.h"
#include "feature/nnls_chroma.h"
#include "feature/onset.h"
#include "feature/pitch.h"
#include "feature/rhythm.h"
#include "feature/spectral.h"
#include "feature/tonnetz.h"
#include "feature/vqt.h"
#include "graph/graph.h"
#include "mastering/api/chain.h"
#include "mastering/api/internal_processor_runner.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/assistant/config_from_params.h"
#include "mastering/assistant/suggester.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/tilt.h"
#include "mastering/final/dither.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_spectrum.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/streaming_preview.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/tape.h"
#include "mastering/spectral/air_band.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/mono_maker.h"
#include "metering/basic.h"
#include "metering/clipping.h"
#include "metering/dynamic_range.h"
#include "metering/lufs.h"
#include "metering/normalize.h"
#include "metering/phase_scope.h"
#include "metering/spectrum.h"
#include "metering/stereo.h"
#include "metering/true_peak.h"
#include "mixing/api/presets.h"
#include "mixing/channel_strip.h"
#include "quick.h"
#include "rt/command.h"
#include "rt/gain_processor.h"
#include "rt/processor_base.h"
#include "sonare.h"
#include "sonare_c.h"
#include "streaming/stream_analyzer.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/frame.h"
#include "util/padding.h"
#include "util/peak.h"
#include "util/types.h"
#include "util/vector_normalize.h"

using namespace emscripten;
using namespace sonare;

// ============================================================================
// Helper functions
// ============================================================================

// ---------------------------------------------------------------------------
// Zero-copy / bulk-copy helpers for the JS ↔ C++ Float32Array boundary.
//
// The naïve embind path (`vecFromJSArray<float>` + `result.set(i, vec[i])`)
// performs one JS↔WASM boundary crossing per element, which is O(N) marshalling
// overhead — measurable at hundreds of microseconds per million samples.
//
// These helpers collapse the marshalling to a single bulk memcpy by wrapping
// the C++ buffer in a `Float32Array` view onto the WASM heap and using the
// JS-side `TypedArray.prototype.set(otherTypedArray)` fast path.
// ---------------------------------------------------------------------------

val vectorToFloat32Array(const std::vector<float>& vec) {
  const size_t n = vec.size();
  val result = val::global("Float32Array").new_(n);
  if (n == 0) return result;
  // Wrap the C++ vector data as a Float32Array view onto the WASM heap and
  // use JS-side TypedArray.set for a single bulk memcpy across the boundary.
  // The view is non-owning; ownership stays with `vec`. Because `result` is a
  // freshly-allocated, independent Float32Array, the caller owns the copy and
  // we drop the view immediately after the set() call.
  val view = val(typed_memory_view(n, vec.data()));
  result.call<void>("set", view);
  return result;
}

val vectorToInt32Array(const std::vector<int>& vec) {
  const size_t n = vec.size();
  val result = val::global("Int32Array").new_(n);
  if (n == 0) return result;
  val view = val(typed_memory_view(n, vec.data()));
  result.call<void>("set", view);
  return result;
}

val vectorToUint8Array(const std::vector<uint8_t>& vec);

// Bulk-copy a JS Float32Array (or any array-like with numeric `.length`) into
// a freshly-allocated std::vector<float>. The single boundary crossing is
// `view.set(arr)` inside JS land; the typed_memory_view wraps the destination
// vector's storage so no intermediate buffer is allocated.
std::vector<float> float32ArrayToVector(val arr) {
  const size_t n = arr["length"].as<size_t>();
  std::vector<float> result(n);
  if (n == 0) return result;
  // Build a Float32Array view onto the destination vector's storage. The view
  // is short-lived: we only keep it long enough to invoke set() before the
  // function returns and the view is dropped.
  val view = val(typed_memory_view(n, result.data()));
  view.call<void>("set", arr);
  return result;
}

// Int32 sibling of float32ArrayToVector. Used where a JS Int32Array carries
// integer sample indices (e.g. remix interval boundaries) that must not be
// round-tripped through float32 — values above 2^24 lose precision as float.
// The typed_memory_view<int32_t> wraps the destination vector's storage so the
// single boundary crossing (view.set(arr)) copies the raw 32-bit integers.
std::vector<int32_t> int32ArrayToVector(val arr) {
  const size_t n = arr["length"].as<size_t>();
  std::vector<int32_t> result(n);
  if (n == 0) return result;
  val view = val(typed_memory_view(n, result.data()));
  view.call<void>("set", arr);
  return result;
}

std::vector<uint8_t> uint8ArrayToVector(val arr) {
  const size_t n = arr["byteLength"].as<size_t>();
  std::vector<uint8_t> result(n);
  if (n == 0) return result;
  val view = val(typed_memory_view(n, result.data()));
  view.call<void>("set", arr);
  return result;
}

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

std::string stringProperty(val object, const char* key, const std::string& default_value) {
  val value = objectProperty(object, key);
  return value.isUndefined() ? default_value : value.as<std::string>();
}

std::vector<Mode> modesFromVal(val modes) {
  std::vector<Mode> out;
  if (modes.isUndefined() || modes.isNull()) {
    return out;
  }
  const int length = modes["length"].as<int>();
  out.reserve(static_cast<size_t>(length));
  for (int i = 0; i < length; ++i) {
    const int mode = modes[i].as<int>();
    if (mode < static_cast<int>(Mode::Major) || mode > static_cast<int>(Mode::Locrian)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "invalid key mode");
    }
    out.push_back(static_cast<Mode>(mode));
  }
  return out;
}

KeyProfileType keyProfileFromInt(int profile_type) {
  switch (profile_type) {
    case 0:
      return KeyProfileType::KrumhanslSchmuckler;
    case 1:
      return KeyProfileType::Temperley;
    case 2:
      return KeyProfileType::Shaath;
    case 3:
      return KeyProfileType::FaraldoEDMT;
    case 4:
      return KeyProfileType::FaraldoEDMA;
    case 5:
      return KeyProfileType::FaraldoEDMM;
    case 6:
      return KeyProfileType::BellmanBudge;
    default:
      return KeyProfileType::KrumhanslSchmuckler;
  }
}

// Module-local mono/stereo runners. They delegate to the shared latency-
// compensating helpers in `mastering::api::internal` so the WASM bridge,
// `MasteringChain`, and `apply_named_processor` all go through the same
// implementation and stay in sync.
void processMono(sonare::rt::ProcessorBase& processor, std::vector<float>& samples,
                 int sample_rate) {
  mastering::api::internal::run_processor_mono(processor, samples, sample_rate);
}

void processStereo(sonare::rt::ProcessorBase& processor, std::vector<float>& left,
                   std::vector<float>& right, int sample_rate) {
  mastering::api::internal::run_processor_stereo(processor, left, right, sample_rate);
}

float integratedLufs(const std::vector<float>& samples, int sample_rate) {
  Audio audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);
  return metering::lufs(audio).integrated_lufs;
}

val chordToVal(const Chord& chord_result) {
  val chord = val::object();
  chord.set("root", static_cast<int>(chord_result.root));
  chord.set("bass", static_cast<int>(chord_result.bass));
  chord.set("quality", static_cast<int>(chord_result.quality));
  chord.set("start", chord_result.start);
  chord.set("end", chord_result.end);
  chord.set("confidence", chord_result.confidence);
  chord.set("name", chord_result.to_string());
  return chord;
}

val chordsToVal(const std::vector<Chord>& chord_results) {
  val chords = val::array();
  for (size_t i = 0; i < chord_results.size(); ++i) {
    chords.call<void>("push", chordToVal(chord_results[i]));
  }
  return chords;
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
  out.set("chords", chordsToVal(result.chords));

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
  std::vector<float> data = float32ArrayToVector(samples);
  return quick::detect_bpm(data.data(), data.size(), sample_rate);
}

val js_detect_key(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  Key key = quick::detect_key(data.data(), data.size(), sample_rate);

  val result = val::object();
  result.set("root", static_cast<int>(key.root));
  result.set("mode", static_cast<int>(key.mode));
  result.set("confidence", key.confidence);
  result.set("name", key.to_string());
  result.set("shortName", key.to_short_string());
  return result;
}

val js_detect_key_with_options(val samples, int sample_rate, int n_fft, int hop_length,
                               bool use_hpss, bool loudness_weighted, float high_pass_hz, val modes,
                               int profile_type, std::string genre_hint) {
  std::vector<float> data = float32ArrayToVector(samples);
  KeyConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.use_hpss = use_hpss;
  config.loudness_weighted = loudness_weighted;
  config.high_pass_hz = high_pass_hz;
  config.modes = modesFromVal(modes);
  if (profile_type >= 0) {
    config.profile_type = keyProfileFromInt(profile_type);
  }
  if (!genre_hint.empty()) {
    config.genre_hint = genre_hint;
  }
  Key key = quick::detect_key(data.data(), data.size(), sample_rate, config);

  val result = val::object();
  result.set("root", static_cast<int>(key.root));
  result.set("mode", static_cast<int>(key.mode));
  result.set("confidence", key.confidence);
  result.set("name", key.to_string());
  result.set("shortName", key.to_short_string());
  return result;
}

val js_detect_key_candidates(val samples, int sample_rate, int n_fft, int hop_length, bool use_hpss,
                             bool loudness_weighted, float high_pass_hz, val modes,
                             int profile_type, std::string genre_hint) {
  std::vector<float> data = float32ArrayToVector(samples);
  KeyConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.use_hpss = use_hpss;
  config.loudness_weighted = loudness_weighted;
  config.high_pass_hz = high_pass_hz;
  config.modes = modesFromVal(modes);
  if (profile_type >= 0) {
    config.profile_type = keyProfileFromInt(profile_type);
  }
  if (!genre_hint.empty()) {
    config.genre_hint = genre_hint;
  }
  const auto candidates =
      quick::detect_key_candidates(data.data(), data.size(), sample_rate, config);
  val out = val::array();
  for (size_t i = 0; i < candidates.size(); ++i) {
    val candidate = val::object();
    val key = val::object();
    key.set("root", static_cast<int>(candidates[i].key.root));
    key.set("mode", static_cast<int>(candidates[i].key.mode));
    key.set("confidence", candidates[i].key.confidence);
    key.set("name", candidates[i].key.to_string());
    key.set("shortName", candidates[i].key.to_short_string());
    candidate.set("key", key);
    candidate.set("correlation", candidates[i].correlation);
    out.call<void>("push", candidate);
  }
  return out;
}

val js_detect_onsets(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  std::vector<float> onsets = quick::detect_onsets(data.data(), data.size(), sample_rate);
  return vectorToFloat32Array(onsets);
}

val js_detect_beats(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  std::vector<float> beats = quick::detect_beats(data.data(), data.size(), sample_rate);
  return vectorToFloat32Array(beats);
}

val js_detect_downbeats(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  std::vector<float> downbeats = quick::detect_downbeats(data.data(), data.size(), sample_rate);
  return vectorToFloat32Array(downbeats);
}

val js_detect_chords(val samples, int sample_rate, float min_duration, float smoothing_window,
                     float threshold, bool use_triads_only, int n_fft, int hop_length,
                     bool use_beat_sync, bool use_hmm, int hmm_beam_width, bool use_key_context,
                     int key_root, int key_mode, bool detect_inversions, int chroma_method) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  ChordConfig config;
  config.min_duration = min_duration;
  config.smoothing_window = smoothing_window;
  config.threshold = threshold;
  config.use_triads_only = use_triads_only;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.use_beat_sync = use_beat_sync;
  config.use_hmm = use_hmm;
  config.hmm_beam_width = hmm_beam_width;
  config.use_key_context = use_key_context;
  config.key_root = static_cast<PitchClass>(key_root);
  config.key_mode = static_cast<Mode>(key_mode);
  config.detect_inversions = detect_inversions;
  config.chroma_method = chroma_method == 1 ? ChromaMethod::NNLS : ChromaMethod::STFT;

  val result = val::object();
  result.set("chords", chordsToVal(detect_chords(audio, config)));
  return result;
}

val js_chord_functional_analysis(val samples, int key_root, int key_mode, int sample_rate,
                                 float min_duration, float smoothing_window, float threshold,
                                 bool use_triads_only, int n_fft, int hop_length,
                                 bool use_beat_sync, bool use_hmm, int hmm_beam_width,
                                 bool use_key_context, bool detect_inversions, int chroma_method) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  ChordConfig config;
  config.min_duration = min_duration;
  config.smoothing_window = smoothing_window;
  config.threshold = threshold;
  config.use_triads_only = use_triads_only;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.use_beat_sync = use_beat_sync;
  config.use_hmm = use_hmm;
  config.hmm_beam_width = hmm_beam_width;
  config.use_key_context = use_key_context;
  config.key_root = static_cast<PitchClass>(key_root);
  config.key_mode = static_cast<Mode>(key_mode);
  config.detect_inversions = detect_inversions;
  config.chroma_method = chroma_method == 1 ? ChromaMethod::NNLS : ChromaMethod::STFT;

  ChordAnalyzer analyzer(audio, config);
  std::vector<std::string> labels =
      analyzer.functional_analysis(static_cast<PitchClass>(key_root), static_cast<Mode>(key_mode));

  val out = val::array();
  for (size_t i = 0; i < labels.size(); ++i) {
    out.call<void>("push", labels[i]);
  }
  return out;
}

val js_analyze(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  AnalysisResult result = quick::analyze(data.data(), data.size(), sample_rate);
  return analysisResultToVal(result);
}

val acousticParametersToVal(const AcousticParameters& params) {
  val out = val::object();
  out.set("rt60", params.rt60);
  out.set("edt", params.edt);
  out.set("c50", params.c50);
  out.set("c80", params.c80);
  out.set("d50", params.d50);
  out.set("rt60Bands", vectorToFloat32Array(params.rt60_bands));
  out.set("edtBands", vectorToFloat32Array(params.edt_bands));
  out.set("c50Bands", vectorToFloat32Array(params.c50_bands));
  out.set("c80Bands", vectorToFloat32Array(params.c80_bands));
  out.set("confidence", params.confidence);
  out.set("isBlind", params.is_blind);
  return out;
}

val js_analyze_impulse_response(val samples, int sample_rate, int n_octave_bands) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  AcousticConfig config;
  config.n_octave_bands = n_octave_bands;
  return acousticParametersToVal(analyze_impulse_response(audio, config));
}

val js_detect_acoustic(val samples, int sample_rate, int n_octave_bands,
                       int n_third_octave_subbands, float min_decay_db,
                       float noise_floor_margin_db) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  AcousticConfig config;
  config.mode = AcousticConfig::Mode::Blind;
  config.n_octave_bands = n_octave_bands;
  config.n_third_octave_subbands = n_third_octave_subbands;
  config.min_decay_db = min_decay_db;
  config.noise_floor_margin_db = noise_floor_margin_db;
  return acousticParametersToVal(detect_acoustic(audio, config));
}

#ifdef SONARE_WITH_ACOUSTIC_SIM
sonare::acoustic::ShoeboxRoom roomFromVal(val opts, float def_absorption) {
  return sonare::acoustic::uniform_shoebox(
      {floatProperty(opts, "lengthM", 7.0f), floatProperty(opts, "widthM", 5.0f),
       floatProperty(opts, "heightM", 3.0f)},
      floatProperty(opts, "absorption", def_absorption));
}

sonare::acoustic::SourceListener placementFromVal(val opts) {
  return {{floatProperty(opts, "sourceX", 1.0f), floatProperty(opts, "sourceY", 1.0f),
           floatProperty(opts, "sourceZ", 1.2f)},
          {floatProperty(opts, "listenerX", 5.0f), floatProperty(opts, "listenerY", 4.0f),
           floatProperty(opts, "listenerZ", 1.7f)}};
}

// Acoustic sample-rate bounds, kept in sync with the C ABI's
// sonare_c_detail::kMinSampleRate / kMaxSampleRate so every binding rejects the
// same out-of-range rates (the C++ functions are otherwise called directly).
constexpr int kAcousticMinSampleRate = 8000;
constexpr int kAcousticMaxSampleRate = 384000;

void validateAcousticSampleRate(int sample_rate) {
  if (sample_rate < kAcousticMinSampleRate || sample_rate > kAcousticMaxSampleRate) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "sampleRate out of supported range [8000, 384000]");
  }
}

// Rejects an empty input buffer and any non-finite sample, matching the C ABI's
// validate_audio_params contract for the estimate/morph entry points.
void validateAcousticInput(const std::vector<float>& data) {
  if (data.empty()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "input buffer is empty");
  }
  for (const float s : data) {
    if (!std::isfinite(s)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "input contains NaN or Inf samples");
    }
  }
}

val js_synthesize_rir(val opts) {
  const int sample_rate = intProperty(opts, "sampleRate", 48000);
  validateAcousticSampleRate(sample_rate);
  sonare::acoustic::RirSynthConfig config;
  config.ism_order = std::max(0, intProperty(opts, "ismOrder", config.ism_order));
  config.late_model = boolProperty(opts, "preferEyring", true)
                          ? sonare::acoustic::ReverbModel::Eyring
                          : sonare::acoustic::ReverbModel::Sabine;
  config.seed = static_cast<unsigned>(std::max(0, intProperty(opts, "seed", 1)));
  config.max_seconds = floatProperty(opts, "maxSeconds", config.max_seconds);
  config.mixing_time_ms = floatProperty(opts, "mixingTimeMs", config.mixing_time_ms);
  config.crossfade_ms = floatProperty(opts, "crossfadeMs", config.crossfade_ms);

  const auto result = sonare::acoustic::synthesize_rir(roomFromVal(opts, 0.2f),
                                                       placementFromVal(opts), sample_rate, config);
  std::vector<float> rir;
  if (!result.rir.empty()) {
    rir.assign(result.rir.data(), result.rir.data() + result.rir.size());
  }
  val out = val::object();
  out.set("rir", vectorToFloat32Array(rir));
  out.set("sampleRate", result.rir.sample_rate());
  out.set("hasError", sonare::has_error(result.diagnostics));
  return out;
}

val js_estimate_room(val samples, int sample_rate, val opts) {
  validateAcousticSampleRate(sample_rate);
  std::vector<float> data = float32ArrayToVector(samples);
  validateAcousticInput(data);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  sonare::RoomEstimateConfig config;
  config.aspect_hint_lw = floatProperty(opts, "aspectHintLw", config.aspect_hint_lw);
  config.aspect_hint_lh = floatProperty(opts, "aspectHintLh", config.aspect_hint_lh);
  config.reference_absorption =
      floatProperty(opts, "referenceAbsorption", config.reference_absorption);
  config.prefer_eyring = boolProperty(opts, "preferEyring", true);
  const int n_bands = intProperty(opts, "nOctaveBands", 0);
  if (n_bands > 0) config.acoustic.n_octave_bands = n_bands;
  const float min_decay_db = floatProperty(opts, "minDecayDb", 0.0f);
  if (min_decay_db > 0.0f) config.acoustic.min_decay_db = min_decay_db;
  const float noise_floor_margin_db = floatProperty(opts, "noiseFloorMarginDb", 0.0f);
  if (noise_floor_margin_db > 0.0f) config.acoustic.noise_floor_margin_db = noise_floor_margin_db;
  switch (intProperty(opts, "mode", 0)) {
    case 1:
      config.acoustic.mode = sonare::AcousticConfig::Mode::Blind;
      break;
    case 2:
      config.acoustic.mode = sonare::AcousticConfig::Mode::ImpulseResponse;
      break;
    default:
      config.acoustic.mode = sonare::AcousticConfig::Mode::Auto;
      break;
  }

  const sonare::RoomEstimate est = sonare::estimate_room(audio, config);
  // The estimator always returns equal-length band vectors; report both at the
  // shared min length so consumers see the same band count as the C ABI/Python.
  const size_t band_count = std::min(est.absorption_bands.size(), est.rt60_bands.size());
  std::vector<float> absorption_bands(est.absorption_bands.begin(),
                                      est.absorption_bands.begin() + band_count);
  std::vector<float> rt60_bands(est.rt60_bands.begin(), est.rt60_bands.begin() + band_count);
  val out = val::object();
  out.set("volume", est.volume);
  out.set("length", est.dims.length);
  out.set("width", est.dims.width);
  out.set("height", est.dims.height);
  out.set("drrDb", est.drr_db);
  out.set("confidence", est.confidence);
  out.set("absorptionBands", vectorToFloat32Array(absorption_bands));
  out.set("rt60Bands", vectorToFloat32Array(rt60_bands));
  return out;
}

val js_room_morph(val samples, int sample_rate, val opts) {
  validateAcousticSampleRate(sample_rate);
  std::vector<float> data = float32ArrayToVector(samples);
  validateAcousticInput(data);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  sonare::effects::acoustic::RoomMorphConfig config;
  config.target = roomFromVal(opts, 0.2f);
  config.placement = placementFromVal(opts);
  config.source_tail_suppression =
      floatProperty(opts, "sourceTailSuppression", config.source_tail_suppression);
  config.wet = floatProperty(opts, "wet", config.wet);
  config.ism_order = std::max(0, intProperty(opts, "ismOrder", config.ism_order));
  config.seed = static_cast<unsigned>(std::max(0, intProperty(opts, "seed", 1)));
  config.max_seconds = floatProperty(opts, "maxSeconds", config.max_seconds);

  const Audio result = sonare::effects::acoustic::room_morph(audio, config);
  std::vector<float> out;
  if (!result.empty()) {
    out.assign(result.data(), result.data() + result.size());
  }
  return vectorToFloat32Array(out);
}
#endif  // SONARE_WITH_ACOUSTIC_SIM

// Analyze with progress callback
val js_analyze_with_progress(val samples, int sample_rate, val progress_callback) {
  std::vector<float> data = float32ArrayToVector(samples);

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

val js_pitch_correct_to_midi(val samples, int sample_rate, float current_midi, float target_midi) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  editing::pitch_editor::PitchCorrector corrector;
  editing::pitch_editor::F0Track track;
  track.sample_rate = sample_rate;
  track.hop_length = 512;
  track.f0_hz = {editing::pitch_editor::PitchCorrector::midi_to_hz(current_midi)};
  track.voiced = {true};
  track.voiced_prob = {1.0f};
  Audio result = corrector.correct_to_midi(audio, track, target_midi);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

val js_note_stretch(val samples, int sample_rate, int onset_sample, int offset_sample,
                    float stretch_ratio) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  editing::pitch_editor::NoteRegion region;
  region.onset_sample = onset_sample;
  region.offset_sample = offset_sample;
  editing::pitch_editor::NoteEditor editor;
  Audio result = editor.stretch_note(audio, region, stretch_ratio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

val js_voice_change(val samples, int sample_rate, float pitch_semitones, float formant_factor) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  editing::voice_changer::VoiceChangerConfig config;
  config.pitch_semitones = pitch_semitones;
  config.formant_factor = formant_factor;
  editing::voice_changer::VoiceChanger changer(config);
  Audio result = changer.process(audio);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// NMF decomposition of a non-negative spectrogram. Mirrors the C ABI
// sonare_decompose / librosa.decompose.decompose. Returns the two factor
// matrices as { w, h }: w is [n_features x n_components] row-major and h is
// [n_components x n_frames] row-major (both flat Float32Array buffers).
val js_decompose(val s, int n_features, int n_frames, int n_components, int n_iter, float beta) {
  std::vector<float> data = float32ArrayToVector(s);
  DecomposeResult result =
      decompose(data.data(), n_features, n_frames, n_components, n_iter, "mu", beta);

  val out = val::object();
  out.set("w", vectorToFloat32Array(result.W));
  out.set("h", vectorToFloat32Array(result.H));
  return out;
}

// Nearest-neighbour spectrogram filter. Mirrors the C ABI sonare_nn_filter /
// librosa.decompose.nn_filter. Returns the smoothed spectrogram
// [n_features x n_frames] as { data, rows, cols }.
val js_nn_filter(val s, int n_features, int n_frames, std::string aggregate, int k, int width) {
  std::vector<float> data = float32ArrayToVector(s);
  if (aggregate.empty()) aggregate = "mean";
  std::vector<float> filtered = nn_filter(data.data(), n_features, n_frames, aggregate, k, width);

  val out = val::object();
  out.set("data", vectorToFloat32Array(filtered));
  out.set("rows", n_features);
  out.set("cols", n_frames);
  return out;
}

// Time-domain remix: reorders / concatenates a signal by (start, end) interval
// slices. Mirrors the C ABI sonare_remix / librosa.effects.remix. @p intervals
// is a flat Int32Array of (start, end) pairs.
val js_remix(val samples, val intervals, int sample_rate, bool align_zeros) {
  std::vector<float> data = float32ArrayToVector(samples);
  // Sample indices must survive as exact integers: converting through float32
  // would round any boundary above 2^24 (16,777,216) and silently misalign the
  // slice. Read the Int32Array straight into int32 storage instead.
  std::vector<int32_t> interval_ints = int32ArrayToVector(intervals);
  if (interval_ints.size() % 2 != 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "remix intervals must be (start, end) pairs");
  }
  std::vector<std::pair<int, int>> pairs;
  pairs.reserve(interval_ints.size() / 2);
  for (size_t i = 0; i + 1 < interval_ints.size(); i += 2) {
    pairs.emplace_back(static_cast<int>(interval_ints[i]), static_cast<int>(interval_ints[i + 1]));
  }
  // sample_rate is validated for API symmetry with the C ABI but not consumed
  // by the time-domain remix itself.
  (void)sample_rate;
  std::vector<float> remixed = remix(data.data(), data.size(), pairs, align_zeros);
  return vectorToFloat32Array(remixed);
}

// HPSS with residual: separates audio into harmonic, percussive and residual
// signals (residual = original - harmonic - percussive). Mirrors the C ABI
// sonare_hpss_with_residual. Returns { harmonic, percussive, residual,
// sampleRate } where all three buffers share the same length and sample rate.
val js_hpss_with_residual(val samples, int sample_rate, int kernel_harmonic,
                          int kernel_percussive) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  HpssConfig config;
  config.kernel_size_harmonic = kernel_harmonic;
  config.kernel_size_percussive = kernel_percussive;

  HpssAudioResultWithResidual result = hpss_with_residual(audio, config);

  std::vector<float> harmonic_vec(result.harmonic.data(),
                                  result.harmonic.data() + result.harmonic.size());
  std::vector<float> percussive_vec(result.percussive.data(),
                                    result.percussive.data() + result.percussive.size());
  std::vector<float> residual_vec(result.residual.data(),
                                  result.residual.data() + result.residual.size());

  val out = val::object();
  out.set("harmonic", vectorToFloat32Array(harmonic_vec));
  out.set("percussive", vectorToFloat32Array(percussive_vec));
  out.set("residual", vectorToFloat32Array(residual_vec));
  out.set("sampleRate", result.harmonic.sample_rate());
  return out;
}

// Phase-vocoder time-scale modification (STFT -> phase_vocoder -> iSTFT).
// Mirrors the C ABI sonare_phase_vocoder. rate < 1.0 = slower, > 1.0 = faster.
val js_phase_vocoder(val samples, int sample_rate, float rate, int n_fft, int hop_length) {
  // Guard the time-scale rate before deriving the output length: a non-finite
  // or non-positive rate yields an opaque embind exception, and a tiny rate
  // would request an enormous output buffer. Mirrors the C ABI rate > 0 check
  // (sonare_phase_vocoder) and adds a sane upper bound.
  if (!std::isfinite(rate) || rate <= 0.0f || rate > 100.0f) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "phaseVocoder: rate must be finite and within (0, 100]");
  }
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  StftConfig stft_config;
  stft_config.n_fft = n_fft;
  stft_config.hop_length = hop_length;
  Spectrogram spec = Spectrogram::compute(audio, stft_config);

  PhaseVocoderConfig pv_config;
  pv_config.hop_length = hop_length;
  Spectrogram stretched = phase_vocoder(spec, rate, pv_config);

  const int expected_length = static_cast<int>(std::ceil(static_cast<float>(audio.size()) / rate));
  Audio result = stretched.to_audio(expected_length);
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
  Audio result = trim_absolute(audio, threshold_db);
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
    out.repair.declick.enabled = boolProperty(declick, "enabled", true);
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
    out.repair.dereverb.enabled = boolProperty(dereverb, "enabled", true);
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
    out.dynamics.compressor.enabled = boolProperty(compressor, "enabled", true);
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
    out.dynamics.deesser.enabled = boolProperty(deesser, "enabled", true);
    auto& dc = out.dynamics.deesser.config;
    dc.frequency_hz = floatProperty(deesser, "frequencyHz", dc.frequency_hz);
    dc.threshold_db = floatProperty(deesser, "thresholdDb", dc.threshold_db);
    dc.ratio = floatProperty(deesser, "ratio", dc.ratio);
    dc.attack_ms = floatProperty(deesser, "attackMs", dc.attack_ms);
    dc.release_ms = floatProperty(deesser, "releaseMs", dc.release_ms);
    dc.range_db = floatProperty(deesser, "rangeDb", dc.range_db);
    dc.bandpass_q = floatProperty(deesser, "bandpassQ", dc.bandpass_q);
  }
  if (hasProperty(dynamics, "transientShaper")) {
    val ts = objectProperty(dynamics, "transientShaper");
    out.dynamics.transient_shaper.enabled = boolProperty(ts, "enabled", true);
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
    out.dynamics.multiband_comp.enabled = boolProperty(mb, "enabled", true);
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
    auto& tc = out.saturation.tape.config;
    tc.drive_db = floatProperty(tape, "driveDb", tc.drive_db);
    tc.saturation = floatProperty(tape, "saturation", tc.saturation);
    tc.hysteresis = floatProperty(tape, "hysteresis", tc.hysteresis);
    tc.output_gain_db = floatProperty(tape, "outputGainDb", tc.output_gain_db);
    tc.speed_ips = floatProperty(tape, "speedIps", tc.speed_ips);
    tc.head_bump_db = floatProperty(tape, "headBumpDb", tc.head_bump_db);
    tc.bias = floatProperty(tape, "bias", tc.bias);
    tc.gap_loss = floatProperty(tape, "gapLoss", tc.gap_loss);
    // Tape is a color stage, so the mere presence of the object must not engage
    // it: an explicit `enabled` wins, otherwise defer to the shared core rule
    // (tape_engages_color) so a `{ tape: { driveDb: 0, saturation: 0 } }` config
    // stays bypassed, consistently with the flat-param chain parser.
    out.saturation.tape.enabled = hasProperty(tape, "enabled")
                                      ? boolProperty(tape, "enabled", true)
                                      : mastering::saturation::tape_engages_color(tc);
  }
  if (hasProperty(saturation, "exciter")) {
    val exciter = objectProperty(saturation, "exciter");
    auto& ec = out.saturation.exciter.config;
    ec.frequency_hz = floatProperty(exciter, "frequencyHz", ec.frequency_hz);
    ec.drive_db = floatProperty(exciter, "driveDb", ec.drive_db);
    ec.amount = floatProperty(exciter, "amount", ec.amount);
    ec.q = floatProperty(exciter, "q", ec.q);
    ec.even_odd_mix = floatProperty(exciter, "evenOddMix", ec.even_odd_mix);
    // Same color-stage rule as tape: explicit `enabled` wins, otherwise defer to
    // the shared core rule (exciter_engages_color) so `{ exciter: { amount: 0 } }`
    // stays bypassed.
    out.saturation.exciter.enabled = hasProperty(exciter, "enabled")
                                         ? boolProperty(exciter, "enabled", true)
                                         : mastering::saturation::exciter_engages_color(ec);
  }

  val spectral = objectProperty(config, "spectral");
  if (hasProperty(spectral, "airBand")) {
    val air_band = objectProperty(spectral, "airBand");
    out.spectral.air_band.enabled = boolProperty(air_band, "enabled", true);
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
    out.stereo.imager.enabled = boolProperty(imager, "enabled", true);
    auto& ic = out.stereo.imager.config;
    ic.width = floatProperty(imager, "width", ic.width);
    ic.output_gain_db = floatProperty(imager, "outputGainDb", ic.output_gain_db);
    ic.decorrelation_amount = floatProperty(imager, "decorrelationAmount", ic.decorrelation_amount);
    ic.preserve_energy = boolProperty(imager, "preserveEnergy", ic.preserve_energy);
  }
  if (hasProperty(stereo, "monoMaker")) {
    val mono_maker = objectProperty(stereo, "monoMaker");
    out.stereo.mono_maker.enabled = boolProperty(mono_maker, "enabled", true);
    out.stereo.mono_maker.config.amount =
        floatProperty(mono_maker, "amount", out.stereo.mono_maker.config.amount);
  }

  val maximizer = objectProperty(config, "maximizer");
  if (hasProperty(maximizer, "truePeakLimiter")) {
    val limiter = objectProperty(maximizer, "truePeakLimiter");
    out.maximizer.true_peak_limiter.enabled = boolProperty(limiter, "enabled", true);
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
    out.loudness.enabled = boolProperty(loudness, "enabled", true);
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
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "stereo channel lengths must match");
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
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "stereo channel lengths must match");
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
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "stereo channel lengths must match");
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

// ---------------------------------------------------------------------------
// StreamingEqualizer wrapper (block-by-block streaming EqualizerProcessor).
// Construct via createEqualizer(config) factory.
// ---------------------------------------------------------------------------

mastering::eq::EqBandType eqBandTypeFromString(const std::string& value) {
  using mastering::eq::EqBandType;
  if (value == "Peak" || value == "peak" || value == "Bell" || value == "bell") {
    return EqBandType::Peak;
  }
  if (value == "LowShelf" || value == "lowShelf") return EqBandType::LowShelf;
  if (value == "HighShelf" || value == "highShelf") return EqBandType::HighShelf;
  if (value == "LowPass" || value == "lowPass" || value == "HighCut" || value == "highCut") {
    return EqBandType::LowPass;
  }
  if (value == "HighPass" || value == "highPass" || value == "LowCut" || value == "lowCut") {
    return EqBandType::HighPass;
  }
  if (value == "BandPass" || value == "bandPass") return EqBandType::BandPass;
  if (value == "Notch" || value == "notch") return EqBandType::Notch;
  if (value == "TiltShelf" || value == "tiltShelf") return EqBandType::TiltShelf;
  if (value == "FlatTilt" || value == "flatTilt") return EqBandType::FlatTilt;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown EQ band type: " + value);
}

mastering::eq::BiquadCoeffMode eqCoeffModeFromString(const std::string& value) {
  using mastering::eq::BiquadCoeffMode;
  if (value == "Rbj" || value == "RBJ" || value == "rbj") return BiquadCoeffMode::Rbj;
  if (value == "Vicanek" || value == "vicanek") return BiquadCoeffMode::Vicanek;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown EQ coefficient mode: " + value);
}

mastering::eq::StereoPlacement eqPlacementFromString(const std::string& value) {
  using mastering::eq::StereoPlacement;
  if (value == "Stereo" || value == "stereo") return StereoPlacement::Stereo;
  if (value == "Left" || value == "left") return StereoPlacement::Left;
  if (value == "Right" || value == "right") return StereoPlacement::Right;
  if (value == "Mid" || value == "mid") return StereoPlacement::Mid;
  if (value == "Side" || value == "side") return StereoPlacement::Side;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown EQ placement: " + value);
}

mastering::eq::PhaseMode eqBandPhaseFromString(const std::string& value) {
  using mastering::eq::PhaseMode;
  if (value == "Inherit" || value == "inherit") return PhaseMode::Inherit;
  if (value == "ZeroLatency" || value == "zeroLatency") return PhaseMode::ZeroLatency;
  if (value == "NaturalPhase" || value == "naturalPhase") return PhaseMode::NaturalPhase;
  if (value == "LinearPhase" || value == "linearPhase") return PhaseMode::LinearPhase;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown EQ band phase mode: " + value);
}

mastering::eq::PhaseMode eqPhaseFromInt(int mode) {
  using mastering::eq::PhaseMode;
  switch (mode) {
    case 1:
      return PhaseMode::ZeroLatency;
    case 2:
      return PhaseMode::NaturalPhase;
    case 3:
      return PhaseMode::LinearPhase;
    default:
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown EQ phase mode");
  }
}

mastering::eq::EqBand eqBandFromVal(val band) {
  mastering::eq::EqBand result;
  result.type = eqBandTypeFromString(stringProperty(band, "type", "Peak"));
  result.coeff_mode = eqCoeffModeFromString(stringProperty(band, "coeffMode", "Rbj"));
  result.frequency_hz = floatProperty(band, "frequencyHz", result.frequency_hz);
  result.gain_db = floatProperty(band, "gainDb", result.gain_db);
  result.q = floatProperty(band, "q", result.q);
  result.enabled = boolProperty(band, "enabled", result.enabled);
  result.slope_db_oct = intProperty(band, "slopeDbOct", result.slope_db_oct);
  result.placement = eqPlacementFromString(stringProperty(band, "placement", "Stereo"));
  result.phase = eqBandPhaseFromString(stringProperty(band, "phase", "Inherit"));
  result.soloed = boolProperty(band, "soloed", result.soloed);
  result.bypassed = boolProperty(band, "bypassed", result.bypassed);
  result.proportional_q = boolProperty(band, "proportionalQ", result.proportional_q);
  result.proportional_q_strength =
      floatProperty(band, "proportionalQStrength", result.proportional_q_strength);

  result.dyn.enabled = boolProperty(band, "dynamic", result.dyn.enabled);
  result.dyn.threshold_db = floatProperty(band, "thresholdDb", result.dyn.threshold_db);
  result.dyn.auto_threshold = boolProperty(band, "autoThreshold", result.dyn.auto_threshold);
  result.dyn.ratio = floatProperty(band, "ratio", result.dyn.ratio);
  result.dyn.range_db = floatProperty(band, "rangeDb", result.dyn.range_db);
  result.dyn.attack_ms = floatProperty(band, "attackMs", result.dyn.attack_ms);
  result.dyn.release_ms = floatProperty(band, "releaseMs", result.dyn.release_ms);
  result.dyn.lookahead_ms = floatProperty(band, "lookaheadMs", result.dyn.lookahead_ms);
  result.dyn.external_sidechain =
      boolProperty(band, "externalSidechain", result.dyn.external_sidechain);
  result.dyn.sidechain_freq_hz =
      floatProperty(band, "sidechainFreqHz", result.dyn.sidechain_freq_hz);
  result.dyn.sidechain_q = floatProperty(band, "sidechainQ", result.dyn.sidechain_q);
  return result;
}

class EqualizerWrapper {
 public:
  explicit EqualizerWrapper(val config) : processor_(makeConfig()) {
    const double sample_rate = floatProperty(config, "sampleRate", 48000.0f);
    const int max_block_size = intProperty(config, "maxBlockSize", 512);
    processor_.prepare(sample_rate, max_block_size);
  }

  void setBand(int index, val band) {
    processor_.set_band(static_cast<size_t>(index), eqBandFromVal(band));
  }

  void clear() { processor_.clear(); }

  void setPhaseMode(int mode) { processor_.set_phase_mode(eqPhaseFromInt(mode)); }

  void setAutoGain(bool enabled) { processor_.set_auto_gain_enabled(enabled); }

  void setGainScale(float scale) { processor_.set_gain_scale(scale); }

  void setOutputGainDb(float gain_db) { processor_.set_output_gain_db(gain_db); }

  void setOutputPan(float pan) { processor_.set_output_pan(pan); }

  // Borrows a mono external sidechain key for dynamic bands that opt into
  // DynamicParams::external_sidechain. The samples are copied into an owned
  // buffer so they remain valid until the next set/clear call.
  void setSidechainMono(val samples) {
    sidechain_left_ = float32ArrayToVector(samples);
    sidechain_right_.clear();
    if (sidechain_left_.empty()) {
      processor_.clear_sidechain();
      return;
    }
    const float* channels[] = {sidechain_left_.data()};
    processor_.set_sidechain(channels, 1, static_cast<int>(sidechain_left_.size()));
  }

  // Borrows a stereo external sidechain key. Both channels must match in length.
  void setSidechainStereo(val left_samples, val right_samples) {
    sidechain_left_ = float32ArrayToVector(left_samples);
    sidechain_right_ = float32ArrayToVector(right_samples);
    if (sidechain_left_.size() != sidechain_right_.size()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "sidechain channel lengths must match");
    }
    if (sidechain_left_.empty()) {
      processor_.clear_sidechain();
      return;
    }
    const float* channels[] = {sidechain_left_.data(), sidechain_right_.data()};
    processor_.set_sidechain(channels, 2, static_cast<int>(sidechain_left_.size()));
  }

  void clearSidechain() {
    processor_.clear_sidechain();
    sidechain_left_.clear();
    sidechain_right_.clear();
  }

  float lastAutoGainDb() const { return processor_.last_auto_gain_db(); }

  int latencySamples() const { return processor_.latency_samples(); }

  val processMono(val samples) {
    std::vector<float> block = float32ArrayToVector(samples);
    if (!block.empty()) {
      float* channels[] = {block.data()};
      processor_.process(channels, 1, static_cast<int>(block.size()));
    }
    return vectorToFloat32Array(block);
  }

  val processStereo(val left_samples, val right_samples) {
    std::vector<float> left = float32ArrayToVector(left_samples);
    std::vector<float> right = float32ArrayToVector(right_samples);
    if (left.size() != right.size()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "stereo channel lengths must match");
    }
    if (!left.empty()) {
      float* channels[] = {left.data(), right.data()};
      processor_.process(channels, 2, static_cast<int>(left.size()));
    }
    val out = val::object();
    out.set("left", vectorToFloat32Array(left));
    out.set("right", vectorToFloat32Array(right));
    return out;
  }

  val spectrum() const {
    const mastering::eq::EqualizerSpectrumSnapshot snapshot = processor_.spectrum_snapshot();

    std::vector<float> pre_left(snapshot.pre_count);
    std::vector<float> pre_right(snapshot.pre_count);
    for (size_t i = 0; i < snapshot.pre_count; ++i) {
      pre_left[i] = snapshot.pre[i].left;
      pre_right[i] = snapshot.pre[i].right;
    }
    std::vector<float> post_left(snapshot.post_count);
    std::vector<float> post_right(snapshot.post_count);
    for (size_t i = 0; i < snapshot.post_count; ++i) {
      post_left[i] = snapshot.post[i].left;
      post_right[i] = snapshot.post[i].right;
    }
    std::vector<float> band_gain_db(snapshot.band_gain_db.begin(), snapshot.band_gain_db.end());
    std::vector<float> profile_db(snapshot.profile_db.begin(), snapshot.profile_db.end());

    val out = val::object();
    out.set("preLeft", vectorToFloat32Array(pre_left));
    out.set("preRight", vectorToFloat32Array(pre_right));
    out.set("postLeft", vectorToFloat32Array(post_left));
    out.set("postRight", vectorToFloat32Array(post_right));
    out.set("bandGainDb", vectorToFloat32Array(band_gain_db));
    out.set("profileDb", vectorToFloat32Array(profile_db));
    out.set("lastAutoGainDb", processor_.last_auto_gain_db());
    out.set("seq", static_cast<double>(snapshot.seq));
    return out;
  }

  void match(val source, val reference, val options) {
    std::vector<float> src = float32ArrayToVector(source);
    std::vector<float> ref = float32ArrayToVector(reference);
    const int sample_rate = intProperty(options, "sampleRate", 48000);
    const int max_bands = intProperty(options, "maxBands", 8);
    Audio src_audio = Audio::from_buffer(src.data(), src.size(), sample_rate);
    Audio ref_audio = Audio::from_buffer(ref.data(), ref.size(), sample_rate);
    mastering::match::MatchEqConfig match_config;
    match_config.max_bands = static_cast<size_t>(max_bands);
    mastering::match::configure_equalizer_from_match(
        processor_, mastering::match::reference_spectrum(src_audio),
        mastering::match::reference_spectrum(ref_audio), match_config);
  }

 private:
  static mastering::eq::EqualizerProcessorConfig makeConfig() {
    mastering::eq::EqualizerProcessorConfig config;
    config.max_channels = 2;
    return config;
  }

  mastering::eq::EqualizerProcessor processor_;
  std::vector<float> sidechain_left_;
  std::vector<float> sidechain_right_;
};

EqualizerWrapper* createEqualizer(val config) { return new EqualizerWrapper(config); }

// ---------------------------------------------------------------------------
// StreamingRetune wrapper (block-by-block voice retune / pitch shift).
// Construct via createStreamingRetune(config) factory.
// ---------------------------------------------------------------------------

editing::voice_changer::StreamingRetuneConfig streamingRetuneConfigFromVal(val config) {
  editing::voice_changer::StreamingRetuneConfig result;
  if (config.isNull() || config.isUndefined()) {
    return result;
  }
  result.semitones = floatProperty(config, "semitones", result.semitones);
  result.mix = floatProperty(config, "mix", result.mix);
  result.grain_size = intProperty(config, "grainSize", result.grain_size);
  result.grain_size = intProperty(config, "grain_size", result.grain_size);
  return result;
}

val streamingRetuneConfigToVal(const editing::voice_changer::StreamingRetuneConfig& config) {
  val out = val::object();
  out.set("semitones", config.semitones);
  out.set("mix", config.mix);
  out.set("grainSize", config.grain_size);
  return out;
}

class StreamingRetuneWrapper {
 public:
  explicit StreamingRetuneWrapper(val config) : retune_(streamingRetuneConfigFromVal(config)) {}

  void prepare(double sample_rate, int max_block_size) {
    retune_.prepare(sample_rate, max_block_size);
  }

  void reset() { retune_.reset(); }

  void setConfig(val config) { retune_.set_config(streamingRetuneConfigFromVal(config)); }

  val config() const { return streamingRetuneConfigToVal(retune_.config()); }

  int grainSize() const { return retune_.grain_size(); }

  val processMono(val samples) {
    std::vector<float> block = float32ArrayToVector(samples);
    std::vector<float> out(block.size());
    retune_.process_block(block.data(), out.data(), static_cast<int>(block.size()));
    return vectorToFloat32Array(out);
  }

 private:
  editing::voice_changer::StreamingRetune retune_;
};

StreamingRetuneWrapper* createStreamingRetune(val config) {
  return new StreamingRetuneWrapper(config);
}

std::string realtimeVoiceChangerConfigTextFromVal(val config) {
  if (config.isNull() || config.isUndefined()) return "neutral-monitor";
  if (config.typeOf().as<std::string>() == "string") return config.as<std::string>();
  return val::global("JSON").call<std::string>("stringify", config);
}

class RealtimeVoiceChangerWrapper {
 public:
  explicit RealtimeVoiceChangerWrapper(val config)
      : changer_(editing::voice_changer::realtime_voice_changer_config_from_json(
            realtimeVoiceChangerConfigTextFromVal(config))) {}

  void prepare(double sample_rate, int max_block_size, int channels) {
    changer_.prepare(sample_rate, max_block_size, channels);
    // Pre-warm the per-instance scratch buffers so the first process* call
    // does not trigger an allocation. The `ensure_*_capacity` helpers only
    // grow; once warmed up to (channels, max_block_size) they stay that size.
    ensure_mono_capacity(static_cast<size_t>(max_block_size));
    ensure_interleaved_capacity(static_cast<size_t>(max_block_size), channels);
    max_block_size_ = max_block_size;
    prepared_channels_ = channels;
    prepared_ = true;
  }

  void reset() { changer_.reset(); }

  void setConfig(val config) {
    changer_.set_config(editing::voice_changer::realtime_voice_changer_config_from_json(
        realtimeVoiceChangerConfigTextFromVal(config)));
  }

  std::string configJson() const {
    return editing::voice_changer::realtime_voice_changer_config_to_json(changer_.config());
  }

  int latencySamples() const { return changer_.latency_samples(); }

  // Element-wise legacy path. NOT RT-safe for high block rates; AudioWorklet
  // consumers should prefer the prepared API below (getMonoInputBuffer /
  // processPreparedMono / getMonoOutputBuffer) which avoids per-sample JS↔C++
  // crossings and per-call allocations entirely.
  val processMono(val samples) {
    require_prepared();
    const int length = samples["length"].as<int>();
    ensure_mono_capacity(static_cast<size_t>(length));
    copyFloat32Array(samples, mono_input_, static_cast<size_t>(length));
    changer_.process_block(mono_input_.data(), mono_output_.data(), length);
    val output = val::global("Float32Array").new_(length);
    for (int i = 0; i < length; ++i) output.set(i, mono_output_[static_cast<size_t>(i)]);
    return output;
  }

  void processMonoInto(val samples, val output) {
    require_prepared();
    const int length = samples["length"].as<int>();
    if (output["length"].as<int>() < length) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "output buffer is too small");
    }
    ensure_mono_capacity(static_cast<size_t>(length));
    copyFloat32Array(samples, mono_input_, static_cast<size_t>(length));
    changer_.process_block(mono_input_.data(), mono_output_.data(), length);
    for (int i = 0; i < length; ++i) output.set(i, mono_output_[static_cast<size_t>(i)]);
  }

  val processInterleaved(val samples, int channels) {
    require_prepared();
    const int length = samples["length"].as<int>();
    val output = val::global("Float32Array").new_(length);
    processInterleavedInto(samples, channels, output);
    return output;
  }

  void processInterleavedInto(val samples, int channels, val output) {
    require_prepared();
    const int length = samples["length"].as<int>();
    if (channels <= 0 || length % channels != 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "invalid interleaved channel count");
    }
    if (output["length"].as<int>() < length) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "output buffer is too small");
    }
    const size_t frames = static_cast<size_t>(length / channels);
    ensure_interleaved_capacity(frames, channels);
    for (int ch = 0; ch < channels; ++ch) {
      for (size_t i = 0; i < frames; ++i) {
        const int index =
            static_cast<int>((i * static_cast<size_t>(channels)) + static_cast<size_t>(ch));
        planar_[static_cast<size_t>(ch)][i] = samples[index].as<float>();
      }
    }
    changer_.process_block(channel_ptrs_.data(), channels, static_cast<int>(frames));
    for (size_t i = 0; i < frames; ++i) {
      for (int ch = 0; ch < channels; ++ch) {
        const int index =
            static_cast<int>((i * static_cast<size_t>(channels)) + static_cast<size_t>(ch));
        output.set(index, planar_[static_cast<size_t>(ch)][i]);
      }
    }
  }

  // ---- Zero-copy "prepared" API ----------------------------------------
  // Caller fills the input view (returned as a typed_memory_view onto the
  // WASM heap), calls processPrepared*, then reads the output view. No JS↔C++
  // sample-level crossings and no allocations on the audio thread.

  val getMonoInputBuffer(int num_samples) {
    require_prepared();
    if (num_samples <= 0 || num_samples > max_block_size_) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeVoiceChanger.getMonoInputBuffer: out-of-range length");
    }
    ensure_mono_capacity(static_cast<size_t>(num_samples));
    return val(typed_memory_view(static_cast<size_t>(num_samples), mono_input_.data()));
  }

  val getMonoOutputBuffer(int num_samples) {
    require_prepared();
    if (num_samples <= 0 || num_samples > max_block_size_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.getMonoOutputBuffer: out-of-range length");
    }
    ensure_mono_capacity(static_cast<size_t>(num_samples));
    return val(typed_memory_view(static_cast<size_t>(num_samples), mono_output_.data()));
  }

  void processPreparedMono(int num_samples) {
    require_prepared();
    if (num_samples <= 0 || num_samples > max_block_size_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.processPreparedMono: out-of-range length");
    }
    if (mono_input_.size() < static_cast<size_t>(num_samples) ||
        mono_output_.size() < static_cast<size_t>(num_samples)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeVoiceChanger.processPreparedMono: getMonoInputBuffer/"
                                    "getMonoOutputBuffer must be called first");
    }
    changer_.process_block(mono_input_.data(), mono_output_.data(), num_samples);
  }

  val getInterleavedInputBuffer(int num_frames, int num_channels) {
    require_prepared();
    if (num_frames <= 0 || num_channels <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeVoiceChanger.getInterleavedInputBuffer: bad dims");
    }
    if (num_frames > max_block_size_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.getInterleavedInputBuffer: frames exceed max block size");
    }
    ensure_interleaved_capacity(static_cast<size_t>(num_frames), num_channels);
    const size_t length = static_cast<size_t>(num_frames) * static_cast<size_t>(num_channels);
    return val(typed_memory_view(length, interleaved_input_.data()));
  }

  val getInterleavedOutputBuffer(int num_frames, int num_channels) {
    require_prepared();
    if (num_frames <= 0 || num_channels <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeVoiceChanger.getInterleavedOutputBuffer: bad dims");
    }
    if (num_frames > max_block_size_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.getInterleavedOutputBuffer: frames exceed max block size");
    }
    ensure_interleaved_capacity(static_cast<size_t>(num_frames), num_channels);
    const size_t length = static_cast<size_t>(num_frames) * static_cast<size_t>(num_channels);
    return val(typed_memory_view(length, interleaved_output_.data()));
  }

  void processPreparedInterleaved(int num_frames, int num_channels) {
    require_prepared();
    if (num_frames <= 0 || num_channels <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeVoiceChanger.processPreparedInterleaved: bad dims");
    }
    if (num_frames > max_block_size_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.processPreparedInterleaved: frames exceed max block size");
    }
    const size_t frames = static_cast<size_t>(num_frames);
    const size_t channel_count = static_cast<size_t>(num_channels);
    const size_t length = frames * channel_count;
    if (interleaved_input_.size() < length || interleaved_output_.size() < length ||
        planar_.size() < channel_count) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.processPreparedInterleaved: getInterleavedInputBuffer/"
          "getInterleavedOutputBuffer must be called first with matching dims");
    }
    for (size_t ch = 0; ch < channel_count; ++ch) {
      float* dst = planar_[ch].data();
      const float* src = interleaved_input_.data() + ch;
      for (size_t i = 0; i < frames; ++i) {
        dst[i] = src[i * channel_count];
      }
    }
    changer_.process_block(channel_ptrs_.data(), num_channels, num_frames);
    for (size_t ch = 0; ch < channel_count; ++ch) {
      const float* src = planar_[ch].data();
      float* dst = interleaved_output_.data() + ch;
      for (size_t i = 0; i < frames; ++i) {
        dst[i * channel_count] = src[i];
      }
    }
  }

  // ---- Planar zero-copy stereo path -----------------------------------
  // Match AudioWorklet's native planar layout: each channel is its own
  // Float32Array, so the worklet can hand the in/out buffers straight
  // through with no interleave/deinterleave passes.

  val getPlanarChannelBuffer(int channel, int num_frames) {
    require_prepared();
    if (num_frames <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeVoiceChanger.getPlanarChannelBuffer: bad frames");
    }
    if (num_frames > max_block_size_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.getPlanarChannelBuffer: frames exceed max block size");
    }
    if (channel < 0 || channel >= prepared_channels_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.getPlanarChannelBuffer: channel out of range");
    }
    ensure_interleaved_capacity(static_cast<size_t>(num_frames), prepared_channels_);
    return val(typed_memory_view(static_cast<size_t>(num_frames),
                                 planar_[static_cast<size_t>(channel)].data()));
  }

  void processPreparedPlanar(int num_frames) {
    require_prepared();
    if (num_frames <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeVoiceChanger.processPreparedPlanar: bad frames");
    }
    if (num_frames > max_block_size_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.processPreparedPlanar: frames exceed max block size");
    }
    const size_t channel_count = static_cast<size_t>(prepared_channels_);
    if (planar_.size() < channel_count) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.processPreparedPlanar: getPlanarChannelBuffer must be called for "
          "each channel before processing");
    }
    for (size_t ch = 0; ch < channel_count; ++ch) {
      if (planar_[ch].size() < static_cast<size_t>(num_frames)) {
        throw sonare::SonareException(
            sonare::ErrorCode::InvalidParameter,
            "RealtimeVoiceChanger.processPreparedPlanar: planar buffer too small for requested "
            "frames");
      }
    }
    changer_.process_block(channel_ptrs_.data(), prepared_channels_, num_frames);
  }

 private:
  static void copyFloat32Array(val source, std::vector<float>& dest, size_t length) {
    for (size_t i = 0; i < length; ++i) dest[i] = source[static_cast<int>(i)].as<float>();
  }

  void ensure_mono_capacity(size_t samples) {
    if (mono_input_.size() < samples) {
      mono_input_.resize(samples);
      mono_output_.resize(samples);
    }
  }

  void ensure_interleaved_capacity(size_t frames, int channels) {
    const size_t channel_count = static_cast<size_t>(channels);
    if (planar_.size() < channel_count) planar_.resize(channel_count);
    if (channel_ptrs_.size() < channel_count) channel_ptrs_.resize(channel_count, nullptr);
    for (size_t ch = 0; ch < channel_count; ++ch) {
      if (planar_[ch].size() < frames) planar_[ch].resize(frames);
      channel_ptrs_[ch] = planar_[ch].data();
    }
    const size_t length = frames * channel_count;
    if (interleaved_input_.size() < length) interleaved_input_.resize(length);
    if (interleaved_output_.size() < length) interleaved_output_.resize(length);
  }

  void require_prepared() const {
    if (!prepared_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.prepare() must be called before processing");
    }
  }

  editing::voice_changer::RealtimeVoiceChanger changer_;
  std::vector<float> mono_input_;
  std::vector<float> mono_output_;
  std::vector<std::vector<float>> planar_;
  std::vector<float*> channel_ptrs_;
  std::vector<float> interleaved_input_;
  std::vector<float> interleaved_output_;
  int max_block_size_ = 0;
  int prepared_channels_ = 0;
  bool prepared_ = false;
};

RealtimeVoiceChangerWrapper* createRealtimeVoiceChanger(val config) {
  return new RealtimeVoiceChangerWrapper(config);
}

val realtimeVoiceChangerPresetNames() {
  val out = val::array();
  const auto names = editing::voice_changer::realtime_voice_changer_preset_names();
  for (size_t i = 0; i < names.size(); ++i) out.call<void>("push", names[i]);
  return out;
}

std::string realtimeVoiceChangerPresetJson(const std::string& id) {
  return editing::voice_changer::realtime_voice_changer_preset_json(
      editing::voice_changer::realtime_voice_changer_preset_from_id(id));
}

val validateRealtimeVoiceChangerPresetJson(const std::string& json) {
  // Full schema-level validation (schemaVersion, id/name string limits,
  // unknown-key rejection, every value range) — must match the C/Node/
  // Python contract. Earlier this only did a from_json→to_json roundtrip,
  // which silently accepted incomplete presets.
  val out = val::object();
  try {
    std::string normalized;
    std::string error;
    if (editing::voice_changer::validate_realtime_voice_changer_preset_json(json, &normalized,
                                                                            &error)) {
      out.set("ok", true);
      out.set("normalizedJson", normalized);
    } else {
      out.set("ok", false);
      out.set("error", error.empty() ? std::string("invalid preset JSON") : error);
    }
  } catch (const std::exception& ex) {
    out.set("ok", false);
    out.set("error", std::string(ex.what()));
  }
  return out;
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
// Overrides accept a flat object whose keys match `parse_chain_config_params`
// dot-notation (e.g. "loudness.targetLufs"). Numeric and boolean values are
// supported. Pass null/undefined for "preset only".
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
  std::vector<float> data = float32ArrayToVector(samples);
  auto preset = mastering::api::preset_from_string(preset_name);
  auto overrides_vec = masteringParamsFromObject(overrides);
  auto result = mastering::api::master_audio_mono(
      preset, data.data(), data.size(), sample_rate,
      overrides_vec.empty() ? nullptr : overrides_vec.data(), overrides_vec.size());

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
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  if (left.size() != right.size()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "stereo channel lengths must match");
  }
  auto preset = mastering::api::preset_from_string(preset_name);
  auto overrides_vec = masteringParamsFromObject(overrides);
  auto result = mastering::api::master_audio_stereo(
      preset, left.data(), right.data(), left.size(), sample_rate,
      overrides_vec.empty() ? nullptr : overrides_vec.data(), overrides_vec.size());

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

val js_master_audio_with_progress(std::string preset_name, val samples, int sample_rate,
                                  val overrides, val progress_callback) {
  std::vector<float> data = float32ArrayToVector(samples);
  auto preset = mastering::api::preset_from_string(preset_name);
  auto config = mastering::api::preset_config(preset);
  auto overrides_vec = masteringParamsFromObject(overrides);
  if (!overrides_vec.empty()) {
    mastering::api::apply_chain_config_overrides(config, overrides_vec.data(),
                                                 overrides_vec.size());
  }
  mastering::api::MasteringChain chain(std::move(config));
  if (!progress_callback.isNull() && !progress_callback.isUndefined()) {
    chain.set_progress_callback([progress_callback](float progress, const char* stage) {
      progress_callback(progress, std::string(stage ? stage : ""));
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

val js_master_audio_stereo_with_progress(std::string preset_name, val left_samples,
                                         val right_samples, int sample_rate, val overrides,
                                         val progress_callback) {
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  if (left.size() != right.size()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "stereo channel lengths must match");
  }
  auto preset = mastering::api::preset_from_string(preset_name);
  auto config = mastering::api::preset_config(preset);
  auto overrides_vec = masteringParamsFromObject(overrides);
  if (!overrides_vec.empty()) {
    mastering::api::apply_chain_config_overrides(config, overrides_vec.data(),
                                                 overrides_vec.size());
  }
  mastering::api::MasteringChain chain(std::move(config));
  if (!progress_callback.isNull() && !progress_callback.isUndefined()) {
    chain.set_progress_callback([progress_callback](float progress, const char* stage) {
      progress_callback(progress, std::string(stage ? stage : ""));
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

val js_mixing_scene_preset_names() {
  val out = val::array();
  auto names = mixing::api::scene_preset_names();
  for (size_t index = 0; index < names.size(); ++index) {
    out.call<void>("push", names[index]);
  }
  return out;
}

std::string js_mixing_scene_preset_json(std::string preset_name) {
  return mixing::api::scene_to_json(
      mixing::api::scene_preset(mixing::api::scene_preset_from_string(preset_name)));
}

// ---------------------------------------------------------------------------
// MixerWasm: persistent scene-based mixer wrapper around the C mixer API
// (sonare_mixer_*). Owns a SonareMixer* built from a scene JSON string, routes
// strips through the compiled routing graph, and sums to a stereo master.
//
// processStereo takes planar inputs: leftChannels[i] / rightChannels[i] are the
// L/R Float32Array for strip i (matching mixStereo's input layout). It returns
// { left, right, sampleRate } with the mixed stereo master. Call delete() (or
// use try/finally) to release the underlying WASM object.
// ---------------------------------------------------------------------------
#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)
class MixerWasm {
 public:
  MixerWasm(SonareMixer* mixer, int sample_rate, int block_size)
      : mixer_(mixer), sample_rate_(sample_rate), block_size_(block_size) {
    if (block_size_ <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "mixer block size must be positive");
    }
    const size_t strip_count = sonare_mixer_strip_count(mixer_);
    left_scratch_.resize(strip_count);
    right_scratch_.resize(strip_count);
    left_ptrs_.resize(strip_count);
    right_ptrs_.resize(strip_count);
    for (size_t index = 0; index < strip_count; ++index) {
      left_scratch_[index].resize(static_cast<size_t>(block_size_));
      right_scratch_[index].resize(static_cast<size_t>(block_size_));
      left_ptrs_[index] = left_scratch_[index].data();
      right_ptrs_[index] = right_scratch_[index].data();
    }
    out_scratch_left_.resize(static_cast<size_t>(block_size_));
    out_scratch_right_.resize(static_cast<size_t>(block_size_));
  }

  ~MixerWasm() {
    if (mixer_ != nullptr) {
      sonare_mixer_destroy(mixer_);
      mixer_ = nullptr;
    }
  }

  MixerWasm(const MixerWasm&) = delete;
  MixerWasm& operator=(const MixerWasm&) = delete;

  static MixerWasm* fromSceneJson(std::string json, int sample_rate, int block_size) {
    SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), sample_rate, block_size);
    if (mixer == nullptr) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to build mixer from scene JSON: ") + sonare_last_error_message());
    }
    return new MixerWasm(mixer, sample_rate, block_size);
  }

  static std::string presetJson(std::string name) {
    char* json = nullptr;
    SonareError err = sonare_mixing_scene_preset_json(name.c_str(), &json);
    if (err != SONARE_OK || json == nullptr) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to get mixing scene preset JSON: ") + sonare_error_message(err));
    }
    std::string out(json);
    sonare_free_string(json);
    return out;
  }

  void compile() {
    SonareError err = sonare_mixer_compile(mixer_);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to compile mixer graph: ") + sonare_error_message(err));
    }
  }

  size_t stripCount() const { return sonare_mixer_strip_count(mixer_); }

  // Schedules sample-accurate insert-parameter automation on the strip at
  // strip_index. insert_index addresses the strip's combined insert sequence
  // [pre-inserts... post-inserts...]. param_id is processor-specific. sample_pos
  // is in absolute samples from the start of processing. curve: 0 = Linear,
  // 1 = Exponential.
  void scheduleInsertAutomation(unsigned int strip_index, unsigned int insert_index,
                                unsigned int param_id, double sample_pos, float value, int curve) {
    SonareStrip* strip = sonare_mixer_strip_at(mixer_, static_cast<size_t>(strip_index));
    if (strip == nullptr) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "mixer strip index out of range");
    }
    SonareError err = sonare_strip_schedule_insert_automation(
        strip, insert_index, param_id, static_cast<int64_t>(sample_pos), value, curve);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to schedule insert automation: ") + sonare_error_message(err));
    }
  }

  // Borrowed strip handle by index in [0, stripCount()). Throws if out of range.
  // The handle is owned by the mixer; do not free it.
  SonareStrip* stripAt(unsigned int strip_index) {
    SonareStrip* strip = sonare_mixer_strip_at(mixer_, static_cast<size_t>(strip_index));
    if (strip == nullptr) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "mixer strip index out of range");
    }
    return strip;
  }

  // Sets the strip's input trim in dB.
  void setInputTrimDb(unsigned int strip_index, float db) {
    checkStripError(sonare_strip_set_input_trim_db(stripAt(strip_index), db),
                    "failed to set input trim");
  }

  // Sets the strip's fader level in dB.
  void setFaderDb(unsigned int strip_index, float db) {
    checkStripError(sonare_strip_set_fader_db(stripAt(strip_index), db), "failed to set fader");
  }

  // Sets the strip's pan position. pan_mode is processor-specific.
  void setPan(unsigned int strip_index, float pan, int pan_mode) {
    checkStripError(sonare_strip_set_pan(stripAt(strip_index), pan, pan_mode), "failed to set pan");
  }

  // Sets the strip's stereo width.
  void setWidth(unsigned int strip_index, float width) {
    checkStripError(sonare_strip_set_width(stripAt(strip_index), width), "failed to set width");
  }

  // Sets the strip's mute state.
  void setMuted(unsigned int strip_index, bool muted) {
    checkStripError(sonare_strip_set_muted(stripAt(strip_index), muted ? 1 : 0),
                    "failed to set muted");
  }

  // Sets the strip's solo state. Takes effect on the next process without a
  // graph recompile.
  void setSoloed(unsigned int strip_index, bool soloed) {
    checkStripError(sonare_strip_set_soloed(stripAt(strip_index), soloed ? 1 : 0),
                    "failed to set soloed");
  }

  // Marks a strip as solo-safe so it is never implied-muted by another strip's
  // solo. Takes effect on the next process without a graph recompile.
  void setSoloSafe(unsigned int strip_index, bool solo_safe) {
    checkStripError(sonare_strip_set_solo_safe(stripAt(strip_index), solo_safe ? 1 : 0),
                    "failed to set solo-safe");
  }

  // Inverts the polarity of the left and/or right channel.
  void setPolarityInvert(unsigned int strip_index, bool invert_left, bool invert_right) {
    checkStripError(sonare_strip_set_polarity_invert(stripAt(strip_index), invert_left ? 1 : 0,
                                                     invert_right ? 1 : 0),
                    "failed to set polarity invert");
  }

  // Sets the strip's pan law. pan_law: 0 = -3 dB, 1 = -4.5 dB, 2 = -6 dB,
  // 3 = linear (0 dB).
  void setPanLaw(unsigned int strip_index, int pan_law) {
    checkStripError(sonare_strip_set_pan_law(stripAt(strip_index), pan_law),
                    "failed to set pan law");
  }

  // Sets a per-strip channel delay in samples. This changes the strip's reported
  // latency; recompile to re-run latency compensation.
  void setChannelDelaySamples(unsigned int strip_index, int delay_samples) {
    checkStripError(sonare_strip_set_channel_delay_samples(stripAt(strip_index), delay_samples),
                    "failed to set channel delay samples");
  }

  // Sets the strip's live VCA gain offset in dB (not persisted to the scene).
  void setVcaOffsetDb(unsigned int strip_index, float offset_db) {
    checkStripError(sonare_strip_set_vca_offset_db(stripAt(strip_index), offset_db),
                    "failed to set VCA offset");
  }

  // Sets independent left/right pan positions (dual-pan mode).
  void setDualPan(unsigned int strip_index, float left_pan, float right_pan) {
    checkStripError(sonare_strip_set_dual_pan(stripAt(strip_index), left_pan, right_pan),
                    "failed to set dual pan");
  }

  // Adds a post-construction send to the strip. timing: 0 = pre-fader,
  // 1 = post-fader. Returns the new send's index.
  size_t addSend(unsigned int strip_index, std::string id, std::string destination_bus_id,
                 float send_db, int timing) {
    size_t index = 0;
    checkStripError(sonare_strip_add_send(stripAt(strip_index), id.c_str(),
                                          destination_bus_id.c_str(), send_db, timing, &index),
                    "failed to add send");
    return index;
  }

  // Sets the send level (in dB) for an existing send by index.
  void setSendDb(unsigned int strip_index, size_t send_index, float send_db) {
    checkStripError(sonare_strip_set_send_db(stripAt(strip_index), send_index, send_db),
                    "failed to set send level");
  }

  // Reads a meter snapshot at the given tap point. tap: 0 = pre-fader,
  // 1 = post-fader (see SonareMeterTap). Returns the full snapshot.
  val meterTap(unsigned int strip_index, int tap) {
    SonareMixMeterSnapshot snapshot{};
    checkStripError(sonare_strip_meter_tap(stripAt(strip_index), tap, &snapshot),
                    "failed to read meter tap");
    return mixMeterSnapshotToVal(snapshot);
  }

  // Reads the strip's current (post-fader) meter snapshot. Tap-less, mirroring
  // the Node/Python stripMeter contract which calls sonare_strip_meter; the
  // tap-selectable variant is meterTap.
  val stripMeter(unsigned int strip_index) {
    SonareMixMeterSnapshot snapshot{};
    checkStripError(sonare_strip_meter(stripAt(strip_index), &snapshot),
                    "failed to read strip meter");
    return mixMeterSnapshotToVal(snapshot);
  }

  // Schedules sample-accurate fader automation on a strip. sample_pos uses the
  // absolute-sample timeline; curve: 0 = Linear, 1 = Exponential.
  void scheduleFaderAutomation(unsigned int strip_index, double sample_pos, float fader_db,
                               int curve) {
    checkStripError(sonare_strip_schedule_fader_automation(
                        stripAt(strip_index), static_cast<int64_t>(sample_pos), fader_db, curve),
                    "failed to schedule fader automation");
  }

  void schedulePanAutomation(unsigned int strip_index, double sample_pos, float pan, int curve) {
    checkStripError(sonare_strip_schedule_pan_automation(
                        stripAt(strip_index), static_cast<int64_t>(sample_pos), pan, curve),
                    "failed to schedule pan automation");
  }

  void scheduleWidthAutomation(unsigned int strip_index, double sample_pos, float width,
                               int curve) {
    checkStripError(sonare_strip_schedule_width_automation(
                        stripAt(strip_index), static_cast<int64_t>(sample_pos), width, curve),
                    "failed to schedule width automation");
  }

  // Schedules sample-accurate send-level automation on a strip's send.
  void scheduleSendAutomation(unsigned int strip_index, size_t send_index, double sample_pos,
                              float db, int curve) {
    checkStripError(
        sonare_strip_schedule_send_automation(stripAt(strip_index), send_index,
                                              static_cast<int64_t>(sample_pos), db, curve),
        "failed to schedule send automation");
  }

  // Reads up to max_points of the strip's most recent goniometer samples.
  // Returns an array of { left, right } points (oldest to newest).
  val readGoniometerLatest(unsigned int strip_index, size_t max_points) {
    SonareStrip* strip = stripAt(strip_index);
    val out = val::array();
    if (max_points == 0) {
      return out;
    }
    std::vector<SonareMixGoniometerPoint> points(max_points);
    const size_t count = sonare_strip_read_goniometer_latest(strip, points.data(), max_points);
    for (size_t index = 0; index < count; ++index) {
      val point = val::object();
      point.set("left", points[index].left);
      point.set("right", points[index].right);
      out.call<void>("push", point);
    }
    return out;
  }

  // Resolves a strip's index from its id. Returns -1 when the id is not found;
  // the TS wrapper maps -1 to null for cross-binding consistency (Node returns
  // number | null).
  int stripById(std::string id) {
    const size_t count = sonare_mixer_strip_count(mixer_);
    SonareStrip* target = sonare_mixer_strip_by_id(mixer_, id.c_str());
    if (target == nullptr) {
      return -1;
    }
    for (size_t index = 0; index < count; ++index) {
      if (sonare_mixer_strip_at(mixer_, index) == target) {
        return static_cast<int>(index);
      }
    }
    return -1;
  }

  // Adds a bus to the mixer topology. role is one of "master", "aux", "submix"
  // (empty defaults to "aux"). Marks the routing graph dirty; call compile (or
  // process) to rebuild.
  void addBus(std::string id, std::string role) {
    SonareError err =
        sonare_mixer_add_bus(mixer_, id.c_str(), role.empty() ? nullptr : role.c_str());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    std::string("failed to add bus: ") + sonare_error_message(err));
    }
  }

  void removeBus(std::string id) {
    SonareError err = sonare_mixer_remove_bus(mixer_, id.c_str());
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to remove bus: ") + sonare_error_message(err));
    }
  }

  size_t busCount() const {
    size_t count = 0;
    SonareError err = sonare_mixer_bus_count(mixer_, &count);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to read bus count: ") + sonare_error_message(err));
    }
    return count;
  }

  // Adds a VCA group with the given gain offset. members is an array of strip-id
  // strings (may be empty).
  void addVcaGroup(std::string id, float gain_db, val members) {
    std::vector<std::string> member_storage;
    std::vector<const char*> member_ptrs;
    if (!members.isUndefined() && !members.isNull()) {
      const int count = members["length"].as<int>();
      member_storage.reserve(static_cast<size_t>(count));
      member_ptrs.reserve(static_cast<size_t>(count));
      for (int i = 0; i < count; ++i) {
        member_storage.push_back(members[i].as<std::string>());
      }
      for (const auto& member : member_storage) {
        member_ptrs.push_back(member.c_str());
      }
    }
    SonareError err = sonare_mixer_add_vca_group(mixer_, id.c_str(), gain_db,
                                                 member_ptrs.empty() ? nullptr : member_ptrs.data(),
                                                 member_ptrs.size());
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to add VCA group: ") + sonare_error_message(err));
    }
  }

  void removeVcaGroup(std::string id) {
    SonareError err = sonare_mixer_remove_vca_group(mixer_, id.c_str());
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to remove VCA group: ") + sonare_error_message(err));
    }
  }

  size_t vcaGroupCount() const {
    size_t count = 0;
    SonareError err = sonare_mixer_vca_group_count(mixer_, &count);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to read VCA group count: ") + sonare_error_message(err));
    }
    return count;
  }

  std::string toSceneJson() const {
    char* json = nullptr;
    SonareError err = sonare_mixer_to_scene_json(mixer_, &json);
    if (err != SONARE_OK || json == nullptr) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("failed to serialize mixer scene: ") + sonare_error_message(err));
    }
    std::string out(json);
    sonare_free_string(json);
    return out;
  }

  val processStereo(val left_channels, val right_channels) {
    const int count = left_channels["length"].as<int>();
    // Reject empty input to match the free js_mix_stereo contract: a zero-strip
    // call would derive a zero-length block and produce an empty master, which
    // is never a useful result. (There is no master-only path here.)
    if (count <= 0 || right_channels["length"].as<int>() != count) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "leftChannels and rightChannels must have the same non-zero length");
    }

    std::vector<std::vector<float>> left_inputs;
    std::vector<std::vector<float>> right_inputs;
    left_inputs.reserve(static_cast<size_t>(count));
    right_inputs.reserve(static_cast<size_t>(count));

    size_t length = 0;
    for (int index = 0; index < count; ++index) {
      left_inputs.push_back(float32ArrayToVector(left_channels[index]));
      right_inputs.push_back(float32ArrayToVector(right_channels[index]));
      if (left_inputs.back().size() != right_inputs.back().size()) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "left and right channel lengths must match");
      }
      if (index == 0) {
        length = left_inputs.back().size();
      } else if (left_inputs.back().size() != length) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "all strips must have the same length");
      }
    }
    if (length > static_cast<size_t>(block_size_)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "block length exceeds the mixer's configured block size");
    }

    std::vector<const float*> left_ptrs(static_cast<size_t>(count));
    std::vector<const float*> right_ptrs(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
      left_ptrs[static_cast<size_t>(index)] = left_inputs[static_cast<size_t>(index)].data();
      right_ptrs[static_cast<size_t>(index)] = right_inputs[static_cast<size_t>(index)].data();
    }

    std::vector<float> out_left(length, 0.0f);
    std::vector<float> out_right(length, 0.0f);
    SonareError err = sonare_mixer_process_stereo(
        mixer_, count > 0 ? left_ptrs.data() : nullptr, count > 0 ? right_ptrs.data() : nullptr,
        static_cast<size_t>(count), out_left.data(), out_right.data(), length);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("mixer process failed: ") + sonare_error_message(err));
    }

    val out = val::object();
    out.set("left", vectorToFloat32Array(out_left));
    out.set("right", vectorToFloat32Array(out_right));
    out.set("sampleRate", sample_rate_);
    return out;
  }

  void processStereoInto(val left_channels, val right_channels, val out_left, val out_right) {
    const int count = left_channels["length"].as<int>();
    if (count < 0 || right_channels["length"].as<int>() != count) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "leftChannels and rightChannels must have the same length");
    }
    if (static_cast<size_t>(count) != left_scratch_.size()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "input channel count must match the mixer's strip count");
    }

    const int length_i = out_left["length"].as<int>();
    if (length_i <= 0 || out_right["length"].as<int>() != length_i) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "output channels must have the same non-zero length");
    }
    const size_t length = static_cast<size_t>(length_i);
    if (length > static_cast<size_t>(block_size_)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "block length exceeds the mixer's configured block size");
    }

    for (int index = 0; index < count; ++index) {
      val left = left_channels[index];
      val right = right_channels[index];
      if (left["length"].as<int>() != length_i || right["length"].as<int>() != length_i) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "all input and output channels must have the same length");
      }
      auto& left_dest = left_scratch_[static_cast<size_t>(index)];
      auto& right_dest = right_scratch_[static_cast<size_t>(index)];
      for (size_t sample = 0; sample < length; ++sample) {
        left_dest[sample] = left[sample].as<float>();
        right_dest[sample] = right[sample].as<float>();
      }
    }

    SonareError err = sonare_mixer_process_stereo(
        mixer_, count > 0 ? left_ptrs_.data() : nullptr, count > 0 ? right_ptrs_.data() : nullptr,
        static_cast<size_t>(count), out_scratch_left_.data(), out_scratch_right_.data(), length);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("mixer process failed: ") + sonare_error_message(err));
    }
    for (size_t sample = 0; sample < length; ++sample) {
      out_left.set(sample, out_scratch_left_[sample]);
      out_right.set(sample, out_scratch_right_[sample]);
    }
  }

  val inputLeftView(size_t index) {
    if (index >= left_scratch_.size()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "mixer input index out of range");
    }
    return val(typed_memory_view(static_cast<size_t>(block_size_), left_scratch_[index].data()));
  }

  val inputRightView(size_t index) {
    if (index >= right_scratch_.size()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "mixer input index out of range");
    }
    return val(typed_memory_view(static_cast<size_t>(block_size_), right_scratch_[index].data()));
  }

  val outputLeftView() {
    return val(typed_memory_view(static_cast<size_t>(block_size_), out_scratch_left_.data()));
  }

  val outputRightView() {
    return val(typed_memory_view(static_cast<size_t>(block_size_), out_scratch_right_.data()));
  }

  void processPreparedStereo(size_t num_samples) {
    if (num_samples == 0 || num_samples > static_cast<size_t>(block_size_)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "invalid prepared mixer block length");
    }
    const size_t count = left_scratch_.size();
    SonareError err = sonare_mixer_process_stereo(
        mixer_, count > 0 ? left_ptrs_.data() : nullptr, count > 0 ? right_ptrs_.data() : nullptr,
        count, out_scratch_left_.data(), out_scratch_right_.data(), num_samples);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("mixer process failed: ") + sonare_error_message(err));
    }
  }

 private:
  static void checkStripError(SonareError err, const char* what) {
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    std::string(what) + ": " + sonare_error_message(err));
    }
  }

  static val mixMeterSnapshotToVal(const SonareMixMeterSnapshot& snapshot) {
    val out = val::object();
    out.set("peakDbL", snapshot.peak_db_l);
    out.set("peakDbR", snapshot.peak_db_r);
    out.set("rmsDbL", snapshot.rms_db_l);
    out.set("rmsDbR", snapshot.rms_db_r);
    out.set("correlation", snapshot.correlation);
    out.set("monoCompatWidth", snapshot.mono_compat_width);
    out.set("monoCompatPeak", snapshot.mono_compat_peak);
    out.set("monoCompatSideRms", snapshot.mono_compat_side_rms);
    out.set("likelyMonoCompatible", snapshot.likely_mono_compatible != 0);
    out.set("momentaryLufs", snapshot.momentary_lufs);
    out.set("shortTermLufs", snapshot.short_term_lufs);
    out.set("integratedLufs", snapshot.integrated_lufs);
    out.set("gainReductionDb", snapshot.gain_reduction_db);
    out.set("truePeakDbL", snapshot.true_peak_db_l);
    out.set("truePeakDbR", snapshot.true_peak_db_r);
    out.set("maxTruePeakDb", snapshot.max_true_peak_db);
    out.set("seq", static_cast<double>(snapshot.seq));
    return out;
  }

  SonareMixer* mixer_ = nullptr;
  int sample_rate_ = 48000;
  int block_size_ = 0;
  std::vector<std::vector<float>> left_scratch_;
  std::vector<std::vector<float>> right_scratch_;
  std::vector<const float*> left_ptrs_;
  std::vector<const float*> right_ptrs_;
  std::vector<float> out_scratch_left_;
  std::vector<float> out_scratch_right_;
};

MixerWasm* createMixerFromSceneJson(std::string json, int sample_rate, int block_size) {
  return MixerWasm::fromSceneJson(std::move(json), sample_rate, block_size);
}
#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH

namespace {

val optionAt(val options, const char* key, int index) {
  if (!hasProperty(options, key)) {
    return val::undefined();
  }
  val value = options[key];
  if (val::global("Array").call<bool>("isArray", value)) {
    return value[index];
  }
  return value;
}

mixing::PanMode panModeFromVal(val value) {
  if (value.isUndefined() || value.isNull()) {
    return mixing::PanMode::Balance;
  }
  if (value.typeOf().as<std::string>() == "number") {
    const int mode = value.as<int>();
    if (mode == 1) return mixing::PanMode::StereoPan;
    if (mode == 2) return mixing::PanMode::DualPan;
    return mixing::PanMode::Balance;
  }
  if (value.typeOf().as<std::string>() != "string") {
    return mixing::PanMode::Balance;
  }
  std::string mode = value.as<std::string>();
  for (char& ch : mode) {
    if (ch == '_') ch = '-';
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (mode == "stereo-pan" || mode == "stereopan" || mode == "pan") {
    return mixing::PanMode::StereoPan;
  }
  if (mode == "dual-pan" || mode == "dualpan") {
    return mixing::PanMode::DualPan;
  }
  return mixing::PanMode::Balance;
}

val meterSnapshotToVal(const mixing::MeterSnapshot& snapshot) {
  val out = val::object();
  out.set("peakDbL", snapshot.peak_db[0]);
  out.set("peakDbR", snapshot.peak_db[1]);
  out.set("rmsDbL", snapshot.rms_db[0]);
  out.set("rmsDbR", snapshot.rms_db[1]);
  out.set("correlation", snapshot.correlation);
  out.set("monoCompatWidth", snapshot.mono_compat_width);
  out.set("monoCompatPeak", snapshot.mono_compat_peak);
  out.set("monoCompatSideRms", snapshot.mono_compat_side_rms);
  out.set("likelyMonoCompatible", snapshot.likely_mono_compatible);
  out.set("momentaryLufs", snapshot.momentary_lufs);
  out.set("shortTermLufs", snapshot.short_term_lufs);
  out.set("integratedLufs", snapshot.integrated_lufs);
  out.set("gainReductionDb", snapshot.gain_reduction_db);
  out.set("truePeakDbL", snapshot.true_peak_db[0]);
  out.set("truePeakDbR", snapshot.true_peak_db[1]);
  out.set("maxTruePeakDb", snapshot.max_true_peak_db);
  out.set("seq", static_cast<double>(snapshot.seq));
  return out;
}

}  // namespace

val js_mix_stereo(val left_channels, val right_channels, int sample_rate, val options) {
  const int count = left_channels["length"].as<int>();
  if (count <= 0 || right_channels["length"].as<int>() != count) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "leftChannels and rightChannels must have the same non-zero length");
  }

  std::vector<std::vector<float>> left_inputs;
  std::vector<std::vector<float>> right_inputs;
  left_inputs.reserve(static_cast<size_t>(count));
  right_inputs.reserve(static_cast<size_t>(count));

  size_t length = 0;
  for (int index = 0; index < count; ++index) {
    left_inputs.push_back(float32ArrayToVector(left_channels[index]));
    right_inputs.push_back(float32ArrayToVector(right_channels[index]));
    if (left_inputs.back().size() != right_inputs.back().size()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "left and right channel lengths must match");
    }
    if (index == 0) {
      length = left_inputs.back().size();
    } else if (left_inputs.back().size() != length) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "all strips must have the same length");
    }
  }

  std::vector<float> out_left(length, 0.0f);
  std::vector<float> out_right(length, 0.0f);
  val meters = val::array();

  for (int index = 0; index < count; ++index) {
    mixing::ChannelStrip strip;
    strip.prepare(sample_rate, static_cast<int>(std::max<size_t>(1, length)));

    val inputTrim = optionAt(options, "inputTrimDb", index);
    if (!inputTrim.isUndefined() && !inputTrim.isNull() &&
        inputTrim.typeOf().as<std::string>() == "number") {
      strip.set_input_trim_db(inputTrim.as<float>());
    }
    val fader = optionAt(options, "faderDb", index);
    if (!fader.isUndefined() && !fader.isNull() && fader.typeOf().as<std::string>() == "number") {
      strip.set_fader_db(fader.as<float>());
    }
    val pan = optionAt(options, "pan", index);
    if (!pan.isUndefined() && !pan.isNull() && pan.typeOf().as<std::string>() == "number") {
      strip.set_pan_mode(panModeFromVal(optionAt(options, "panMode", index)));
      strip.set_pan(pan.as<float>());
    }
    val width = optionAt(options, "width", index);
    if (!width.isUndefined() && !width.isNull() && width.typeOf().as<std::string>() == "number") {
      strip.set_width(width.as<float>());
    }
    val muted = optionAt(options, "muted", index);
    if (!muted.isUndefined() && !muted.isNull() && muted.typeOf().as<std::string>() == "boolean") {
      strip.set_muted(muted.as<bool>());
    }

    float* channels[] = {left_inputs[static_cast<size_t>(index)].data(),
                         right_inputs[static_cast<size_t>(index)].data()};
    strip.process(channels, 2, static_cast<int>(length));
    for (size_t sample = 0; sample < length; ++sample) {
      out_left[sample] += left_inputs[static_cast<size_t>(index)][sample];
      out_right[sample] += right_inputs[static_cast<size_t>(index)][sample];
    }
    meters.call<void>("push", meterSnapshotToVal(strip.meter_snapshot()));
  }

  val out = val::object();
  out.set("left", vectorToFloat32Array(out_left));
  out.set("right", vectorToFloat32Array(out_right));
  out.set("sampleRate", sample_rate);
  out.set("meters", meters);
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

std::string js_mastering_assistant_suggest(val samples, int sample_rate, val params_obj) {
  std::vector<float> data = float32ArrayToVector(samples);
  std::vector<mastering::api::Param> params = masteringParamsFromObject(params_obj);
  const mastering::assistant::AssistantConfig config =
      mastering::assistant::assistant_config_from_params(params.data(), params.size());
  const auto result =
      mastering::assistant::suggest_chain(data.data(), data.size(), sample_rate, config);
  return mastering::assistant::assistant_result_to_json(result);
}

std::string js_mastering_audio_profile(val samples, int sample_rate, val params_obj) {
  std::vector<float> data = float32ArrayToVector(samples);
  std::vector<mastering::api::Param> params = masteringParamsFromObject(params_obj);
  const mastering::assistant::AudioProfileConfig config =
      mastering::assistant::audio_profile_config_from_params(params.data(), params.size());
  const auto profile =
      mastering::assistant::analyze_audio_profile(data.data(), data.size(), sample_rate, config);
  return mastering::assistant::audio_profile_to_json(profile);
}

std::vector<mastering::maximizer::StreamingPlatform> streamingPlatformsFromVal(val platforms) {
  std::vector<mastering::maximizer::StreamingPlatform> out;
  if (platforms.isUndefined() || platforms.isNull()) {
    return out;
  }
  if (!val::global("Array").call<bool>("isArray", platforms)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "platforms must be an array");
  }
  const int length = platforms["length"].as<int>();
  out.reserve(static_cast<size_t>(length));
  for (int index = 0; index < length; ++index) {
    val platform = platforms[index];
    out.push_back({stringProperty(platform, "name", ""),
                   floatProperty(platform, "targetLufs", -14.0f),
                   floatProperty(platform, "ceilingDb", -1.0f)});
  }
  return out;
}

std::string js_mastering_streaming_preview(val samples, int sample_rate, val platforms_obj) {
  std::vector<float> data = float32ArrayToVector(samples);
  const Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  const auto platforms = streamingPlatformsFromVal(platforms_obj);
  const auto results = platforms.empty()
                           ? mastering::maximizer::streaming_preview(audio)
                           : mastering::maximizer::streaming_preview(audio, platforms);
  return mastering::maximizer::streaming_preview_to_json(results);
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

// Inverse: Mel power spectrogram [n_mels x n_frames] -> STFT power spectrogram
// [(n_fft/2 + 1) x n_frames]. Mirrors feature::mel_to_stft.
//
// hop_length is intentionally absent: feature::mel_to_stft does not consume it.
val js_mel_to_stft(val mel_power, int n_mels, int n_frames, int sample_rate, int n_fft, float fmin,
                   float fmax) {
  std::vector<float> data = float32ArrayToVector(mel_power);

  MelConfig config;
  config.n_fft = n_fft;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;

  std::vector<float> stft = mel_to_stft(data.data(), n_mels, n_frames, config, sample_rate);

  val out = val::object();
  out.set("nBins", n_fft / 2 + 1);
  out.set("nFrames", n_frames);
  out.set("power", vectorToFloat32Array(stft));
  return out;
}

// Inverse: Mel power spectrogram -> audio via Griffin-Lim. Mirrors
// feature::mel_to_audio.
val js_mel_to_audio(val mel_power, int n_mels, int n_frames, int sample_rate, int n_fft,
                    int hop_length, float fmin, float fmax, int n_iter) {
  std::vector<float> data = float32ArrayToVector(mel_power);

  MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;

  Audio result = mel_to_audio(data.data(), n_mels, n_frames, config, n_iter, sample_rate);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// Inverse: MFCC matrix [n_mfcc x n_frames] -> Mel power spectrogram (dB scale).
// Mirrors feature::mfcc_to_mel.
val js_mfcc_to_mel(val mfcc, int n_mfcc, int n_frames, int n_mels) {
  std::vector<float> data = float32ArrayToVector(mfcc);

  std::vector<float> mel = mfcc_to_mel(data.data(), n_mfcc, n_frames, n_mels);

  val out = val::object();
  out.set("nMels", n_mels);
  out.set("nFrames", n_frames);
  out.set("power", vectorToFloat32Array(mel));
  return out;
}

// Inverse: MFCC matrix -> audio via Griffin-Lim. Mirrors feature::mfcc_to_audio.
val js_mfcc_to_audio(val mfcc, int n_mfcc, int n_frames, int n_mels, int sample_rate, int n_fft,
                     int hop_length, float fmin, float fmax, int n_iter) {
  std::vector<float> data = float32ArrayToVector(mfcc);

  MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;

  Audio result = mfcc_to_audio(data.data(), n_mfcc, n_frames, config, n_iter, sample_rate);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
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

val js_nnls_chroma(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  Chroma chroma = nnls_chroma(audio);

  val out = val::object();
  out.set("nChroma", chroma.n_chroma());
  out.set("nFrames", chroma.n_frames());

  std::vector<float> data_vec(chroma.data(), chroma.data() + chroma.n_chroma() * chroma.n_frames());
  out.set("data", vectorToFloat32Array(data_vec));
  return out;
}

// ============================================================================
// Analysis - Sections / Melody
// ============================================================================

// Mirrors sonare_analyze_sections / SonareSectionResult and the Node/Python
// analyzeSections: detects song-structure sections and returns an array of
// { type, name, start, end, energyLevel, confidence }.
val js_analyze_sections(val samples, int sample_rate, int n_fft = 2048, int hop_length = 512,
                        float min_section_sec = 4.0f) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  // Fall back to the struct defaults when raw emscripten passes 0 for a
  // missing argument, so the JS-facing defaults stay consistent with the
  // C ABI / Node bindings (n_fft=2048, hop_length=512, min_section_sec=4.0).
  SectionConfig config;
  if (n_fft > 0) config.n_fft = n_fft;
  if (hop_length > 0) config.hop_length = hop_length;
  if (min_section_sec > 0.0f) config.min_section_sec = min_section_sec;

  SectionAnalyzer analyzer(audio, config);

  val sections = val::array();
  for (const Section& section : analyzer.sections()) {
    val item = val::object();
    item.set("type", static_cast<int>(section.type));
    item.set("name", section.type_string());
    item.set("start", section.start);
    item.set("end", section.end);
    item.set("energyLevel", section.energy_level);
    item.set("confidence", section.confidence);
    sections.call<void>("push", item);
  }
  return sections;
}

// Mirrors sonare_analyze_melody / SonareMelodyResult: extracts the melody
// contour via YIN and returns { points: [{ time, frequency, confidence }],
// pitchRangeOctaves, pitchStability, meanFrequency, vibratoRate }.
val js_analyze_melody(val samples, int sample_rate, float fmin = 65.0f, float fmax = 2093.0f,
                      int frame_length = 2048, int hop_length = 256, float threshold = 0.1f) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  // Fall back to the struct defaults when raw emscripten passes 0 for a
  // missing argument, so the JS-facing defaults stay consistent with the
  // C ABI / Node bindings (fmin=65, fmax=2093, frame_length=2048,
  // hop_length=256, threshold=0.1).
  MelodyConfig config;
  if (fmin > 0.0f) config.fmin = fmin;
  if (fmax > 0.0f) config.fmax = fmax;
  if (frame_length > 0) config.frame_length = frame_length;
  if (hop_length > 0) config.hop_length = hop_length;
  if (threshold > 0.0f) config.threshold = threshold;

  MelodyAnalyzer analyzer(audio, config);
  const MelodyContour& contour = analyzer.contour();

  val points = val::array();
  for (const PitchPoint& point : contour.pitches) {
    val item = val::object();
    item.set("time", point.time);
    item.set("frequency", point.frequency);
    item.set("confidence", point.confidence);
    points.call<void>("push", item);
  }

  val out = val::object();
  out.set("points", points);
  out.set("pitchRangeOctaves", contour.pitch_range_octaves);
  out.set("pitchStability", contour.pitch_stability);
  out.set("meanFrequency", contour.mean_frequency);
  out.set("vibratoRate", contour.vibrato_rate);
  return out;
}

// ============================================================================
// Features - Constant-Q / Variable-Q transforms
// ============================================================================

// Shared serializer for CQT/VQT magnitude results, mirroring SonareCqtResult:
// { nBins, nFrames, hopLength, sampleRate, magnitude (nBins*nFrames row-major),
// frequencies (nBins) }.
val cqtResultToVal(const CqtResult& result) {
  val out = val::object();
  out.set("nBins", result.n_bins());
  out.set("nFrames", result.n_frames());
  out.set("hopLength", result.hop_length());
  out.set("sampleRate", result.sample_rate());
  out.set("magnitude", vectorToFloat32Array(result.magnitude()));
  out.set("frequencies", vectorToFloat32Array(result.frequencies()));
  return out;
}

val js_cqt(val samples, int sample_rate, int hop_length, float fmin, int n_bins,
           int bins_per_octave) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  CqtConfig config;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.n_bins = n_bins;
  config.bins_per_octave = bins_per_octave;

  return cqtResultToVal(cqt(audio, config));
}

val js_vqt(val samples, int sample_rate, int hop_length, float fmin, int n_bins,
           int bins_per_octave, float gamma) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  VqtConfig config;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.n_bins = n_bins;
  config.bins_per_octave = bins_per_octave;
  config.gamma = gamma;

  return cqtResultToVal(vqt(audio, config));
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

// Spectral contrast: peak-to-valley energy per band per frame. Mirrors the C
// ABI sonare_spectral_contrast / librosa.feature.spectral_contrast. Returns a
// row-major matrix [(n_bands + 1) x n_frames] as { data, rows, cols }, with the
// extra row holding the residual band.
val js_spectral_contrast(val samples, int sample_rate, int n_fft, int hop_length, int n_bands,
                         float fmin, float quantile) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> contrast = spectral_contrast(spec, sample_rate, n_bands, fmin, quantile);

  const int rows = n_bands + 1;
  const int cols = rows > 0 ? static_cast<int>(contrast.size()) / rows : 0;

  val out = val::object();
  out.set("data", vectorToFloat32Array(contrast));
  out.set("rows", rows);
  out.set("cols", cols);
  return out;
}

// Polynomial coefficients fit to each frame's spectrum. Mirrors the C ABI
// sonare_poly_features / librosa.feature.poly_features. Returns a row-major
// matrix [(order + 1) x n_frames] as { data, rows, cols } (coefficients ordered
// high-to-low).
val js_poly_features(val samples, int sample_rate, int n_fft, int hop_length, int order) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> coeffs = poly_features(spec, sample_rate, order);

  const int rows = order + 1;
  const int cols = rows > 0 ? static_cast<int>(coeffs.size()) / rows : 0;

  val out = val::object();
  out.set("data", vectorToFloat32Array(coeffs));
  out.set("rows", rows);
  out.set("cols", cols);
  return out;
}

// Raw zero-crossing sample indices. Mirrors the C ABI sonare_zero_crossings /
// librosa.zero_crossings (returns indices i where sign(y[i]) != sign(y[i-1])).
val js_zero_crossings(val samples, float threshold, bool ref_magnitude, bool pad, bool zero_pos) {
  std::vector<float> data = float32ArrayToVector(samples);
  std::vector<int> indices =
      zero_crossings(data.data(), data.size(), threshold, ref_magnitude, pad, zero_pos);
  return vectorToInt32Array(indices);
}

// Per-octave tuning offset from a list of detected pitches. Mirrors the C ABI
// sonare_pitch_tuning / librosa.pitch_tuning.
float js_pitch_tuning(val frequencies, float resolution, int bins_per_octave) {
  std::vector<float> data = float32ArrayToVector(frequencies);
  return pitch_tuning(data, resolution, bins_per_octave);
}

// Global tuning offset of an audio signal. Mirrors the C ABI
// sonare_estimate_tuning / librosa.estimate_tuning.
float js_estimate_tuning(val samples, int sample_rate, int n_fft, int hop_length, float resolution,
                         int bins_per_octave) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  return estimate_tuning(audio, n_fft, hop_length, resolution, bins_per_octave);
}

// ============================================================================
// Features - Pitch
// ============================================================================

val js_pitch_yin(val samples, int sample_rate, int frame_length, int hop_length, float fmin,
                 float fmax, float threshold, bool fill_na) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  PitchConfig config;
  config.frame_length = frame_length;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  config.threshold = threshold;
  config.fill_na = fill_na;

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
                  float fmax, float threshold, bool fill_na) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  PitchConfig config;
  config.frame_length = frame_length;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  config.threshold = threshold;
  config.fill_na = fill_na;

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

TempogramMode tempogramModeFromValue(val mode) {
  if (mode.isUndefined() || mode.isNull()) return TempogramMode::kAutocorrelation;
  if (mode.typeOf().as<std::string>() == "number") {
    const int mode_id = mode.as<int>();
    if (mode_id == SONARE_TEMPOGRAM_AUTOCORRELATION) return TempogramMode::kAutocorrelation;
    if (mode_id == SONARE_TEMPOGRAM_COSINE) return TempogramMode::kCosine;
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "tempogram mode must be 'autocorrelation' or 'cosine'");
  }
  const std::string value = mode.as<std::string>();
  if (value == "autocorrelation" || value == "auto" || value == "ac") {
    return TempogramMode::kAutocorrelation;
  }
  if (value == "cosine") return TempogramMode::kCosine;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "tempogram mode must be 'autocorrelation' or 'cosine'");
}

val js_tempogram(val onset_envelope, int sample_rate, int hop_length, int win_length, val mode) {
  std::vector<float> data = float32ArrayToVector(onset_envelope);
  TempogramConfig config;
  config.hop_length = hop_length;
  config.win_length = win_length;
  config.mode = tempogramModeFromValue(mode);
  auto result = tempogram(data, sample_rate, config);
  val out = val::object();
  out.set("nFrames", static_cast<int>(data.size()));
  out.set("winLength", win_length);
  out.set("data", vectorToFloat32Array(result));
  return out;
}

val js_cyclic_tempogram(val onset_envelope, int sample_rate, int hop_length, int win_length,
                        float bpm_min, int n_bins) {
  std::vector<float> data = float32ArrayToVector(onset_envelope);
  TempogramConfig config;
  config.hop_length = hop_length;
  config.win_length = win_length;
  config.center = true;
  config.norm = false;
  auto result = cyclic_tempogram(data, sample_rate, config, bpm_min, n_bins);
  val out = val::object();
  out.set("nFrames", static_cast<int>(data.size()));
  out.set("nBins", n_bins);
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

val js_onset_envelope(val samples, int sample_rate, int n_fft, int hop_length, int n_mels) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  MelConfig mel_config;
  mel_config.n_fft = n_fft;
  mel_config.hop_length = hop_length;
  mel_config.n_mels = n_mels;
  return vectorToFloat32Array(compute_onset_strength(audio, mel_config, OnsetConfig()));
}

val js_fourier_tempogram(val onset_envelope, int sample_rate, int hop_length, int win_length) {
  std::vector<float> data = float32ArrayToVector(onset_envelope);
  TempogramConfig config;
  config.hop_length = hop_length;
  config.win_length = win_length;
  auto result = fourier_tempogram(data, sample_rate, config);
  val out = val::object();
  out.set("nBins", win_length / 2 + 1);
  out.set("nFrames", static_cast<int>(data.size()));
  out.set("data", vectorToFloat32Array(result));
  return out;
}

val js_tempogram_ratio(val tempogram_data, int win_length, int sample_rate, int hop_length) {
  std::vector<float> data = float32ArrayToVector(tempogram_data);
  return vectorToFloat32Array(tempogram_ratio(data, win_length, sample_rate, hop_length));
}

// ============================================================================
// Analysis - LUFS metering
// ============================================================================

val js_lufs(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  metering::LufsResult result = metering::lufs(audio);
  val out = val::object();
  out.set("integratedLufs", result.integrated_lufs);
  out.set("momentaryLufs", result.momentary_lufs);
  out.set("shortTermLufs", result.short_term_lufs);
  out.set("loudnessRange", result.loudness_range);
  return out;
}

val js_momentary_lufs(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  return vectorToFloat32Array(metering::momentary_lufs(audio));
}

val js_short_term_lufs(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  return vectorToFloat32Array(metering::short_term_lufs(audio));
}

// ITU-R BS.1770-4 multi-channel loudness over an interleaved buffer. Mirrors
// the C ABI sonare_lufs_interleaved. @p samples holds frames * channels values
// in channel-interleaved order. Returns the SonareLufsResult fields as
// { integratedLufs, momentaryLufs, shortTermLufs, loudnessRange }.
val js_lufs_interleaved(val samples, int channels, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  // Derive the per-channel frame count from the interleaved buffer length so the
  // JS/Python facades share one (samples, channels, sampleRate) signature.
  const size_t frames = channels > 0 ? data.size() / static_cast<size_t>(channels) : 0;
  metering::LufsResult result =
      metering::lufs_interleaved(data.data(), frames, channels, sample_rate);
  val out = val::object();
  out.set("integratedLufs", result.integrated_lufs);
  out.set("momentaryLufs", result.momentary_lufs);
  out.set("shortTermLufs", result.short_term_lufs);
  out.set("loudnessRange", result.loudness_range);
  return out;
}

// EBU R128 / Tech 3342 Loudness Range (LRA) in LU for a mono buffer. Mirrors
// the C ABI sonare_ebur128_loudness_range.
float js_ebur128_loudness_range(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  return metering::ebur128_loudness_range(audio);
}

// ============================================================================
// Metering — offline basic / true-peak / clipping / dynamic-range
// ============================================================================

float js_metering_peak_db(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  return metering::peak_db(audio);
}

float js_metering_rms_db(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  return metering::rms_db(audio);
}

float js_metering_crest_factor_db(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  return metering::crest_factor_db(audio);
}

float js_metering_dc_offset(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  return metering::dc_offset(audio);
}

float js_metering_true_peak_db(val samples, int sample_rate, int oversample_factor) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  const int factor = oversample_factor <= 0 ? 4 : oversample_factor;
  return metering::true_peak_db(audio, factor);
}

val js_metering_detect_clipping(val samples, int sample_rate, float threshold,
                                int min_region_samples) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  const float effective_threshold = threshold <= 0.0f ? 0.999f : threshold;
  const size_t effective_min =
      min_region_samples <= 0 ? 1u : static_cast<size_t>(min_region_samples);
  metering::ClippingResult result =
      metering::detect_clipping(audio, effective_threshold, effective_min);
  val regions = val::array();
  for (size_t i = 0; i < result.regions.size(); ++i) {
    val region = val::object();
    region.set("startSample", static_cast<double>(result.regions[i].start_sample));
    region.set("endSample", static_cast<double>(result.regions[i].end_sample));
    region.set("length", static_cast<double>(result.regions[i].length));
    region.set("peak", result.regions[i].peak);
    regions.call<void>("push", region);
  }
  val out = val::object();
  out.set("clippedSamples", static_cast<double>(result.clipped_samples));
  out.set("clippingRatio", result.clipping_ratio);
  out.set("maxClippedPeak", result.max_clipped_peak);
  out.set("regions", regions);
  return out;
}

val js_metering_dynamic_range(val samples, int sample_rate, float window_sec, float hop_sec,
                              float low_percentile, float high_percentile) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  metering::DynamicRangeConfig cfg;
  if (window_sec > 0.0f) cfg.window_sec = window_sec;
  if (hop_sec > 0.0f) cfg.hop_sec = hop_sec;
  if (low_percentile > 0.0f) cfg.low_percentile = low_percentile;
  if (high_percentile > 0.0f) cfg.high_percentile = high_percentile;
  if (cfg.low_percentile >= cfg.high_percentile) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "meteringDynamicRange: lowPercentile must be smaller than highPercentile");
  }
  metering::DynamicRangeResult result = metering::dynamic_range(audio, cfg);
  val out = val::object();
  out.set("dynamicRangeDb", result.dynamic_range_db);
  out.set("lowPercentileDb", result.low_percentile_db);
  out.set("highPercentileDb", result.high_percentile_db);
  out.set("windowRmsDb", vectorToFloat32Array(result.window_rms_db));
  return out;
}

// ============================================================================
// Metering — stereo / phase-scope / spectrum (offline)
// ============================================================================

namespace {

void ensureStereoPair(const val& left, const val& right, int sample_rate, const char* fn_label,
                      std::vector<float>* out_left, std::vector<float>* out_right) {
  if (sample_rate <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  std::string(fn_label) + ": sampleRate must be positive");
  }
  *out_left = float32ArrayToVector(left);
  *out_right = float32ArrayToVector(right);
  if (out_left->size() != out_right->size()) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        std::string(fn_label) + ": left and right must have the same length");
  }
}

}  // namespace

float js_metering_stereo_correlation(val left, val right, int sample_rate) {
  std::vector<float> l;
  std::vector<float> r;
  ensureStereoPair(left, right, sample_rate, "meteringStereoCorrelation", &l, &r);
  return metering::correlation(l.data(), r.data(), l.size());
}

float js_metering_stereo_width(val left, val right, int sample_rate) {
  std::vector<float> l;
  std::vector<float> r;
  ensureStereoPair(left, right, sample_rate, "meteringStereoWidth", &l, &r);
  return metering::stereo_width(l.data(), r.data(), l.size());
}

val js_metering_vectorscope(val left, val right, int sample_rate) {
  std::vector<float> l;
  std::vector<float> r;
  ensureStereoPair(left, right, sample_rate, "meteringVectorscope", &l, &r);
  std::vector<metering::VectorscopePoint> points =
      metering::vectorscope(l.data(), r.data(), l.size());
  std::vector<float> mid(points.size());
  std::vector<float> side(points.size());
  for (size_t i = 0; i < points.size(); ++i) {
    mid[i] = points[i].mid;
    side[i] = points[i].side;
  }
  val out = val::object();
  out.set("mid", vectorToFloat32Array(mid));
  out.set("side", vectorToFloat32Array(side));
  return out;
}

val js_metering_phase_scope(val left, val right, int sample_rate) {
  std::vector<float> l;
  std::vector<float> r;
  ensureStereoPair(left, right, sample_rate, "meteringPhaseScope", &l, &r);
  metering::PhaseScopeResult result = metering::phase_scope(l.data(), r.data(), l.size());
  std::vector<float> mid(result.points.size());
  std::vector<float> side(result.points.size());
  std::vector<float> radius(result.points.size());
  std::vector<float> angle(result.points.size());
  for (size_t i = 0; i < result.points.size(); ++i) {
    mid[i] = result.points[i].mid;
    side[i] = result.points[i].side;
    radius[i] = result.points[i].radius;
    angle[i] = result.points[i].angle_rad;
  }
  val out = val::object();
  out.set("mid", vectorToFloat32Array(mid));
  out.set("side", vectorToFloat32Array(side));
  out.set("radius", vectorToFloat32Array(radius));
  out.set("angleRad", vectorToFloat32Array(angle));
  out.set("correlation", result.correlation);
  out.set("averageAbsAngleRad", result.average_abs_angle_rad);
  out.set("maxRadius", result.max_radius);
  return out;
}

val js_metering_spectrum(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  metering::SpectrumConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("nFft")) {
      const int n = options["nFft"].as<int>();
      if (n > 0) cfg.n_fft = n;
    }
    if (options.hasOwnProperty("applyOctaveSmoothing")) {
      cfg.apply_octave_smoothing = options["applyOctaveSmoothing"].as<bool>();
    }
    if (options.hasOwnProperty("octaveFraction")) {
      const int f = options["octaveFraction"].as<int>();
      if (f > 0) cfg.octave_fraction = f;
    }
    if (options.hasOwnProperty("dbRef")) {
      const float ref = options["dbRef"].as<float>();
      if (ref > 0.0f) cfg.db_ref = ref;
    }
    if (options.hasOwnProperty("dbAmin")) {
      const float amin = options["dbAmin"].as<float>();
      if (amin > 0.0f) cfg.db_amin = amin;
    }
  }
  if ((cfg.n_fft & (cfg.n_fft - 1)) != 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "meteringSpectrum: nFft must be a power of two");
  }
  metering::SpectrumResult result = metering::spectrum(audio, cfg);
  val out = val::object();
  out.set("frequencies", vectorToFloat32Array(result.frequencies));
  out.set("magnitude", vectorToFloat32Array(result.magnitude));
  out.set("power", vectorToFloat32Array(result.power));
  out.set("db", vectorToFloat32Array(result.db));
  out.set("nFft", result.n_fft);
  out.set("sampleRate", result.sample_rate);
  return out;
}

// ============================================================================
// Mastering — offline repair processors (declick / denoise_classical)
// ============================================================================

val js_mastering_repair_declick(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  mastering::repair::DeclickConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("threshold")) {
      cfg.threshold = options["threshold"].as<float>();
    }
    if (options.hasOwnProperty("neighborRatio")) {
      cfg.neighbor_ratio = options["neighborRatio"].as<float>();
    }
    if (options.hasOwnProperty("maxClickSamples")) {
      const int v = options["maxClickSamples"].as<int>();
      if (v <= 0) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "masteringRepairDeclick: maxClickSamples must be positive");
      }
      cfg.max_click_samples = static_cast<size_t>(v);
    }
    if (options.hasOwnProperty("lpcOrder")) {
      cfg.lpc_order = options["lpcOrder"].as<int>();
    }
    if (options.hasOwnProperty("residualRatio")) {
      cfg.residual_ratio = options["residualRatio"].as<float>();
    }
  }
  Audio result = mastering::repair::declick(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

namespace {

mastering::repair::DenoiseMode parseDenoiseMode(const std::string& name,
                                                mastering::repair::DenoiseMode fallback) {
  std::string s = name;
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "logmmse" || s == "log_mmse" || s == "lsa") {
    return mastering::repair::DenoiseMode::LogMmse;
  }
  if (s == "mmsestsa" || s == "mmse_stsa" || s == "stsa") {
    return mastering::repair::DenoiseMode::MmseStsa;
  }
  if (s == "spectralsubtraction" || s == "spectral_subtraction" || s == "ss") {
    return mastering::repair::DenoiseMode::SpectralSubtraction;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown denoise mode: " + name);
}

mastering::repair::DenoiseNoiseEstimator parseDenoiseNoiseEstimator(
    const std::string& name, mastering::repair::DenoiseNoiseEstimator fallback) {
  std::string s = name;
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "quantile") return mastering::repair::DenoiseNoiseEstimator::Quantile;
  if (s == "mcra") return mastering::repair::DenoiseNoiseEstimator::Mcra;
  if (s == "imcra") return mastering::repair::DenoiseNoiseEstimator::Imcra;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown denoise noise estimator: " + name);
}

}  // namespace

val js_mastering_repair_denoise_classical(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  mastering::repair::DenoiseClassicalConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("mode")) {
      cfg.mode = parseDenoiseMode(options["mode"].as<std::string>(), cfg.mode);
    }
    if (options.hasOwnProperty("noiseEstimator")) {
      cfg.noise_estimator = parseDenoiseNoiseEstimator(options["noiseEstimator"].as<std::string>(),
                                                       cfg.noise_estimator);
    }
    if (options.hasOwnProperty("nFft")) cfg.n_fft = options["nFft"].as<int>();
    if (options.hasOwnProperty("hopLength")) cfg.hop_length = options["hopLength"].as<int>();
    if (options.hasOwnProperty("ddAlpha")) cfg.dd_alpha = options["ddAlpha"].as<float>();
    if (options.hasOwnProperty("gainFloor")) cfg.gain_floor = options["gainFloor"].as<float>();
    if (options.hasOwnProperty("overSubtraction")) {
      cfg.over_subtraction = options["overSubtraction"].as<float>();
    }
    if (options.hasOwnProperty("spectralFloor")) {
      cfg.spectral_floor = options["spectralFloor"].as<float>();
    }
    if (options.hasOwnProperty("noiseEstimationQuantile")) {
      cfg.noise_estimation_quantile = options["noiseEstimationQuantile"].as<float>();
    }
    if (options.hasOwnProperty("speechPresenceGain")) {
      cfg.speech_presence_gain = options["speechPresenceGain"].as<bool>();
    }
    if (options.hasOwnProperty("gainSmoothing")) {
      cfg.gain_smoothing = options["gainSmoothing"].as<bool>();
    }
  }
  if (cfg.n_fft <= 0 || (cfg.n_fft & (cfg.n_fft - 1)) != 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDenoiseClassical: nFft must be a positive power of two");
  }
  if (cfg.hop_length <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "masteringRepairDenoiseClassical: hopLength must be positive");
  }
  Audio result = mastering::repair::denoise_classical(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

val js_mastering_repair_declip(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  mastering::repair::DeclipConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("clipThreshold")) {
      cfg.clip_threshold = options["clipThreshold"].as<float>();
    }
    if (options.hasOwnProperty("lpcOrder")) cfg.lpc_order = options["lpcOrder"].as<int>();
    if (options.hasOwnProperty("iterations")) cfg.iterations = options["iterations"].as<int>();
    if (options.hasOwnProperty("lpcBlend")) cfg.lpc_blend = options["lpcBlend"].as<float>();
  }
  Audio result = mastering::repair::declip(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

namespace {

mastering::repair::DecrackleMode parseDecrackleMode(const std::string& name,
                                                    mastering::repair::DecrackleMode fallback) {
  std::string s = name;
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "median") return mastering::repair::DecrackleMode::Median;
  if (s == "waveletshrinkage" || s == "wavelet_shrinkage" || s == "wavelet") {
    return mastering::repair::DecrackleMode::WaveletShrinkage;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown decrackle mode: " + name);
}

mastering::repair::TrimSilenceMode parseTrimSilenceMode(
    const std::string& name, mastering::repair::TrimSilenceMode fallback) {
  std::string s = name;
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "peak") return mastering::repair::TrimSilenceMode::Peak;
  if (s == "lufsgated" || s == "lufs_gated" || s == "lufs") {
    return mastering::repair::TrimSilenceMode::LufsGated;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown trim silence mode: " + name);
}

}  // namespace

val js_mastering_repair_decrackle(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  mastering::repair::DecrackleConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("threshold")) cfg.threshold = options["threshold"].as<float>();
    if (options.hasOwnProperty("mode")) {
      cfg.mode = parseDecrackleMode(options["mode"].as<std::string>(), cfg.mode);
    }
    if (options.hasOwnProperty("levels")) cfg.levels = options["levels"].as<int>();
  }
  Audio result = mastering::repair::decrackle(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

val js_mastering_repair_dehum(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  mastering::repair::DehumConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("fundamentalHz")) {
      cfg.fundamental_hz = options["fundamentalHz"].as<float>();
    }
    if (options.hasOwnProperty("harmonics")) cfg.harmonics = options["harmonics"].as<int>();
    if (options.hasOwnProperty("q")) cfg.q = options["q"].as<float>();
    if (options.hasOwnProperty("adaptive")) cfg.adaptive = options["adaptive"].as<bool>();
    if (options.hasOwnProperty("searchRangeHz")) {
      cfg.search_range_hz = options["searchRangeHz"].as<float>();
    }
    if (options.hasOwnProperty("adaptation")) cfg.adaptation = options["adaptation"].as<float>();
    if (options.hasOwnProperty("frameSize")) cfg.frame_size = options["frameSize"].as<int>();
    if (options.hasOwnProperty("pllBandwidth")) {
      cfg.pll_bandwidth = options["pllBandwidth"].as<float>();
    }
  }
  Audio result = mastering::repair::dehum(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

val js_mastering_repair_dereverb_classical(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  mastering::repair::DereverbClassicalConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("threshold")) cfg.threshold = options["threshold"].as<float>();
    if (options.hasOwnProperty("attenuation")) {
      cfg.attenuation = options["attenuation"].as<float>();
    }
    if (options.hasOwnProperty("nFft")) cfg.n_fft = options["nFft"].as<int>();
    if (options.hasOwnProperty("hopLength")) cfg.hop_length = options["hopLength"].as<int>();
    if (options.hasOwnProperty("t60Sec")) cfg.t60_sec = options["t60Sec"].as<float>();
    if (options.hasOwnProperty("lateDelayMs")) {
      cfg.late_delay_ms = options["lateDelayMs"].as<float>();
    }
    if (options.hasOwnProperty("overSubtraction")) {
      cfg.over_subtraction = options["overSubtraction"].as<float>();
    }
    if (options.hasOwnProperty("spectralFloor")) {
      cfg.spectral_floor = options["spectralFloor"].as<float>();
    }
    if (options.hasOwnProperty("wpeEnabled")) {
      cfg.wpe_enabled = options["wpeEnabled"].as<bool>();
    }
    if (options.hasOwnProperty("wpeIterations")) {
      cfg.wpe_iterations = options["wpeIterations"].as<int>();
    }
    if (options.hasOwnProperty("wpeTaps")) cfg.wpe_taps = options["wpeTaps"].as<int>();
    if (options.hasOwnProperty("wpeStrength")) {
      cfg.wpe_strength = options["wpeStrength"].as<float>();
    }
  }
  if (cfg.n_fft <= 0 || (cfg.n_fft & (cfg.n_fft - 1)) != 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDereverbClassical: nFft must be a positive power of two");
  }
  if (cfg.hop_length <= 0 || cfg.hop_length > cfg.n_fft) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDereverbClassical: hopLength must be in (0, nFft]");
  }
  Audio result = mastering::repair::dereverb_classical(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

val js_mastering_repair_trim_silence(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  mastering::repair::TrimSilenceConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("threshold")) cfg.threshold = options["threshold"].as<float>();
    if (options.hasOwnProperty("paddingSamples")) {
      const int v = options["paddingSamples"].as<int>();
      if (v < 0) {
        throw sonare::SonareException(
            sonare::ErrorCode::InvalidParameter,
            "masteringRepairTrimSilence: paddingSamples must be non-negative");
      }
      cfg.padding_samples = static_cast<size_t>(v);
    }
    if (options.hasOwnProperty("mode")) {
      cfg.mode = parseTrimSilenceMode(options["mode"].as<std::string>(), cfg.mode);
    }
    if (options.hasOwnProperty("gateLufs")) cfg.gate_lufs = options["gateLufs"].as<float>();
    if (options.hasOwnProperty("windowMs")) cfg.window_ms = options["windowMs"].as<float>();
  }
  Audio result = mastering::repair::trim_silence(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

// ============================================================================
// Mastering — offline dynamics processors (compressor / gate / transient_shaper)
// ============================================================================

namespace {

mastering::dynamics::DetectorMode parseCompressorDetector(
    val value, mastering::dynamics::DetectorMode fallback) {
  const std::string type = value.typeOf().as<std::string>();
  if (type == "number") {
    const int code = value.as<int>();
    switch (code) {
      case 0:
        return mastering::dynamics::DetectorMode::Peak;
      case 1:
        return mastering::dynamics::DetectorMode::Rms;
      case 2:
        return mastering::dynamics::DetectorMode::LogRms;
      default:
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "masteringDynamicsCompressor: unknown detector code");
    }
  }
  if (type == "string") {
    std::string s = value.as<std::string>();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "peak") return mastering::dynamics::DetectorMode::Peak;
    if (s == "rms") return mastering::dynamics::DetectorMode::Rms;
    if (s == "log_rms" || s == "logrms") return mastering::dynamics::DetectorMode::LogRms;
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "masteringDynamicsCompressor: unknown detector mode: " + s);
  }
  return fallback;
}

template <typename Processor>
void runDynamicsOffline(Processor& processor, std::vector<float>& samples, int sample_rate,
                        int& latency_samples_out) {
  if (samples.empty()) {
    latency_samples_out = 0;
    return;
  }
  processor.prepare(sample_rate, static_cast<int>(samples.size()));
  float* channels[] = {samples.data()};
  processor.process(channels, 1, static_cast<int>(samples.size()));
  latency_samples_out = processor.latency_samples();
}

val makeDynamicsResult(const std::vector<float>& samples, int latency_samples) {
  val out = val::object();
  out.set("samples", vectorToFloat32Array(samples));
  out.set("latencySamples", latency_samples);
  return out;
}

}  // namespace

val js_mastering_dynamics_compressor(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  mastering::dynamics::CompressorConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("thresholdDb")) {
      cfg.threshold_db = options["thresholdDb"].as<float>();
    }
    if (options.hasOwnProperty("ratio")) cfg.ratio = options["ratio"].as<float>();
    if (options.hasOwnProperty("attackMs")) cfg.attack_ms = options["attackMs"].as<float>();
    if (options.hasOwnProperty("releaseMs")) cfg.release_ms = options["releaseMs"].as<float>();
    if (options.hasOwnProperty("kneeDb")) cfg.knee_db = options["kneeDb"].as<float>();
    if (options.hasOwnProperty("makeupGainDb")) {
      cfg.makeup_gain_db = options["makeupGainDb"].as<float>();
    }
    if (options.hasOwnProperty("autoMakeup")) cfg.auto_makeup = options["autoMakeup"].as<bool>();
    if (options.hasOwnProperty("detector")) {
      cfg.detector = parseCompressorDetector(options["detector"], cfg.detector);
    }
    if (options.hasOwnProperty("sidechainHpfEnabled")) {
      cfg.sidechain_hpf_enabled = options["sidechainHpfEnabled"].as<bool>();
    }
    if (options.hasOwnProperty("sidechainHpfHz")) {
      cfg.sidechain_hpf_hz = options["sidechainHpfHz"].as<float>();
    }
    if (options.hasOwnProperty("pdrTimeMs")) cfg.pdr_time_ms = options["pdrTimeMs"].as<float>();
    if (options.hasOwnProperty("pdrReleaseScale")) {
      cfg.pdr_release_scale = options["pdrReleaseScale"].as<float>();
    }
  }
  mastering::dynamics::Compressor processor(cfg);
  int latency = 0;
  runDynamicsOffline(processor, data, sample_rate, latency);
  return makeDynamicsResult(data, latency);
}

val js_mastering_dynamics_gate(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  mastering::dynamics::GateConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("thresholdDb")) {
      cfg.threshold_db = options["thresholdDb"].as<float>();
    }
    if (options.hasOwnProperty("attackMs")) cfg.attack_ms = options["attackMs"].as<float>();
    if (options.hasOwnProperty("releaseMs")) cfg.release_ms = options["releaseMs"].as<float>();
    if (options.hasOwnProperty("rangeDb")) cfg.range_db = options["rangeDb"].as<float>();
    if (options.hasOwnProperty("holdMs")) cfg.hold_ms = options["holdMs"].as<float>();
    if (options.hasOwnProperty("closeThresholdDb")) {
      cfg.close_threshold_db = options["closeThresholdDb"].as<float>();
    }
    if (options.hasOwnProperty("keyHpfHz")) cfg.key_hpf_hz = options["keyHpfHz"].as<float>();
  }
  mastering::dynamics::Gate processor(cfg);
  int latency = 0;
  runDynamicsOffline(processor, data, sample_rate, latency);
  return makeDynamicsResult(data, latency);
}

val js_mastering_dynamics_transient_shaper(val samples, int sample_rate, val options) {
  std::vector<float> data = float32ArrayToVector(samples);
  mastering::dynamics::TransientShaperConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (options.hasOwnProperty("attackGainDb")) {
      cfg.attack_gain_db = options["attackGainDb"].as<float>();
    }
    if (options.hasOwnProperty("sustainGainDb")) {
      cfg.sustain_gain_db = options["sustainGainDb"].as<float>();
    }
    if (options.hasOwnProperty("fastAttackMs")) {
      cfg.fast_attack_ms = options["fastAttackMs"].as<float>();
    }
    if (options.hasOwnProperty("fastReleaseMs")) {
      cfg.fast_release_ms = options["fastReleaseMs"].as<float>();
    }
    if (options.hasOwnProperty("slowAttackMs")) {
      cfg.slow_attack_ms = options["slowAttackMs"].as<float>();
    }
    if (options.hasOwnProperty("slowReleaseMs")) {
      cfg.slow_release_ms = options["slowReleaseMs"].as<float>();
    }
    if (options.hasOwnProperty("sensitivity")) {
      cfg.sensitivity = options["sensitivity"].as<float>();
    }
    if (options.hasOwnProperty("maxGainDb")) cfg.max_gain_db = options["maxGainDb"].as<float>();
    if (options.hasOwnProperty("gainSmoothingMs")) {
      cfg.gain_smoothing_ms = options["gainSmoothingMs"].as<float>();
    }
    if (options.hasOwnProperty("lookaheadMs")) {
      cfg.lookahead_ms = options["lookaheadMs"].as<float>();
    }
  }
  mastering::dynamics::TransientShaper processor(cfg);
  int latency = 0;
  runDynamicsOffline(processor, data, sample_rate, latency);
  return makeDynamicsResult(data, latency);
}

// ============================================================================
// Editing — 12-TET scale quantizer
// ============================================================================

namespace {

editing::pitch_editor::ScaleQuantizerConfig makeScaleConfig(int root, int mode_mask,
                                                            float reference_midi) {
  if (root < 0 || root > 11) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "scaleQuantizer: root must be in [0, 11]");
  }
  if (mode_mask == 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "scaleQuantizer: modeMask must be non-zero");
  }
  editing::pitch_editor::ScaleQuantizerConfig cfg;
  cfg.root = root;
  cfg.mode_mask = static_cast<uint16_t>(mode_mask);
  if (reference_midi > 0.0f) cfg.reference_midi = reference_midi;
  return cfg;
}

}  // namespace

float js_scale_quantize_midi(int root, int mode_mask, float midi, float reference_midi) {
  editing::pitch_editor::ScaleQuantizer q(makeScaleConfig(root, mode_mask, reference_midi));
  return q.quantize_midi(midi);
}

float js_scale_correction_semitones(int root, int mode_mask, float midi, float reference_midi) {
  editing::pitch_editor::ScaleQuantizer q(makeScaleConfig(root, mode_mask, reference_midi));
  return q.correction_semitones(midi);
}

bool js_scale_pitch_class_enabled(int root, int mode_mask, int pitch_class) {
  if (pitch_class < 0 || pitch_class > 11) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "scalePitchClassEnabled: pitchClass must be in [0, 11]");
  }
  editing::pitch_editor::ScaleQuantizer q(makeScaleConfig(root, mode_mask, 0.0f));
  return q.pitch_class_enabled(pitch_class);
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
///
/// Uses the same single-bulk-memcpy path as vectorToFloat32Array/Int32Array: a
/// non-owning typed_memory_view over the C++ buffer plus a JS-side
/// TypedArray.prototype.set, avoiding one JS<->WASM boundary crossing per
/// element (the per-element set() defeats the bandwidth-reduction purpose of the
/// quantized readFramesU8/readFramesI16 fast paths).
val vectorToUint8Array(const std::vector<uint8_t>& vec) {
  const size_t n = vec.size();
  val result = val::global("Uint8Array").new_(n);
  if (n == 0) return result;
  val view = val(typed_memory_view(n, vec.data()));
  result.call<void>("set", view);
  return result;
}

/// @brief Helper to convert int16 vector to Int16Array.
val vectorToInt16Array(const std::vector<int16_t>& vec) {
  const size_t n = vec.size();
  val result = val::global("Int16Array").new_(n);
  if (n == 0) return result;
  val view = val(typed_memory_view(n, vec.data()));
  result.call<void>("set", view);
  return result;
}

/// @brief JavaScript wrapper for StreamAnalyzer.
class StreamAnalyzerWrapper {
 public:
  StreamAnalyzerWrapper(int sample_rate, int n_fft, int hop_length, int n_mels, float fmin,
                        float fmax, float tuning_ref_hz, bool compute_magnitude, bool compute_mel,
                        bool compute_chroma, bool compute_onset, bool compute_spectral,
                        int emit_every_n_frames, int magnitude_downsample,
                        float key_update_interval_sec, float bpm_update_interval_sec, int window,
                        int output_format) {
    StreamConfig config;
    config.sample_rate = sample_rate;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    config.n_mels = n_mels;
    config.window = window == 1   ? WindowType::Hamming
                    : window == 2 ? WindowType::Blackman
                    : window == 3 ? WindowType::Rectangular
                                  : WindowType::Hann;
    config.fmin = fmin;
    config.fmax = fmax;
    config.tuning_ref_hz = tuning_ref_hz;
    config.compute_magnitude = compute_magnitude;
    config.compute_mel = compute_mel;
    config.compute_chroma = compute_chroma;
    config.compute_onset = compute_onset;
    config.compute_spectral = compute_spectral;
    config.emit_every_n_frames = emit_every_n_frames;
    config.magnitude_downsample = magnitude_downsample;
    config.output_format = output_format == 1   ? OutputFormat::Int16
                           : output_format == 2 ? OutputFormat::Uint8
                                                : OutputFormat::Float32;
    config.key_update_interval_sec = key_update_interval_sec;
    config.bpm_update_interval_sec = bpm_update_interval_sec;
    config_ = config;
    analyzer_ = std::make_unique<StreamAnalyzer>(config);
  }

  /// @brief Returns the sample rate.
  int sampleRate() const { return config_.sample_rate; }

  void process(val samples) {
    std::vector<float> data = float32ArrayToVector(samples);
    analyzer_->process(data.data(), data.size());
  }

  void processWithOffset(val samples, size_t sample_offset) {
    std::vector<float> data = float32ArrayToVector(samples);
    analyzer_->process(data.data(), data.size(), sample_offset);
  }

  size_t availableFrames() const { return analyzer_->available_frames(); }

  /// @brief Reads frames in Float32 SOA format.
  val readFramesSoa(size_t max_frames) {
    FrameBuffer buffer;
    analyzer_->read_frames_soa(max_frames, buffer);

    val out = val::object();
    out.set("nFrames", buffer.n_frames);
    out.set("nMels", config_.n_mels);
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
    estimate.set("chordStartTime", s.estimate.chord_start_time);

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
      c.set("startTime", chord.start_time);
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

// Canonical AutomationCurve ordinals (Linear=0, Exp=1, Hold=2, SCurve=3) are
// shared with the C ABI and other bindings; conversion is a direct cast.
sonare::automation::CurveType automationCurveFromInt(int curve) {
  if (curve < 0 || curve > 3) {
    return sonare::automation::CurveType::Linear;
  }
  return static_cast<sonare::automation::CurveType>(curve);
}

int automationCurveToInt(sonare::automation::CurveType curve) { return static_cast<int>(curve); }

#if defined(SONARE_WITH_GRAPH)

std::unique_ptr<rt::ProcessorBase> makeWasmGraphProcessor(val node) {
  const int type = intProperty(node, "type", 0);
  switch (type) {
    case 0:
      return std::make_unique<rt::PassProcessor>();
    case 1:
      return std::make_unique<rt::GainProcessor>(floatProperty(node, "gainDb", 0.0f));
    default:
      return nullptr;
  }
}
#endif

class RealtimeEngineWasm {
 public:
  RealtimeEngineWasm(double sample_rate, int max_block_size, int command_capacity,
                     int telemetry_capacity) {
    engine_.prepare(sample_rate, max_block_size, capacity(command_capacity),
                    capacity(telemetry_capacity));
  }

  void prepare(double sample_rate, int max_block_size, int command_capacity,
               int telemetry_capacity) {
    engine_.prepare(sample_rate, max_block_size, capacity(command_capacity),
                    capacity(telemetry_capacity));
  }

  void play(int64_t render_frame) {
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kTransportPlay;
    command.sample_time = render_frame;
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue play command");
    }
  }

  void stop(int64_t render_frame) {
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kTransportStop;
    command.sample_time = render_frame;
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue stop command");
    }
  }

  void seekSample(int64_t timeline_sample, int64_t render_frame) {
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kTransportSeekSample;
    command.sample_time = render_frame;
    command.arg.i = timeline_sample;
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue seek command");
    }
  }

  void seekPpq(double ppq, int64_t render_frame) {
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kTransportSeekPpq;
    command.sample_time = render_frame;
    // Engine reads the PPQ scalar from the full-precision double slot
    // (kTransportSeekPpq -> transport_.seek_ppq(command.arg.d)); writing the
    // float slot of the union would surface as garbage. Match the C API.
    command.arg.d = ppq;
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue seek command");
    }
  }

  void setTempo(double bpm) { engine_.set_tempo(bpm); }
  void setTimeSignature(int numerator, int denominator) {
    engine_.set_time_signature(numerator, denominator);
  }
  void setLoop(double start_ppq, double end_ppq, bool enabled) {
    engine_.set_loop(start_ppq, end_ppq, enabled);
  }

  void addParameter(val info) {
    const uint32_t id = static_cast<uint32_t>(intProperty(info, "id", 0));
    if (id == 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "parameter id must be non-zero");
    }
    parameter_strings_.push_back(stringProperty(info, "name", ""));
    parameter_strings_.push_back(stringProperty(info, "unit", ""));
    sonare::automation::ParameterInfo parameter{};
    parameter.id = id;
    parameter.name = parameter_strings_[parameter_strings_.size() - 2].c_str();
    parameter.unit = parameter_strings_[parameter_strings_.size() - 1].c_str();
    parameter.min_value = floatProperty(info, "minValue", 0.0f);
    parameter.max_value = floatProperty(info, "maxValue", 1.0f);
    parameter.default_value = floatProperty(info, "defaultValue", 0.0f);
    parameter.rt_safe = boolProperty(info, "rtSafe", true);
    parameter.default_curve = automationCurveFromInt(intProperty(info, "defaultCurve", 1));
    if (!parameters_.add(parameter)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "duplicate parameter id");
    }
  }

  int parameterCount() const { return static_cast<int>(parameters_.parameter_count()); }

  val parameterInfoByIndex(int index) const {
    sonare::automation::ParameterInfo info{};
    if (index < 0 || !parameters_.parameter_info_by_index(static_cast<size_t>(index), &info)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "parameter index out of range");
    }
    return parameterToVal(info);
  }

  val parameterInfo(int id) const {
    sonare::automation::ParameterInfo info{};
    if (!parameters_.parameter_info(static_cast<uint32_t>(id), &info)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown parameter id");
    }
    return parameterToVal(info);
  }

  void setAutomationLane(int param_id, val points) {
    // NOTE: this surfaces a non-RT-safe parameter synchronously (a throw),
    // whereas setParameter/setParameterSmoothed and the canonical C API
    // (sonare_engine_set_automation_lane) report the same misuse asynchronously
    // via kNonRealtimeSafeParameter telemetry. The synchronous throw is kept
    // here intentionally because setAutomationLane is a control-thread (offline)
    // setter, so an immediate, actionable error is preferable to a deferred
    // telemetry record; the queued realtime writes keep the telemetry contract.
    if (!parameters_.parameter_is_realtime_safe(static_cast<uint32_t>(param_id))) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "parameter is not realtime safe");
    }
    sonare::automation::AutomationLane lane(static_cast<uint32_t>(param_id));
    std::vector<sonare::automation::Breakpoint> breakpoints;
    const int count = points["length"].as<int>();
    breakpoints.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
      val point = points[i];
      breakpoints.push_back({objectProperty(point, "ppq").as<double>(),
                             floatProperty(point, "value", 0.0f),
                             automationCurveFromInt(intProperty(point, "curveToNext", 1))});
    }
    lane.set_points(std::move(breakpoints));
    bool replaced = false;
    for (auto& existing : automation_lanes_) {
      if (existing.target_param_id() == static_cast<uint32_t>(param_id)) {
        existing = std::move(lane);
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      automation_lanes_.push_back(std::move(lane));
    }
    engine_.automation().set_lanes(automation_lanes_);
  }

  int automationLaneCount() const { return static_cast<int>(engine_.automation().lane_count()); }

  void setMarkers(val markers) {
    marker_strings_.clear();
    std::vector<sonare::transport::Marker> prepared;
    const int count = markers["length"].as<int>();
    prepared.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
      val marker = markers[i];
      marker_strings_.push_back(stringProperty(marker, "name", ""));
      prepared.push_back({objectProperty(marker, "ppq").as<double>(),
                          static_cast<uint32_t>(intProperty(marker, "id", i + 1)),
                          marker_strings_.back().c_str()});
    }
    engine_.set_markers(std::move(prepared));
  }

  int markerCount() const { return static_cast<int>(engine_.marker_count()); }

  val markerByIndex(int index) const {
    sonare::transport::Marker marker{};
    if (index < 0 || !engine_.marker_by_index(static_cast<size_t>(index), &marker)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "marker index out of range");
    }
    return markerToVal(marker);
  }

  val marker(int id) const {
    sonare::transport::Marker marker{};
    if (!engine_.marker_by_id(static_cast<uint32_t>(id), &marker)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown marker id");
    }
    return markerToVal(marker);
  }

  void seekMarker(int id, int64_t render_frame) {
    // Mirror the C API (sonare_engine_seek_marker): a sample-accurate seek is
    // queued as a kSeekMarker command so it lands at the requested render frame
    // instead of mutating transport state immediately.
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kSeekMarker;
    command.target_id = static_cast<uint32_t>(id);
    command.sample_time = render_frame;
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue seek marker command");
    }
  }

  void setParameter(int param_id, float value, int64_t render_frame) {
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kSetParam;
    command.target_id = static_cast<uint32_t>(param_id);
    command.sample_time = render_frame;
    command.arg.f = value;
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue set parameter command");
    }
  }

  void setParameterSmoothed(int param_id, float value, int64_t render_frame) {
    sonare::rt::Command command{};
    command.type = sonare::rt::CommandType::kSetParamSmoothed;
    command.target_id = static_cast<uint32_t>(param_id);
    command.sample_time = render_frame;
    command.arg.f = value;
    if (!engine_.push_command(command)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to queue set parameter command");
    }
  }

  val getTransportState() const {
    const sonare::transport::TransportState state = engine_.transport().snapshot();
    val out = val::object();
    out.set("playing", state.playing);
    out.set("looping", state.looping);
    out.set("renderFrame", static_cast<double>(state.render_frame));
    out.set("samplePosition", static_cast<double>(state.sample_position));
    out.set("ppq", state.ppq_position);
    out.set("bpm", state.bpm);
    out.set("barStartPpq", state.bar_start_ppq);
    out.set("barCount", static_cast<double>(state.bar_count));
    val time_signature = val::object();
    time_signature.set("numerator", state.time_sig.numerator);
    time_signature.set("denominator", state.time_sig.denominator);
    // The transport TimeSignature carries no confidence; mirror the C ABI which
    // reports a fixed 1.0 for the engine-driven (authoritative) time signature.
    time_signature.set("confidence", 1.0f);
    out.set("timeSignature", time_signature);
    out.set("loopStartPpq", state.loop_start_ppq);
    out.set("loopEndPpq", state.loop_end_ppq);
    out.set("sampleRate", state.sample_rate);
    return out;
  }

  void setLoopFromMarkers(int start_marker_id, int end_marker_id) {
    if (!engine_.set_loop_from_markers(static_cast<uint32_t>(start_marker_id),
                                       static_cast<uint32_t>(end_marker_id))) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown loop marker id");
    }
  }

  void setMetronome(val config) {
    sonare::engine::MetronomeConfig metronome{};
    metronome.enabled = boolProperty(config, "enabled", false);
    metronome.beat_gain = floatProperty(config, "beatGain", 0.35f);
    metronome.accent_gain = floatProperty(config, "accentGain", 0.7f);
    metronome.click_samples = intProperty(config, "clickSamples", 96);
    // clickSeconds is optional: a value > 0 overrides the engine's 2 ms default
    // click length (parity with the C-ABI/Python/Node click_seconds field). A
    // missing or 0 value leaves the struct default in place.
    const double click_seconds = hasProperty(config, "clickSeconds")
                                     ? objectProperty(config, "clickSeconds").as<double>()
                                     : 0.0;
    if (click_seconds > 0.0) {
      metronome.click_seconds = click_seconds;
    }
    engine_.set_metronome_config(metronome);
  }

  val metronome() const {
    const sonare::engine::MetronomeConfig& config = engine_.metronome_config();
    val out = val::object();
    out.set("enabled", config.enabled);
    out.set("beatGain", config.beat_gain);
    out.set("accentGain", config.accent_gain);
    out.set("clickSamples", config.click_samples);
    out.set("clickSeconds", config.click_seconds);
    return out;
  }

  int64_t countInEndSample(int64_t start_sample, int bars) const {
    return engine_.count_in_end_sample(start_sample, bars);
  }

  void setGraph(val spec) {
#if defined(SONARE_WITH_GRAPH)
    auto graph = std::make_unique<sonare::graph::Graph>();
    val nodes = spec["nodes"];
    const int node_count = nodes["length"].as<int>();
    if (node_count <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "graph nodes must not be empty");
    }
    const int num_channels = intProperty(spec, "numChannels", 2);
    for (int i = 0; i < node_count; ++i) {
      val node = nodes[i];
      auto processor = makeWasmGraphProcessor(node);
      if (!processor) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "unsupported graph node type");
      }
      const std::string id = stringProperty(node, "id", "");
      const int ports = intProperty(node, "numPorts", num_channels);
      if (!graph->add_node(id, std::move(processor), ports)) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "failed to add graph node");
      }
    }
    val connections = spec["connections"];
    const int connection_count = connections["length"].as<int>();
    for (int i = 0; i < connection_count; ++i) {
      val connection = connections[i];
      sonare::graph::Connection graph_connection{};
      graph_connection.source_node = stringProperty(connection, "sourceNode", "");
      graph_connection.source_port = intProperty(connection, "sourcePort", 0);
      graph_connection.dest_node = stringProperty(connection, "destNode", "");
      graph_connection.dest_port = intProperty(connection, "destPort", 0);
      graph_connection.mix = intProperty(connection, "mix", 1) == 0
                                 ? sonare::graph::Connection::Mix::Replace
                                 : sonare::graph::Connection::Mix::Add;
      if (!graph->connect(std::move(graph_connection))) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "failed to connect graph");
      }
    }
    if (!graph->compile()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to compile graph");
    }
    const auto state = engine_.transport().snapshot();
    graph->prepare(state.sample_rate, engine_.max_block_size());
    const std::string input_node = stringProperty(spec, "inputNode", "");
    const std::string output_node = stringProperty(spec, "outputNode", "");
    if (!engine_.swap_graph(std::move(graph), input_node.c_str(), output_node.c_str(),
                            num_channels)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to swap graph");
    }
    if (hasProperty(spec, "parameterBindings")) {
      val bindings = spec["parameterBindings"];
      const int binding_count = bindings["length"].as<int>();
      for (int i = 0; i < binding_count; ++i) {
        val binding = bindings[i];
        if (!engine_.bind_graph_parameter(static_cast<uint32_t>(intProperty(binding, "paramId", 0)),
                                          stringProperty(binding, "nodeId", "").c_str())) {
          throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                        "failed to bind graph parameter");
        }
      }
    }
#else
    (void)spec;
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "graph support is not enabled");
#endif
  }

  int graphNodeCount() const {
#if defined(SONARE_WITH_GRAPH)
    return static_cast<int>(engine_.graph_node_count());
#else
    return 0;
#endif
  }

  int graphConnectionCount() const {
#if defined(SONARE_WITH_GRAPH)
    return static_cast<int>(engine_.graph_connection_count());
#else
    return 0;
#endif
  }

  void setClips(val clips) {
    const int count = clips["length"].as<int>();
    clip_storage_.clear();
    clip_ptrs_.clear();
    clip_storage_.reserve(static_cast<size_t>(count));
    clip_ptrs_.reserve(static_cast<size_t>(count));
    std::vector<sonare::engine::ClipSchedule> schedules;
    schedules.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count; ++i) {
      val clip_val = clips[i];
      val channels_val = clip_val["channels"];
      const int channel_count = channels_val["length"].as<int>();
      if (channel_count <= 0) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "clip channels must not be empty");
      }
      clip_storage_.emplace_back();
      clip_ptrs_.emplace_back();
      auto& storage = clip_storage_.back();
      auto& pointers = clip_ptrs_.back();
      storage.reserve(static_cast<size_t>(channel_count));
      pointers.reserve(static_cast<size_t>(channel_count));
      int64_t num_samples = 0;
      for (int ch = 0; ch < channel_count; ++ch) {
        std::vector<float> channel = float32ArrayToVector(channels_val[ch]);
        if (ch == 0) {
          num_samples = static_cast<int64_t>(channel.size());
          if (num_samples <= 0) {
            throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                          "clip channels must not be empty");
          }
        } else if (static_cast<int64_t>(channel.size()) != num_samples) {
          throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                        "all clip channels must have the same length");
        }
        storage.push_back(std::move(channel));
        pointers.push_back(storage.back().data());
      }

      sonare::engine::ClipSchedule schedule{};
      schedule.id = static_cast<uint32_t>(intProperty(clip_val, "id", i + 1));
      schedule.buffer = {pointers.data(), channel_count, num_samples};
      schedule.start_ppq = objectProperty(clip_val, "startPpq").as<double>();
      // clip_offset_samples / fade_*_samples are int64_t in ClipSchedule; read
      // them at full 64-bit precision (like length_samples below) so large
      // offsets above 2^31 samples do not silently truncate/sign-flip.
      schedule.clip_offset_samples =
          hasProperty(clip_val, "clipOffsetSamples")
              ? objectProperty(clip_val, "clipOffsetSamples").as<int64_t>()
              : 0;
      schedule.length_samples = hasProperty(clip_val, "lengthSamples")
                                    ? objectProperty(clip_val, "lengthSamples").as<int64_t>()
                                    : num_samples;
      schedule.loop = boolProperty(clip_val, "loop", false);
      schedule.gain = floatProperty(clip_val, "gain", 1.0f);
      schedule.fade_in_samples = hasProperty(clip_val, "fadeInSamples")
                                     ? objectProperty(clip_val, "fadeInSamples").as<int64_t>()
                                     : 0;
      schedule.fade_out_samples = hasProperty(clip_val, "fadeOutSamples")
                                      ? objectProperty(clip_val, "fadeOutSamples").as<int64_t>()
                                      : 0;
      schedules.push_back(schedule);
    }
    engine_.set_clips(std::move(schedules));
  }

  int clipCount() const { return static_cast<int>(engine_.clip_count()); }

  void setCaptureBuffer(int num_channels, int capacity_frames) {
    if (num_channels <= 0 || capacity_frames <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "capture buffer dimensions must be positive");
    }
    capture_storage_.assign(static_cast<size_t>(num_channels),
                            std::vector<float>(static_cast<size_t>(capacity_frames), 0.0f));
    capture_ptrs_.clear();
    capture_ptrs_.reserve(capture_storage_.size());
    for (auto& channel : capture_storage_) {
      capture_ptrs_.push_back(channel.data());
    }
    engine_.set_capture_segment(
        {capture_ptrs_.data(), num_channels, static_cast<int64_t>(capacity_frames)});
  }

  void armCapture(bool armed) { engine_.set_capture_armed(armed); }
  void setCapturePunch(int64_t start_sample, int64_t end_sample, bool enabled) {
    engine_.set_capture_punch(start_sample, end_sample, enabled);
  }
  void resetCapture() { engine_.reset_capture(); }

  val captureStatus() const {
    val out = val::object();
    out.set("capturedFrames", static_cast<double>(engine_.captured_frames()));
    out.set("overflowCount", engine_.capture_overflow_count());
    out.set("armed", engine_.capture_armed());
    out.set("punchEnabled", engine_.capture_punch_enabled());
    return out;
  }

  val capturedAudio() const {
    const int64_t frames = engine_.captured_frames();
    val out = val::array();
    for (size_t ch = 0; ch < capture_storage_.size(); ++ch) {
      const size_t count =
          static_cast<size_t>(std::min<int64_t>(frames, capture_storage_[ch].size()));
      std::vector<float> channel(capture_storage_[ch].begin(),
                                 capture_storage_[ch].begin() + count);
      out.set(static_cast<int>(ch), vectorToFloat32Array(channel));
    }
    return out;
  }

  val process(val channels_val) {
    ChannelBlock block = readChannels(channels_val);
    engine_.process(block.pointers.data(), static_cast<int>(block.storage.size()), block.frames);
    return channelsToJs(block);
  }

  // ---- Zero-copy "prepared" realtime path ------------------------------
  // The AudioWorklet render thread fills the per-channel input views (returned
  // as typed_memory_views onto persistent WASM-heap storage), calls
  // processPrepared(numFrames) which runs engine_.process() IN PLACE, then reads
  // the same views back. No std::vector or JS Float32Array is allocated per
  // quantum, so process() never touches the C++/JS heap allocators on the audio
  // thread (mirrors RealtimeVoiceChanger's prepared API). Call
  // prepareChannels(numChannels, maxFrames) once on the main thread first.
  void prepareChannels(int num_channels, int max_frames) {
    if (num_channels <= 0 || max_frames <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeEngine.prepareChannels: dimensions must be positive");
    }
    prepared_channels_ = num_channels;
    prepared_capacity_ = max_frames;
    prepared_storage_.assign(static_cast<size_t>(num_channels),
                             std::vector<float>(static_cast<size_t>(max_frames), 0.0f));
    prepared_ptrs_.clear();
    prepared_ptrs_.reserve(prepared_storage_.size());
    for (auto& channel : prepared_storage_) {
      prepared_ptrs_.push_back(channel.data());
    }
  }

  val getChannelBuffer(int channel, int num_frames) {
    if (channel < 0 || channel >= prepared_channels_) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeEngine.getChannelBuffer: channel out of range; call "
                                    "prepareChannels() first");
    }
    if (num_frames <= 0 || num_frames > prepared_capacity_) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeEngine.getChannelBuffer: out-of-range frame count");
    }
    return val(typed_memory_view(static_cast<size_t>(num_frames),
                                 prepared_storage_[static_cast<size_t>(channel)].data()));
  }

  void processPrepared(int num_frames) {
    if (prepared_channels_ <= 0 || prepared_storage_.empty()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "RealtimeEngine.processPrepared: prepareChannels() must be "
                                    "called first");
    }
    if (num_frames <= 0 || num_frames > prepared_capacity_) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeEngine.processPrepared: out-of-range frame count");
    }
    engine_.process(prepared_ptrs_.data(), prepared_channels_, num_frames);
  }

  val processWithMonitor(val channels_val) {
    ChannelBlock block = readChannels(channels_val);
    ChannelBlock monitor;
    monitor.frames = block.frames;
    monitor.storage.assign(block.storage.size(),
                           std::vector<float>(static_cast<size_t>(block.frames), 0.0f));
    monitor.pointers.reserve(monitor.storage.size());
    for (auto& channel : monitor.storage) {
      monitor.pointers.push_back(channel.data());
    }
    engine_.process_with_monitor(block.pointers.data(), monitor.pointers.data(),
                                 static_cast<int>(block.storage.size()), block.frames);
    val out = val::object();
    out.set("output", channelsToJs(block));
    out.set("monitor", channelsToJs(monitor));
    return out;
  }

  val renderOffline(val channels_val, int block_size) {
    ChannelBlock block = readChannels(channels_val);
    engine_.render_offline(block.pointers.data(), static_cast<int>(block.storage.size()),
                           block.frames, block_size);
    return channelsToJs(block);
  }

  val bounceOffline(val options_val) {
    const int64_t total_frames = objectProperty(options_val, "totalFrames").as<int64_t>();
    const int block_size = intProperty(options_val, "blockSize", 128);
    const int num_channels = intProperty(options_val, "numChannels", 2);
    const int source_sample_rate = intProperty(options_val, "sourceSampleRate", 48000);
    const int target_sample_rate = intProperty(options_val, "targetSampleRate", 48000);
    if (total_frames <= 0 || block_size <= 0 || num_channels <= 0 || source_sample_rate <= 0 ||
        target_sample_rate <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "invalid bounce options");
    }

    std::vector<std::vector<float>> channels(static_cast<size_t>(num_channels),
                                             std::vector<float>(static_cast<size_t>(total_frames)));
    std::vector<float*> pointers;
    pointers.reserve(channels.size());
    for (auto& channel : channels) {
      pointers.push_back(channel.data());
    }
    engine_.render_offline(pointers.data(), num_channels, total_frames, block_size);

    if (source_sample_rate != target_sample_rate) {
      for (auto& channel : channels) {
        channel = resample(channel.data(), channel.size(), source_sample_rate, target_sample_rate);
      }
    }

    std::vector<float> interleaved = interleave(channels);
    const size_t frames = channels.empty() ? 0 : channels[0].size();
    if (boolProperty(options_val, "normalizeLufs", false)) {
      // Pull the canonical fallback target from the C API so the WASM facade
      // never drifts away from the C/Node/Python bounce normalization target.
      // See SONARE_DEFAULT_BOUNCE_TARGET_LUFS in src/sonare_c_types.h and the
      // sentinel handling in sonare_engine_bounce_offline.
      const float target_lufs =
          floatProperty(options_val, "targetLufs", SONARE_DEFAULT_BOUNCE_TARGET_LUFS);
      metering::normalize_interleaved_to_lufs(interleaved, frames, num_channels, target_sample_rate,
                                              target_lufs);
    }

    const int dither = intProperty(options_val, "dither", 0);
    if (dither != 0) {
      mastering::final::DitherConfig config{};
      config.type = ditherTypeFromInt(dither);
      config.target_bits = intProperty(options_val, "ditherBits", 16);
      if (config.target_bits <= 0) config.target_bits = 16;
      // Match the C API: seed == 0 means "keep the library default seed".
      const auto requested_seed = static_cast<uint32_t>(intProperty(options_val, "ditherSeed", 0));
      if (requested_seed != 0) config.seed = requested_seed;
      Audio dithered = mastering::final::dither(
          Audio::from_buffer(interleaved.data(), interleaved.size(), target_sample_rate), config);
      interleaved.assign(dithered.data(), dithered.data() + dithered.size());
    }

    const auto loudness =
        metering::lufs_interleaved(interleaved.data(), frames, num_channels, target_sample_rate);
    val out = val::object();
    out.set("interleaved", vectorToFloat32Array(interleaved));
    out.set("frames", static_cast<double>(frames));
    out.set("numChannels", num_channels);
    out.set("sampleRate", target_sample_rate);
    out.set("integratedLufs", loudness.integrated_lufs);
    return out;
  }

  val freezeOffline(val options_val) {
    const int64_t total_frames = objectProperty(options_val, "totalFrames").as<int64_t>();
    const int block_size = intProperty(options_val, "blockSize", 128);
    const int num_channels = intProperty(options_val, "numChannels", 2);
    if (total_frames <= 0 || block_size <= 0 || num_channels <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "invalid freeze options");
    }

    std::vector<std::vector<float>> frozen(static_cast<size_t>(num_channels),
                                           std::vector<float>(static_cast<size_t>(total_frames)));
    std::vector<float*> render_pointers;
    render_pointers.reserve(frozen.size());
    for (auto& channel : frozen) {
      render_pointers.push_back(channel.data());
    }
    engine_.render_offline(render_pointers.data(), num_channels, total_frames, block_size);

    clip_storage_.clear();
    clip_ptrs_.clear();
    clip_storage_.push_back(std::move(frozen));
    clip_ptrs_.emplace_back();
    clip_ptrs_.back().reserve(clip_storage_.back().size());
    for (const auto& channel : clip_storage_.back()) {
      clip_ptrs_.back().push_back(channel.data());
    }

    sonare::engine::ClipSchedule schedule{};
    schedule.id = static_cast<uint32_t>(intProperty(options_val, "clipId", 1));
    if (schedule.id == 0) schedule.id = 1;
    schedule.buffer = {clip_ptrs_.back().data(), num_channels, total_frames};
    // Read startPpq at full double precision to match setClips() and the
    // double-typed ClipSchedule.start_ppq field; a Float32 read would quantize a
    // frozen clip at a large PPQ position to a different sample than the same
    // clip placed via setClips.
    schedule.start_ppq = hasProperty(options_val, "startPpq")
                             ? objectProperty(options_val, "startPpq").as<double>()
                             : 0.0;
    schedule.clip_offset_samples = 0;
    schedule.length_samples = total_frames;
    schedule.loop = false;
    schedule.gain = floatProperty(options_val, "gain", 1.0f);
    if (schedule.gain == 0.0f) schedule.gain = 1.0f;
    engine_.set_clips({schedule});

    val out = val::object();
    out.set("clipId", schedule.id);
    out.set("frames", static_cast<double>(total_frames));
    out.set("numChannels", num_channels);
    return out;
  }

  val drainTelemetry(int max_records) {
    val out = val::array();
    if (max_records <= 0) return out;
    sonare::engine::Telemetry telemetry{};
    int count = 0;
    while (count < max_records && engine_.pop_telemetry(telemetry)) {
      val item = val::object();
      item.set("type", static_cast<int>(telemetry.type));
      item.set("error", static_cast<int>(telemetry.error));
      item.set("renderFrame", static_cast<double>(telemetry.render_frame));
      item.set("timelineSample", static_cast<double>(telemetry.timeline_sample));
      item.set("audibleTimelineSample", static_cast<double>(telemetry.audible_timeline_sample));
      item.set("graphLatencySamplesQ8", telemetry.graph_latency_samples_q8);
      item.set("value", telemetry.value);
      out.set(count++, item);
    }
    return out;
  }

  val drainMeterTelemetry(int max_records) {
    val out = val::array();
    if (max_records <= 0) return out;
    sonare::engine::MeterTelemetryRecord meter{};
    int count = 0;
    while (count < max_records && engine_.pop_meter_telemetry(meter)) {
      val item = val::object();
      item.set("targetId", meter.target_id);
      item.set("renderFrame", static_cast<double>(meter.render_frame));
      item.set("seq", static_cast<double>(meter.seq));
      item.set("peakDbL", meter.peak_db[0]);
      item.set("peakDbR", meter.peak_db[1]);
      item.set("rmsDbL", meter.rms_db[0]);
      item.set("rmsDbR", meter.rms_db[1]);
      item.set("truePeakDbL", meter.true_peak_db[0]);
      item.set("truePeakDbR", meter.true_peak_db[1]);
      item.set("maxTruePeakDb", meter.max_true_peak_db);
      item.set("correlation", meter.correlation);
      item.set("monoCompatWidth", meter.mono_compat_width);
      item.set("momentaryLufs", meter.momentary_lufs);
      item.set("shortTermLufs", meter.short_term_lufs);
      item.set("integratedLufs", meter.integrated_lufs);
      item.set("gainReductionDb", meter.gain_reduction_db);
      item.set("droppedRecords", meter.dropped_records);
      out.set(count++, item);
    }
    return out;
  }

 private:
  // Maps a JS-supplied queue depth to the engine's size_t capacity. A value <= 0
  // selects the engine default (1024), matching the Node/Python bindings.
  static size_t capacity(int requested) {
    return requested > 0 ? static_cast<size_t>(requested) : 1024;
  }

  struct ChannelBlock {
    std::vector<std::vector<float>> storage;
    std::vector<float*> pointers;
    int frames = 0;
  };

  static ChannelBlock readChannels(val channels_val) {
    const int count = channels_val["length"].as<int>();
    if (count <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "channels must not be empty");
    }
    ChannelBlock block;
    block.storage.reserve(static_cast<size_t>(count));
    block.pointers.reserve(static_cast<size_t>(count));
    for (int ch = 0; ch < count; ++ch) {
      std::vector<float> channel = float32ArrayToVector(channels_val[ch]);
      if (ch == 0) {
        block.frames = static_cast<int>(channel.size());
        if (block.frames <= 0) {
          throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                        "channels must not be empty");
        }
      } else if (static_cast<int>(channel.size()) != block.frames) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "all channels must have the same length");
      }
      block.storage.push_back(std::move(channel));
    }
    for (auto& channel : block.storage) {
      block.pointers.push_back(channel.data());
    }
    return block;
  }

  static val channelsToJs(const ChannelBlock& block) {
    val out = val::array();
    for (size_t ch = 0; ch < block.storage.size(); ++ch) {
      out.set(static_cast<int>(ch), vectorToFloat32Array(block.storage[ch]));
    }
    return out;
  }

  static std::vector<float> interleave(const std::vector<std::vector<float>>& channels) {
    if (channels.empty()) return {};
    const size_t frames = channels[0].size();
    std::vector<float> out(frames * channels.size());
    for (size_t frame = 0; frame < frames; ++frame) {
      for (size_t ch = 0; ch < channels.size(); ++ch) {
        out[frame * channels.size() + ch] = channels[ch][frame];
      }
    }
    return out;
  }

  static mastering::final::DitherType ditherTypeFromInt(int value) {
    switch (value) {
      case 1:
        return mastering::final::DitherType::Rpdf;
      case 2:
        return mastering::final::DitherType::Tpdf;
      case 3:
        return mastering::final::DitherType::NoiseShaped;
      default:
        return mastering::final::DitherType::None;
    }
  }

  static val parameterToVal(const sonare::automation::ParameterInfo& info) {
    val out = val::object();
    out.set("id", info.id);
    out.set("name", std::string(info.name ? info.name : ""));
    out.set("unit", std::string(info.unit ? info.unit : ""));
    out.set("minValue", info.min_value);
    out.set("maxValue", info.max_value);
    out.set("defaultValue", info.default_value);
    out.set("rtSafe", info.rt_safe);
    out.set("defaultCurve", automationCurveToInt(info.default_curve));
    return out;
  }

  static val markerToVal(const sonare::transport::Marker& marker) {
    val out = val::object();
    out.set("id", marker.id);
    out.set("ppq", marker.ppq);
    out.set("name", std::string(marker.name ? marker.name : ""));
    return out;
  }

  sonare::engine::RealtimeEngine engine_{};
  sonare::automation::ParameterRegistry parameters_{};
  std::vector<sonare::automation::AutomationLane> automation_lanes_;
  std::deque<std::string> parameter_strings_;
  std::deque<std::string> marker_strings_;
  std::vector<std::vector<std::vector<float>>> clip_storage_;
  std::vector<std::vector<const float*>> clip_ptrs_;
  std::vector<std::vector<float>> capture_storage_;
  std::vector<float*> capture_ptrs_;
  // Persistent per-channel scratch for the zero-copy prepared process() path.
  std::vector<std::vector<float>> prepared_storage_;
  std::vector<float*> prepared_ptrs_;
  int prepared_channels_ = 0;
  int prepared_capacity_ = 0;
};

#if defined(SONARE_WITH_ARRANGEMENT)
// ============================================================================
// Headless DAW project embind wrapper over the C ABI
// sonare_c_project.{h,cpp}. The wrapper owns an opaque SonareProject* (created
// in the constructor, destroyed in the destructor) and marshals the flat C
// surface into embind-friendly std::string / Float32Array shapes. The C-ABI
// translation unit (sonare_c_project.cpp) is compiled into this target under
// BUILD_ARRANGEMENT; the symbols are reached through the public sonare_c.h.
// ============================================================================

// Result of ProjectWasm::compile() surfaced to JS: a small object with the
// diagnostic count, whether a renderable timeline was produced, and the joined
// human-readable diagnostic messages.
val projectCompileResultToVal(const SonareProjectCompileResult& result) {
  val out = val::object();
  out.set("diagnosticCount", static_cast<double>(result.diagnostic_count));
  out.set("hasTimeline", result.has_timeline != 0);
  out.set("messages", std::string(result.messages != nullptr ? result.messages : ""));
  return out;
}

struct ProjectWasm {
  ProjectWasm() {
    SonareProject* handle = nullptr;
    const SonareError err = sonare_project_create(&handle);
    if (err != SONARE_OK || handle == nullptr) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to create headless project");
    }
    project_ = std::shared_ptr<SonareProject>(handle, sonare_project_destroy);
  }

  // Adopts an already-created handle (used by the fromJson factory). The handle
  // must be non-null; ownership transfers to the shared_ptr.
  explicit ProjectWasm(SonareProject* adopted) : project_(adopted, sonare_project_destroy) {}

  // The handle is owned through a shared_ptr (deleter = sonare_project_destroy),
  // so the wrapper is freely copyable/movable. embind returns the fromJson
  // factory by value, which requires a copy/move-constructible holder; sharing
  // the refcounted handle keeps that safe (the C project is destroyed once, when
  // the last wrapper goes away).

  // Serializes the project (+ MIDI content) to deterministic JSON.
  std::string toJson() const {
    char* json = nullptr;
    size_t len = 0;
    const SonareError err = sonare_project_serialize(project_.get(), &json, &len);
    if (err != SONARE_OK || json == nullptr) {
      sonare_free_string(json);
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to serialize project");
    }
    std::string out(json, len);
    sonare_free_string(json);
    return out;
  }

  // Deserializes project JSON into a NEW project. Throws on malformed input,
  // surfacing the joined diagnostic messages to JS.
  static ProjectWasm fromJson(const std::string& json) {
    SonareProject* handle = nullptr;
    char* diag = nullptr;
    const SonareError err = sonare_project_deserialize(json.data(), json.size(), &handle, &diag);
    if (err != SONARE_OK || handle == nullptr) {
      std::string message =
          diag != nullptr ? std::string(diag) : std::string("invalid project JSON");
      sonare_free_string(diag);
      sonare_project_destroy(handle);
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat, message);
    }
    sonare_free_string(diag);
    return ProjectWasm(handle);
  }

  // Sets the project sample rate (Hz). Must be > 0.
  void setSampleRate(double sample_rate) {
    const SonareError err = sonare_project_set_sample_rate(project_.get(), sample_rate);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "invalid project sample rate");
    }
  }

  uint32_t addTrack(val desc) {
    SonareProjectTrackDesc d{};
    std::string name;
    if (!desc.isUndefined() && !desc.isNull()) {
      if (hasProperty(desc, "kind")) {
        val kind = desc["kind"];
        if (kind.typeOf().as<std::string>() == "string") {
          const std::string k = kind.as<std::string>();
          d.kind = k == "midi" ? SONARE_TRACK_MIDI
                               : (k == "aux" ? SONARE_TRACK_AUX : SONARE_TRACK_AUDIO);
        } else {
          d.kind = kind.as<int>();
        }
      }
      if (hasProperty(desc, "name")) {
        name = desc["name"].as<std::string>();
        d.name = name.c_str();
      }
    }
    uint32_t out = 0;
    const SonareError err = sonare_project_add_track(project_.get(), &d, &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to add track");
    }
    return out;
  }

  uint32_t addClip(val desc) {
    if (desc.isUndefined() || desc.isNull()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "addClip expects a descriptor object");
    }
    SonareProjectClipDesc d{};
    std::vector<float> audio;
    std::string source_uri;
    d.track_id = hasProperty(desc, "trackId") ? desc["trackId"].as<uint32_t>() : 0;
    d.is_midi = hasProperty(desc, "isMidi") && desc["isMidi"].as<bool>() ? 1 : 0;
    d.start_ppq = hasProperty(desc, "startPpq") ? desc["startPpq"].as<double>() : 0.0;
    d.length_ppq = hasProperty(desc, "lengthPpq") ? desc["lengthPpq"].as<double>() : 0.0;
    d.source_offset_ppq =
        hasProperty(desc, "sourceOffsetPpq") ? desc["sourceOffsetPpq"].as<double>() : 0.0;
    d.gain = hasProperty(desc, "gain") ? desc["gain"].as<float>() : 1.0f;
    d.audio_channels = hasProperty(desc, "audioChannels") ? desc["audioChannels"].as<int>() : 1;
    d.audio_sample_rate =
        hasProperty(desc, "audioSampleRate") ? desc["audioSampleRate"].as<int>() : 0;
    if (hasProperty(desc, "audio")) {
      audio = float32ArrayToVector(desc["audio"]);
      d.audio_interleaved = audio.data();
      const int channels = d.audio_channels > 0 ? d.audio_channels : 1;
      d.audio_frames = static_cast<int64_t>(audio.size()) / channels;
    }
    if (hasProperty(desc, "sourceUri")) {
      source_uri = desc["sourceUri"].as<std::string>();
      d.source_uri = source_uri.c_str();
    }
    uint32_t out = 0;
    const SonareError err = sonare_project_add_clip(project_.get(), &d, &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to add clip");
    }
    return out;
  }

  val addMidiClip(double start_ppq, double length_ppq) {
    uint32_t track = 0;
    uint32_t clip = 0;
    const SonareError err =
        sonare_project_add_midi_clip(project_.get(), start_ppq, length_ppq, &track, &clip);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to add MIDI clip");
    }
    val out = val::object();
    out.set("trackId", track);
    out.set("clipId", clip);
    return out;
  }

  uint32_t splitClip(uint32_t clip_id, double split_ppq) {
    uint32_t out = 0;
    const SonareError err = sonare_project_split_clip(project_.get(), clip_id, split_ppq, &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to split clip");
    }
    return out;
  }

  void trimClip(uint32_t clip_id, double start_ppq, double length_ppq) {
    const SonareError err =
        sonare_project_trim_clip(project_.get(), clip_id, start_ppq, length_ppq);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to trim clip");
    }
  }

  void moveClip(uint32_t clip_id, double start_ppq, uint32_t track_id) {
    const SonareError err = sonare_project_move_clip(project_.get(), clip_id, start_ppq, track_id);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to move clip");
    }
  }

  void undo() {
    const SonareError err = sonare_project_undo(project_.get());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "nothing to undo");
    }
  }

  void redo() {
    const SonareError err = sonare_project_redo(project_.get());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "nothing to redo");
    }
  }

  void setMidiEvents(uint32_t clip_id, val events) {
    const size_t count =
        events.isUndefined() || events.isNull() ? 0 : events["length"].as<size_t>();
    std::vector<SonareMidiEventPod> pods(count);
    for (size_t i = 0; i < count; ++i) {
      val entry = events[i];
      if (val::global("Array").call<bool>("isArray", entry)) {
        pods[i].ppq = entry[0].as<double>();
        pods[i].data0 = entry[1].as<uint32_t>();
        pods[i].data1 = entry[2].as<uint32_t>();
      } else {
        pods[i].ppq = entry["ppq"].as<double>();
        pods[i].data0 = entry["data0"].as<uint32_t>();
        pods[i].data1 = hasProperty(entry, "data1") ? entry["data1"].as<uint32_t>() : 0;
      }
    }
    const SonareError err = sonare_project_set_midi_events(
        project_.get(), clip_id, pods.empty() ? nullptr : pods.data(), pods.size());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set MIDI events");
    }
  }

  uint32_t importSmf(val data) {
    std::vector<uint8_t> bytes = uint8ArrayToVector(data);
    uint32_t first_clip = 0;
    const SonareError err = sonare_project_import_smf(
        project_.get(), bytes.empty() ? nullptr : bytes.data(), bytes.size(), &first_clip);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat, "failed to import SMF");
    }
    return first_clip;
  }

  val exportSmf() {
    uint8_t* bytes = nullptr;
    size_t len = 0;
    const SonareError err = sonare_project_export_smf(project_.get(), &bytes, &len);
    if (err != SONARE_OK) {
      sonare_free_bytes(bytes);
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to export SMF");
    }
    std::vector<uint8_t> out(bytes, bytes + len);
    sonare_free_bytes(bytes);
    return vectorToUint8Array(out);
  }

  void setProgram(uint32_t clip_id, int program, int bank) {
    const SonareError err = sonare_project_set_program(project_.get(), clip_id, program, bank);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set MIDI program");
    }
  }

  void setProgramOnChannel(uint32_t clip_id, uint32_t group, uint32_t channel, int program,
                           int bank) {
    const SonareError err =
        sonare_project_set_program_on_channel(project_.get(), clip_id, static_cast<uint8_t>(group),
                                              static_cast<uint8_t>(channel), program, bank);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set MIDI program");
    }
  }

  void setMidiFx(uint32_t clip_id, const std::string& config_json) {
    const SonareError err =
        sonare_project_set_midi_fx(project_.get(), clip_id, config_json.c_str());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to set MIDI FX");
    }
  }

  float autoTempo(val audio, int sample_rate) {
    std::vector<float> samples = float32ArrayToVector(audio);
    float bpm = 0.0f;
    const SonareError err = sonare_project_auto_tempo(project_.get(), samples.data(),
                                                      samples.size(), sample_rate, &bpm);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to detect project tempo");
    }
    return bpm;
  }

  double snapToGrid(double ppq, double strength) {
    double out = 0.0;
    const SonareError err = sonare_project_snap_to_grid(project_.get(), ppq, strength, &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to snap to grid");
    }
    return out;
  }

  // Compiles the project into a renderable timeline, returning a small JS
  // object { diagnosticCount, hasTimeline, messages }.
  val compile() {
    SonareProjectCompileResult result{};
    const SonareError err = sonare_project_compile(project_.get(), &result);
    if (err != SONARE_OK) {
      sonare_project_free_compile_result(&result);
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to compile project");
    }
    val out = projectCompileResultToVal(result);
    sonare_project_free_compile_result(&result);
    return out;
  }

  // Compiles + renders the project offline to interleaved float audio, returning
  // a Float32Array. Uses C-ABI defaults (project sample rate, 2 channels, block
  // 128) when no options are provided; total_frames defaults to 0, which yields
  // an empty render for an empty project.
  val bounce(val options) {
    SonareProjectBounceOptions opts{};
    if (!options.isUndefined() && !options.isNull()) {
      if (hasProperty(options, "totalFrames")) {
        opts.total_frames = static_cast<int64_t>(options["totalFrames"].as<double>());
      }
      if (hasProperty(options, "blockSize")) {
        opts.block_size = options["blockSize"].as<int>();
      }
      if (hasProperty(options, "numChannels")) {
        opts.num_channels = options["numChannels"].as<int>();
      }
      if (hasProperty(options, "sampleRate")) {
        opts.sample_rate = options["sampleRate"].as<int>();
      }
      if (hasProperty(options, "instrumentLatencySamples")) {
        opts.instrument_latency_samples = options["instrumentLatencySamples"].as<int>();
      }
    }
    float* interleaved = nullptr;
    size_t len = 0;
    const SonareError err = sonare_project_bounce(project_.get(), &opts, &interleaved, &len);
    if (err != SONARE_OK) {
      sonare_free_floats(interleaved);
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to bounce project");
    }
    std::vector<float> samples(interleaved, interleaved + len);
    sonare_free_floats(interleaved);
    return vectorToFloat32Array(samples);
  }

  std::shared_ptr<SonareProject> project_;
};

uint32_t js_project_abi_version() { return sonare_project_abi_version(); }
#endif  // SONARE_WITH_ARRANGEMENT

// ============================================================================
// Version
// ============================================================================

std::string js_version() { return SONARE_VERSION_STRING; }

uint32_t js_engine_abi_version() { return sonare::rt::kEngineAbiVersion; }

uint32_t js_voice_changer_abi_version() { return editing::voice_changer::kVoiceChangerAbiVersion; }

// POD-flat ↔ nested C++ field bridge for the realtime voice-changer config.
// X(cpp_path, pod_field) — cpp_path is the dotted member on the C++
// RealtimeVoiceChangerConfig; pod_field is the flat key exposed to JS (matching
// the Python/C-ABI POD mirror). Calling the C++ accessors directly keeps this
// binding self-contained: the C-ABI translation unit is not linked into WASM.
#define SONARE_WASM_VC_FIELDS(X)                            \
  X(input_gain_db, input_gain_db)                           \
  X(output_gain_db, output_gain_db)                         \
  X(wet_mix, wet_mix)                                       \
  X(retune.semitones, retune_semitones)                     \
  X(retune.mix, retune_mix)                                 \
  X(retune.grain_size, retune_grain_size)                   \
  X(formant.factor, formant_factor)                         \
  X(formant.amount, formant_amount)                         \
  X(formant.body, formant_body)                             \
  X(formant.brightness, formant_brightness)                 \
  X(formant.nasal, formant_nasal)                           \
  X(eq.highpass_hz, eq_highpass_hz)                         \
  X(eq.body_db, eq_body_db)                                 \
  X(eq.presence_db, eq_presence_db)                         \
  X(eq.air_db, eq_air_db)                                   \
  X(gate.threshold_db, gate_threshold_db)                   \
  X(gate.attack_ms, gate_attack_ms)                         \
  X(gate.release_ms, gate_release_ms)                       \
  X(gate.range_db, gate_range_db)                           \
  X(compressor.threshold_db, compressor_threshold_db)       \
  X(compressor.ratio, compressor_ratio)                     \
  X(compressor.attack_ms, compressor_attack_ms)             \
  X(compressor.release_ms, compressor_release_ms)           \
  X(compressor.makeup_gain_db, compressor_makeup_gain_db)   \
  X(deesser.frequency_hz, deesser_frequency_hz)             \
  X(deesser.threshold_db, deesser_threshold_db)             \
  X(deesser.ratio, deesser_ratio)                           \
  X(deesser.range_db, deesser_range_db)                     \
  X(reverb.mix, reverb_mix)                                 \
  X(reverb.time_ms, reverb_time_ms)                         \
  X(reverb.damping, reverb_damping)                         \
  X(reverb.seed, reverb_seed)                               \
  X(limiter.ceiling_db, limiter_ceiling_db)                 \
  X(limiter.release_ms, limiter_release_ms)                 \
  X(limiter.enable_isp_limiter, limiter_enable_isp_limiter) \
  X(limiter.isp_ceiling_dbtp, limiter_isp_ceiling_dbtp)

// Validates a preset ordinal against the C++ VoiceCharacterPreset enum range.
// The C-ABI and C++ enumerators share an identical ordering, so the integer
// ordinal exposed to JS maps straight onto the C++ enum.
bool vc_preset_in_range(int preset) {
  return preset >= 0 &&
         preset <= static_cast<int>(editing::voice_changer::VoiceCharacterPreset::DarkVillain);
}

// Maps a voice-character preset ordinal to its canonical id string (e.g.
// "bright-idol"). Returns null for an out-of-range / unknown ordinal.
val js_voice_character_preset_id(int preset) {
  if (!vc_preset_in_range(preset)) return val::null();
  const char* id = editing::voice_changer::realtime_voice_changer_preset_id(
      static_cast<editing::voice_changer::VoiceCharacterPreset>(preset));
  if (id == nullptr || id[0] == '\0') return val::null();
  return val(std::string(id));
}

// Returns the voice-changer config for a preset ordinal as a JS object. Field
// names match the Python/C-ABI POD mirror exactly. Null for an out-of-range
// ordinal.
val js_realtime_voice_changer_preset_config(int preset) {
  if (!vc_preset_in_range(preset)) return val::null();
  const auto cfg = editing::voice_changer::realtime_voice_changer_preset(
      static_cast<editing::voice_changer::VoiceCharacterPreset>(preset));
  val out = val::object();
#define X(cpp_path, pod_field) out.set(#pod_field, cfg.cpp_path);
  SONARE_WASM_VC_FIELDS(X)
#undef X
  return out;
}
#undef SONARE_WASM_VC_FIELDS

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

  enum_<Mode>("Mode")
      .value("Major", Mode::Major)
      .value("Minor", Mode::Minor)
      .value("Dorian", Mode::Dorian)
      .value("Phrygian", Mode::Phrygian)
      .value("Lydian", Mode::Lydian)
      .value("Mixolydian", Mode::Mixolydian)
      .value("Locrian", Mode::Locrian);

  enum_<ChordQuality>("ChordQuality")
      .value("Major", ChordQuality::Major)
      .value("Minor", ChordQuality::Minor)
      .value("Diminished", ChordQuality::Diminished)
      .value("Augmented", ChordQuality::Augmented)
      .value("Dominant7", ChordQuality::Dominant7)
      .value("Major7", ChordQuality::Major7)
      .value("Minor7", ChordQuality::Minor7)
      .value("Sus2", ChordQuality::Sus2)
      .value("Sus4", ChordQuality::Sus4)
      .value("Unknown", ChordQuality::Unknown)
      .value("Add9", ChordQuality::Add9)
      .value("MinorAdd9", ChordQuality::MinorAdd9)
      .value("Dim7", ChordQuality::Dim7)
      .value("HalfDim7", ChordQuality::HalfDim7)
      .value("Major9", ChordQuality::Major9)
      .value("Dominant9", ChordQuality::Dominant9)
      .value("Sus2Add4", ChordQuality::Sus2Add4);

  enum_<SectionType>("SectionType")
      .value("Intro", SectionType::Intro)
      .value("Verse", SectionType::Verse)
      .value("PreChorus", SectionType::PreChorus)
      .value("Chorus", SectionType::Chorus)
      .value("Bridge", SectionType::Bridge)
      .value("Instrumental", SectionType::Instrumental)
      .value("Outro", SectionType::Outro)
      .value("Unknown", SectionType::Unknown);

  // Quick API (high-level)
  function("detectBpm", &js_detect_bpm);
  function("detectKey", &js_detect_key);
  function("_detectKeyWithOptions", &js_detect_key_with_options);
  function("_detectKeyCandidates", &js_detect_key_candidates);
  function("detectOnsets", &js_detect_onsets);
  function("detectBeats", &js_detect_beats);
  function("detectDownbeats", &js_detect_downbeats);
  function("detectChords", &js_detect_chords);
  function("chordFunctionalAnalysis", &js_chord_functional_analysis);
  function("analyze", &js_analyze);
  function("analyzeImpulseResponse", &js_analyze_impulse_response);
  function("detectAcoustic", &js_detect_acoustic);
#ifdef SONARE_WITH_ACOUSTIC_SIM
  function("synthesizeRir", &js_synthesize_rir);
  function("estimateRoom", &js_estimate_room);
  function("roomMorph", &js_room_morph);
#endif
  function("analyzeWithProgress", &js_analyze_with_progress);
  function("analyzeBpm", &js_analyze_bpm);
  function("analyzeRhythm", &js_analyze_rhythm);
  function("analyzeDynamics", &js_analyze_dynamics);
  function("analyzeTimbre", &js_analyze_timbre);
  function("detectKeyCandidates", &js_detect_key_candidates_default);
  function("hasFfmpegSupport", &js_has_ffmpeg_support);
  function("version", &js_version);
  function("engineAbiVersion", &js_engine_abi_version);
  function("voiceChangerAbiVersion", &js_voice_changer_abi_version);
  function("voiceCharacterPresetId", &js_voice_character_preset_id);
  function("realtimeVoiceChangerPresetConfig", &js_realtime_voice_changer_preset_config);

  // Effects
  function("hpss", &js_hpss);
  function("harmonic", &js_harmonic);
  function("percussive", &js_percussive);
  function("timeStretch", &js_time_stretch);
  function("pitchShift", &js_pitch_shift);
  function("pitchCorrectToMidi", &js_pitch_correct_to_midi);
  function("noteStretch", &js_note_stretch);
  function("voiceChange", &js_voice_change);
  function("decompose", &js_decompose);
  function("nnFilter", &js_nn_filter);
  function("remix", &js_remix);
  function("hpssWithResidual", &js_hpss_with_residual);
  function("phaseVocoder", &js_phase_vocoder);
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
  function("masteringAssistantSuggest", &js_mastering_assistant_suggest);
  function("masteringAudioProfile", &js_mastering_audio_profile);
  function("masteringStreamingPreview", &js_mastering_streaming_preview);
  function("masteringChain", &js_mastering_chain);
  function("masteringChainStereo", &js_mastering_chain_stereo);
  function("masteringChainWithProgress", &js_mastering_chain_with_progress);
  function("masteringChainStereoWithProgress", &js_mastering_chain_stereo_with_progress);
  function("masteringPresetNames", &js_mastering_preset_names);
  function("masterAudio", &js_master_audio);
  function("masterAudioStereo", &js_master_audio_stereo);
  function("masterAudioWithProgress", &js_master_audio_with_progress);
  function("masterAudioStereoWithProgress", &js_master_audio_stereo_with_progress);
  function("mixingScenePresetNames", &js_mixing_scene_preset_names);
  function("mixingScenePresetJson", &js_mixing_scene_preset_json);
  function("mixStereo", &js_mix_stereo);
#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)
  class_<MixerWasm>("Mixer")
      .function("compile", &MixerWasm::compile)
      .function("processStereo", &MixerWasm::processStereo)
      .function("processStereoInto", &MixerWasm::processStereoInto)
      .function("inputLeftView", &MixerWasm::inputLeftView)
      .function("inputRightView", &MixerWasm::inputRightView)
      .function("outputLeftView", &MixerWasm::outputLeftView)
      .function("outputRightView", &MixerWasm::outputRightView)
      .function("processPreparedStereo", &MixerWasm::processPreparedStereo)
      .function("stripCount", &MixerWasm::stripCount)
      .function("scheduleInsertAutomation", &MixerWasm::scheduleInsertAutomation)
      .function("stripById", &MixerWasm::stripById)
      .function("setInputTrimDb", &MixerWasm::setInputTrimDb)
      .function("setFaderDb", &MixerWasm::setFaderDb)
      .function("setPan", &MixerWasm::setPan)
      .function("setWidth", &MixerWasm::setWidth)
      .function("setMuted", &MixerWasm::setMuted)
      .function("setSoloed", &MixerWasm::setSoloed)
      .function("setSoloSafe", &MixerWasm::setSoloSafe)
      .function("setPolarityInvert", &MixerWasm::setPolarityInvert)
      .function("setPanLaw", &MixerWasm::setPanLaw)
      .function("setChannelDelaySamples", &MixerWasm::setChannelDelaySamples)
      .function("setVcaOffsetDb", &MixerWasm::setVcaOffsetDb)
      .function("setDualPan", &MixerWasm::setDualPan)
      .function("addSend", &MixerWasm::addSend)
      .function("setSendDb", &MixerWasm::setSendDb)
      .function("meterTap", &MixerWasm::meterTap)
      .function("stripMeter", &MixerWasm::stripMeter)
      .function("scheduleFaderAutomation", &MixerWasm::scheduleFaderAutomation)
      .function("schedulePanAutomation", &MixerWasm::schedulePanAutomation)
      .function("scheduleWidthAutomation", &MixerWasm::scheduleWidthAutomation)
      .function("scheduleSendAutomation", &MixerWasm::scheduleSendAutomation)
      .function("readGoniometerLatest", &MixerWasm::readGoniometerLatest)
      .function("addBus", &MixerWasm::addBus)
      .function("removeBus", &MixerWasm::removeBus)
      .function("busCount", &MixerWasm::busCount)
      .function("addVcaGroup", &MixerWasm::addVcaGroup)
      .function("removeVcaGroup", &MixerWasm::removeVcaGroup)
      .function("vcaGroupCount", &MixerWasm::vcaGroupCount)
      .function("toSceneJson", &MixerWasm::toSceneJson);
  function("createMixerFromSceneJson", &createMixerFromSceneJson, allow_raw_pointers());
#endif
  function("trim", &js_trim);

  class_<RealtimeEngineWasm>("RealtimeEngine")
      .constructor<double, int, int, int>()
      .function("prepare", &RealtimeEngineWasm::prepare)
      .function("setParameter", &RealtimeEngineWasm::setParameter)
      .function("setParameterSmoothed", &RealtimeEngineWasm::setParameterSmoothed)
      .function("getTransportState", &RealtimeEngineWasm::getTransportState)
      .function("play", &RealtimeEngineWasm::play)
      .function("stop", &RealtimeEngineWasm::stop)
      .function("seekSample", &RealtimeEngineWasm::seekSample)
      .function("seekPpq", &RealtimeEngineWasm::seekPpq)
      .function("setTempo", &RealtimeEngineWasm::setTempo)
      .function("setTimeSignature", &RealtimeEngineWasm::setTimeSignature)
      .function("setLoop", &RealtimeEngineWasm::setLoop)
      .function("addParameter", &RealtimeEngineWasm::addParameter)
      .function("parameterCount", &RealtimeEngineWasm::parameterCount)
      .function("parameterInfoByIndex", &RealtimeEngineWasm::parameterInfoByIndex)
      .function("parameterInfo", &RealtimeEngineWasm::parameterInfo)
      .function("setAutomationLane", &RealtimeEngineWasm::setAutomationLane)
      .function("automationLaneCount", &RealtimeEngineWasm::automationLaneCount)
      .function("setMarkers", &RealtimeEngineWasm::setMarkers)
      .function("markerCount", &RealtimeEngineWasm::markerCount)
      .function("markerByIndex", &RealtimeEngineWasm::markerByIndex)
      .function("marker", &RealtimeEngineWasm::marker)
      .function("seekMarker", &RealtimeEngineWasm::seekMarker)
      .function("setLoopFromMarkers", &RealtimeEngineWasm::setLoopFromMarkers)
      .function("setMetronome", &RealtimeEngineWasm::setMetronome)
      .function("metronome", &RealtimeEngineWasm::metronome)
      .function("countInEndSample", &RealtimeEngineWasm::countInEndSample)
      .function("setGraph", &RealtimeEngineWasm::setGraph)
      .function("graphNodeCount", &RealtimeEngineWasm::graphNodeCount)
      .function("graphConnectionCount", &RealtimeEngineWasm::graphConnectionCount)
      .function("setClips", &RealtimeEngineWasm::setClips)
      .function("clipCount", &RealtimeEngineWasm::clipCount)
      .function("setCaptureBuffer", &RealtimeEngineWasm::setCaptureBuffer)
      .function("armCapture", &RealtimeEngineWasm::armCapture)
      .function("setCapturePunch", &RealtimeEngineWasm::setCapturePunch)
      .function("resetCapture", &RealtimeEngineWasm::resetCapture)
      .function("captureStatus", &RealtimeEngineWasm::captureStatus)
      .function("capturedAudio", &RealtimeEngineWasm::capturedAudio)
      .function("process", &RealtimeEngineWasm::process)
      .function("prepareChannels", &RealtimeEngineWasm::prepareChannels)
      .function("getChannelBuffer", &RealtimeEngineWasm::getChannelBuffer)
      .function("processPrepared", &RealtimeEngineWasm::processPrepared)
      .function("processWithMonitor", &RealtimeEngineWasm::processWithMonitor)
      .function("renderOffline", &RealtimeEngineWasm::renderOffline)
      .function("bounceOffline", &RealtimeEngineWasm::bounceOffline)
      .function("freezeOffline", &RealtimeEngineWasm::freezeOffline)
      .function("drainTelemetry", &RealtimeEngineWasm::drainTelemetry)
      .function("drainMeterTelemetry", &RealtimeEngineWasm::drainMeterTelemetry);

#if defined(SONARE_WITH_ARRANGEMENT)
  // Headless DAW project. fromJson is a static factory returning a
  // by-value Project; bounce takes an optional options object.
  class_<ProjectWasm>("Project")
      .constructor<>()
      .class_function("fromJson", &ProjectWasm::fromJson)
      .function("toJson", &ProjectWasm::toJson)
      .function("setSampleRate", &ProjectWasm::setSampleRate)
      .function("addTrack", &ProjectWasm::addTrack)
      .function("addClip", &ProjectWasm::addClip)
      .function("addMidiClip", &ProjectWasm::addMidiClip)
      .function("splitClip", &ProjectWasm::splitClip)
      .function("trimClip", &ProjectWasm::trimClip)
      .function("moveClip", &ProjectWasm::moveClip)
      .function("undo", &ProjectWasm::undo)
      .function("redo", &ProjectWasm::redo)
      .function("setMidiEvents", &ProjectWasm::setMidiEvents)
      .function("importSmf", &ProjectWasm::importSmf)
      .function("exportSmf", &ProjectWasm::exportSmf)
      .function("setProgram", &ProjectWasm::setProgram)
      .function("setProgramOnChannel", &ProjectWasm::setProgramOnChannel)
      .function("setMidiFx", &ProjectWasm::setMidiFx)
      .function("autoTempo", &ProjectWasm::autoTempo)
      .function("snapToGrid", &ProjectWasm::snapToGrid)
      .function("compile", &ProjectWasm::compile)
      .function("bounce", &ProjectWasm::bounce);
  function("projectAbiVersion", &js_project_abi_version);
#endif

  // Features - Spectrogram
  function("stft", &js_stft);
  function("stftDb", &js_stft_db);

  // Features - Mel Spectrogram
  function("melSpectrogram", &js_mel_spectrogram);
  function("mfcc", &js_mfcc);

  // Features - Inverse reconstruction
  function("melToStft", &js_mel_to_stft);
  function("melToAudio", &js_mel_to_audio);
  function("mfccToMel", &js_mfcc_to_mel);
  function("mfccToAudio", &js_mfcc_to_audio);

  // Features - Chroma
  function("chroma", &js_chroma);
  function("nnlsChroma", &js_nnls_chroma);

  // Features - Constant-Q / Variable-Q
  function("cqt", &js_cqt);
  function("vqt", &js_vqt);

  // Analysis - Sections / Melody
  function("analyzeSections", &js_analyze_sections);
  function("analyzeMelody", &js_analyze_melody);

  // Features - Spectral
  function("spectralCentroid", &js_spectral_centroid);
  function("spectralBandwidth", &js_spectral_bandwidth);
  function("spectralRolloff", &js_spectral_rolloff);
  function("spectralFlatness", &js_spectral_flatness);
  function("zeroCrossingRate", &js_zero_crossing_rate);
  function("rmsEnergy", &js_rms_energy);
  function("spectralContrast", &js_spectral_contrast);
  function("polyFeatures", &js_poly_features);
  function("zeroCrossings", &js_zero_crossings);

  // Features - Pitch
  function("pitchYin", &js_pitch_yin);
  function("pitchPyin", &js_pitch_pyin);
  function("pitchTuning", &js_pitch_tuning);
  function("estimateTuning", &js_estimate_tuning);

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
  function("cyclicTempogram", &js_cyclic_tempogram);
  function("plp", &js_plp);
  function("onsetEnvelope", &js_onset_envelope);
  function("fourierTempogram", &js_fourier_tempogram);
  function("tempogramRatio", &js_tempogram_ratio);

  // Analysis - LUFS metering
  function("lufs", &js_lufs);
  function("momentaryLufs", &js_momentary_lufs);
  function("shortTermLufs", &js_short_term_lufs);
  function("lufsInterleaved", &js_lufs_interleaved);
  function("ebur128LoudnessRange", &js_ebur128_loudness_range);

  // Metering — basic / true-peak / clipping / dynamic range
  function("meteringPeakDb", &js_metering_peak_db);
  function("meteringRmsDb", &js_metering_rms_db);
  function("meteringCrestFactorDb", &js_metering_crest_factor_db);
  function("meteringDcOffset", &js_metering_dc_offset);
  function("meteringTruePeakDb", &js_metering_true_peak_db);
  function("meteringDetectClipping", &js_metering_detect_clipping);
  function("meteringDynamicRange", &js_metering_dynamic_range);

  // Metering — stereo / phase-scope / spectrum
  function("meteringStereoCorrelation", &js_metering_stereo_correlation);
  function("meteringStereoWidth", &js_metering_stereo_width);
  function("meteringVectorscope", &js_metering_vectorscope);
  function("meteringPhaseScope", &js_metering_phase_scope);
  function("meteringSpectrum", &js_metering_spectrum);

  // Mastering — offline repair processors
  function("masteringRepairDeclick", &js_mastering_repair_declick);
  function("masteringRepairDenoiseClassical", &js_mastering_repair_denoise_classical);
  function("masteringRepairDeclip", &js_mastering_repair_declip);
  function("masteringRepairDecrackle", &js_mastering_repair_decrackle);
  function("masteringRepairDehum", &js_mastering_repair_dehum);
  function("masteringRepairDereverbClassical", &js_mastering_repair_dereverb_classical);
  function("masteringRepairTrimSilence", &js_mastering_repair_trim_silence);

  // Mastering — offline dynamics processors
  function("masteringDynamicsCompressor", &js_mastering_dynamics_compressor);
  function("masteringDynamicsGate", &js_mastering_dynamics_gate);
  function("masteringDynamicsTransientShaper", &js_mastering_dynamics_transient_shaper);

  // Editing — scale quantizer
  function("scaleQuantizeMidi", &js_scale_quantize_midi);
  function("scaleCorrectionSemitones", &js_scale_correction_semitones);
  function("scalePitchClassEnabled", &js_scale_pitch_class_enabled);

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

  // Streaming - StreamingEqualizer
  class_<EqualizerWrapper>("StreamingEqualizer")
      .function("setBand", &EqualizerWrapper::setBand)
      .function("clear", &EqualizerWrapper::clear)
      .function("setPhaseMode", &EqualizerWrapper::setPhaseMode)
      .function("setAutoGain", &EqualizerWrapper::setAutoGain)
      .function("setGainScale", &EqualizerWrapper::setGainScale)
      .function("setOutputGainDb", &EqualizerWrapper::setOutputGainDb)
      .function("setOutputPan", &EqualizerWrapper::setOutputPan)
      .function("setSidechainMono", &EqualizerWrapper::setSidechainMono)
      .function("setSidechainStereo", &EqualizerWrapper::setSidechainStereo)
      .function("clearSidechain", &EqualizerWrapper::clearSidechain)
      .function("lastAutoGainDb", &EqualizerWrapper::lastAutoGainDb)
      .function("latencySamples", &EqualizerWrapper::latencySamples)
      .function("processMono", &EqualizerWrapper::processMono)
      .function("processStereo", &EqualizerWrapper::processStereo)
      .function("spectrum", &EqualizerWrapper::spectrum)
      .function("match", &EqualizerWrapper::match);
  function("createEqualizer", &createEqualizer, allow_raw_pointers());

  // Streaming - StreamingRetune
  class_<StreamingRetuneWrapper>("StreamingRetune")
      .function("prepare", &StreamingRetuneWrapper::prepare)
      .function("reset", &StreamingRetuneWrapper::reset)
      .function("setConfig", &StreamingRetuneWrapper::setConfig)
      .function("config", &StreamingRetuneWrapper::config)
      .function("grainSize", &StreamingRetuneWrapper::grainSize)
      .function("processMono", &StreamingRetuneWrapper::processMono);
  function("createStreamingRetune", &createStreamingRetune, allow_raw_pointers());

  class_<RealtimeVoiceChangerWrapper>("RealtimeVoiceChanger")
      .function("prepare", &RealtimeVoiceChangerWrapper::prepare)
      .function("reset", &RealtimeVoiceChangerWrapper::reset)
      .function("setConfig", &RealtimeVoiceChangerWrapper::setConfig)
      .function("configJson", &RealtimeVoiceChangerWrapper::configJson)
      .function("latencySamples", &RealtimeVoiceChangerWrapper::latencySamples)
      .function("processMono", &RealtimeVoiceChangerWrapper::processMono)
      .function("processMonoInto", &RealtimeVoiceChangerWrapper::processMonoInto)
      .function("processInterleaved", &RealtimeVoiceChangerWrapper::processInterleaved)
      .function("processInterleavedInto", &RealtimeVoiceChangerWrapper::processInterleavedInto)
      .function("getMonoInputBuffer", &RealtimeVoiceChangerWrapper::getMonoInputBuffer)
      .function("getMonoOutputBuffer", &RealtimeVoiceChangerWrapper::getMonoOutputBuffer)
      .function("processPreparedMono", &RealtimeVoiceChangerWrapper::processPreparedMono)
      .function("getInterleavedInputBuffer",
                &RealtimeVoiceChangerWrapper::getInterleavedInputBuffer)
      .function("getInterleavedOutputBuffer",
                &RealtimeVoiceChangerWrapper::getInterleavedOutputBuffer)
      .function("processPreparedInterleaved",
                &RealtimeVoiceChangerWrapper::processPreparedInterleaved)
      .function("getPlanarChannelBuffer", &RealtimeVoiceChangerWrapper::getPlanarChannelBuffer)
      .function("processPreparedPlanar", &RealtimeVoiceChangerWrapper::processPreparedPlanar);
  function("createRealtimeVoiceChanger", &createRealtimeVoiceChanger, allow_raw_pointers());
  function("realtimeVoiceChangerPresetNames", &realtimeVoiceChangerPresetNames);
  function("realtimeVoiceChangerPresetJson", &realtimeVoiceChangerPresetJson);
  function("validateRealtimeVoiceChangerPresetJson", &validateRealtimeVoiceChangerPresetJson);

  // Streaming - StreamAnalyzer
  class_<StreamAnalyzerWrapper>("StreamAnalyzer")
      .constructor<int, int, int, int, float, float, float, bool, bool, bool, bool, bool, int, int,
                   float, float, int, int>()
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
