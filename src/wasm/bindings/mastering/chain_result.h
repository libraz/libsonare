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

emscripten::val masteringMonoResultToVal(const sonare::mastering::api::MonoChainResult& result);

emscripten::val masteringStereoResultToVal(const sonare::mastering::api::StereoChainResult& result);

/// @brief Install the JS progress and cancel callbacks on a chain. A callback
/// that is null or undefined is left uninstalled rather than wrapped, except that a cancel
/// callback alone installs a no-op progress sink: an empty chain polls cancel only beside one.
inline void installMasteringChainCallbacks(sonare::mastering::api::MasteringChain& chain,
                                           const emscripten::val& progress_callback,
                                           const emscripten::val& cancel_callback) {
  const bool has_progress = !progress_callback.isNull() && !progress_callback.isUndefined();
  const bool has_cancel = !cancel_callback.isNull() && !cancel_callback.isUndefined();
  if (has_progress) {
    chain.set_progress_callback([progress_callback](float progress, const char* stage) {
      progress_callback(progress, std::string(stage ? stage : ""));
    });
  } else if (has_cancel) {
    chain.set_progress_callback([](float, const char*) {});
  }
  if (!cancel_callback.isNull() && !cancel_callback.isUndefined()) {
    chain.set_cancel_callback(
        [cancel_callback] { return cancelCallbackRequested(cancel_callback); });
  }
}

}  // namespace sonare

#endif  // __EMSCRIPTEN__

#endif  // SONARE_WASM_BINDINGS_MASTERING_CHAIN_RESULT_H_
