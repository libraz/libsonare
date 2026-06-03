#pragma once

/// @file diagnostic.h
/// @brief Shared, dependency-free diagnostic record for non-fatal error
///        reporting across offline/control-thread APIs.
///
/// Subsystems that validate user-supplied geometry, projects, or configuration
/// return `Diagnostic[]` instead of throwing, so callers (and the C-ABI, which
/// maps these onto `SonareError`) can surface problems without crashing. Used
/// by the room-acoustics module (geometry validation) and, later, by the
/// arrangement engine's `CompileResult`.

#include <string>
#include <vector>

namespace sonare {

/// @brief A single non-fatal diagnostic message.
///
/// @note CANONICAL severity ordering. This is the project-wide reference scheme:
///   ascending severity, `Info(0) < Warning(1) < Error(2)`. Two historical
///   diagnostic types predate this canonical form and use DIFFERENT ordinals,
///   so they are deliberately NOT migrated onto this struct:
///     - `arrangement::Diagnostic::Severity` is `kError=0, kWarning=1` and its
///       ordinal is EXPOSED NUMERICALLY through the C ABI
///       (`SonareProjectDiagnostic.severity`) — a frozen wire value.
///     - `serialize::DiagnosticSeverity` is `kWarning=0, kError=1` (INVERTED
///       relative to arrangement); it is latent (never crosses a boundary as a
///       number).
///   Unifying them would silently change those ordinals, so instead each pins
///   its mapping with a `static_assert` near its definition. New diagnostic
///   producers should use THIS canonical type and ordering.
struct Diagnostic {
  enum class Severity {
    Info,     ///< Informational; no action required.
    Warning,  ///< Recoverable; result is usable but degraded or clamped.
    Error,    ///< Invalid input; the requested result could not be produced.
  };

  Severity severity = Severity::Info;
  std::string code;     ///< Stable machine-readable id, e.g. "acoustic.source_outside_room".
  std::string message;  ///< Human-readable detail.
};

/// @brief Convenience: true if any diagnostic in the list is an Error.
inline bool has_error(const std::vector<Diagnostic>& diagnostics) noexcept {
  for (const auto& d : diagnostics) {
    if (d.severity == Diagnostic::Severity::Error) return true;
  }
  return false;
}

}  // namespace sonare
