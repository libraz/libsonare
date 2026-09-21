/// @file decomposition.cpp
/// @brief Embind bindings for NMF decomposition, nearest-neighbour filtering and interval remix.

#ifdef __EMSCRIPTEN__

#include <cmath>
#include <limits>

#include "wasm/bindings/common/common.h"

// NMF decomposition of a non-negative spectrogram. Mirrors the C ABI
// sonare_decompose / librosa.decompose.decompose. Returns the two factor
// matrices as { w, h }: w is [n_features x n_components] row-major and h is
// [n_components x n_frames] row-major (both flat Float32Array buffers).
val js_decompose(val s, const val& n_features_val, const val& n_frames_val,
                 const val& n_components_val, const val& n_iter_val, const val& beta_val) {
  const int n_features = checkedIntFromVal(n_features_val, "nFeatures");
  const int n_frames = checkedIntFromVal(n_frames_val, "nFrames");
  const int n_components = checkedIntFromVal(n_components_val, "nComponents");
  const int n_iter = checkedIntFromVal(n_iter_val, "nIter");
  const float beta = checkedFloatFromVal(beta_val, "beta");
  std::vector<float> data = float32ArrayToVector(s);
  if (n_components <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "n_components must be positive");
  }
  if (n_iter <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "n_iter must be positive");
  }
  if (n_features <= 0 || n_frames <= 0 ||
      static_cast<size_t>(n_features) >
          std::numeric_limits<size_t>::max() / static_cast<size_t>(std::max(1, n_frames)) ||
      static_cast<size_t>(n_features) * static_cast<size_t>(n_frames) > data.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "spectrogram dimensions exceed input");
  }
  DecomposeResult result =
      decompose(data.data(), n_features, n_frames, n_components, n_iter, "mu", beta);

  val out = val::object();
  out.set("w", vectorToFloat32Array(result.W));
  out.set("h", vectorToFloat32Array(result.H));
  return out;
}

// NMF decomposition with a selectable initialiser. Mirrors the C ABI
// sonare_decompose_with_init / librosa.decompose.decompose (init). Identical to
// js_decompose but exposes the initialisation strategy: "random" (default,
// deterministic seed) or "nndsvd" (SVD-based warm start). Returns { w, h }.
val js_decompose_with_init(val s, const val& n_features_val, const val& n_frames_val,
                           const val& n_components_val, const val& n_iter_val, const val& beta_val,
                           std::string init) {
  const int n_features = checkedIntFromVal(n_features_val, "nFeatures");
  const int n_frames = checkedIntFromVal(n_frames_val, "nFrames");
  const int n_components = checkedIntFromVal(n_components_val, "nComponents");
  const int n_iter = checkedIntFromVal(n_iter_val, "nIter");
  const float beta = checkedFloatFromVal(beta_val, "beta");
  std::vector<float> data = float32ArrayToVector(s);
  if (n_components <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "n_components must be positive");
  }
  if (n_iter <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "n_iter must be positive");
  }
  if (n_features <= 0 || n_frames <= 0 ||
      static_cast<size_t>(n_features) >
          std::numeric_limits<size_t>::max() / static_cast<size_t>(std::max(1, n_frames)) ||
      static_cast<size_t>(n_features) * static_cast<size_t>(n_frames) > data.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "spectrogram dimensions exceed input");
  }
  if (init.empty()) init = "random";
  DecomposeResult result =
      decompose(data.data(), n_features, n_frames, n_components, n_iter, "mu", beta, init);

  val out = val::object();
  out.set("w", vectorToFloat32Array(result.W));
  out.set("h", vectorToFloat32Array(result.H));
  return out;
}

// Phase-carrying NMF separation. Mirrors the C ABI sonare_decompose_stems.
// Unlike decompose(), which returns W/H factors of a magnitude spectrogram,
// this applies a per-component soft mask to the ORIGINAL complex spectrogram,
// so every returned component keeps the source's phase and is directly
// listenable. Returns { components: Float32Array[], w, h }.
val js_decompose_stems(val samples, const val& sample_rate_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  Audio audio = loadValidatedAudio(samples, sample_rate);
  DecomposeStemsConfig config;
  // 0 is the "use the built-in default" sentinel the C ABI documents on
  // SonareDecomposeStemsConfig, so an explicit 0 must land on the same
  // effective value here as it does through sonare_decompose_stems. Reading
  // each field with 0 as its fallback lets an absent key and an explicit 0
  // take that one path. Everything else is left to validate_config, which
  // decompose_stems applies to the config it is handed, so a negative count or
  // a sub-unit mask power is rejected with the same error the C ABI returns.
  const int n_components = intProperty(options, "nComponents", 0);
  const int n_fft = intProperty(options, "nFft", 0);
  const int hop_length = intProperty(options, "hopLength", 0);
  const int n_iter = intProperty(options, "nIter", 0);
  const float beta = floatProperty(options, "beta", 0.0f);
  const float mask_power = floatProperty(options, "maskPower", 0.0f);
  const std::string init = stringProperty(options, "init", config.init);
  // Rejected before the sentinel promotion, exactly as the C ABI does: a
  // negative value must not be quietly swallowed by the "0 or less keeps the
  // default" rule that follows.
  if (n_components < 0 || n_fft < 0 || hop_length < 0 || n_iter < 0 || !std::isfinite(beta) ||
      !std::isfinite(mask_power) || mask_power < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "decomposeStems: counts must not be negative, beta and maskPower must "
                          "be finite, and maskPower must not be negative");
  }
  if (n_components > 0) config.n_components = n_components;
  if (n_fft > 0) config.n_fft = n_fft;
  if (hop_length > 0) config.hop_length = hop_length;
  if (n_iter > 0) config.n_iter = n_iter;
  if (beta != 0.0f) config.beta = beta;
  if (mask_power > 0.0f) config.mask_power = mask_power;
  config.init = init.empty() ? std::string("random") : init;

  DecomposeStemsResult result = decompose_stems(audio.data(), audio.size(), sample_rate, config);
  val components = val::array();
  for (const std::vector<float>& component : result.components) {
    components.call<void>("push", vectorToFloat32Array(component));
  }
  val out = val::object();
  out.set("components", components);
  out.set("w", vectorToFloat32Array(result.W));
  out.set("h", vectorToFloat32Array(result.H));
  out.set("sampleRate", sample_rate);
  return out;
}

