/// @file decomposition.cpp
/// @brief Embind bindings for NMF decomposition, nearest-neighbour filtering and interval remix.

#ifdef __EMSCRIPTEN__

#include <limits>

#include "effects/decompose.h"
#include "util/parameter_checks.h"
#include "wasm/bindings/common/common.h"

namespace {

// Shared option reader for decomposeStems / decomposeStemsLinked. An absent key
// keeps the core default (an absent hop derives it from nFft); a given value is
// read literally and checked as the C ABI checks SonareDecomposeStemsConfig.
DecomposeStemsConfig readDecomposeStemsConfig(val options) {
  namespace check = sonare::util::check;
  DecomposeStemsConfig config;
  config.n_components = intProperty(options, "nComponents", config.n_components);
  config.n_fft = intProperty(options, "nFft", config.n_fft);
  config.hop_length = intProperty(options, "hopLength", config.hop_length);
  config.n_iter = intProperty(options, "nIter", config.n_iter);
  config.beta = floatProperty(options, "beta", config.beta);
  config.init = stringProperty(options, "init", config.init);
  config.mask_power = floatProperty(options, "maskPower", config.mask_power);
  check::integer_at_least(config.n_components, 1, "nComponents");
  check::integer_at_least(config.n_fft, 1, "nFft");
  if (hasProperty(options, "hopLength")) check::integer_at_least(config.hop_length, 1, "hopLength");
  check::integer_at_least(config.n_iter, 1, "nIter");
  check::finite_float(config.beta, "beta");
  check::at_least(config.mask_power, 1.0, "maskPower");
  return config;
}

}  // namespace

