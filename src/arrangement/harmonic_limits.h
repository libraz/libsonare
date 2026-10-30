#pragma once

/// @file harmonic_limits.h
/// @brief Limits on harmonic annotation content shared by every path that
///        writes it: the C ABI setters and the project loader.

#include <cstddef>

namespace sonare::arrangement {

/// Most extension numbers one ChordSymbol carries.
inline constexpr std::size_t kMaxChordExtensions = 32;

}  // namespace sonare::arrangement
