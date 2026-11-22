#pragma once

/// @file gs_efx_realization.h
/// @brief Which sound-making layer realises the GS insertion units.

#include <cstdint>

namespace sonare::midi::synth {

/// Which sound-making layer realises every insertion unit. Both share the
/// GS protocol layer; kModern runs the insert chain gs_efx_insert_chain
/// describes, kClassic runs the generated 32 kHz graph of the type.
enum class GsEfxRealization : uint8_t { kModern, kClassic };

}  // namespace sonare::midi::synth
