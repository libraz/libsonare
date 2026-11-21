/// @file feature_music.cpp
/// @brief Embind bindings for chroma, CQT/VQT, section, and melody feature APIs.

#ifdef __EMSCRIPTEN__

#include <array>
#include <cmath>
#include <limits>
#include <optional>

#include "analysis/boundary_detector.h"
#include "analysis/section_analyzer.h"
#include "core/spectrum.h"
#include "feature/chroma.h"
#include "feature/cqt.h"
#include "feature/nnls_chroma.h"
#include "util/constants.h"
#include "util/parameter_checks.h"
#include "wasm/bindings/common/common.h"

// ============================================================================
// Features - Chroma
// ============================================================================

val js_chroma(val samples, const val& sample_rate, const val& n_fft, const val& hop_length) {
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));

  ChromaConfig config;
  config.n_fft = checkedIntFromVal(n_fft, "nFft");
  config.hop_length = domainNumberFromVal<int>(hop_length, "hopLength", kStftHopDomain);

  Chroma chroma = Chroma::compute(audio, config);

  val out = val::object();
  out.set("nChroma", chroma.n_chroma());
  out.set("nFrames", chroma.n_frames());
  out.set("sampleRate", chroma.sample_rate());
  out.set("hopLength", chroma.hop_length());

  out.set("features", vectorToFloat32Array(chroma.data(), chroma.n_chroma() * chroma.n_frames()));

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

  out.set("features", vectorToFloat32Array(chroma.data(), chroma.n_chroma() * chroma.n_frames()));

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
  const int bins_per_octave =
      domainNumberFromVal<int>(bins_per_octave_val, "binsPerOctave", kChromaBinsPerOctaveDomain);
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));

  ChromaCensConfig config;
  config.base = make_chroma_cqt_config(
      domainNumberFromVal<int>(hop_length, "hopLength", kCqtHopLengthDomain),
      domainNumberFromVal<int>(n_chroma, "nChroma", kChromaCountDomain), bins_per_octave);
  return chromaToVal(sonare::detail::with_derived_cqt_grid(
      "bins_per_octave", "sample_rate", [&] { return chroma_cens(audio, config); }));
}

val js_chroma_cqt(val samples, const val& sample_rate, const val& hop_length, const val& n_chroma,
                  const val& bins_per_octave_val) {
  const int bins_per_octave =
      domainNumberFromVal<int>(bins_per_octave_val, "binsPerOctave", kChromaBinsPerOctaveDomain);
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));

  const ChromaCqtConfig config = make_chroma_cqt_config(
      domainNumberFromVal<int>(hop_length, "hopLength", kCqtHopLengthDomain),
      domainNumberFromVal<int>(n_chroma, "nChroma", kChromaCountDomain), bins_per_octave);
  return chromaToVal(sonare::detail::with_derived_cqt_grid(
      "bins_per_octave", "sample_rate", [&] { return chroma_cqt(audio, config); }));
}

val js_bass_chroma(val samples, const val& sample_rate, const val& hop_length,
                   const val& n_chroma) {
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));

  BassChromaConfig config;
  config.cqt.hop_length = domainNumberFromVal<int>(hop_length, "hopLength", kCqtHopLengthDomain);
  config.n_chroma = domainNumberFromVal<int>(n_chroma, "nChroma", kChromaCountDomain);
  return chromaToVal(bass_chroma(audio, config));
}

val js_nnls_chroma(val samples, const val& sample_rate, bool enable_stft_blend,
                   const val& stft_blend_weight_val, const val& stft_blend_n_fft,
                   const val& hop_length_val) {
  const val hop_length = hop_length_val.isUndefined() || hop_length_val.isNull()
                             ? val(constants::kDefaultHopLength)
                             : hop_length_val;
  const float stft_blend_weight = checkedFloatFromVal(stft_blend_weight_val, "stftBlendWeight");
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));

  NnlsChromaConfig config = NnlsChromaConfig::fast(
      domainNumberFromVal<int>(hop_length, "hopLength", kNnlsHopLengthDomain));
  config.enable_stft_blend = enable_stft_blend;
  config.stft_blend_weight = stft_blend_weight;
  config.stft_blend_n_fft = checkedIntFromVal(stft_blend_n_fft, "stftBlendNFft");
  validate_nnls_chroma_config(config);
  Chroma chroma = nnls_chroma(audio, config);

  val out = val::object();
  out.set("nChroma", chroma.n_chroma());
  out.set("nFrames", chroma.n_frames());

  out.set("data", vectorToFloat32Array(chroma.data(), chroma.n_chroma() * chroma.n_frames()));
  return out;
}

