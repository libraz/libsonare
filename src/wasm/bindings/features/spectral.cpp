/// @file feature_spectral.cpp
/// @brief Embind bindings for spectral descriptor feature APIs.

#ifdef __EMSCRIPTEN__

#include "wasm/bindings/common/common.h"

namespace {

std::vector<float> load_segment_matrix(const char* fn_name, val input, int rows, int cols,
                                       const char* data_name) {
  if (rows <= 0 || cols <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(fn_name) + ": matrix dimensions must be positive");
  }
  std::size_t expected = 0;
  if (!sonare::numeric::checked_size_product(static_cast<std::size_t>(rows),
                                             static_cast<std::size_t>(cols),
                                             kMaxWasmFloat32Elements, &expected) ||
      wasmFloat32ArrayLength(input, data_name) != expected) {
    throw SonareException(ErrorCode::InvalidParameter, std::string(fn_name) + ": " + data_name +
                                                           " length must equal rows * cols");
  }
  std::vector<float> data = float32ArrayToVector(input);
  if (!std::all_of(data.begin(), data.end(), [](float value) { return std::isfinite(value); })) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(fn_name) + ": " + data_name + " contains NaN or Inf");
  }
  return data;
}

val segment_matrix_result(std::vector<float> values, int rows, int cols) {
  val out = val::object();
  out.set("rows", rows);
  out.set("cols", cols);
  out.set("values", vectorToFloat32Array(values));
  return out;
}

}  // namespace

// ============================================================================
// Features - Spectral
// ============================================================================

val js_spectral_centroid(val samples, int sample_rate, int n_fft, int hop_length) {
  Audio audio = loadValidatedAudio(samples, sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> centroid = spectral_centroid(spec, sample_rate);

  return vectorToFloat32Array(centroid);
}

val js_spectral_bandwidth(val samples, int sample_rate, int n_fft, int hop_length, float p) {
  Audio audio = loadValidatedAudio(samples, sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> bandwidth = spectral_bandwidth(spec, sample_rate, p);

  return vectorToFloat32Array(bandwidth);
}

val js_spectral_rolloff(val samples, int sample_rate, int n_fft, int hop_length,
                        float roll_percent) {
  Audio audio = loadValidatedAudio(samples, sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> rolloff = spectral_rolloff(spec, sample_rate, roll_percent);

  return vectorToFloat32Array(rolloff);
}

val js_spectral_flatness(val samples, int sample_rate, int n_fft, int hop_length) {
  Audio audio = loadValidatedAudio(samples, sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> flatness = spectral_flatness(spec);

  return vectorToFloat32Array(flatness);
}

val js_spectral_flux(val samples, int sample_rate, int n_fft, int hop_length, int lag) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  return vectorToFloat32Array(spectral_flux(Spectrogram::compute(audio, config), lag));
}

val js_zero_crossing_rate(val samples, int sample_rate, int frame_length, int hop_length) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  std::vector<float> zcr = zero_crossing_rate(audio, frame_length, hop_length);
  return vectorToFloat32Array(zcr);
}

val js_rms_energy(val samples, int sample_rate, int frame_length, int hop_length) {
  Audio audio = loadValidatedAudio(samples, sample_rate);
  std::vector<float> rms = rms_energy(audio, frame_length, hop_length);
  return vectorToFloat32Array(rms);
}

// Spectral contrast: peak-to-valley energy per band per frame. Mirrors the C
// ABI sonare_spectral_contrast / librosa.feature.spectral_contrast. Returns a
// row-major matrix [(n_bands + 1) x n_frames] as { data, rows, cols }, with the
// extra row holding the residual band.
val js_spectral_contrast(val samples, int sample_rate, int n_fft, int hop_length, int n_bands,
                         float fmin, float quantile) {
  Audio audio = loadValidatedAudio(samples, sample_rate);

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
  Audio audio = loadValidatedAudio(samples, sample_rate);

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

// ============================================================================
// Features - Segmentation (librosa.segment-compatible)
// ============================================================================

val js_segment_cross_similarity(val x, int x_rows, int x_cols, val y, int y_rows, int y_cols, int k,
                                const std::string& metric, const std::string& mode) {
  std::vector<float> x_data = load_segment_matrix("segmentCrossSimilarity", x, x_rows, x_cols, "x");
  std::vector<float> y_data = load_segment_matrix("segmentCrossSimilarity", y, y_rows, y_cols, "y");
  if (x_rows != y_rows || k < 0) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "segmentCrossSimilarity: feature dimensions must match and k must be non-negative");
  }
  const int rows = mode == "affinity" ? y_cols : x_cols;
  const int cols = mode == "affinity" ? x_cols : y_cols;
  return segment_matrix_result(cross_similarity(x_data.data(), x_rows, x_cols, y_data.data(),
                                                y_rows, y_cols, k, metric, mode),
                               rows, cols);
}

val js_segment_recurrence_matrix(val data, int rows, int cols, int k, int width, bool sym,
                                 const std::string& metric, const std::string& mode) {
  std::vector<float> values =
      load_segment_matrix("segmentRecurrenceMatrix", data, rows, cols, "data");
  if (k < 0 || width < 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "segmentRecurrenceMatrix: k and width must be non-negative");
  }
  return segment_matrix_result(
      recurrence_matrix(values.data(), rows, cols, k, width, sym, metric, mode), cols, cols);
}

