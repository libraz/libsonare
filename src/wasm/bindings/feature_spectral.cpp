/// @file feature_spectral.cpp
/// @brief Embind bindings for spectral descriptor feature APIs.

#ifdef __EMSCRIPTEN__

#include "common.h"

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

void registerFeatureSpectralBindings() {
  function("spectralCentroid", &js_spectral_centroid);
  function("spectralBandwidth", &js_spectral_bandwidth);
  function("spectralRolloff", &js_spectral_rolloff);
  function("spectralFlatness", &js_spectral_flatness);
  function("zeroCrossingRate", &js_zero_crossing_rate);
  function("rmsEnergy", &js_rms_energy);
  function("spectralContrast", &js_spectral_contrast);
  function("polyFeatures", &js_poly_features);
  function("zeroCrossings", &js_zero_crossings);
}

#endif  // __EMSCRIPTEN__
