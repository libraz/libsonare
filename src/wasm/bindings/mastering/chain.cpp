/// @file mastering_chain.cpp
/// @brief Embind bindings for mastering chain and loudness facade APIs.

#ifdef __EMSCRIPTEN__

#include "rt/aliasing_control.h"
#include "util/zero_is_default.h"
#include "wasm/bindings/common/common.h"
#include "wasm/bindings/mastering/chain_result.h"

namespace sonare {
namespace {

emscripten::val masteringLoudnessSummaryToVal(
    const sonare::mastering::api::MasteringLoudnessSummary& summary) {
  emscripten::val out = emscripten::val::object();
  out.set("integratedLufs", summary.integrated_lufs);
  setMeasured(out, "maxMomentaryLufs", summary.max_momentary_lufs);
  setMeasured(out, "maxShortTermLufs", summary.max_short_term_lufs);
  out.set("truePeakDbtp", summary.true_peak_dbtp);
  setMeasured(out, "loudnessRangeLu", summary.loudness_range_lu);
  return out;
}

void setMasteringReport(emscripten::val& out,
                        const sonare::mastering::api::MasteringReport& report) {
  emscripten::val report_out = emscripten::val::object();
  report_out.set("before", masteringLoudnessSummaryToVal(report.before));
  report_out.set("after", masteringLoudnessSummaryToVal(report.after));
  report_out.set("appliedGainDb", report.applied_gain_db);
  report_out.set("maxGainReductionDb", report.max_gain_reduction_db);
  setMeasured(report_out, "loudnessGainDb", report.loudness_gain_db);
  report_out.set("loudnessTargetLimited", report.loudness_target_limited);
  report_out.set("bandEnergyDeltaDb", vectorToFloat32Array(report.band_energy_delta_db.data(),
                                                           report.band_energy_delta_db.size()));
  out.set("report", report_out);
}

void setChainMetrics(emscripten::val& out, const mastering::api::ChainMetrics& metrics,
                     std::uint32_t substitutions, const std::optional<std::size_t>& declip_runs) {
  out.set("outputTruePeakDbtp", metrics.output_true_peak_dbtp);
  setMeasured(out, "outputLoudnessRangeLu", metrics.output_loudness_range_lu);
  out.set("loudnessTargetLimited", metrics.loudness_target_limited);
  setMeasured(out, "loudnessGainDb", metrics.loudness_gain_db);
  out.set("nonFiniteSubstitutionCount", static_cast<double>(substitutions));
  setOptionalCount(out, "declipUnreconstructedRuns", declip_runs);
  emscripten::val reductions = emscripten::val::array();
  for (const auto& reduction : metrics.stage_gain_reductions) {
    emscripten::val entry = emscripten::val::object();
    entry.set("stage", reduction.stage);
    entry.set("gainReductionDb", reduction.gain_reduction_db);
    reductions.call<void>("push", entry);
  }
  out.set("stageGainReductions", reductions);
  setMasteringReport(out, metrics.report);
}

/// @brief Write the level, stage-list and metric fields every mastering-chain
/// result object carries, whichever channel layout produced it.
template <typename Result>
void setChainLevelsAndStages(emscripten::val& out, const Result& result) {
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  out.set("stages", stringVectorToVal(result.stages));
  setChainMetrics(out, result, result.non_finite_substitution_count,
                  result.declip_unreconstructed_runs);
}

}  // namespace

/// @brief The JS object every mono mastering entry point returns. `master_audio`
/// and the chain share one result type, so this is one owner for both.
emscripten::val masteringMonoResultToVal(const sonare::mastering::api::MonoChainResult& result) {
  emscripten::val out = emscripten::val::object();
  out.set("samples", vectorToFloat32Array(result.samples));
  setChainLevelsAndStages(out, result);
  return out;
}

/// @brief The JS object every stereo mastering entry point returns.
emscripten::val masteringStereoResultToVal(
    const sonare::mastering::api::StereoChainResult& result) {
  emscripten::val out = emscripten::val::object();
  out.set("left", vectorToFloat32Array(result.left));
  out.set("right", vectorToFloat32Array(result.right));
  setChainLevelsAndStages(out, result);
  return out;
}

}  // namespace sonare

val js_mastering(val samples, const val& sample_rate_val, const val& target_lufs_val,
                 const val& ceiling_dbtp_val, const val& true_peak_oversample_val,
                 const val& release_ms_val, bool apply_gain_at_input_rate) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int true_peak_oversample =
      checkedIntFromVal(true_peak_oversample_val, "truePeakOversample");
  const float target_lufs = checkedFloatFromVal(target_lufs_val, "targetLufs");
  const float ceiling_dbtp = checkedFloatFromVal(ceiling_dbtp_val, "ceilingDbtp");
  const float release_ms = checkedFloatFromVal(release_ms_val, "releaseMs");
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);

  mastering::maximizer::LoudnessOptimizeConfig config;
  config.target_lufs = target_lufs;
  config.ceiling_dbtp = ceiling_dbtp;
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

  val out = val::object();
  out.set("samples", vectorToFloat32Array(result.audio.data(), result.audio.size()));
  out.set("sampleRate", result.audio.sample_rate());
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  out.set("latencySamples", result.latency_samples);
  out.set("loudnessTargetLimited", result.loudness_target_limited);
  out.set("nonFiniteSubstitutionCount", static_cast<double>(result.non_finite_substitution_count));
  return out;
}

