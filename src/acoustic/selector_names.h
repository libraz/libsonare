#pragma once

/// @file selector_names.h
/// @brief Names of the integer selectors the room-acoustics surfaces accept, so a binding that
///        takes a name resolves it against one table. The position of a name is its selector
///        value (SONARE_MATERIAL_PRESET_* / SONARE_ACOUSTIC_MODE_* in the C ABI).

#include <array>
#include <cstddef>

namespace sonare::acoustic {

/// @brief Material preset selector names; 0 is "none", then MaterialPreset in declaration order.
inline constexpr std::array<const char*, 6> kMaterialPresetSelectorNames = {
    "none", "concrete", "wood", "curtain", "carpet", "glass"};

/// @brief Acoustic analyzer mode selector names, in AcousticConfig::Mode order.
inline constexpr std::array<const char*, 3> kAcousticModeSelectorNames = {"auto", "blind",
                                                                          "impulse_response"};

/// @brief Position of @p name in @p names, or -1 when it is not one of them.
template <std::size_t N>
int selector_from_name(const std::array<const char*, N>& names, const char* name) {
  for (std::size_t i = 0; i < N; ++i) {
    const char* candidate = names[i];
    std::size_t k = 0;
    while (candidate[k] != '\0' && name[k] == candidate[k]) ++k;
    if (candidate[k] == '\0' && name[k] == '\0') return static_cast<int>(i);
  }
  return -1;
}

}  // namespace sonare::acoustic
