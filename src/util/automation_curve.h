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

#include <cmath>
#include <cstdint>

namespace sonare {

/// @brief Interpolation shape between two automation breakpoints.
///
/// Ordinal values are stable across the C ABI and bindings.
enum class AutomationCurve : std::int32_t {
  Linear = 0,       ///< Linear interpolation between value(a) and value(b).
  Exponential = 1,  ///< Signed-log exponential interpolation; falls back to linear only when the
                    ///< endpoints straddle zero (opposite signs).
  Hold = 2,         ///< Hold value(a) until reaching the next breakpoint at b.
  SCurve = 3,       ///< Smoothstep 3t^2 - 2t^3 interpolation.
};

/// @brief Canonical breakpoint interpolation, shared by every automation lane.
/// @param curve Curve shape that governs the segment from @p va to @p vb.
/// @param va Start value (breakpoint a).
/// @param vb End value (breakpoint b).
/// @param t Normalized position in [0, 1] along the segment.
/// @details Defined once here so the engine PPQ-domain lane and the mixer
///          sample-accurate lane produce identical shapes for identical
///          breakpoint data and curve enum (they previously diverged on the
///          Exponential branch). Caller is responsible for clamping @p t.
inline double interpolate_curve(AutomationCurve curve, double va, double vb, double t) noexcept {
  switch (curve) {
    case AutomationCurve::Hold:
      return va;
    case AutomationCurve::Exponential: {
      // Interpolate in a signed, epsilon-shifted log domain so the curve stays
      // exponential even when an endpoint touches or crosses zero. Plain
      // log-interpolation is undefined for non-positive values; only endpoints
      // that straddle zero (opposite signs) fall back to linear.
      constexpr double kExpEpsilon = 1.0e-9;
      const double sa = va < 0.0 ? -1.0 : 1.0;
      const double sb = vb < 0.0 ? -1.0 : 1.0;
      if (sa == sb) {
        const double la = std::log(std::abs(va) + kExpEpsilon);
        const double lb = std::log(std::abs(vb) + kExpEpsilon);
        return sa * (std::exp(la + (lb - la) * t) - kExpEpsilon);
      }
      return va + (vb - va) * t;
    }
    case AutomationCurve::SCurve: {
      const double shaped = t * t * (3.0 - 2.0 * t);
      return va + (vb - va) * shaped;
    }
    case AutomationCurve::Linear:
    default:
      return va + (vb - va) * t;
  }
}

}  // namespace sonare
