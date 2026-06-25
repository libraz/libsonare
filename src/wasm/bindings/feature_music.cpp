/// @file feature_music.cpp
/// @brief Embind bindings for chroma, CQT/VQT, section, and melody feature APIs.

#ifdef __EMSCRIPTEN__

#include "common.h"

// ============================================================================
// Features - Chroma
// ============================================================================

val js_chroma(val samples, int sample_rate, int n_fft, int hop_length) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
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

val chromaToVal(const Chroma& chroma) {
  val out = val::object();
  out.set("nChroma", chroma.n_chroma());
  out.set("nFrames", chroma.n_frames());
  out.set("sampleRate", chroma.sample_rate());
  out.set("hopLength", chroma.hop_length());

  std::vector<float> features_vec(chroma.data(),
                                  chroma.data() + chroma.n_chroma() * chroma.n_frames());
  out.set("features", vectorToFloat32Array(features_vec));

  auto mean = chroma.mean_energy();
  val mean_arr = val::array();
  for (int i = 0; i < 12; ++i) {
    mean_arr.call<void>("push", mean[static_cast<size_t>(i)]);
  }
  out.set("meanEnergy", mean_arr);
  return out;
}

val js_chroma_cens(val samples, int sample_rate, int hop_length, int n_chroma) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  ChromaCensConfig config;
  config.base.cqt.hop_length = hop_length;
  config.base.n_chroma = n_chroma;
  return chromaToVal(chroma_cens(audio, config));
}

val js_bass_chroma(val samples, int sample_rate, int hop_length, int n_chroma) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  BassChromaConfig config;
  config.cqt.hop_length = hop_length;
  config.n_chroma = n_chroma;
  return chromaToVal(bass_chroma(audio, config));
}

val js_nnls_chroma(val samples, int sample_rate) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
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
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
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
                      int frame_length = 2048, int hop_length = 256, float threshold = 0.1f,
                      bool use_pyin = false, bool center = true) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  // Fall back to the struct defaults when raw emscripten passes 0 for a
  // missing argument, so the JS-facing defaults stay consistent with the
  // C ABI / Node bindings (fmin=65, fmax=2093, frame_length=2048,
  // hop_length=256, threshold=0.1). use_pyin/center are plain bools (default
  // use_pyin=false, center=true) selecting the pYIN tracker and frame
  // centering, matching MelodyConfig::use_pyin / MelodyConfig::center.
  MelodyConfig config;
  if (fmin > 0.0f) config.fmin = fmin;
  if (fmax > 0.0f) config.fmax = fmax;
  if (frame_length > 0) config.frame_length = frame_length;
  if (hop_length > 0) config.hop_length = hop_length;
  if (threshold > 0.0f) config.threshold = threshold;
  config.use_pyin = use_pyin;
  config.center = center;

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
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  CqtConfig config;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.n_bins = n_bins;
  config.bins_per_octave = bins_per_octave;

  return cqtResultToVal(cqt(audio, config));
}

val js_pseudo_cqt(val samples, int sample_rate, int hop_length, float fmin, int n_bins,
                  int bins_per_octave) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  CqtConfig config;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.n_bins = n_bins;
  config.bins_per_octave = bins_per_octave;

  return cqtResultToVal(pseudo_cqt(audio, config));
}

val js_hybrid_cqt(val samples, int sample_rate, int hop_length, float fmin, int n_bins,
                  int bins_per_octave) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  CqtConfig config;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.n_bins = n_bins;
  config.bins_per_octave = bins_per_octave;

  return cqtResultToVal(hybrid_cqt(audio, config));
}

val js_vqt(val samples, int sample_rate, int hop_length, float fmin, int n_bins,
           int bins_per_octave, float gamma) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  VqtConfig config;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.n_bins = n_bins;
  config.bins_per_octave = bins_per_octave;
  config.gamma = gamma;

  return cqtResultToVal(vqt(audio, config));
}

void registerFeatureMusicBindings() {
  function("chroma", &js_chroma);
  function("chromaCens", &js_chroma_cens);
  function("bassChroma", &js_bass_chroma);
  function("nnlsChroma", &js_nnls_chroma);
  function("cqt", &js_cqt);
  function("pseudoCqt", &js_pseudo_cqt);
  function("hybridCqt", &js_hybrid_cqt);
  function("vqt", &js_vqt);
  function("analyzeSections", &js_analyze_sections);
  function("analyzeMelody", &js_analyze_melody);
}

#endif  // __EMSCRIPTEN__
