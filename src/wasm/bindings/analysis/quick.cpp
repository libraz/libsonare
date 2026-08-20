/// @file quick_analysis.cpp
/// @brief Embind bindings for high-level analysis, detailed analysis, and acoustic APIs.

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <limits>
#include <string>
#include <type_traits>

#include "analysis/analysis_json.h"
#include "analysis/meter_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "util/numeric_validation.h"
#include "wasm/bindings/common/common.h"

std::vector<Mode> modesFromVal(val modes) {
  std::vector<Mode> out;
  if (modes.isUndefined() || modes.isNull()) {
    return out;
  }
  const int length = modes["length"].as<int>();
  out.reserve(static_cast<size_t>(length));
  for (int i = 0; i < length; ++i) {
    const int mode = modes[i].as<int>();
    requireOrdinalInRange(mode, static_cast<int>(Mode::Major), static_cast<int>(Mode::Locrian),
                          "key mode");
    out.push_back(static_cast<Mode>(mode));
  }
  return out;
}

// Rejects an out-of-range profile_type, mirroring the C ABI's fill_key_profile
// (core_common.cpp), which returns false — rejecting — on an unmapped
// SonareKeyProfileType instead of silently falling back to a default profile.
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
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "invalid key profile");
  }
}

// Rejects an out-of-range chroma_method, mirroring the C ABI (only 0 = STFT and
// 1 = NNLS are defined) instead of silently treating any non-1 value as STFT.
void validateChromaMethod(int chroma_method) {
  if (chroma_method != 0 && chroma_method != 1) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid chromaMethod (expected 0 = STFT or 1 = NNLS)");
  }
}

