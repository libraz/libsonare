#pragma once

/// @file automation_curve.h
/// @brief Canonical automation breakpoint interpolation curve enum.
/// @details Header-only, dependency-free. Shared by `sonare::automation`
///          (engine PPQ-domain lanes) and `sonare::mixing` (sample-accurate SPSC
///          lanes) so both subsystems and their C / Node / Python / WASM ABIs
///          expose a single ordinal scheme.
///
///          The integer values are part of the public C ABI:
///          - `SonareAutomationPoint.curve_to_next` (engine)
///          - `sonare_strip_schedule_*_automation` `int curve` (mixer)
///          - WASM `automationCurveFromInt`/`automationCurveToInt`
///          - Python `AutomationCurve` IntEnum
///          - Node `AutomationCurve` string-to-int mapping
///
///          Do NOT reorder these without an ABI break.

#include <cstdint>

namespace sonare {

/// @brief Interpolation shape between two automation breakpoints.
///
/// Ordinal values are stable across the C ABI and bindings.
enum class AutomationCurve : std::int32_t {
  Linear = 0,       ///< Linear interpolation between value(a) and value(b).
  Exponential = 1,  ///< Exponential interpolation (log-domain), falls back to linear if either
                    ///< endpoint is non-positive.
  Hold = 2,         ///< Hold value(a) until reaching the next breakpoint at b.
  SCurve = 3,       ///< Smoothstep 3t^2 - 2t^3 interpolation.
};

}  // namespace sonare
