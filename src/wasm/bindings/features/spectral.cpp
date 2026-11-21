/// @file feature_spectral.cpp
/// @brief Embind bindings for spectral descriptor feature APIs.

#ifdef __EMSCRIPTEN__

#include "feature/spectral.h"

#include "c_api/segment_input_domains.h"
#include "core/spectrum.h"
#include "feature/onset.h"
#include "wasm/bindings/common/common.h"

namespace {

std::vector<float> load_segment_matrix(val input, int rows, int cols, const char* data_name) {
  std::size_t expected = 0;
  if (!sonare::numeric::checked_size_product(static_cast<std::size_t>(rows),
                                             static_cast<std::size_t>(cols),
                                             kMaxWasmFloat32Elements, &expected) ||
      wasmFloat32ArrayLength(input, data_name) != expected) {
    throw WasmRangeError(std::string(data_name) + " length must equal rows * cols");
  }
  std::vector<float> data = float32ArrayToVector(input);
  for (std::size_t i = 0; i < data.size(); ++i) {
    if (!std::isfinite(data[i])) {
      throw WasmRangeError(std::string(data_name) + " contains a non-finite value at index " +
                           std::to_string(i));
    }
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

val js_spectral_centroid(val samples, const val& sample_rate_val, const val& n_fft_val,
                         const val& hop_length_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kStftHopDomain);
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> centroid = spectral_centroid(spec, sample_rate);

  return vectorToFloat32Array(centroid);
}

val js_spectral_bandwidth(val samples, const val& sample_rate_val, const val& n_fft_val,
                          const val& hop_length_val, const val& p_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kStftHopDomain);
  const float p = checkedFloatFromVal(p_val, "p");
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> bandwidth = spectral_bandwidth(spec, sample_rate, p);

  return vectorToFloat32Array(bandwidth);
}

val js_spectral_rolloff(val samples, const val& sample_rate_val, const val& n_fft_val,
                        const val& hop_length_val, const val& roll_percent_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kStftHopDomain);
  const float roll_percent = checkedFloatFromVal(roll_percent_val, "rollPercent");
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> rolloff = spectral_rolloff(spec, sample_rate, roll_percent);

  return vectorToFloat32Array(rolloff);
}

val js_spectral_flatness(val samples, const val& sample_rate_val, const val& n_fft_val,
                         const val& hop_length_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kStftHopDomain);
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);
  std::vector<float> flatness = spectral_flatness(spec);

  return vectorToFloat32Array(flatness);
}

val js_spectral_flux(val samples, const val& sample_rate_val, const val& n_fft_val,
                     const val& hop_length_val, const val& lag_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kStftHopDomain);
  const int lag = domainNumberFromVal<int>(lag_val, "lag", kOnsetCountDomain);
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);
  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  return vectorToFloat32Array(spectral_flux(Spectrogram::compute(audio, config), lag));
}

val js_zero_crossing_rate(val samples, const val& sample_rate_val, const val& frame_length_val,
                          const val& hop_length_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int frame_length =
      domainNumberFromVal<int>(frame_length_val, "frameLength", kSpectralCountDomain);
  const int hop_length =
      domainNumberFromVal<int>(hop_length_val, "hopLength", kSpectralCountDomain);
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);
  std::vector<float> zcr = zero_crossing_rate(audio, frame_length, hop_length);
  return vectorToFloat32Array(zcr);
}

val js_rms_energy(val samples, const val& sample_rate_val, const val& frame_length_val,
                  const val& hop_length_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int frame_length =
      domainNumberFromVal<int>(frame_length_val, "frameLength", kSpectralCountDomain);
  const int hop_length =
      domainNumberFromVal<int>(hop_length_val, "hopLength", kSpectralCountDomain);
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);
  std::vector<float> rms = rms_energy(audio, frame_length, hop_length);
  return vectorToFloat32Array(rms);
}

