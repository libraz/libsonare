/// @file quick_analysis.cpp
/// @brief Embind bindings for high-level analysis, detailed analysis, and acoustic APIs.

#ifdef __EMSCRIPTEN__

#include "analysis/analysis_json.h"
#include "common.h"

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
  val rhythmTimeSig = val::object();
  rhythmTimeSig.set("numerator", result.rhythm.time_signature.numerator);
  rhythmTimeSig.set("denominator", result.rhythm.time_signature.denominator);
  rhythmTimeSig.set("confidence", result.rhythm.time_signature.confidence);
  rhythm.set("timeSignature", rhythmTimeSig);
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
  // Reject out-of-range enum-like fields up front, matching the C ABI's
  // sonare_detect_chords_ex: chroma_method must be 0/1, and when key context is
  // enabled the key root/mode must be in range (otherwise they are unused).
  validateChromaMethod(chroma_method);
  if (use_key_context) {
    validateKey(key_root, key_mode);
  }

  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
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
  // Mirror the C ABI's sonare_chord_functional_analysis: chroma_method must be
  // 0/1, and key_root/key_mode are range-checked unconditionally because they
  // both drive the Roman-numeral labelling and (when use_key_context is set)
  // the chord-detection key context, which share the same parameters here.
  validateChromaMethod(chroma_method);
  validateKey(key_root, key_mode);

  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
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
  result.key.root = PitchClass::C;
  result.key.mode = Mode::Major;
  result.key.confidence = 0.8f;
  result.time_signature = {4, 4, 0.7f};
  result.beats.push_back({0.25f, 0, 0.6f});
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

val js_analyze_impulse_response(val samples, int sample_rate, int n_octave_bands) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  AcousticConfig config;
  config.n_octave_bands = n_octave_bands;
  return acousticParametersToVal(analyze_impulse_response(audio, config));
}

val js_detect_acoustic(val samples, int sample_rate, int n_octave_bands,
                       int n_third_octave_subbands, float min_decay_db,
                       float noise_floor_margin_db) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
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
      ShoeboxRoom room;
      room.dims = dims;
      Material wall;
      wall.absorption.reserve(bands.size());
      for (float a : bands) wall.absorption.push_back(std::clamp(a, 0.0f, 0.999f));
      wall.scattering.reserve(bands.size());
      for (size_t i = 0; i < bands.size(); ++i) {
        const float scattering = i < scattering_bands.size() ? scattering_bands[i] : 0.0f;
        wall.scattering.push_back(std::clamp(scattering, 0.0f, 1.0f));
      }
      for (Material& w : room.walls) w = wall;
      return room;
    }
  }

  return uniform_shoebox(dims, floatProperty(opts, "absorption", def_absorption));
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
  const float crossfade_ms = floatProperty(opts, "crossfadeMs", 0.0f);
  if (crossfade_ms > 0.0f) config.crossfade_ms = crossfade_ms;

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
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
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
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
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
  config.late_model = boolProperty(opts, "preferEyring", true)
                          ? sonare::acoustic::ReverbModel::Eyring
                          : sonare::acoustic::ReverbModel::Sabine;
  config.mixing_time_ms = floatProperty(opts, "mixingTimeMs", config.mixing_time_ms);
  const float crossfade_ms = floatProperty(opts, "crossfadeMs", 0.0f);
  if (crossfade_ms > 0.0f) config.crossfade_ms = crossfade_ms;

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

  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  MusicAnalyzer analyzer(audio);

  // Set progress callback if provided
  if (!progress_callback.isNull() && !progress_callback.isUndefined()) {
    analyzer.set_progress_callback([progress_callback](float progress, const char* stage) {
      progress_callback(progress, std::string(stage ? stage : ""));
    });
  }

  AnalysisResult result = analyzer.analyze();
  return analysisResultToVal(result);
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
  function("_analysisResultSchemaPaths", &js_analysis_result_schema_paths);
  function("_analysisResultSchemaFixture", &js_analysis_result_schema_fixture);
  function("analyzeImpulseResponse", &js_analyze_impulse_response);
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