// Rejects an out-of-range key root / mode, mirroring the C ABI's range checks
// (PitchClass::C..B = [0, 11], Mode::Major..Locrian = [0, 6]) instead of
// silently clamping garbage to C / Major via static_cast.
void validateKey(int key_root, int key_mode) {
  if (key_root < static_cast<int>(PitchClass::C) || key_root > static_cast<int>(PitchClass::B) ||
      key_mode < static_cast<int>(Mode::Major) || key_mode > static_cast<int>(Mode::Locrian)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "invalid key root or mode");
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
  validate_offline_audio_input(samples.data(), samples.size(), sample_rate);
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

val timeSignatureToVal(const TimeSignature& time_signature) {
  val out = val::object();
  out.set("numerator", time_signature.numerator);
  out.set("denominator", time_signature.denominator);
  out.set("confidence", time_signature.confidence);
  return out;
}

// Emits a float series as a plain JS number array rather than a Float32Array.
// The analysis and meter results are field-for-field mirrors of
// analysis_result_to_json / meter_result_to_json, which serialize these as JSON
// number arrays, and the cross-surface field-set test compares the two.
val numberArrayFromVector(const std::vector<float>& values) {
  val out = val::array();
  for (float value : values) {
    out.call<void>("push", value);
  }
  return out;
}

val numberArrayFromVector(const std::vector<int>& values) {
  val out = val::array();
  for (int value : values) {
    out.call<void>("push", value);
  }
  return out;
}

/// @brief Converts AnalysisResult to JavaScript object.
/// @param result Analysis result
/// @return JavaScript object with all analysis data
val analysisResultToVal(const AnalysisResult& result) {
  val out = val::object();

  // BPM
  out.set("bpm", result.bpm);
  out.set("bpmConfidence", result.bpm_confidence);
  val bpmCandidates = val::array();
  for (const auto& candidate : result.bpm_candidates) {
    val value = val::object();
    value.set("value", candidate.value);
    value.set("confidence", candidate.confidence);
    switch (candidate.relation) {
      case BpmCandidateRelation::Primary:
        value.set("relation", "primary");
        break;
      case BpmCandidateRelation::Half:
        value.set("relation", "half");
        break;
      case BpmCandidateRelation::Double:
        value.set("relation", "double");
        break;
      case BpmCandidateRelation::Other:
        value.set("relation", "other");
        break;
    }
    bpmCandidates.call<void>("push", value);
  }
  out.set("bpmCandidates", bpmCandidates);

  // Key
  val key = val::object();
  key.set("root", static_cast<int>(result.key.root));
  key.set("mode", static_cast<int>(result.key.mode));
  key.set("confidence", result.key.confidence);
  key.set("name", result.key.to_string());
  key.set("shortName", result.key.to_short_string());
  out.set("key", key);

  // Time signature
  out.set("timeSignature", timeSignatureToVal(result.time_signature));
  val timeSignatureCandidates = val::array();
  for (const auto& candidate : result.time_signature_candidates) {
    timeSignatureCandidates.call<void>("push", timeSignatureToVal(candidate));
  }
  out.set("timeSignatureCandidates", timeSignatureCandidates);

  // Beats
  val beats = val::array();
  for (size_t i = 0; i < result.beats.size(); ++i) {
    val beat = val::object();
    beat.set("time", result.beats[i].time);
    beat.set("strength", result.beats[i].strength);
    beats.call<void>("push", beat);
  }
  out.set("beats", beats);

  // Downbeats as indices into beats, plus the meter phase they start from.
  // Field names and order mirror analysis_result_to_json /
  // analysis_result_schema_paths, which the cross-surface field-set test
  // compares this object against.
  val downbeatIndices = val::array();
  for (int index : result.downbeat_indices) {
    downbeatIndices.call<void>("push", index);
  }
  out.set("downbeatIndices", downbeatIndices);
  out.set("downbeatPhase", result.downbeat_phase);

  // Beat-level evidence the downbeat and meter decisions score, one value per
  // entry of `beats`. An empty stream means the analysis could not produce it —
  // lowFrequencyEnergy without audio, chordChange before chords are analyzed —
  // which is not the same as every beat having scored zero.
  val beatObservations = val::object();
  beatObservations.set("onsetStrength",
                       numberArrayFromVector(result.beat_observations.onset_strength));
  beatObservations.set("lowFrequencyEnergy",
                       numberArrayFromVector(result.beat_observations.low_frequency_energy));
  beatObservations.set("chordChange", numberArrayFromVector(result.beat_observations.chord_change));
  out.set("beatObservations", beatObservations);
  // Beat-indexed like the streams above, and likewise empty rather than absent
  // when it was not produced — here because the caller did not ask for it.
  out.set("beatLocalBpm", numberArrayFromVector(result.beat_local_bpm));

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

  // Dynamics. Field order mirrors analysis_result_to_json /
  // analysis_result_schema_paths (crestFactor before loudnessRangeDb) so the two
  // hand-maintained serializers read identically; both are anchored to the same
  // canonical schema by the cross-surface field-set tests.
  val dynamics = val::object();
  dynamics.set("dynamicRangeDb", result.dynamics.dynamic_range_db);
  dynamics.set("peakDb", result.dynamics.peak_db);
  dynamics.set("rmsDb", result.dynamics.rms_db);
  dynamics.set("crestFactor", result.dynamics.crest_factor);
  dynamics.set("loudnessRangeDb", result.dynamics.loudness_range_db);
  dynamics.set("isCompressed", result.dynamics.is_compressed);
  out.set("dynamics", dynamics);

  // Rhythm
  val rhythm = val::object();
  rhythm.set("syncopation", result.rhythm.syncopation);
  rhythm.set("grooveType", result.rhythm.groove_type);
  rhythm.set("patternRegularity", result.rhythm.pattern_regularity);
  rhythm.set("tempoStability", result.rhythm.tempo_stability);
  rhythm.set("timeSignature", timeSignatureToVal(result.rhythm.time_signature));
  out.set("rhythm", rhythm);

  // Melody
  val melody = val::object();
  melody.set("pitchRangeOctaves", result.melody.pitch_range_octaves);
  melody.set("pitchStability", result.melody.pitch_stability);
  melody.set("meanFrequency", result.melody.mean_frequency);
  melody.set("vibratoRate", result.melody.vibrato_rate);
  val melodyPitches = val::array();
  for (size_t i = 0; i < result.melody.pitches.size(); ++i) {
    val pitch = val::object();
    pitch.set("time", result.melody.pitches[i].time);
    pitch.set("frequency", result.melody.pitches[i].frequency);
    pitch.set("confidence", result.melody.pitches[i].confidence);
    melodyPitches.call<void>("push", pitch);
  }
  melody.set("pitches", melodyPitches);
  out.set("melody", melody);

  // Form
  out.set("form", result.form);

  return out;
}

// ============================================================================
// Quick API (high-level)
// ============================================================================

float js_detect_bpm(val samples, int sample_rate) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  return quick::detect_bpm(audio.data(), audio.size(), sample_rate);
}

val js_detect_key(val samples, int sample_rate) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  Key key = quick::detect_key(audio.data(), audio.size(), sample_rate);

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
  Audio audio = loadValidatedAudio(samples, sample_rate);
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
  Key key = quick::detect_key(audio.data(), audio.size(), sample_rate, config);

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
  Audio audio = loadValidatedAudio(samples, sample_rate);
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
      quick::detect_key_candidates(audio.data(), audio.size(), sample_rate, config);
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

val js_detect_onsets(val samples, int sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  OnsetDetectConfig config;
  const auto integer = [&](const char* name, int fallback) {
    const val value = options[name];
    return value.isUndefined() ? fallback : value.as<int>();
  };
  const auto number = [&](const char* name, float fallback) {
    const val value = options[name];
    return value.isUndefined() ? fallback : value.as<float>();
  };
  config.n_fft = integer("nFft", config.n_fft);
  config.hop_length = integer("hopLength", config.hop_length);
  config.threshold = number("threshold", config.threshold);
  config.pre_max = integer("preMax", config.pre_max);
  config.post_max = integer("postMax", config.post_max);
  config.pre_avg = integer("preAvg", config.pre_avg);
  config.post_avg = integer("postAvg", config.post_avg);
  config.delta = number("delta", config.delta);
  config.wait = integer("wait", config.wait);
  config.backtrack = !options["backtrack"].isUndefined() && options["backtrack"].as<bool>();
  config.backtrack_range = integer("backtrackRange", config.backtrack_range);
  std::vector<float> onsets = detect_onsets(audio, config);
  return vectorToFloat32Array(onsets);
}

