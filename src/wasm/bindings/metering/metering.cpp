/// @file metering.cpp
/// @brief Embind bindings for offline loudness and metering APIs.

#ifdef __EMSCRIPTEN__

#include "wasm/bindings/common/common.h"

// ============================================================================
// Analysis - LUFS metering
// ============================================================================

val js_lufs(val samples, int sample_rate) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  metering::LufsResult result = metering::lufs(audio);
  val out = val::object();
  out.set("integratedLufs", result.integrated_lufs);
  out.set("momentaryLufs", result.momentary_lufs);
  out.set("shortTermLufs", result.short_term_lufs);
  out.set("maxMomentaryLufs", result.max_momentary_lufs);
  out.set("maxShortTermLufs", result.max_short_term_lufs);
  out.set("loudnessRange", result.loudness_range);
  return out;
}

val js_momentary_lufs(val samples, int sample_rate) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  return vectorToFloat32Array(metering::momentary_lufs(audio));
}

val js_short_term_lufs(val samples, int sample_rate) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  return vectorToFloat32Array(metering::short_term_lufs(audio));
}

// ITU-R BS.1770-4 multi-channel loudness over an interleaved buffer. Mirrors
// the C ABI sonare_lufs_interleaved. @p samples holds frames * channels values
// in channel-interleaved order. Returns the SonareLufsResult fields as
// { integratedLufs, momentaryLufs, shortTermLufs, loudnessRange }.
val js_lufs_interleaved(val samples, int channels, int sample_rate) {
  // Derive the per-channel frame count from the interleaved buffer length so the
  // JS/Python facades share one (samples, channels, sampleRate) signature. The
  // shared helper validates the buffer and rejects a length that is not a whole
  // number of frames (the C ABI takes frames explicitly, so it cannot truncate).
  size_t frames = 0;
  std::vector<float> data = loadValidatedInterleaved(samples, channels, sample_rate, &frames);
  metering::LufsResult result =
      metering::lufs_interleaved(data.data(), frames, channels, sample_rate);
  val out = val::object();
  out.set("integratedLufs", result.integrated_lufs);
  out.set("momentaryLufs", result.momentary_lufs);
  out.set("shortTermLufs", result.short_term_lufs);
  out.set("maxMomentaryLufs", result.max_momentary_lufs);
  out.set("maxShortTermLufs", result.max_short_term_lufs);
  out.set("loudnessRange", result.loudness_range);
  return out;
}

// EBU R128 / Tech 3342 Loudness Range (LRA) in LU for a mono buffer. Mirrors
// the C ABI sonare_ebur128_loudness_range.
float js_ebur128_loudness_range(val samples, int sample_rate) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  return metering::ebur128_loudness_range(audio);
}

// ============================================================================
// Metering — offline basic / true-peak / clipping / dynamic-range
// ============================================================================

float js_metering_peak_db(val samples, int sample_rate) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  return metering::peak_db(audio);
}

float js_metering_rms_db(val samples, int sample_rate) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  return metering::rms_db(audio);
}

float js_metering_silence_ratio(val samples, int sample_rate, float threshold_db, int frame_length,
                                int hop_length) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  return metering::silence_ratio(audio, threshold_db, frame_length, hop_length);
}

float js_metering_crest_factor_db(val samples, int sample_rate) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  return metering::crest_factor_db(audio);
}

float js_metering_crest_factor_db_stereo(val left_samples, val right_samples, int sample_rate) {
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples",
                               "meteringCrestFactorDbStereo input", true);
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  validate_offline_audio_input(left.data(), left.size(), sample_rate);
  validate_offline_audio_input(right.data(), right.size(), sample_rate);
  std::vector<float> interleaved(left.size() * 2);
  for (size_t index = 0; index < left.size(); ++index) {
    interleaved[2 * index] = left[index];
    interleaved[2 * index + 1] = right[index];
  }
  return metering::crest_factor_db_interleaved(interleaved.data(), left.size(), 2);
}

float js_metering_dc_offset(val samples, int sample_rate) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  return metering::dc_offset(audio);
}

float js_metering_true_peak_db(val samples, int sample_rate, int oversample_factor) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  const int factor = oversample_factor == 0 ? 4 : oversample_factor;
  if (factor < 1 || factor > 16 || (factor & (factor - 1)) != 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "oversample must be 0 or a power of two from 1 to 16");
  }
  return metering::true_peak_db(audio, factor);
}

