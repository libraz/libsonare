/// @file assist_registry.cpp
/// @brief Translation-unit anchor for the header-only AssistRegistry.
///
/// AssistRegistry is fully defined in assist_registry.h; this TU exists so the
/// registry participates in the sonare_assist static library (and gives the
/// linker a home for any future out-of-line members) without forcing every
/// consumer to be header-only.

#include "midi/assist/assist_registry.h"

namespace sonare::midi::assist {

// Intentionally empty: see header. Keeping a definition here anchors the symbol
// set for sonare_assist and documents the seam's library membership.

}  // namespace sonare::midi::assist