val js_detect_beats(val samples, int sample_rate) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  std::vector<float> beats = quick::detect_beats(audio.data(), audio.size(), sample_rate);
  return vectorToFloat32Array(beats);
}

val js_detect_downbeats(val samples, int sample_rate) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  std::vector<float> downbeats = quick::detect_downbeats(audio.data(), audio.size(), sample_rate);
  return vectorToFloat32Array(downbeats);
}

val js_detect_chords(val samples, int sample_rate, float min_duration, float smoothing_window,
                     float threshold, bool use_triads_only, int n_fft, int hop_length,
                     bool use_beat_sync, bool use_hmm, int hmm_beam_width, bool use_key_context,
                     int key_root, int key_mode, bool detect_inversions, int chroma_method) {
  // Reject out-of-range enum-like fields up front, matching the C ABI's
  // sonare_detect_chords_ex: chroma_method must be 0/1, and when key context is
  // enabled the key root/mode must be in range (otherwise they are unused).
  validateChromaMethod(chroma_method);
  if (use_key_context) {
    validateKey(key_root, key_mode);
  }

  Audio audio = loadValidatedAudio(samples, sample_rate);

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
  // Mirror the C ABI's sonare_chord_functional_analysis: chroma_method must be
  // 0/1, and key_root/key_mode are range-checked unconditionally because they
  // both drive the Roman-numeral labelling and (when use_key_context is set)
  // the chord-detection key context, which share the same parameters here.
  validateChromaMethod(chroma_method);
  validateKey(key_root, key_mode);

  Audio audio = loadValidatedAudio(samples, sample_rate);

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

// Reads an optional JS number into a config field. Field semantics (positive
// ranges, even sizes, powers of two, ...) stay with the core validators, so
// what this owns is the JS-number narrowing: a non-finite or out-of-range
// Number must not reach a static_cast to int/float. An undefined or null value
// leaves the core default in place. @p subject prefixes the error message.
template <typename Field>
void setNumberOption(const val& options, const char* key, const char* subject, Field* field) {
  const val value = options[key];
  if (value.isUndefined() || value.isNull()) return;
  const double raw = value.as<double>();
  Field converted{};
  bool converted_ok = false;
  if constexpr (std::is_integral_v<Field>) {
    converted_ok = sonare::numeric::checked_round_cast(raw, &converted);
  } else {
    converted_ok = sonare::numeric::checked_float_cast(raw, &converted);
  }
  if (!converted_ok) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(subject) + ": " + key + " must be a finite in-range number");
  }
  *field = converted;
}

// Reads a meter candidate-numerator list from a JS array-like. The entry-count
// bound is applied to the JS length before any element is read, because the
// core can only see the vector after the whole array has been converted; the
// non-empty rule and the [2, 32] per-entry range stay with the core validators.
// @p subject names the field, e.g. "analyze: meterCandidateNumerators".
std::vector<int> meterCandidateNumeratorsFromVal(const val& numerators, const char* subject) {
  const std::size_t length = wasmArrayLikeLength(numerators, subject);
  if (length > static_cast<std::size_t>(sonare::kMaxMeterCandidateNumerators)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(subject) + " must hold at most " +
                              std::to_string(sonare::kMaxMeterCandidateNumerators) + " entries");
  }
  std::vector<int> out;
  out.reserve(length);
  for (int i = 0; i < static_cast<int>(length); ++i) {
    int converted = 0;
    if (!sonare::numeric::checked_round_cast(numerators[i].as<double>(), &converted)) {
      throw SonareException(ErrorCode::InvalidParameter,
                            std::string(subject) + " entries must be finite in-range numbers");
    }
    out.push_back(converted);
  }
  return out;
}

