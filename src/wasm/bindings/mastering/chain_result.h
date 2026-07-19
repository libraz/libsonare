#ifndef SONARE_WASM_BINDINGS_MASTERING_CHAIN_RESULT_H_
#define SONARE_WASM_BINDINGS_MASTERING_CHAIN_RESULT_H_

#ifdef __EMSCRIPTEN__

#include <emscripten/val.h>

#include "mastering/api/result_types.h"

namespace sonare {

/// @brief Append the chain-metric fields (output true peak, LRA, per-stage gain
/// reductions) shared by every mastering-chain result object. MonoChainResult
/// and StereoChainResult both derive ChainMetrics, so the same builder serves
/// both paths.
inline void setChainMetrics(emscripten::val& out,
                            const sonare::mastering::api::ChainMetrics& metrics) {
  out.set("outputTruePeakDbtp", metrics.output_true_peak_dbtp);
  out.set("outputLra", metrics.output_lra);
  emscripten::val reductions = emscripten::val::array();
  for (const auto& reduction : metrics.stage_gain_reductions) {
    emscripten::val entry = emscripten::val::object();
    entry.set("stage", reduction.stage);
    entry.set("gainReductionDb", reduction.gain_reduction_db);
    reductions.call<void>("push", entry);
  }
  out.set("stageGainReductions", reductions);
}

}  // namespace sonare

#endif  // __EMSCRIPTEN__

#endif  // SONARE_WASM_BINDINGS_MASTERING_CHAIN_RESULT_H_
