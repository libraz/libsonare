/// @file feature_music.cpp
/// @brief Embind bindings for chroma, CQT/VQT, section, and melody feature APIs.

#ifdef __EMSCRIPTEN__

#include <cmath>
#include <limits>

#include "util/constants.h"
#include "util/numeric_validation.h"
#include "wasm/bindings/common/common.h"

// ============================================================================
// Features - Chroma
// ============================================================================

val js_chroma(val samples, const val& sample_rate, const val& n_fft, const val& hop_length) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));

  ChromaConfig config;
  config.n_fft = checkedIntFromVal(n_fft, "nFft");
  config.hop_length = checkedIntFromVal(hop_length, "hopLength");

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

val js_chroma_cens(val samples, const val& sample_rate, const val& hop_length, const val& n_chroma,
                   const val& bins_per_octave_val) {
  const int bins_per_octave = checkedIntFromVal(bins_per_octave_val, "binsPerOctave");
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  SONARE_CHECK(bins_per_octave > 0 && bins_per_octave <= std::numeric_limits<int>::max() / 7,
               ErrorCode::InvalidParameter);

  ChromaCensConfig config;
  config.base.cqt.hop_length = checkedIntFromVal(hop_length, "hopLength");
  config.base.cqt.bins_per_octave = bins_per_octave;
  config.base.cqt.n_bins = 7 * bins_per_octave;
  config.base.n_chroma = checkedIntFromVal(n_chroma, "nChroma");
  return chromaToVal(chroma_cens(audio, config));
}

val js_chroma_cqt(val samples, const val& sample_rate, const val& hop_length, const val& n_chroma,
                  const val& bins_per_octave_val) {
  const int bins_per_octave = checkedIntFromVal(bins_per_octave_val, "binsPerOctave");
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  SONARE_CHECK(bins_per_octave > 0 && bins_per_octave <= std::numeric_limits<int>::max() / 7,
               ErrorCode::InvalidParameter);

  ChromaCqtConfig config;
  config.cqt.hop_length = checkedIntFromVal(hop_length, "hopLength");
  config.cqt.bins_per_octave = bins_per_octave;
  config.cqt.n_bins = 7 * bins_per_octave;
  config.n_chroma = checkedIntFromVal(n_chroma, "nChroma");
  return chromaToVal(chroma_cqt(audio, config));
}

val js_bass_chroma(val samples, const val& sample_rate, const val& hop_length,
                   const val& n_chroma) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));

  BassChromaConfig config;
  config.cqt.hop_length = checkedIntFromVal(hop_length, "hopLength");
  config.n_chroma = checkedIntFromVal(n_chroma, "nChroma");
  return chromaToVal(bass_chroma(audio, config));
}

val js_nnls_chroma_ex(val samples, const val& sample_rate, bool enable_stft_blend,
                      const val& stft_blend_weight_val, const val& stft_blend_n_fft,
                      const val& hop_length) {
  const float stft_blend_weight = checkedFloatFromVal(stft_blend_weight_val, "stftBlendWeight");
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));

  NnlsChromaConfig config;
  config.enable_stft_blend = enable_stft_blend;
  config.stft_blend_weight = stft_blend_weight;
  config.stft_blend_n_fft = checkedIntFromVal(stft_blend_n_fft, "stftBlendNFft");
  config.cqt.hop_length = checkedIntFromVal(hop_length, "hopLength");
  Chroma chroma = nnls_chroma(audio, config);

  val out = val::object();
  out.set("nChroma", chroma.n_chroma());
  out.set("nFrames", chroma.n_frames());

  std::vector<float> data_vec(chroma.data(), chroma.data() + chroma.n_chroma() * chroma.n_frames());
  out.set("data", vectorToFloat32Array(data_vec));
  return out;
}

val js_nnls_chroma(val samples, const val& sample_rate, bool enable_stft_blend,
                   const val& stft_blend_weight_val, const val& stft_blend_n_fft) {
  return js_nnls_chroma_ex(samples, sample_rate, enable_stft_blend, stft_blend_weight_val,
                           stft_blend_n_fft, val(constants::kDefaultHopLength));
}

// ============================================================================
// Analysis - Sections / Melody
// ============================================================================