// ============================================================================
// Analysis - Sections / Melody
// ============================================================================

// Mirrors sonare_analyze_sections / SonareSectionResult and the Node/Python
// analyzeSections: detects song-structure sections and returns an array of
// { type, name, start, end, energyLevel, confidence, group }.
// Embind passes every argument, so the narrowed sizes carry no C++ default.
val js_analyze_sections(val samples, const val& sample_rate, const val& n_fft_val,
                        const val& hop_length_val, const val& min_section_sec_val,
                        const val& threshold_val) {
  SectionConfig config;
  config.n_fft = domainNumberFromVal<int>(n_fft_val, "nFft", kSectionCountDomain);
  config.hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kSectionCountDomain);
  config.min_section_sec = checkedFloatFromVal(min_section_sec_val, "minSectionSec");
  // An absent threshold is the library default, not a value the facade restates.
  if (!threshold_val.isUndefined() && !threshold_val.isNull()) {
    config.boundary_threshold = checkedFloatFromVal(threshold_val, "threshold");
  }
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));
  // The request keys, as the Node addon reports them through the C entry.
  sonare::util::check::with_camel_paths(
      [&] { validate_section_config(config); },
      std::array<sonare::util::check::MemberKey, 4>{{{"n_fft", "nFft"},
                                                     {"hop_length", "hopLength"},
                                                     {"min_section_sec", "minSectionSec"},
                                                     {"boundary_threshold", "threshold"}}});

  SectionAnalyzer analyzer(audio, config);

  val sections = val::array();
  for (const Section& section : analyzer.sections()) {
    val item = val::object();
    item.set("type", static_cast<int>(section.type));
    item.set("name", section.type_string());
    item.set("startSec", section.start);
    item.set("endSec", section.end);
    item.set("energyLevel", section.energy_level);
    item.set("confidence", section.confidence);
    item.set("group", section.group);
    sections.call<void>("push", item);
  }
  val out = val::object();
  out.set("sections", sections);
  out.set("form", analyzer.form());
  return out;
}

// Mirrors sonare_detect_boundaries / SonareBoundaryResult and the Python
// detect_boundaries: the unlabelled layer analyzeSections is built on, returning
// the transitions plus the novelty curve they were picked from.
// The options bag seeds itself from BoundaryConfig, so an omitted field takes
// the core's own default rather than a restated copy of it.
val js_detect_boundaries(val samples, const val& sample_rate, val options) {
  const int sr = checkedIntFromVal(sample_rate, "sampleRate");

  BoundaryConfig config;
  if (!options.isUndefined() && !options.isNull()) {
    config.n_fft =
        optionalDomainValue<int>(objectProperty(options, "nFft"), "nFft", kBoundaryCountDomain)
            .value_or(config.n_fft);
    config.hop_length = optionalDomainValue<int>(objectProperty(options, "hopLength"), "hopLength",
                                                 kBoundaryCountDomain)
                            .value_or(config.hop_length);
    config.kernel_size = optionalDomainValue<int>(objectProperty(options, "kernelSize"),
                                                  "kernelSize", kBoundaryCountDomain)
                             .value_or(config.kernel_size);
    config.threshold = floatProperty(options, "threshold", config.threshold);
    config.absolute_threshold =
        floatProperty(options, "absoluteThreshold", config.absolute_threshold);
    config.n_mfcc =
        optionalDomainValue<int>(objectProperty(options, "nMfcc"), "nMfcc", kBoundaryCountDomain)
            .value_or(config.n_mfcc);
    config.n_chroma = optionalDomainValue<int>(objectProperty(options, "nChroma"), "nChroma",
                                               kBoundaryCountDomain)
                          .value_or(config.n_chroma);
    config.peak_distance = floatProperty(options, "peakDistanceSec", config.peak_distance);
    config.reference_window = floatProperty(options, "referenceWindowSec", config.reference_window);
    config.use_mfcc = boolProperty(options, "useMfcc", config.use_mfcc);
    config.use_chroma = boolProperty(options, "useChroma", config.use_chroma);
  }
  // The detector does not refuse these itself; checked before the audio is loaded, in the
  // request keys the Node addon reports through the C entry.
  sonare::util::check::with_camel_paths(
      [&] { validate_boundary_config(config); },
      std::array<sonare::util::check::MemberKey, 11>{{{"n_fft", "nFft"},
                                                      {"hop_length", "hopLength"},
                                                      {"kernel_size", "kernelSize"},
                                                      {"n_mfcc", "nMfcc"},
                                                      {"n_chroma", "nChroma"},
                                                      {"threshold", "threshold"},
                                                      {"absolute_threshold", "absoluteThreshold"},
                                                      {"peak_distance", "peakDistanceSec"},
                                                      {"reference_window", "referenceWindowSec"},
                                                      {"use_mfcc", "useMfcc"},
                                                      {"use_chroma", "useChroma"}}});

  Audio audio = loadValidatedAudio("samples", samples, sr);
  BoundaryDetector detector(audio, config);

  val boundaries = val::array();
  for (const Boundary& boundary : detector.boundaries()) {
    val item = val::object();
    item.set("time", boundary.time);
    item.set("frame", boundary.frame);
    item.set("strength", boundary.strength);
    boundaries.call<void>("push", item);
  }

  val out = val::object();
  out.set("boundaries", boundaries);
  out.set("noveltyCurve", vectorToFloat32Array(detector.novelty_curve()));
  out.set("noveltyPeak", detector.novelty_peak());
  // The analysis rate, not the caller's: input above 22.05 kHz is resampled
  // before any feature is computed, and `frame` indexes that grid.
  out.set("sampleRate", detector.sample_rate());
  out.set("hopLength", detector.hop_length());
  out.set("nFrames", detector.n_frames());
  out.set("frameStride", detector.frame_stride());
  return out;
}