// The chain configuration crosses as document text; the core reader is the only parser, which
// keeps WASM in lockstep with the C ABI, Node and Python.
mastering::api::MasteringChainConfig masteringChainConfigFromDocument(const val& document,
                                                                      bool streaming,
                                                                      std::string_view root) {
  const bool absent = document.isUndefined() || document.isNull();
  const std::string text =
      absent ? std::string("{\"params\":{}}") : stringFromVal(document, "configJson");
  const auto origin = mastering::api::chain_params_argument(root);
  if (streaming) return mastering::api::streaming_chain_config_from_json(text, origin);
  return absent ? mastering::api::MasteringChainConfig{}
                : mastering::api::chain_config_from_json(text, origin);
}

val js_mastering_chain(val samples, const val& sample_rate_val, val config,
                       const val& progress_callback, const val& cancel_callback) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const bool has_progress = callbackGiven(progress_callback, "onProgress");
  const bool has_cancel = callbackGiven(cancel_callback, "cancel");
  std::vector<float> data = float32ArrayToVector(samples, "samples");
  validate_offline_audio_input("samples", data.data(), data.size(), sample_rate);
  mastering::api::MasteringChain chain(masteringChainConfigFromDocument(config));
  if (!has_progress && !has_cancel) {
    return masteringMonoResultToVal(chain.process_mono(data.data(), data.size(), sample_rate));
  }
  installMasteringChainCallbacks(chain, progress_callback, cancel_callback);
  const auto result = chain.process_mono_cancellable(data.data(), data.size(), sample_rate);
  if (!result) {
    throw SonareException(ErrorCode::Cancelled, "mastering cancelled");
  }
  return masteringMonoResultToVal(*result);
}

val js_mastering_chain_stereo(val left_samples, val right_samples, const val& sample_rate_val,
                              val config, const val& progress_callback,
                              const val& cancel_callback) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const bool has_progress = callbackGiven(progress_callback, "onProgress");
  const bool has_cancel = callbackGiven(cancel_callback, "cancel");
  validateWasmFloat32ArrayPair(left_samples, "left", right_samples, "right", true);
  std::vector<float> left = float32ArrayToVector(left_samples, "left");
  std::vector<float> right = float32ArrayToVector(right_samples, "right");
  validate_offline_audio_input("left", left.data(), left.size(), sample_rate);
  validate_offline_audio_input("right", right.data(), right.size(), sample_rate);

  mastering::api::MasteringChain chain(masteringChainConfigFromDocument(config));
  if (!has_progress && !has_cancel) {
    return masteringStereoResultToVal(
        chain.process_stereo(left.data(), right.data(), left.size(), sample_rate));
  }
  installMasteringChainCallbacks(chain, progress_callback, cancel_callback);
  const auto result =
      chain.process_stereo_cancellable(left.data(), right.data(), left.size(), sample_rate);
  if (!result) {
    throw SonareException(ErrorCode::Cancelled, "mastering cancelled");
  }
  return masteringStereoResultToVal(*result);
}

val streamingLoudnessGainToVal(const mastering::api::StreamingLoudnessGain& gain) {
  val out = val::object();
  setMeasured(out, "loudnessStaticGainDb", gain.loudness_static_gain_db);
  setMeasured(out, "truePeakDb", gain.true_peak_db);
  setMeasured(out, "integratedLufs", gain.integrated_lufs);
  return out;
}

val js_mastering_streaming_loudness_gain(val samples, const val& sample_rate_val, val config) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  std::vector<float> data = float32ArrayToVector(samples, "samples");
  validate_offline_audio_input("samples", data.data(), data.size(), sample_rate);
  return streamingLoudnessGainToVal(mastering::api::streaming_loudness_gain_mono(
      masteringChainConfigFromDocument(config, true), data.data(), data.size(), sample_rate));
}

val js_mastering_streaming_loudness_gain_stereo(val left_samples, val right_samples,
                                                const val& sample_rate_val, val config) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left", right_samples, "right", true);
  std::vector<float> left = float32ArrayToVector(left_samples, "left");
  std::vector<float> right = float32ArrayToVector(right_samples, "right");
  validate_offline_audio_input("left", left.data(), left.size(), sample_rate);
  validate_offline_audio_input("right", right.data(), right.size(), sample_rate);
  return streamingLoudnessGainToVal(mastering::api::streaming_loudness_gain_stereo(
      masteringChainConfigFromDocument(config, true), left.data(), right.data(), left.size(),
      sample_rate));
}

void registerMasteringChainBindings() {
  function("mastering", &js_mastering);
  function("masteringChain", &js_mastering_chain);
  function("masteringChainStereo", &js_mastering_chain_stereo);
  function("masteringStreamingLoudnessGain", &js_mastering_streaming_loudness_gain);
  function("masteringStreamingLoudnessGainStereo", &js_mastering_streaming_loudness_gain_stereo);
}

#endif  // __EMSCRIPTEN__
