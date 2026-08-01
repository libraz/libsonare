/// @file features.cpp
/// @brief Registers the analysis-only feature subset for the small WASM entry.

#ifdef __EMSCRIPTEN__

#include "wasm/bindings/common/common.h"

void registerAnalysisFeatureBindings() {
  registerFeatureCoreBindings();
  registerFeatureMusicBindings();
  registerFeaturePitchBindings();
  registerFeatureSpectralBindings();
  registerFeatureSpectrogramBindings();
  registerMeteringBindings();
}

#endif  // __EMSCRIPTEN__