// Mirrors sonare_analyze_melody / SonareMelodyResult: extracts the melody
// contour via YIN and returns { points: [{ time, frequency, confidence }],
// pitchRangeOctaves, pitchStability, meanFrequency, vibratoRate }.
// Embind passes every argument, so the narrowed sizes carry no C++ default, and
// neither can the float parameters that precede them.
val js_analyze_melody(val samples, const val& sample_rate, const val& fmin_val, const val& fmax_val,
                      const val& frame_length_val, const val& hop_length_val,
                      const val& threshold_val, bool use_pyin, bool center) {
  const int frame_length =
      domainNumberFromVal<int>(frame_length_val, "frameLength", kMelodyFrameDomain);
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kMelodyFrameDomain);
  const float fmin = domainNumberFromVal<float>(fmin_val, "f0MinHz", kMelodyF0MinHzDomain);
  // The core states f0MaxHz only against f0MinHz, so the reader asks for a finite float.
  const float fmax = checkedFloatFromVal(fmax_val, "f0MaxHz");
  const float threshold =
      domainNumberFromVal<float>(threshold_val, "threshold", kMelodyThresholdDomain);
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));

  MelodyConfig config;
  config.fmin = fmin;
  config.fmax = fmax;
  config.frame_length = frame_length;
  config.hop_length = hop_length;
  config.threshold = threshold;
  config.use_pyin = use_pyin;
  config.center = center;
  validate_melody_config(config);

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
// frequencies (nBins), gamma (the VQT bandwidth offset it ran with; 0 for a CQT) }.
val cqtResultToVal(const CqtResult& result, float gamma = 0.0f) {
  val out = val::object();
  out.set("nBins", result.n_bins());
  out.set("nFrames", result.n_frames());
  out.set("hopLength", result.hop_length());
  out.set("sampleRate", result.sample_rate());
  out.set("magnitude", vectorToFloat32Array(result.magnitude()));
  out.set("frequencies", vectorToFloat32Array(result.frequencies()));
  out.set("gamma", gamma);
  return out;
}

val js_cqt(val samples, const val& sample_rate, const val& hop_length, const val& fmin_val,
           const val& n_bins, const val& bins_per_octave, const val& filter_scale_val) {
  const float fmin = checkedFloatFromVal(fmin_val, "fminHz");
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));

  CqtConfig config;
  config.hop_length = domainNumberFromVal<int>(hop_length, "hopLength", kCqtHopLengthDomain);
  config.fmin = fmin;
  config.n_bins = domainNumberFromVal<int>(n_bins, "nBins", kCqtCountDomain);
  config.bins_per_octave =
      domainNumberFromVal<int>(bins_per_octave, "binsPerOctave", kCqtCountDomain);
  config.filter_scale =
      optionalDomainNumber(filter_scale_val, "filterScale").value_or(config.filter_scale);

  return cqtResultToVal(
      util::check::with_camel_paths([&] { return cqt(audio, config); }, kCqtRequestKeys));
}