// Spectral contrast: peak-to-valley energy per band per frame. Mirrors the C
// ABI sonare_spectral_contrast / librosa.feature.spectral_contrast. Returns a
// row-major matrix [(n_bands + 1) x n_frames] as { data, rows, cols }, with the
// extra row holding the residual band.
val js_spectral_contrast(val samples, const val& sample_rate_val, const val& n_fft_val,
                         const val& hop_length_val, const val& n_bands_val, const val& fmin_val,
                         const val& quantile_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kStftHopDomain);
  const int n_bands = domainNumberFromVal<int>(n_bands_val, "nBands", kSpectralCountDomain);
  const float fmin = checkedFloatFromVal(fmin_val, "fmin");
  const float quantile = checkedFloatFromVal(quantile_val, "quantile");
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);

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
val js_poly_features(val samples, const val& sample_rate_val, const val& n_fft_val,
                     const val& hop_length_val, const val& order_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kStftHopDomain);
  const int order = domainNumberFromVal<int>(order_val, "order", kSpectralOrderDomain);
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);

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
val js_zero_crossings(val samples, const val& threshold_val, bool ref_magnitude, bool pad,
                      bool zero_pos) {
  const float threshold = checkedFloatFromVal(threshold_val, "threshold");
  std::vector<float> data = float32ArrayToVector(samples);
  std::vector<int> indices =
      zero_crossings(data.data(), data.size(), threshold, ref_magnitude, pad, zero_pos);
  return vectorToInt32Array(indices);
}

// ============================================================================
// Features - Segmentation (librosa.segment-compatible)
// ============================================================================

val js_segment_cross_similarity(val x, const val& x_rows_val, const val& x_cols_val, val y,
                                const val& y_rows_val, const val& y_cols_val, const val& k_val,
                                const std::string& metric, const std::string& mode) {
  const int x_rows =
      domainNumberFromVal<int>(x_rows_val, "xRows", sonare_c_detail::kSegmentMatrixExtentDomain);
  const int x_cols =
      domainNumberFromVal<int>(x_cols_val, "xCols", sonare_c_detail::kSegmentMatrixExtentDomain);
  const int y_rows =
      domainNumberFromVal<int>(y_rows_val, "yRows", sonare_c_detail::kSegmentMatrixExtentDomain);
  const int y_cols =
      domainNumberFromVal<int>(y_cols_val, "yCols", sonare_c_detail::kSegmentMatrixExtentDomain);
  const int k = optionalDomainValue<int>(k_val, "k", sonare_c_detail::kSegmentNeighbourCountDomain)
                    .value_or(0);
  std::vector<float> x_data = load_segment_matrix(x, x_rows, x_cols, "x");
  std::vector<float> y_data = load_segment_matrix(y, y_rows, y_cols, "y");
  if (x_rows != y_rows) {
    throw WasmRangeError("feature dimensions must match");
  }
  const int rows = mode == "affinity" ? y_cols : x_cols;
  const int cols = mode == "affinity" ? x_cols : y_cols;
  return segment_matrix_result(cross_similarity(x_data.data(), x_rows, x_cols, y_data.data(),
                                                y_rows, y_cols, k, metric, mode),
                               rows, cols);
}

val js_segment_recurrence_matrix(val data, const val& rows_val, const val& cols_val,
                                 const val& k_val, const val& width_val, bool sym,
                                 const std::string& metric, const std::string& mode) {
  const int rows =
      domainNumberFromVal<int>(rows_val, "rows", sonare_c_detail::kSegmentMatrixExtentDomain);
  const int cols =
      domainNumberFromVal<int>(cols_val, "cols", sonare_c_detail::kSegmentMatrixExtentDomain);
  const int k = optionalDomainValue<int>(k_val, "k", sonare_c_detail::kSegmentNeighbourCountDomain)
                    .value_or(0);
  const int width =
      domainNumberFromVal<int>(width_val, "width", sonare_c_detail::kSegmentRecurrenceWidthDomain);
  std::vector<float> values = load_segment_matrix(data, rows, cols, "data");
  return segment_matrix_result(
      recurrence_matrix(values.data(), rows, cols, k, width, sym, metric, mode), cols, cols);
}

