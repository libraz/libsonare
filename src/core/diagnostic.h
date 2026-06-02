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
