#pragma once

/// @file control_ranges.h
/// @brief The accepted range of the controls several effect inserts share.
///
/// Each limit is defined once here and read by every insert's clamp and by the
/// GS EFX conversion layer, so a translated byte range can never reach past
/// what the receiving control accepts.

namespace sonare::effects::common {

/// Largest loop-gain magnitude a feedback control accepts, either sign: the
/// GS EFX translated range's +-98%. Every loop's other gains sit at or below
/// unity, so the loop stays stable at it.
inline constexpr float kMaxFeedback = 0.98f;

/// Longest centre / pre-delay, in milliseconds, a modulated-delay insert
/// accepts live: the top of the GS EFX pre-delay ladder.
inline constexpr float kMaxModulationPreDelayMs = 100.0f;

/// Lowest level, in dB, a level control accepts; the silent end of a level
/// conversion lands here.
inline constexpr float kLevelFloorDb = -120.0f;

}  // namespace sonare::effects::common
