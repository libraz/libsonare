#pragma once

/// @file solo_mute.h
/// @brief Shared solo / solo-safe implied-mute rule.

namespace sonare::mixing {

inline bool solo_implies_mute(bool any_solo, bool strip_soloed, bool strip_solo_safe) noexcept {
  return any_solo && !strip_soloed && !strip_solo_safe;
}

}  // namespace sonare::mixing