val js_segment_recurrence_to_lag(val recurrence, const val& n_val, bool pad) {
  const int n = domainNumberFromVal<int>(n_val, "n", sonare_c_detail::kSegmentMatrixExtentDomain);
  std::vector<float> values = load_segment_matrix(recurrence, n, n, "recurrence");
  return segment_matrix_result(recurrence_to_lag(values.data(), n, pad), pad ? 2 * n : n, n);
}

val js_segment_lag_to_recurrence(val lag, const val& rows_val, const val& lags_val) {
  const int rows =
      domainNumberFromVal<int>(rows_val, "rows", sonare_c_detail::kSegmentMatrixExtentDomain);
  const int lags =
      domainNumberFromVal<int>(lags_val, "lags", sonare_c_detail::kSegmentMatrixExtentDomain);
  std::vector<float> values = load_segment_matrix(lag, lags, rows, "lag");
  return segment_matrix_result(lag_to_recurrence(values.data(), rows, lags), rows, rows);
}

val js_segment_subsegment(val data, const val& rows_val, const val& cols_val, val boundaries,
                          const val& n_segments_val) {
  const int rows =
      domainNumberFromVal<int>(rows_val, "rows", sonare_c_detail::kSegmentMatrixExtentDomain);
  const int cols =
      domainNumberFromVal<int>(cols_val, "cols", sonare_c_detail::kSegmentMatrixExtentDomain);
  const int n_segments = domainNumberFromVal<int>(n_segments_val, "nSegments",
                                                  sonare_c_detail::kSubsegmentCountDomain);
  std::vector<float> values = load_segment_matrix(data, rows, cols, "data");
  const std::vector<int32_t> source = int32ArrayToVector(boundaries);
  const std::vector<int> points(source.begin(), source.end());
  return vectorToInt32Array(subsegment(values.data(), rows, cols, points, n_segments));
}

val js_segment_agglomerative(val data, const val& rows_val, const val& cols_val, const val& k_val,
                             const std::string& linkage) {
  const int rows =
      domainNumberFromVal<int>(rows_val, "rows", sonare_c_detail::kSegmentMatrixExtentDomain);
  const int cols =
      domainNumberFromVal<int>(cols_val, "cols", sonare_c_detail::kSegmentMatrixExtentDomain);
  const int k =
      domainNumberFromVal<int>(k_val, "k", sonare_c_detail::kAgglomerativeClusterCountDomain);
  std::vector<float> values = load_segment_matrix(data, rows, cols, "data");
  return vectorToInt32Array(agglomerative(values.data(), rows, cols, k, linkage));
}

val js_segment_path_enhance(val recurrence, const val& n_val, const val& win_val,
                            const val& max_ratio_val, const val& min_ratio_val,
                            const val& n_filters_val) {
  const int n = domainNumberFromVal<int>(n_val, "n", sonare_c_detail::kSegmentMatrixExtentDomain);
  const int win =
      domainNumberFromVal<int>(win_val, "win", sonare_c_detail::kPathEnhanceWindowDomain);
  const int max_ratio = domainNumberFromVal<int>(max_ratio_val, "maxRatio",
                                                 sonare_c_detail::kPathEnhanceMaxRatioDomain);
  const int min_ratio = domainNumberFromVal<int>(min_ratio_val, "minRatio",
                                                 sonare_c_detail::kPathEnhanceMinRatioDomain);
  const int n_filters = domainNumberFromVal<int>(n_filters_val, "nFilters",
                                                 sonare_c_detail::kPathEnhanceFilterCountDomain);
  std::vector<float> values = load_segment_matrix(recurrence, n, n, "recurrence");
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