// Nearest-neighbour spectrogram filter. Mirrors the C ABI sonare_nn_filter /
// librosa.decompose.nn_filter. Returns the smoothed spectrogram
// [n_features x n_frames] as { data, rows, cols }.
val js_nn_filter(val s, const val& n_features_val, const val& n_frames_val, std::string aggregate,
                 const val& k_val, const val& width_val) {
  const int n_features = checkedIntFromVal(n_features_val, "nFeatures");
  const int n_frames = checkedIntFromVal(n_frames_val, "nFrames");
  const int k = checkedIntFromVal(k_val, "k");
  const int width = checkedIntFromVal(width_val, "width");
  std::vector<float> data = float32ArrayToVector(s);
  if (n_features <= 0 || n_frames <= 0 ||
      static_cast<size_t>(n_features) >
          std::numeric_limits<size_t>::max() / static_cast<size_t>(std::max(1, n_frames)) ||
      static_cast<size_t>(n_features) * static_cast<size_t>(n_frames) > data.size()) {
    throw SonareException(ErrorCode::InvalidParameter, "spectrogram dimensions exceed input");
  }
  if (aggregate.empty()) aggregate = "mean";
  std::vector<float> filtered = nn_filter(data.data(), n_features, n_frames, aggregate, k, width);

  val out = val::object();
  out.set("data", vectorToFloat32Array(filtered));
  out.set("rows", n_features);
  out.set("cols", n_frames);
  return out;
}

// Time-domain remix: reorders / concatenates a signal by (start, end) interval
// slices. Mirrors the C ABI sonare_remix / librosa.effects.remix. @p intervals
// is a flat Int32Array of (start, end) pairs.
val js_remix(val samples, val intervals, const val& sample_rate, bool align_zeros) {
  // Validate finite samples, non-empty input, and the sample-rate range up front
  // so js_remix rejects exactly what the C ABI's run_offline (sonare_remix) does,
  // rather than copying NaN/Inf through or silently accepting a bad rate.
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  // Sample indices must survive as exact integers: converting through float32
  // would round any boundary above 2^24 (16,777,216) and silently misalign the
  // slice. Read the Int32Array straight into int32 storage instead.
  std::vector<int32_t> interval_ints = int32ArrayToVector(intervals);
  if (interval_ints.size() % 2 != 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "remix intervals must be (start, end) pairs");
  }
  std::vector<std::pair<int, int>> pairs;
  pairs.reserve(interval_ints.size() / 2);
  for (size_t i = 0; i + 1 < interval_ints.size(); i += 2) {
    pairs.emplace_back(static_cast<int>(interval_ints[i]), static_cast<int>(interval_ints[i + 1]));
  }
  std::vector<float> remixed = remix(audio.data(), audio.size(), pairs, align_zeros);
  return vectorToFloat32Array(remixed);
}

// Resolves the cut points remix() would use, without cutting. Mirrors the C ABI
// sonare_remix_aligned_intervals. Returns a flat Int32Array of (start, end)
// pairs so a host can apply ONE cut set to every channel of a multichannel
// take; calling remix() per channel snaps each channel independently.
val js_remix_aligned_intervals(val samples, val intervals, const val& sample_rate,
                               bool align_zeros) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  std::vector<int32_t> interval_ints = int32ArrayToVector(intervals);
  if (interval_ints.size() % 2 != 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "remix intervals must be (start, end) pairs");
  }
  std::vector<std::pair<int, int>> pairs;
  pairs.reserve(interval_ints.size() / 2);
  for (size_t i = 0; i + 1 < interval_ints.size(); i += 2) {
    pairs.emplace_back(static_cast<int>(interval_ints[i]), static_cast<int>(interval_ints[i + 1]));
  }
  const std::vector<std::pair<int, int>> resolved =
      align_remix_intervals(audio.data(), audio.size(), pairs, align_zeros);
  std::vector<int> flat;
  flat.reserve(resolved.size() * 2);
  for (const auto& pair : resolved) {
    flat.push_back(pair.first);
    flat.push_back(pair.second);
  }
  return vectorToInt32Array(flat);
}

void registerEffectsDecompositionBindings() {
  function("decompose", &js_decompose);
  function("decomposeWithInit", &js_decompose_with_init);
  function("decomposeStems", &js_decompose_stems);
  function("nnFilter", &js_nn_filter);
  function("remix", &js_remix);
  function("remixAlignedIntervals", &js_remix_aligned_intervals);
}

#endif  // __EMSCRIPTEN__
