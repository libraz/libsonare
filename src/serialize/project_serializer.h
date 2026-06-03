#pragma once

/// @file project_serializer.h
/// @brief Deterministic JSON serialization for the headless arrangement Project
///        .
///
/// This subsystem is OFFLINE / control-plane only. It performs NO file or device
/// I/O: it operates purely on in-memory @ref sonare::arrangement::Project /
/// @ref sonare::arrangement::MidiContentStore values and JSON strings. The host
/// (CLI / bindings) is responsible for reading/writing bytes to storage.
///
/// Determinism / byte-equality
/// ---------------------------
/// All numbers are emitted through @ref sonare::util::json::dump, which writes
/// doubles at max_digits10 precision (locale-independent), so the decimal text is
/// round-trippable and byte-deterministic across runs and builds. Object keys are
/// stored in a std::map and dumped in sorted order, so field order is stable
/// without per-struct ordering bookkeeping. The serializer never reads the clock,
/// a random source, or any environment state. Therefore:
///   - serialize(P) produces identical bytes for the same logical Project, and
///   - serialize(deserialize(serialize(P))) == serialize(P), byte-for-byte.
///
/// Forward-compatibility
/// ---------------------
/// Unknown JSON fields on input are SAFELY IGNORED, EXCEPT @ref
/// sonare::arrangement::AssistSidecar payloads, which are preserved LOSSLESSLY
/// (module_id + schema_version + opaque payload bytes) even for unregistered
/// modules / unknown payload schema versions. The core never interprets sidecar
/// payload bytes; binary payloads are carried as deterministic base64.
///
/// Mixer topology
/// --------------
/// The mixer @ref sonare::mixing::api::Scene is embedded via scene_to_json /
/// scene_from_json under SONARE_WITH_MIXING (the tested mixer serializer is the
/// single source of truth). In a mixing-OFF build the Scene struct fields are
/// serialized directly with util::json (same stable key order in both paths) so
/// round-trip still works without the mixing JSON helpers.
///
/// Error handling
/// --------------
/// Malformed / truncated / garbage input never crashes or reads out of bounds:
/// @ref project_from_json catches @ref sonare::util::json::JsonError and any
/// structural inconsistency and returns a @ref DeserializeResult carrying
/// diagnostics with an empty optional Project.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "arrangement/edit_command.h"  // MidiContentStore
#include "arrangement/edit_model.h"

namespace sonare::serialize {

/// @brief Project schema version written as the mandatory top-level "version"
///        integer. Bumped when the serialized JSON shape changes incompatibly.
///        Distinct from the engine RT ABI (rt::kEngineAbiVersion) and from the
///        flat project struct ABI; this layer only owns the JSON schema.
inline constexpr uint32_t SONARE_PROJECT_SCHEMA_VERSION = 1;

/// @brief Severity of a deserialize diagnostic.
///
/// @note This ordinal scheme (`kWarning=0, kError=1`) is INVERTED relative to
///   `arrangement::Diagnostic::Severity` (`kError=0, kWarning=1`) and to the
///   project-wide canonical `sonare::Diagnostic::Severity`
///   (`Info=0 < Warning=1 < Error=2`, see core/diagnostic.h). It is latent —
///   this layer never exposes the numeric ordinal across a boundary — so the
///   `static_assert`s below merely FREEZE the historical values and document
///   the divergence so it can never drift silently into a wire mismatch.
enum class DiagnosticSeverity : uint32_t {
  kWarning = 0,
  kError = 1,
};
static_assert(static_cast<uint32_t>(DiagnosticSeverity::kWarning) == 0u,
              "serialize DiagnosticSeverity::kWarning ordinal is frozen (see core/diagnostic.h)");
static_assert(static_cast<uint32_t>(DiagnosticSeverity::kError) == 1u,
              "serialize DiagnosticSeverity::kError ordinal is frozen (see core/diagnostic.h)");

/// @brief A single diagnostic produced while deserializing a project.
struct Diagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::kError;
  /// Stable machine-readable code (e.g. "malformed_json", "missing_version",
  /// "unsupported_schema_version").
  std::string code;
  /// Human-readable detail. Never contains a clock/PID/random value.
  std::string message;
};

/// @brief Result of @ref project_from_json. On success `project` and `midi` hold
///        the rebuilt model; on failure `project` is empty and `diagnostics`
///        explains why. `diagnostics` may also carry warnings on success.
struct DeserializeResult {
  std::optional<arrangement::Project> project;
  arrangement::MidiContentStore midi;
  std::vector<Diagnostic> diagnostics;

  /// @brief True when a project was successfully rebuilt.
  bool ok() const noexcept { return project.has_value(); }

  /// @brief True when any diagnostic is an error.
  bool has_error() const noexcept {
    for (const auto& d : diagnostics) {
      if (d.severity == DiagnosticSeverity::kError) return true;
    }
    return false;
  }
};

/// @brief Serializes a project (+ its MIDI content store) to a deterministic
///        JSON string. Stable key order, schema "version" field, round-trippable
///        float text. Pure: no I/O, no clock, no random.
std::string project_to_json(const arrangement::Project& project,
                            const arrangement::MidiContentStore& midi);

/// @brief Deserializes a project JSON produced by @ref project_to_json. Unknown
///        fields are ignored; AssistSidecars are preserved verbatim. Malformed /
///        truncated / garbage input never throws or reads OOB: it returns a
///        DeserializeResult with diagnostics and an empty Project.
DeserializeResult project_from_json(const std::string& json);

}  // namespace sonare::serialize