val js_analyze(val samples, int sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  MusicAnalyzerConfig config;
  // Field semantics (positive BPM range, even nFft, positive beam width, ...)
  // are enforced by validate_config when MusicAnalyzer is constructed below.
  auto set_number = [&](const char* key, auto& field) {
    setNumberOption(options, key, "analyze", &field);
  };
  auto set_bool = [&](const char* key, bool& field) {
    const val value = options[key];
    if (!value.isUndefined() && !value.isNull()) field = value.as<bool>();
  };
  set_number("nFft", config.n_fft);
  set_number("hopLength", config.hop_length);
  set_number("bpmMin", config.bpm_min);
  set_number("bpmMax", config.bpm_max);
  set_number("startBpm", config.start_bpm);
  set_bool("useTriadsOnly", config.use_triads_only);
  set_bool("useHpss", config.use_hpss);
  set_number("chromaHighpassHz", config.chroma_highpass_hz);
  set_bool("useBassWeighted", config.use_bass_weighted);
  set_number("chromaHopMultiplier", config.chroma_hop_multiplier);
  set_bool("useChordHmm", config.use_chord_hmm);
  set_bool("useChordKeyContext", config.use_chord_key_context);
  set_number("chordHmmBeamWidth", config.chord_hmm_beam_width);
  set_bool("detectChordInversions", config.detect_chord_inversions);
  set_bool("adaptiveTempo", config.adaptive_tempo);
  set_number("tempoUpdateIntervalBeats", config.tempo_update_interval_beats);
  set_bool("computeTempoCurve", config.compute_tempo_curve);
  set_number("meterDenominator", config.meter_denominator);
  // meterCandidateNumerators is an array, so set_number (a scalar reader) does
  // not apply. An undefined/null value leaves the core default {3, 4, 6} in
  // place.
  const val meter_numerators = options["meterCandidateNumerators"];
  if (!meter_numerators.isUndefined() && !meter_numerators.isNull()) {
    config.meter_candidate_numerators =
        meterCandidateNumeratorsFromVal(meter_numerators, "analyze: meterCandidateNumerators");
  }
  MusicAnalyzer analyzer(audio, config);
  AnalysisResult result = analyzer.analyze();
  return analysisResultToVal(result);
}

val js_estimate_meter(val beat_times, val beat_strengths, val options) {
  // Budget both arrays before either one is copied. Their lengths are
  // deliberately NOT compared here: estimate_meter_from_beats rejects a
  // mismatch itself, so every surface reports it with the same message.
  validateWasmFloat32ArrayPair(beat_times, "estimateMeter: beatTimes", beat_strengths,
                               "estimateMeter: beatStrengths", "estimateMeter",
                               /*require_matching_lengths=*/false);

  MeterConfig config;
  const val candidate_numerators = options["candidateNumerators"];
  if (!candidate_numerators.isUndefined() && !candidate_numerators.isNull()) {
    config.candidate_numerators =
        meterCandidateNumeratorsFromVal(candidate_numerators, "estimateMeter: candidateNumerators");
  }
  setNumberOption(options, "denominator", "estimateMeter", &config.denominator);
  setNumberOption(options, "downbeatWeight", "estimateMeter", &config.downbeat_weight);
  setNumberOption(options, "measureWeight", "estimateMeter", &config.measure_weight);
  setNumberOption(options, "subdivisionWeight", "estimateMeter", &config.subdivision_weight);
  setNumberOption(options, "compoundSubdivisionThreshold", "estimateMeter",
                  &config.compound_subdivision_threshold);

  // validate_meter_config runs inside estimate_meter_from_beats, so the empty
  // candidate list, the per-entry range, the denominator rule, and the weight
  // finiteness are all rejected by the core rather than re-checked here.
  const MeterResult result = estimate_meter_from_beats(
      float32ArrayToVector(beat_times), float32ArrayToVector(beat_strengths), config);

  // Field names and order mirror meter_result_to_json.
  val out = val::object();
  out.set("timeSignature", timeSignatureToVal(result.time_signature));
  out.set("downbeatPhase", result.downbeat_phase);
  out.set("grouping", numberArrayFromVector(result.grouping));
  out.set("candidateScores", numberArrayFromVector(result.candidate_scores));
  val candidates = val::array();
  for (const TimeSignature& candidate : result.candidates) {
    candidates.call<void>("push", timeSignatureToVal(candidate));
  }
  out.set("candidates", candidates);
  return out;
}

val js_analysis_result_schema_paths() {
  val out = val::array();
  const auto& paths = sonare::analysis_result_schema_paths();
  for (size_t i = 0; i < paths.size(); ++i) {
    out.call<void>("push", paths[i]);
  }
  return out;
}

val js_analysis_result_schema_fixture() {
  AnalysisResult result;
  result.bpm = 120.0f;
  result.bpm_confidence = 0.9f;
  result.bpm_candidates.push_back({120.0f, 0.9f, BpmCandidateRelation::Primary});
  result.key.root = PitchClass::C;
  result.key.mode = Mode::Major;
  result.key.confidence = 0.8f;
  result.time_signature = {4, 4, 0.7f};
  result.time_signature_candidates.push_back({4, 4, 0.7f});
  result.beats.push_back({0.25f, 0, 0.6f});
  result.downbeat_indices.push_back(0);
  result.downbeat_phase = 0;
  result.beat_observations.onset_strength.push_back(0.6f);
  result.beat_observations.low_frequency_energy.push_back(0.4f);
  result.beat_observations.chord_change.push_back(0.2f);
  result.beat_local_bpm.push_back(118.5f);
  result.chords.push_back({PitchClass::C, ChordQuality::Major, 0.0f, 1.0f, 0.8f, PitchClass::C});
  result.sections.push_back({SectionType::Verse, 0.0f, 1.0f, 0.5f, 0.9f});
  result.timbre = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};
  result.dynamics = {12.0f, -1.0f, -14.0f, 13.0f, 3.0f, false};
  result.rhythm.time_signature = {4, 4, 0.75f};
  result.rhythm.syncopation = 0.1f;
  result.rhythm.groove_type = "straight";
  result.rhythm.pattern_regularity = 0.8f;
  result.rhythm.tempo_stability = 0.9f;
  result.melody.pitch_range_octaves = 1.0f;
  result.melody.pitch_stability = 0.7f;
  result.melody.mean_frequency = 440.0f;
  result.melody.vibrato_rate = 5.0f;
  result.melody.pitches.push_back({0.0f, 440.0f, 0.95f});
  result.form = "A";
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