val js_segment_recurrence_to_lag(val recurrence, int n, bool pad) {
  std::vector<float> values =
      load_segment_matrix("segmentRecurrenceToLag", recurrence, n, n, "recurrence");
  return segment_matrix_result(recurrence_to_lag(values.data(), n, pad), n, pad ? 2 * n - 1 : n);
}

val js_segment_lag_to_recurrence(val lag, int rows, int lags) {
  std::vector<float> values = load_segment_matrix("segmentLagToRecurrence", lag, rows, lags, "lag");
  return segment_matrix_result(lag_to_recurrence(values.data(), rows, lags), rows, rows);
}

val js_segment_subsegment(val data, int rows, int cols, val boundaries, int n_segments) {
  std::vector<float> values = load_segment_matrix("segmentSubsegment", data, rows, cols, "data");
  if (n_segments <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "segmentSubsegment: n_segments must be positive");
  }
  const std::vector<int32_t> source = int32ArrayToVector(boundaries);
  const std::vector<int> points(source.begin(), source.end());
  return vectorToInt32Array(subsegment(values.data(), rows, cols, points, n_segments));
}

val js_segment_agglomerative(val data, int rows, int cols, int k, const std::string& linkage) {
  std::vector<float> values = load_segment_matrix("segmentAgglomerative", data, rows, cols, "data");
  if (k <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "segmentAgglomerative: k must be positive");
  }
  return vectorToInt32Array(agglomerative(values.data(), rows, cols, k, linkage));
}

val js_segment_path_enhance(val recurrence, int n, int win, int max_ratio, int min_ratio,
                            int n_filters) {
  std::vector<float> values =
      load_segment_matrix("segmentPathEnhance", recurrence, n, n, "recurrence");
  if (win <= 0 || max_ratio <= 0 || min_ratio < 0 || n_filters <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "segmentPathEnhance: invalid enhancement parameters");
  }
  return segment_matrix_result(path_enhance(values.data(), n, win, max_ratio, min_ratio, n_filters),
                               n, n);
}

void registerFeatureSpectralBindings() {
  function("spectralCentroid", &js_spectral_centroid);
  function("spectralBandwidth", &js_spectral_bandwidth);
  function("spectralRolloff", &js_spectral_rolloff);
  function("spectralFlatness", &js_spectral_flatness);
  function("spectralFlux", &js_spectral_flux);
  function("zeroCrossingRate", &js_zero_crossing_rate);
  function("rmsEnergy", &js_rms_energy);
  function("spectralContrast", &js_spectral_contrast);
  function("polyFeatures", &js_poly_features);
  function("zeroCrossings", &js_zero_crossings);
  function("segmentCrossSimilarity", &js_segment_cross_similarity);
  function("segmentRecurrenceMatrix", &js_segment_recurrence_matrix);
  function("segmentRecurrenceToLag", &js_segment_recurrence_to_lag);
  function("segmentLagToRecurrence", &js_segment_lag_to_recurrence);
  function("segmentSubsegment", &js_segment_subsegment);
  function("segmentAgglomerative", &js_segment_agglomerative);
  function("segmentPathEnhance", &js_segment_path_enhance);
}

#endif  // __EMSCRIPTEN__