// NMF decomposition of a non-negative spectrogram. Mirrors the C ABI
// sonare_decompose / librosa.decompose.decompose. Returns the two factor
// matrices as { w, h }: w is [n_features x n_components] row-major and h is
// [n_components x n_frames] row-major (both flat Float32Array buffers).
val js_decompose(val s, const val& n_features_val, const val& n_frames_val,
                 const val& n_components_val, const val& n_iter_val, const val& beta_val) {
  const int n_features =
      domainNumberFromVal<int>(n_features_val, "nFeatures", sonare::kDecomposeNFeaturesDomain);
  const int n_frames =
      domainNumberFromVal<int>(n_frames_val, "nFrames", sonare::kDecomposeNFramesDomain);
  const int n_components = domainNumberFromVal<int>(n_components_val, "nComponents",
                                                    sonare::kDecomposeNComponentsDomain);
  // An omitted (undefined or null) iteration count takes the core default.
  const int n_iter = n_iter_val.isUndefined() || n_iter_val.isNull()
                         ? kDecomposeDefaultIter
                         : checkedIntFromVal(n_iter_val, "nIter");
  const float beta = checkedFloatFromVal(beta_val, "beta");
  std::vector<float> data = float32ArrayToVector(s);
  if (n_iter <= 0) {
    throw WasmRangeError("nIter must be an integer in [1, 2147483647]");
  }
  if (static_cast<size_t>(n_features) >
          std::numeric_limits<size_t>::max() / static_cast<size_t>(std::max(1, n_frames)) ||
      static_cast<size_t>(n_features) * static_cast<size_t>(n_frames) > data.size()) {
    throw WasmRangeError("spectrogram dimensions exceed input");
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
  const int n_features =
      domainNumberFromVal<int>(n_features_val, "nFeatures", sonare::kDecomposeNFeaturesDomain);
  const int n_frames =
      domainNumberFromVal<int>(n_frames_val, "nFrames", sonare::kDecomposeNFramesDomain);
  const int n_components = domainNumberFromVal<int>(n_components_val, "nComponents",
                                                    sonare::kDecomposeNComponentsDomain);
  // An omitted (undefined or null) iteration count takes the core default.
  const int n_iter = n_iter_val.isUndefined() || n_iter_val.isNull()
                         ? kDecomposeDefaultIter
                         : checkedIntFromVal(n_iter_val, "nIter");
  const float beta = checkedFloatFromVal(beta_val, "beta");
  std::vector<float> data = float32ArrayToVector(s);
  if (n_iter <= 0) {
    throw WasmRangeError("nIter must be an integer in [1, 2147483647]");
  }
  if (static_cast<size_t>(n_features) >
          std::numeric_limits<size_t>::max() / static_cast<size_t>(std::max(1, n_frames)) ||
      static_cast<size_t>(n_features) * static_cast<size_t>(n_frames) > data.size()) {
    throw WasmRangeError("spectrogram dimensions exceed input");
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
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);
  const DecomposeStemsConfig config = readDecomposeStemsConfig(options);

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

// Multi-channel form of decomposeStems: one NMF model and one soft mask
// shared across every channel, so no interchannel level or phase difference
// moves. Mirrors the C ABI sonare_decompose_stems_linked, calling the core
// decompose_stems_linked directly rather than the C ABI, matching
// js_decompose_stems and js_mastering_repair_denoise_classical_linked's
// pattern in repair.cpp. A single channel reproduces decomposeStems bit for
// bit (decompose_stems is itself implemented as the one-channel call).
// Returns { components: Float32Array[][], w, h } where components[k][c] is
// component k's signal on channel c.
val js_decompose_stems_linked(val channels, const val& sample_rate_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const std::vector<Audio> loaded = loadValidatedChannelSet("channels", channels, sample_rate);
  const DecomposeStemsConfig config = readDecomposeStemsConfig(options);

  std::vector<const float*> pointers;
  pointers.reserve(loaded.size());
  for (const Audio& channel : loaded) pointers.push_back(channel.data());
  const DecomposeStemsLinkedResult result = decompose_stems_linked(
      pointers.data(), pointers.size(), loaded.front().size(), sample_rate, config);

  val components = val::array();
  for (const std::vector<std::vector<float>>& component : result.components) {
    val channel_signals = val::array();
    for (const std::vector<float>& signal : component) {
      channel_signals.call<void>("push", vectorToFloat32Array(signal));
    }
    components.call<void>("push", channel_signals);
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
  const int k =
      k_val.isUndefined() || k_val.isNull() ? kNnFilterDefaultK : checkedIntFromVal(k_val, "k");
  const int width = width_val.isUndefined() || width_val.isNull()
                        ? kNnFilterDefaultWidth
                        : checkedIntFromVal(width_val, "width");
  std::vector<float> data = float32ArrayToVector(s);
  if (n_features <= 0 || n_frames <= 0 ||
      static_cast<size_t>(n_features) >
          std::numeric_limits<size_t>::max() / static_cast<size_t>(std::max(1, n_frames)) ||
      static_cast<size_t>(n_features) * static_cast<size_t>(n_frames) > data.size()) {
    throw WasmRangeError("spectrogram dimensions exceed input");
  }
  if (aggregate.empty()) aggregate = "mean";
  std::vector<float> filtered = nn_filter(data.data(), n_features, n_frames, aggregate, k, width);

  val out = val::object();
  out.set("data", vectorToFloat32Array(filtered));
  out.set("rows", n_features);
  out.set("cols", n_frames);
  return out;
}

// Absent or null `alignZeros` is true; anything else must be a boolean.
static bool alignZerosFromVal(const val& value) {
  if (value.isUndefined() || value.isNull()) return true;
  if (!value.isTrue() && !value.isFalse()) throw WasmTypeError("alignZeros must be a boolean");
  return value.isTrue();
}

// Time-domain remix: reorders / concatenates a signal by (start, end) interval
// slices. Mirrors the C ABI sonare_remix / librosa.effects.remix. @p intervals
// is a flat Int32Array of (start, end) pairs.
val js_remix(val samples, val intervals, const val& sample_rate, const val& align_zeros_val) {
  const bool align_zeros = alignZerosFromVal(align_zeros_val);
  // Validate finite samples, non-empty input, and the sample-rate range up front
  // so js_remix rejects exactly what the C ABI's run_offline (sonare_remix) does,
  // rather than copying NaN/Inf through or silently accepting a bad rate.
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));
  // Sample indices must survive as exact integers: converting through float32
  // would round any boundary above 2^24 (16,777,216) and silently misalign the
  // slice. Read the Int32Array straight into int32 storage instead.
  std::vector<int32_t> interval_ints = int32ArrayToVector(intervals);
  if (interval_ints.size() % 2 != 0) {
    throw WasmRangeError("remix intervals must be (start, end) pairs");
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
                               const val& align_zeros_val) {
  const bool align_zeros = alignZerosFromVal(align_zeros_val);
  Audio audio =
      loadValidatedAudio("samples", samples, checkedIntFromVal(sample_rate, "sampleRate"));
  std::vector<int32_t> interval_ints = int32ArrayToVector(intervals);
  if (interval_ints.size() % 2 != 0) {
    throw WasmRangeError("remix intervals must be (start, end) pairs");
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
  function("decomposeStemsLinked", &js_decompose_stems_linked);
  function("nnFilter", &js_nn_filter);
  function("remix", &js_remix);
  function("remixAlignedIntervals", &js_remix_aligned_intervals);
}

#endif  // __EMSCRIPTEN__