val js_analyze_impulse_response_ex(val samples, int sample_rate, int n_octave_bands,
                                   float min_decay_db) {
  if (!std::isfinite(min_decay_db) || min_decay_db <= 0.0f) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "analyzeImpulseResponse: minDecayDb must be finite and > 0");
  }
  Audio audio = loadValidatedAudio(samples, sample_rate);
  AcousticConfig config;
  config.n_octave_bands = n_octave_bands;
  config.min_decay_db = min_decay_db;
  return acousticParametersToVal(analyze_impulse_response(audio, config));
}

val js_analyze_impulse_response(val samples, int sample_rate, int n_octave_bands) {
  return js_analyze_impulse_response_ex(samples, sample_rate, n_octave_bands, 30.0f);
}

val js_detect_acoustic(val samples, int sample_rate, int n_octave_bands,
                       int n_third_octave_subbands, float min_decay_db,
                       float noise_floor_margin_db) {
  // Mirror the C ABI's sonare_detect_acoustic guard so a negative band/subband
  // count or non-positive decay window is rejected here too, instead of silently
  // producing an empty-subband result (the C++ core treats them as benign).
  if (n_octave_bands < 0 || n_third_octave_subbands < 0 || min_decay_db <= 0.0f ||
      noise_floor_margin_db < 0.0f) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "detectAcoustic parameters out of range");
  }
  Audio audio = loadValidatedAudio(samples, sample_rate);
  AcousticConfig config;
  config.mode = AcousticConfig::Mode::Blind;
  config.n_octave_bands = n_octave_bands;
  config.n_third_octave_subbands = n_third_octave_subbands;
  config.min_decay_db = min_decay_db;
  config.noise_floor_margin_db = noise_floor_margin_db;
  return acousticParametersToVal(detect_acoustic(audio, config));
}

#ifdef SONARE_WITH_ACOUSTIC_SIM
// Mirrors the C ABI's per-material-band cap (sonare_c_acoustic.cpp) so a crafted
// bandAbsorption/bandScattering array cannot drive an unbounded per-wall
// allocation. The C ABI is bypassed here (synthesize_rir is called directly), so
// this binding must re-apply the same guard.

// Finite [0, 1] test matching the C ABI's `unit` predicate for absorption /
// scattering coefficients. Out-of-range values are rejected (not silently
// clamped) so the same mistake surfaces the same error on every surface.
bool isUnitCoefficient(float v) { return std::isfinite(v) && v >= 0.0f && v <= 1.0f; }

// Maps a materialPreset selector (mirroring SONARE_MATERIAL_PRESET_*: 1 concrete,
// 2 wood, 3 curtain, 4 carpet, 5 glass) onto a MaterialPreset. Returns false for
// 0/none or any unknown value, leaving the per-band/scalar path to apply.
bool materialPresetFromInt(int selector, sonare::acoustic::MaterialPreset* out) {
  using sonare::acoustic::MaterialPreset;
  switch (selector) {
    case 1:
      *out = MaterialPreset::Concrete;
      return true;
    case 2:
      *out = MaterialPreset::Wood;
      return true;
    case 3:
      *out = MaterialPreset::Curtain;
      return true;
    case 4:
      *out = MaterialPreset::Carpet;
      return true;
    case 5:
      *out = MaterialPreset::Glass;
      return true;
    default:
      return false;
  }
}

