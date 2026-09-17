/// @file mastering_chain.cpp
/// @brief Embind bindings for mastering chain and loudness facade APIs.

#ifdef __EMSCRIPTEN__

#include "rt/aliasing_control.h"
#include "util/zero_is_default.h"
#include "wasm/bindings/common/common.h"
#include "wasm/bindings/mastering/chain_result.h"

val js_mastering(val samples, const val& sample_rate_val, const val& target_lufs_val,
                 const val& ceiling_db_val, const val& true_peak_oversample_val,
                 const val& release_ms_val, bool apply_gain_at_input_rate) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int true_peak_oversample =
      checkedIntFromVal(true_peak_oversample_val, "truePeakOversample");
  const float target_lufs = checkedFloatFromVal(target_lufs_val, "targetLufs");
  const float ceiling_db = checkedFloatFromVal(ceiling_db_val, "ceilingDb");
  const float release_ms = checkedFloatFromVal(release_ms_val, "releaseMs");
  Audio audio = loadValidatedAudio(samples, sample_rate);

  mastering::maximizer::LoudnessOptimizeConfig config;
  config.target_lufs = target_lufs;
  config.ceiling_db = ceiling_db;
  // Keep C-ABI sentinel semantics: 0 requests the default oversample factor,
  // and any other value reaches the validator that rejects it rather than
  // being swapped for the default it would have failed against.
  if (true_peak_oversample != 0) config.true_peak_oversample = true_peak_oversample;
  // release_ms == 0 requests the library default; any other value is applied as
  // asked and rejected by the shared loudness validator if it is not positive.
  // Filtering on `> 0` here instead would discard a negative or non-finite
  // request silently, which is what the other surfaces stopped doing.
  config.release_ms = ZeroIsDefault(release_ms).or_default(config.release_ms);
  config.apply_gain_at_input_rate = apply_gain_at_input_rate;

  auto result = mastering::maximizer::loudness_optimize(audio, config);
  std::vector<float> out_vec(result.audio.data(), result.audio.data() + result.audio.size());

  val out = val::object();
  out.set("samples", vectorToFloat32Array(out_vec));
  out.set("sampleRate", result.audio.sample_rate());
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  out.set("loudnessTargetLimited", result.loudness_target_limited);
  out.set("nonFiniteSubstitutionCount", static_cast<double>(result.non_finite_substitution_count));
  return out;
}

// ---------------------------------------------------------------------------
// Helpers: build a MasteringChainConfig from the flattened parameter map the
// TypeScript facade sends.
// ---------------------------------------------------------------------------

mastering::api::MasteringChainConfig masteringChainConfigFromVal(val config) {
  // The facade flattens its nested config to canonical dotted parameters and
  // sends them in this private envelope. Parsing them with the core parser is
  // what keeps WASM in lockstep with the C ABI, Node and Python: a second
  // reader here would be a second set of accepted names and defaults.
  val flat_params = objectProperty(config, "__flatParams");
  if (!flat_params.isUndefined()) {
    const std::vector<mastering::api::Param> params = masteringParamsFromObject(flat_params);
    return mastering::api::parse_chain_config_params(params.data(), params.size());
  }
  throw SonareException(ErrorCode::InvalidParameter,
                        "mastering chain config must be the flattened parameter map the facade "
                        "builds; a nested config object passed straight to the module is not read");
}

val js_mastering_chain(val samples, const val& sample_rate_val, val config) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  mastering::api::MasteringChain chain(masteringChainConfigFromVal(config));
  auto result = chain.process_mono(data.data(), data.size(), sample_rate);

  return masteringMonoResultToVal(result);
}

val js_mastering_chain_stereo(val left_samples, val right_samples, const val& sample_rate_val,
                              val config) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples",
                               "masteringChainStereo input", true);
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  validate_offline_audio_input(left.data(), left.size(), sample_rate);
  validate_offline_audio_input(right.data(), right.size(), sample_rate);

  mastering::api::MasteringChain chain(masteringChainConfigFromVal(config));
  auto result = chain.process_stereo(left.data(), right.data(), left.size(), sample_rate);

  return masteringStereoResultToVal(result);
}

// Mastering chain (mono) with progress callback
val js_mastering_chain_with_progress(val samples, const val& sample_rate_val, val config,
                                     val progress_callback, val cancel_callback) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  mastering::api::MasteringChain chain(masteringChainConfigFromVal(config));
  installMasteringChainCallbacks(chain, progress_callback, cancel_callback);
  const auto result = chain.process_mono_cancellable(data.data(), data.size(), sample_rate);
  if (!result) {
    throw SonareException(ErrorCode::Cancelled, "mastering cancelled");
  }

  return masteringMonoResultToVal(*result);
}

// Mastering chain (stereo) with progress callback
val js_mastering_chain_stereo_with_progress(val left_samples, val right_samples,
                                            const val& sample_rate_val, val config,
                                            val progress_callback, val cancel_callback) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples",
                               "masteringChainStereoWithProgress input", true);
  std::vector<float> left = float32ArrayToVector(left_samples);
  std::vector<float> right = float32ArrayToVector(right_samples);
  validate_offline_audio_input(left.data(), left.size(), sample_rate);
  validate_offline_audio_input(right.data(), right.size(), sample_rate);

  mastering::api::MasteringChain chain(masteringChainConfigFromVal(config));
  installMasteringChainCallbacks(chain, progress_callback, cancel_callback);
  const auto result =
      chain.process_stereo_cancellable(left.data(), right.data(), left.size(), sample_rate);
  if (!result) {
    throw SonareException(ErrorCode::Cancelled, "mastering cancelled");
  }

  return masteringStereoResultToVal(*result);
}

void registerMasteringChainBindings() {
  function("mastering", &js_mastering);
  function("masteringChain", &js_mastering_chain);
  function("masteringChainStereo", &js_mastering_chain_stereo);
  function("masteringChainWithProgress", &js_mastering_chain_with_progress);
  function("masteringChainStereoWithProgress", &js_mastering_chain_stereo_with_progress);
}

#endif  // __EMSCRIPTEN__