val js_pseudo_cqt(val samples, const val& sample_rate, const val& hop_length, const val& fmin_val,
                  const val& n_bins, const val& bins_per_octave, const val& filter_scale_val) {
  const float fmin = checkedFloatFromVal(fmin_val, "fminHz");
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));

  CqtConfig config;
  config.hop_length = domainNumberFromVal<int>(hop_length, "hopLength", kCqtHopLengthDomain);
  config.fmin = fmin;
  config.n_bins = domainNumberFromVal<int>(n_bins, "nBins", kCqtCountDomain);
  config.bins_per_octave =
      domainNumberFromVal<int>(bins_per_octave, "binsPerOctave", kCqtCountDomain);
  config.filter_scale =
      optionalDomainNumber(filter_scale_val, "filterScale").value_or(config.filter_scale);

  return cqtResultToVal(
      util::check::with_camel_paths([&] { return pseudo_cqt(audio, config); }, kCqtRequestKeys));
}

val js_hybrid_cqt(val samples, const val& sample_rate, const val& hop_length, const val& fmin_val,
                  const val& n_bins, const val& bins_per_octave, const val& filter_scale_val) {
  const float fmin = checkedFloatFromVal(fmin_val, "fminHz");
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));

  CqtConfig config;
  config.hop_length = domainNumberFromVal<int>(hop_length, "hopLength", kCqtHopLengthDomain);
  config.fmin = fmin;
  config.n_bins = domainNumberFromVal<int>(n_bins, "nBins", kCqtCountDomain);
  config.bins_per_octave =
      domainNumberFromVal<int>(bins_per_octave, "binsPerOctave", kCqtCountDomain);
  config.filter_scale =
      optionalDomainNumber(filter_scale_val, "filterScale").value_or(config.filter_scale);

  return cqtResultToVal(
      util::check::with_camel_paths([&] { return hybrid_cqt(audio, config); }, kCqtRequestKeys));
}

val js_vqt(val samples, const val& sample_rate, const val& hop_length, const val& fmin_val,
           const val& n_bins, const val& bins_per_octave, const val& filter_scale_val,
           const val& gamma_val) {
  const float fmin = checkedFloatFromVal(fmin_val, "fminHz");
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));

  VqtConfig config;
  config.hop_length = domainNumberFromVal<int>(hop_length, "hopLength", kCqtHopLengthDomain);
  config.fmin = fmin;
  config.n_bins = domainNumberFromVal<int>(n_bins, "nBins", kCqtCountDomain);
  config.bins_per_octave =
      domainNumberFromVal<int>(bins_per_octave, "binsPerOctave", kCqtCountDomain);
  config.filter_scale =
      optionalDomainNumber(filter_scale_val, "filterScale").value_or(config.filter_scale);
  // Absent or NaN is the automatic ERB-derived bandwidth, as on the C ABI; a given value is an
  // offset in Hz.
  const std::optional<float> gamma = optionalDomainNumber(gamma_val, "gamma");
  if (gamma.has_value() && !std::isnan(*gamma)) util::check::at_least(*gamma, 0.0, "gamma");
  config.gamma = gamma.value_or(std::numeric_limits<float>::quiet_NaN());

  return cqtResultToVal(
      util::check::with_camel_paths([&] { return vqt(audio, config); }, kCqtRequestKeys),
      resolve_vqt_gamma(config));
}

void registerFeatureMusicBindings() {
  function("chroma", &js_chroma);
  function("chromaCens", &js_chroma_cens);
  function("chromaCqt", &js_chroma_cqt);
  function("bassChroma", &js_bass_chroma);
  function("nnlsChroma", &js_nnls_chroma);
  function("cqt", &js_cqt);
  function("pseudoCqt", &js_pseudo_cqt);
  function("hybridCqt", &js_hybrid_cqt);
  function("vqt", &js_vqt);
  function("analyzeSections", &js_analyze_sections);
  function("detectBoundaries", &js_detect_boundaries);
  function("analyzeMelody", &js_analyze_melody);
}

#endif  // __EMSCRIPTEN__