// Builds a uniform shoebox from a JS options object, honouring the same wall-
// material precedence as the C ABI: materialPreset (non-zero) > per-band
// bandAbsorption (Float32Array/number[]) > scalar absorption.
sonare::acoustic::ShoeboxRoom roomFromVal(val opts, float def_absorption) {
  using namespace sonare::acoustic;
  const RoomDimensions dims{floatProperty(opts, "lengthM", 7.0f),
                            floatProperty(opts, "widthM", 5.0f),
                            floatProperty(opts, "heightM", 3.0f)};
  if (!std::isfinite(dims.length) || !std::isfinite(dims.width) || !std::isfinite(dims.height)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "room dimensions must be finite");
  }

  MaterialPreset preset{};
  if (materialPresetFromInt(intProperty(opts, "materialPreset", 0), &preset)) {
    ShoeboxRoom room;
    room.dims = dims;
    const Material wall = make_material(preset);
    for (Material& w : room.walls) w = wall;
    return room;
  }

  if (hasProperty(opts, "bandAbsorption")) {
    const std::vector<float> bands = float32ArrayToVector(opts["bandAbsorption"]);
    if (!bands.empty()) {
      const std::vector<float> scattering_bands = hasProperty(opts, "bandScattering")
                                                      ? float32ArrayToVector(opts["bandScattering"])
                                                      : std::vector<float>{};
      if (bands.size() > sonare::acoustic::kMaxMaterialBands ||
          scattering_bands.size() > sonare::acoustic::kMaxMaterialBands) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "material band count exceeds the maximum of 64");
      }
      // Reject any non-finite or out-of-[0, 1] per-band coefficient, matching the C
      // ABI's `unit` predicate so the same invalid band table fails identically on
      // every surface (rather than being silently clamped only here).
      for (float a : bands) {
        if (!isUnitCoefficient(a)) {
          throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                        "bandAbsorption values must be within [0, 1]");
        }
      }
      for (float s : scattering_bands) {
        if (!isUnitCoefficient(s)) {
          throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                        "bandScattering values must be within [0, 1]");
        }
      }
      ShoeboxRoom room;
      room.dims = dims;
      Material wall;
      wall.absorption.reserve(bands.size());
      // Clamp the accepted in-range per-band absorption to [0, 0.999], matching the
      // C-ABI oracle's make_room clamp so the same band table yields the same
      // reflection energy on every surface (a raw 1.0 gives beta=0 here but 0.0316
      // in the C ABI, diverging the RIR early reflections).
      for (float a : bands) {
        wall.absorption.push_back(std::clamp(a, 0.0f, 0.999f));
      }
      wall.scattering.reserve(bands.size());
      for (size_t i = 0; i < bands.size(); ++i) {
        const float scattering = i < scattering_bands.size() ? scattering_bands[i] : 0.0f;
        wall.scattering.push_back(scattering);
      }
      for (Material& w : room.walls) w = wall;
      return room;
    }
  }

  const float scalar = floatProperty(opts, "absorption", def_absorption);
  if (!isUnitCoefficient(scalar)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "absorption must be within [0, 1]");
  }
  return uniform_shoebox(dims, scalar);
}

sonare::acoustic::SourceListener placementFromVal(val opts) {
  return {{floatProperty(opts, "sourceX", 1.0f), floatProperty(opts, "sourceY", 1.0f),
           floatProperty(opts, "sourceZ", 1.2f)},
          {floatProperty(opts, "listenerX", 5.0f), floatProperty(opts, "listenerY", 4.0f),
           floatProperty(opts, "listenerZ", 1.7f)}};
}

// Acoustic sample-rate bounds, sourced from the shared core limits
// (sonare::kMinAudioSampleRate / kMaxAudioSampleRate) so every binding rejects
// the same out-of-range rates (the C++ functions are otherwise called directly).
void validateAcousticSampleRate(int sample_rate) {
  if (sample_rate < sonare::kMinAudioSampleRate || sample_rate > sonare::kMaxAudioSampleRate) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "sampleRate out of supported range [" +
                                      std::to_string(sonare::kMinAudioSampleRate) + ", " +
                                      std::to_string(sonare::kMaxAudioSampleRate) + "]");
  }
}

// Validates the RIR shape/timing config against the same bounds the C ABI checks
// before building a room, so WASM rejects (rather than silently accepts) the
// NaN/out-of-range inputs the C ABI/Python already refuse.
void validateRirShapeAndTiming(const sonare::acoustic::SourceListener& placement,
                               const sonare::acoustic::RirSynthConfig& config) {
  using namespace sonare::acoustic;
  const auto finite3 = [](const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
  };
  if (!finite3(placement.source) || !finite3(placement.listener)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "source/listener position must be finite");
  }
  if (!std::isfinite(config.max_seconds) || config.max_seconds < 0.0f ||
      config.max_seconds > kMaxRirSeconds || !std::isfinite(config.mixing_time_ms) ||
      config.mixing_time_ms < 0.0f || config.mixing_time_ms > kMaxRirMixingTimeMs ||
      !std::isfinite(config.crossfade_ms) || config.crossfade_ms < 0.0f ||
      config.crossfade_ms > kMaxRirCrossfadeMs) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "RIR timing parameters out of range");
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

// Wire string for a diagnostic severity, matching the RirDiagnostic type.
const char* rirSeverityName(sonare::Diagnostic::Severity severity) {
  switch (severity) {
    case sonare::Diagnostic::Severity::Error:
      return "error";
    case sonare::Diagnostic::Severity::Warning:
      return "warning";
    case sonare::Diagnostic::Severity::Info:
      break;
  }
  return "info";
}

