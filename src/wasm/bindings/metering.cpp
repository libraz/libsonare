/// @file metering.cpp
/// @brief Embind bindings for offline loudness and metering APIs.

#ifdef __EMSCRIPTEN__

#include "common.h"

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
  // A NEGATIVE percentile selects the library default (matching the C ABI);
  // 0.0..1.0 is taken verbatim, so 0 is a real request for the 0th percentile.
  if (low_percentile >= 0.0f) cfg.low_percentile = low_percentile;
  if (high_percentile >= 0.0f) cfg.high_percentile = high_percentile;
  // The core accepts low == high (a valid 0-LU range); reject only an inverted
  // pair.
  if (cfg.low_percentile > cfg.high_percentile) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "meteringDynamicRange: lowPercentile must not exceed highPercentile");
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

// Display-sized mid/side vectorscope. Mirrors js_metering_vectorscope but
// decimates the point series to at most max_points points (0 = one point per
// input sample). Backs the C ABI sonare_metering_vectorscope_decimated.
val js_metering_vectorscope_decimated(val left, val right, int sample_rate, size_t max_points) {
  std::vector<float> l;
  std::vector<float> r;
  ensureStereoPair(left, right, sample_rate, "meteringVectorscopeDecimated", &l, &r);
  std::vector<metering::VectorscopePoint> points =
      metering::vectorscope(l.data(), r.data(), l.size(), max_points);
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

// Display-sized phase scope. Mirrors js_metering_phase_scope but decimates the
// point series to at most max_points points (0 = one point per input sample);
// the summary stats are always computed over the full-resolution signal. Backs
// the C ABI sonare_metering_phase_scope_decimated.
val js_metering_phase_scope_decimated(val left, val right, int sample_rate, size_t max_points) {
  std::vector<float> l;
  std::vector<float> r;
  ensureStereoPair(left, right, sample_rate, "meteringPhaseScopeDecimated", &l, &r);
  metering::PhaseScopeResult result =
      metering::phase_scope(l.data(), r.data(), l.size(), max_points);
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

// True single-frame magnitude / power / dB spectrum: one Hann-windowed nFft FFT
// of the window [frameOffset, frameOffset + nFft), zero-padded past the end. NOT
// time-averaged like js_metering_spectrum. Backs the C ABI
// sonare_metering_spectrum_frame.
val js_metering_spectrum_frame(val samples, int sample_rate, size_t frame_offset, val options) {
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
                                  "meteringSpectrumFrame: nFft must be a power of two");
  }
  metering::SpectrumResult result = metering::spectrum_frame(audio, frame_offset, cfg);
  val out = val::object();
  out.set("frequencies", vectorToFloat32Array(result.frequencies));
  out.set("magnitude", vectorToFloat32Array(result.magnitude));
  out.set("power", vectorToFloat32Array(result.power));
  out.set("db", vectorToFloat32Array(result.db));
  out.set("nFft", result.n_fft);
  out.set("sampleRate", result.sample_rate);
  return out;
}

void registerMeteringBindings() {
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
  function("meteringVectorscopeDecimated", &js_metering_vectorscope_decimated);
  function("meteringPhaseScope", &js_metering_phase_scope);
  function("meteringPhaseScopeDecimated", &js_metering_phase_scope_decimated);
  function("meteringSpectrum", &js_metering_spectrum);
  function("meteringSpectrumFrame", &js_metering_spectrum_frame);
}

#endif  // __EMSCRIPTEN__
