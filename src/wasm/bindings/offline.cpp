/// @file offline.cpp
/// @brief Aggregates offline Embind binding registration groups.

#ifdef __EMSCRIPTEN__

#include "common.h"

void registerOfflineBindings() {
  registerFeatureSpectrogramBindings();
  registerFeatureMusicBindings();
  registerFeatureSpectralBindings();
  registerFeaturePitchBindings();
  registerFeatureCoreBindings();
  registerMeteringBindings();
  registerRepairBindings();
  registerOfflineDynamicsEditingBindings();
}

#endif  // __EMSCRIPTEN__
