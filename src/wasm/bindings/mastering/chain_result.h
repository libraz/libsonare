#ifndef SONARE_WASM_BINDINGS_MASTERING_CHAIN_RESULT_H_
#define SONARE_WASM_BINDINGS_MASTERING_CHAIN_RESULT_H_

#ifdef __EMSCRIPTEN__

#include <emscripten/val.h>

#include <string>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/result_types.h"
#include "wasm/bindings/common/common.h"

namespace sonare {

inline emscripten::val masteringLoudnessSummaryToVal(
    const sonare::mastering::api::MasteringLoudnessSummary& summary) {
  emscripten::val out = emscripten::val::object();
  out.set("integratedLufs", summary.integrated_lufs);
  out.set("maxMomentaryLufs", summary.max_momentary_lufs);
  out.set("maxShortTermLufs", summary.max_short_term_lufs);
  out.set("truePeakDbtp", summary.true_peak_dbtp);
  out.set("loudnessRange", summary.loudness_range);
  return out;
}

inline void setMasteringReport(emscripten::val& out,
                               const sonare::mastering::api::MasteringReport& report) {
  emscripten::val report_out = emscripten::val::object();
  report_out.set("before", masteringLoudnessSummaryToVal(report.before));
  report_out.set("after", masteringLoudnessSummaryToVal(report.after));
  report_out.set("appliedGainDb", report.applied_gain_db);
  report_out.set("maxGainReductionDb", report.max_gain_reduction_db);
  report_out.set("loudnessTargetLimited", report.loudness_target_limited);
  std::vector<float> band_energy(report.band_energy_delta_db.begin(),
                                 report.band_energy_delta_db.end());
  report_out.set("bandEnergyDeltaDb", vectorToFloat32Array(band_energy));
  out.set("report", report_out);
}

/// @brief Append the chain-metric fields (output true peak, LRA, per-stage gain
/// reductions, substitution count) shared by every mastering-chain result
/// object. Takes the whole result rather than its ChainMetrics base: the
/// substitution count lives on the audio-result base instead, and a builder
/// given only one of the two bases would silently drop whichever it cannot see.
template <typename Result>
inline void setChainMetrics(emscripten::val& out, const Result& metrics) {
  out.set("outputTruePeakDbtp", metrics.output_true_peak_dbtp);
  out.set("outputLra", metrics.output_lra);
  out.set("loudnessTargetLimited", metrics.loudness_target_limited);
  out.set("nonFiniteSubstitutionCount", static_cast<double>(metrics.non_finite_substitution_count));
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
inline void setChainLevelsAndStages(emscripten::val& out, const Result& result) {
  out.set("sampleRate", result.sample_rate);
  out.set("inputLufs", result.input_lufs);
  out.set("outputLufs", result.output_lufs);
  out.set("appliedGainDb", result.applied_gain_db);
  emscripten::val stages = emscripten::val::array();
  for (const auto& stage : result.stages) {
    stages.call<void>("push", stage);
  }
  out.set("stages", stages);
  setChainMetrics(out, result);
}

/// @brief The JS object every mono mastering entry point returns. `master_audio`
/// and the chain share one result type, so this is one owner for both.
inline emscripten::val masteringMonoResultToVal(
    const sonare::mastering::api::MonoChainResult& result) {
  emscripten::val out = emscripten::val::object();
  out.set("samples", vectorToFloat32Array(result.samples));
  setChainLevelsAndStages(out, result);
  return out;
}

/// @brief The JS object every stereo mastering entry point returns.
inline emscripten::val masteringStereoResultToVal(
    const sonare::mastering::api::StereoChainResult& result) {
  emscripten::val out = emscripten::val::object();
  out.set("left", vectorToFloat32Array(result.left));
  out.set("right", vectorToFloat32Array(result.right));
  setChainLevelsAndStages(out, result);
  return out;
}

/// @brief Install the JS progress and cancel callbacks on a chain. A callback
/// that is null or undefined is left uninstalled rather than wrapped.
inline void installMasteringChainCallbacks(sonare::mastering::api::MasteringChain& chain,
                                           const emscripten::val& progress_callback,
                                           const emscripten::val& cancel_callback) {
  if (!progress_callback.isNull() && !progress_callback.isUndefined()) {
    chain.set_progress_callback([progress_callback](float progress, const char* stage) {
      progress_callback(progress, std::string(stage ? stage : ""));
    });
  }
  if (!cancel_callback.isNull() && !cancel_callback.isUndefined()) {
    chain.set_cancel_callback(
        [cancel_callback] { return cancelCallbackRequested(cancel_callback); });
  }
}

}  // namespace sonare

#endif  // __EMSCRIPTEN__

#endif  // SONARE_WASM_BINDINGS_MASTERING_CHAIN_RESULT_H_