// Transcribes the synthesizer's whole diagnostic list onto the result object.
// The synthesizer reports five distinct geometry errors plus the clamp /
// no-tail warnings, so a lone hasError boolean cannot tell a caller which one
// fired, nor that a maxSeconds clamp shortened the tail of an otherwise
// successful RIR. `errorMessage` carries the first error as "code: message",
// matching the string the C ABI leaves in sonare_last_error_message().
void setRirDiagnostics(val& out, const std::vector<sonare::Diagnostic>& diagnostics) {
  val entries = val::array();
  std::string error_message;
  for (const sonare::Diagnostic& diagnostic : diagnostics) {
    val entry = val::object();
    entry.set("code", diagnostic.code);
    entry.set("message", diagnostic.message);
    entry.set("severity", std::string(rirSeverityName(diagnostic.severity)));
    entries.call<void>("push", entry);
    if (error_message.empty() && diagnostic.severity == sonare::Diagnostic::Severity::Error) {
      error_message = diagnostic.code + ": " + diagnostic.message;
    }
  }
  out.set("diagnostics", entries);
  out.set("errorMessage", error_message);
}

val js_synthesize_rir(val opts) {
  const int sample_rate = intProperty(opts, "sampleRate", 48000);
  validateAcousticSampleRate(sample_rate);
  sonare::acoustic::RirSynthConfig config;
  // Match the C ABI: reject a negative ISM order instead of clamping it to 0.
  config.ism_order = intProperty(opts, "ismOrder", config.ism_order);
  if (config.ism_order < 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "ismOrder must be non-negative");
  }
  config.late_model = boolProperty(opts, "preferEyring", true)
                          ? sonare::acoustic::ReverbModel::Eyring
                          : sonare::acoustic::ReverbModel::Sabine;
  // seed <= 0 keeps the RirSynthConfig default (1), matching the C ABI's
  // "seed == 0 keeps the library default" so seed:0 yields the same RIR on every
  // surface instead of seeding the PRNG with 0.
  if (const int seed_in = intProperty(opts, "seed", 0); seed_in > 0)
    config.seed = static_cast<unsigned>(seed_in);
  config.max_seconds = floatProperty(opts, "maxSeconds", config.max_seconds);
  config.mixing_time_ms = floatProperty(opts, "mixingTimeMs", config.mixing_time_ms);
  const float crossfade_ms = floatProperty(opts, "crossfadeMs", 0.0f);
  if (crossfade_ms > 0.0f) config.crossfade_ms = crossfade_ms;
  config.air_absorption_enabled =
      boolProperty(opts, "airAbsorptionEnabled", config.air_absorption_enabled);
  // airTemperatureC / airHumidityPercent == 0 keep the ISO reference climate
  // (20 degC, 50 % RH), matching the C ABI's "0 means the library default" rule
  // so the same options object yields the same RIR on every surface. An
  // implausible climate is reported through diagnostics/hasError by the core.
  const float air_temperature_c = floatProperty(opts, "airTemperatureC", 0.0f);
  if (air_temperature_c != 0.0f) config.air.temperature_c = air_temperature_c;
  const float air_humidity_percent = floatProperty(opts, "airHumidityPercent", 0.0f);
  if (air_humidity_percent != 0.0f) config.air.humidity_percent = air_humidity_percent;

  const auto placement = placementFromVal(opts);
  validateRirShapeAndTiming(placement, config);
  const auto result =
      sonare::acoustic::synthesize_rir(roomFromVal(opts, 0.2f), placement, sample_rate, config);
  std::vector<float> rir;
  if (!result.rir.empty()) {
    rir.assign(result.rir.data(), result.rir.data() + result.rir.size());
  }
  val out = val::object();
  out.set("rir", vectorToFloat32Array(rir));
  out.set("sampleRate", result.rir.sample_rate());
  out.set("hasError", sonare::has_error(result.diagnostics));
  setRirDiagnostics(out, result.diagnostics);
  return out;
}