val js_metering_detect_clipping(val samples, int sample_rate, float threshold,
                                int min_region_samples) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  if (min_region_samples < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "minRegionSamples must be non-negative");
  }
  const float effective_threshold = threshold <= 0.0f ? 0.999f : threshold;
  const size_t effective_min =
      min_region_samples == 0 ? 1u : static_cast<size_t>(min_region_samples);
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
  Audio audio = loadValidatedAudio(samples, sample_rate);
  const metering::DynamicRangeConfig cfg = metering::dynamic_range_config_from_public(
      window_sec, hop_sec, low_percentile, high_percentile);
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
  if (sample_rate < sonare::kMinAudioSampleRate || sample_rate > sonare::kMaxAudioSampleRate) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  std::string(fn_label) + ": sampleRate is out of range");
  }
  const std::size_t left_length = wasmFloat32ArrayLength(left, "left channel");
  const std::size_t right_length = wasmFloat32ArrayLength(right, "right channel");
  validateWasmFloat32ElementBudget({left_length, right_length}, "stereo meter input");
  if (left_length != right_length) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        std::string(fn_label) + ": left and right must have the same length");
  }
  *out_left = float32ArrayToVector(left);
  *out_right = float32ArrayToVector(right);
  validate_offline_audio_input(out_left->data(), out_left->size(), sample_rate);
  validate_offline_audio_input(out_right->data(), out_right->size(), sample_rate);
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
  Audio audio = loadValidatedAudio(samples, sample_rate);
  metering::SpectrumConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (hasProperty(options, "nFft")) {
      const int n = options["nFft"].as<int>();
      if (n < 0) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "meteringSpectrum: nFft must be non-negative");
      }
      if (n > 0) cfg.n_fft = n;
    }
    if (hasProperty(options, "applyOctaveSmoothing")) {
      cfg.apply_octave_smoothing = options["applyOctaveSmoothing"].as<bool>();
    }
    if (hasProperty(options, "octaveFraction")) {
      const int f = options["octaveFraction"].as<int>();
      if (f < 0) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "meteringSpectrum: octaveFraction must be non-negative");
      }
      if (f > 0) cfg.octave_fraction = f;
    }
    if (hasProperty(options, "dbRef")) {
      const float ref = options["dbRef"].as<float>();
      if (ref < 0.0f) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "meteringSpectrum: dbRef must be non-negative");
      }
      if (ref > 0.0f) cfg.db_ref = ref;
    }
    if (hasProperty(options, "dbAmin")) {
      const float amin = options["dbAmin"].as<float>();
      if (amin < 0.0f) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "meteringSpectrum: dbAmin must be non-negative");
      }
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
  Audio audio = loadValidatedAudio(samples, sample_rate);
  metering::SpectrumConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    if (hasProperty(options, "nFft")) {
      const int n = options["nFft"].as<int>();
      if (n < 0) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "meteringSpectrumFrame: nFft must be non-negative");
      }
      if (n > 0) cfg.n_fft = n;
    }
    if (hasProperty(options, "applyOctaveSmoothing")) {
      cfg.apply_octave_smoothing = options["applyOctaveSmoothing"].as<bool>();
    }
    if (hasProperty(options, "octaveFraction")) {
      const int f = options["octaveFraction"].as<int>();
      if (f < 0) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "meteringSpectrumFrame: octaveFraction must be non-negative");
      }
      if (f > 0) cfg.octave_fraction = f;
    }
    if (hasProperty(options, "dbRef")) {
      const float ref = options["dbRef"].as<float>();
      if (ref < 0.0f) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "meteringSpectrumFrame: dbRef must be non-negative");
      }
      if (ref > 0.0f) cfg.db_ref = ref;
    }
    if (hasProperty(options, "dbAmin")) {
      const float amin = options["dbAmin"].as<float>();
      if (amin < 0.0f) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "meteringSpectrumFrame: dbAmin must be non-negative");
      }
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

val emit_waveform_peaks_result(const metering::WaveformPeaksResult& result) {
  val out = val::object();
  out.set("min", vectorToFloat32Array(result.min));
  out.set("max", vectorToFloat32Array(result.max));
  out.set("channels", result.channels);
  out.set("bucketCount", result.bucket_count);
  out.set("samplesPerBucket", result.samples_per_bucket);
  return out;
}

val js_waveform_peaks(val samples, int channels, size_t samples_per_bucket) {
  std::vector<float> data = float32ArrayToVector(samples);
  // Reject a length that is not a whole number of interleaved frames instead of
  // silently dropping a trailing partial frame (matches the Node/Python facades,
  // which throw on the same input).
  if (channels <= 0 || data.size() % static_cast<size_t>(channels) != 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "waveformPeaks: samples length must be a multiple of channels");
  }
  const size_t frames = data.size() / static_cast<size_t>(channels);
  return emit_waveform_peaks_result(
      metering::waveform_peaks(data.data(), frames, channels, samples_per_bucket));
}

val js_waveform_peak_pyramid(val samples, int channels, val js_levels) {
  std::vector<float> data = float32ArrayToVector(samples);
  std::vector<size_t> levels;
  const uint32_t n = js_levels["length"].as<uint32_t>();
  levels.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    levels.push_back(js_levels[i].as<size_t>());
  }
  // Reject a length that is not a whole number of interleaved frames instead of
  // silently dropping a trailing partial frame (matches the Node/Python facades,
  // which throw on the same input).
  if (channels <= 0 || data.size() % static_cast<size_t>(channels) != 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "waveformPeakPyramid: samples length must be a multiple of channels");
  }
  const size_t frames = data.size() / static_cast<size_t>(channels);
  const auto pyramid = metering::waveform_peak_pyramid(data.data(), frames, channels, levels);
  val out = val::array();
  for (size_t i = 0; i < pyramid.size(); ++i) {
    out.set(i, emit_waveform_peaks_result(pyramid[i]));
  }
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
  function("meteringSilenceRatio", &js_metering_silence_ratio);
  function("meteringCrestFactorDb", &js_metering_crest_factor_db);
  function("meteringCrestFactorDbStereo", &js_metering_crest_factor_db_stereo);
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
  function("waveformPeaks", &js_waveform_peaks);
  function("waveformPeakPyramid", &js_waveform_peak_pyramid);
}

#endif  // __EMSCRIPTEN__
