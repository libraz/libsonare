#ifndef SONARE_WASM_BINDINGS_MASTERING_CHAIN_RESULT_H_
#define SONARE_WASM_BINDINGS_MASTERING_CHAIN_RESULT_H_

#ifdef __EMSCRIPTEN__

#include <emscripten/val.h>

#include <vector>

#include "mastering/api/result_types.h"

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
/// reductions) shared by every mastering-chain result object. MonoChainResult
/// and StereoChainResult both derive ChainMetrics, so the same builder serves
/// both paths.
inline void setChainMetrics(emscripten::val& out,
                            const sonare::mastering::api::ChainMetrics& metrics) {
  out.set("outputTruePeakDbtp", metrics.output_true_peak_dbtp);
  out.set("outputLra", metrics.output_lra);
  out.set("loudnessTargetLimited", metrics.loudness_target_limited);
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

}  // namespace sonare

#endif  // __EMSCRIPTEN__

#endif  // SONARE_WASM_BINDINGS_MASTERING_CHAIN_RESULT_H_