val js_estimate_room(val samples, int sample_rate, val opts) {
  validateAcousticSampleRate(sample_rate);
  std::vector<float> data = float32ArrayToVector(samples);
  validateAcousticInput(data);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  sonare::RoomEstimateConfig config;
  // Match the C ABI: an explicit 0 aspect hint means "use the default 1.0", so
  // the same input is accepted identically on every surface (raw 0 would be
  // rejected by the core's finite-positive check).
  config.aspect_hint_lw = floatProperty(opts, "aspectHintLw", config.aspect_hint_lw);
  if (config.aspect_hint_lw == 0.0f) config.aspect_hint_lw = 1.0f;
  config.aspect_hint_lh = floatProperty(opts, "aspectHintLh", config.aspect_hint_lh);
  if (config.aspect_hint_lh == 0.0f) config.aspect_hint_lh = 1.0f;
  config.reference_absorption =
      floatProperty(opts, "referenceAbsorption", config.reference_absorption);
  config.prefer_eyring = boolProperty(opts, "preferEyring", true);
  const int n_bands = intProperty(opts, "nOctaveBands", 0);
  if (n_bands != 0) config.acoustic.n_octave_bands = n_bands;
  const float min_decay_db = floatProperty(opts, "minDecayDb", 0.0f);
  if (min_decay_db != 0.0f) config.acoustic.min_decay_db = min_decay_db;
  const float noise_floor_margin_db = floatProperty(opts, "noiseFloorMarginDb", 0.0f);
  if (noise_floor_margin_db != 0.0f) config.acoustic.noise_floor_margin_db = noise_floor_margin_db;
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
  // Absorption (from the inverse problem) and RT60 (from the decay fit) are
  // independent estimates and either can fail on its own. Both arrays report at
  // the longer length with the failed side NaN-filled, exactly as the C ABI does
  // (sonare_c_acoustic.cpp): truncating to the shorter side discarded a
  // fully-computed vector precisely when its sibling failed, which is when the
  // caller needs the surviving one most.
  const size_t band_count = std::max(est.absorption_bands.size(), est.rt60_bands.size());
  const auto pad_with_nan = [band_count](std::vector<float> values) {
    values.resize(band_count, std::numeric_limits<float>::quiet_NaN());
    return values;
  };
  const std::vector<float> absorption_bands = pad_with_nan(est.absorption_bands);
  const std::vector<float> rt60_bands = pad_with_nan(est.rt60_bands);
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
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  sonare::effects::acoustic::RoomMorphConfig config;
  config.target = roomFromVal(opts, 0.2f);
  config.placement = placementFromVal(opts);
  config.source_tail_suppression =
      floatProperty(opts, "sourceTailSuppression", config.source_tail_suppression);
  config.wet = floatProperty(opts, "wet", config.wet);
  config.ism_order = intProperty(opts, "ismOrder", config.ism_order);
  // seed <= 0 keeps the RirSynthConfig default (1), matching the C ABI's
  // "seed == 0 keeps the library default" so seed:0 yields the same RIR on every
  // surface instead of seeding the PRNG with 0.
  if (const int seed_in = intProperty(opts, "seed", 0); seed_in > 0)
    config.seed = static_cast<unsigned>(seed_in);
  config.max_seconds = floatProperty(opts, "maxSeconds", config.max_seconds);
  config.late_model = boolProperty(opts, "preferEyring", true)
                          ? sonare::acoustic::ReverbModel::Eyring
                          : sonare::acoustic::ReverbModel::Sabine;
  config.mixing_time_ms = floatProperty(opts, "mixingTimeMs", config.mixing_time_ms);
  const float crossfade_ms = floatProperty(opts, "crossfadeMs", 0.0f);
  if (crossfade_ms != 0.0f) config.crossfade_ms = crossfade_ms;
  // Air absorption on the target room; the zero-means-ISO-reference rule is the
  // same as synthesizeRir above. An implausible climate throws here (the morph
  // core validates rather than diagnosing), matching the C ABI.
  config.air_absorption_enabled =
      boolProperty(opts, "airAbsorptionEnabled", config.air_absorption_enabled);
  const float air_temperature_c = floatProperty(opts, "airTemperatureC", 0.0f);
  if (air_temperature_c != 0.0f) config.air.temperature_c = air_temperature_c;
  const float air_humidity_percent = floatProperty(opts, "airHumidityPercent", 0.0f);
  if (air_humidity_percent != 0.0f) config.air.humidity_percent = air_humidity_percent;

  const Audio result = sonare::effects::acoustic::room_morph(audio, config);
  std::vector<float> out;
  if (!result.empty()) {
    out.assign(result.data(), result.data() + result.size());
  }
  return vectorToFloat32Array(out);
}
#endif  // SONARE_WITH_ACOUSTIC_SIM

// Analyze with progress callback
val js_analyze_with_progress(val samples, int sample_rate, val progress_callback,
                             val cancel_callback) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  MusicAnalyzer analyzer(audio);
  if (!progress_callback.isNull() && !progress_callback.isUndefined()) {
    analyzer.set_progress_callback([progress_callback](float progress, const char* stage) {
      progress_callback(progress, std::string(stage ? stage : ""));
    });
  }
  if (!cancel_callback.isNull() && !cancel_callback.isUndefined()) {
    analyzer.set_cancel_callback(
        [cancel_callback] { return cancelCallbackRequested(cancel_callback); });
  }

  const auto result = analyzer.analyze_cancellable();
  if (!result) {
    throw SonareException(ErrorCode::Cancelled, "analysis cancelled");
  }
  return analysisResultToVal(*result);
}

void registerQuickAnalysisBindings() {
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
  function("estimateMeter", &js_estimate_meter);
  function("_analysisResultSchemaPaths", &js_analysis_result_schema_paths);
  function("_analysisResultSchemaFixture", &js_analysis_result_schema_fixture);
  function("analyzeImpulseResponse", &js_analyze_impulse_response);
  function("analyzeImpulseResponseEx", &js_analyze_impulse_response_ex);
  function("detectAcoustic", &js_detect_acoustic);
#ifdef SONARE_WITH_ACOUSTIC_SIM
  function("synthesizeRir", &js_synthesize_rir);
  function("estimateRoom", &js_estimate_room);
  function("roomMorph", &js_room_morph);
#endif
  function("analyzeWithProgress", &js_analyze_with_progress);

  registerQuickDetailedAnalysisBindings();
}

#endif  // __EMSCRIPTEN__