// Mirrors sonare_analyze_sections / SonareSectionResult and the Node/Python
// analyzeSections: detects song-structure sections and returns an array of
// { type, name, start, end, energyLevel, confidence }.
// Embind passes every argument, so the narrowed sizes carry no C++ default.
val js_analyze_sections(val samples, const val& sample_rate, const val& n_fft_val,
                        const val& hop_length_val, const val& min_section_sec_val) {
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  const float min_section_sec = checkedFloatFromVal(min_section_sec_val, "minSectionSec");
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  // Mirror the flat C ABI config contract (sonare_analyze_sections): reject
  // non-positive sizing instead of silently substituting struct defaults, so
  // WASM rejects identically to the C ABI / Node. The TS layer (which always
  // passes explicit values) carries the matching guards.
  if (n_fft <= 0 || hop_length <= 0 || !numeric::finite_non_negative(min_section_sec)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "analyzeSections: require nFft > 0, hopLength > 0, minSectionSec >= 0");
  }

  SectionConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.min_section_sec = min_section_sec;

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
// Embind passes every argument, so the narrowed sizes carry no C++ default, and
// neither can the float parameters that precede them.
val js_analyze_melody(val samples, const val& sample_rate, float fmin, const val& fmax_val,
                      const val& frame_length_val, const val& hop_length_val,
                      const val& threshold_val, bool use_pyin, bool center) {
  const int frame_length = checkedIntFromVal(frame_length_val, "frameLength");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  const float fmax = checkedFloatFromVal(fmax_val, "fmax");
  const float threshold = checkedFloatFromVal(threshold_val, "threshold");
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  // Mirror the flat C ABI config contract (sonare_analyze_melody_ex): reject an
  // inverted/zero frequency range, non-positive sizing and a non-positive
  // threshold instead of silently substituting struct defaults. use_pyin/center
  // are plain bools selecting the pYIN tracker and frame centering.
  // fmin is a plain embind float, so nothing narrowed it on the way in.
  if (!numeric::finite_positive(fmin) || !numeric::finite_ordered_range(fmin, fmax) ||
      frame_length <= 0 || hop_length <= 0 || !numeric::finite_positive(threshold)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "analyzeMelody: require fmin > 0, fmax > fmin, frameLength > 0, "
                          "hopLength > 0, threshold > 0");
  }

  MelodyConfig config;
  config.fmin = fmin;
  config.fmax = fmax;
  config.frame_length = frame_length;
  config.hop_length = hop_length;
  config.threshold = threshold;
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

val js_cqt(val samples, const val& sample_rate, const val& hop_length, float fmin,
           const val& n_bins, const val& bins_per_octave) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  // fmin is a plain embind float, so nothing narrowed it on the way in.
  if (!numeric::finite_positive(fmin)) {
    throw SonareException(ErrorCode::InvalidParameter, "cqt: require fmin > 0");
  }

  CqtConfig config;
  config.hop_length = checkedIntFromVal(hop_length, "hopLength");
  config.fmin = fmin;
  config.n_bins = checkedIntFromVal(n_bins, "nBins");
  config.bins_per_octave = checkedIntFromVal(bins_per_octave, "binsPerOctave");

  return cqtResultToVal(cqt(audio, config));
}

val js_pseudo_cqt(val samples, const val& sample_rate, const val& hop_length, const val& fmin_val,
                  const val& n_bins, const val& bins_per_octave) {
  const float fmin = checkedFloatFromVal(fmin_val, "fmin");
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));

  CqtConfig config;
  config.hop_length = checkedIntFromVal(hop_length, "hopLength");
  config.fmin = fmin;
  config.n_bins = checkedIntFromVal(n_bins, "nBins");
  config.bins_per_octave = checkedIntFromVal(bins_per_octave, "binsPerOctave");

  return cqtResultToVal(pseudo_cqt(audio, config));
}

val js_hybrid_cqt(val samples, const val& sample_rate, const val& hop_length, const val& fmin_val,
                  const val& n_bins, const val& bins_per_octave) {
  const float fmin = checkedFloatFromVal(fmin_val, "fmin");
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));

  CqtConfig config;
  config.hop_length = checkedIntFromVal(hop_length, "hopLength");
  config.fmin = fmin;
  config.n_bins = checkedIntFromVal(n_bins, "nBins");
  config.bins_per_octave = checkedIntFromVal(bins_per_octave, "binsPerOctave");

  return cqtResultToVal(hybrid_cqt(audio, config));
}

val js_vqt(val samples, const val& sample_rate, const val& hop_length, float fmin,
           const val& n_bins, const val& bins_per_octave, float gamma) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  // Mirror sonare_vqt: a NaN gamma selects automatic bandwidth, an infinity does not.
  if (!numeric::finite_positive(fmin) || std::isinf(gamma)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "vqt: require fmin > 0 and a non-infinite gamma");
  }

  VqtConfig config;
  config.hop_length = checkedIntFromVal(hop_length, "hopLength");
  config.fmin = fmin;
  config.n_bins = checkedIntFromVal(n_bins, "nBins");
  config.bins_per_octave = checkedIntFromVal(bins_per_octave, "binsPerOctave");
  config.gamma = gamma;

  return cqtResultToVal(vqt(audio, config));
}

void registerFeatureMusicBindings() {
  function("chroma", &js_chroma);
  function("chromaCens", &js_chroma_cens);
  function("chromaCqt", &js_chroma_cqt);
  function("bassChroma", &js_bass_chroma);
  function("nnlsChroma", &js_nnls_chroma);
  function("nnlsChromaEx", &js_nnls_chroma_ex);
  function("cqt", &js_cqt);
  function("pseudoCqt", &js_pseudo_cqt);
  function("hybridCqt", &js_hybrid_cqt);
  function("vqt", &js_vqt);
  function("analyzeSections", &js_analyze_sections);
  function("analyzeMelody", &js_analyze_melody);
}

#endif  // __EMSCRIPTEN__
