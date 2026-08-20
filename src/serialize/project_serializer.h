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
/// The mixer @ref sonare::mixing::api::Scene is embedded through the canonical
/// @c scene_to_json / @c scene_from_json helpers. They are control-plane-only
/// utilities and are available in mixing-OFF builds as well, so every project
/// build uses one scene schema walker and one stable key order.
///
/// Error handling
/// --------------
/// Malformed / truncated / garbage input never crashes or reads out of bounds:
/// @ref project_from_json catches @ref sonare::util::json::JsonError and any
/// structural inconsistency and returns a @ref DeserializeResult carrying
/// diagnostics with an empty optional Project.
///
/// Persistence budget
/// ------------------
/// A saved document must be readable by @ref project_from_json, which admits an
/// input only within @ref sonare::resource::kDefaultProjectImportResourceLimits.
/// The edit API is not bounded by that budget, so a caller can assemble a
/// project whose document would exceed it. @ref project_to_json therefore
/// measures the document it is about to emit against the same budget and throws
/// @c SonareException(ErrorCode::InvalidState) rather than returning bytes that
/// nothing can load back; a project within budget is serialized exactly as
/// before, byte for byte. Callers that must report an over-budget document as
/// something other than a project-state error — the MIDI import preflight,
/// which rejects the import as invalid input — use
/// @ref try_project_to_json instead.
///
/// Entity ID policy
/// ----------------
/// Source, track, clip, and marker IDs use the inclusive range
/// `[1, UINT32_MAX - 1]`. Zero is the public failure/not-found sentinel and
/// `UINT32_MAX` is reserved as the allocator-exhausted marker. Deserialization
/// rejects either reserved value and rejects duplicate IDs within each entity
/// namespace. Importing `UINT32_MAX - 1` is valid, but subsequent allocation in
/// that namespace fails explicitly by returning zero rather than wrapping.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "arrangement/edit_command.h"  // MidiContentStore
#include "arrangement/edit_model.h"
#include "util/resource_limits.h"

namespace sonare::serialize {

/// @brief Latest project schema version understood by this serializer. A
///        document containing only opaque automation lanes is still emitted as
///        schema version 1 for byte compatibility; version 2 is selected when
///        a typed automation lane is present.
///        Distinct from the engine RT ABI (rt::kEngineAbiVersion) and from the
///        flat project struct ABI; this layer only owns the JSON schema.
inline constexpr uint32_t SONARE_PROJECT_SCHEMA_VERSION = 2;
inline constexpr uint32_t SONARE_PROJECT_SCHEMA_VERSION_OPAQUE = 1;

/// @brief Severity of a deserialize diagnostic.
///
/// @note This ordinal scheme (`kWarning=0, kError=1`) is INVERTED relative to
///   `arrangement::Diagnostic::Severity` (`kError=0, kWarning=1`) and to the
///   project-wide canonical `sonare::Diagnostic::Severity`
///   (`Info=0 < Warning=1 < Error=2`, see core/diagnostic.h). It is latent —
///   this layer never exposes the numeric ordinal across a boundary — so the
///   `static_assert`s below merely FREEZE the historical values and document
///   the divergence so it can never drift silently into a wire mismatch.
///   Consequently: never order-compare these values ("severity >= kError"
///   style) or cast them into another Severity enum — compare against the
///   enumerators by equality only, and map per-value when converting.
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

/// @brief Size of an encoded project, in the same terms the import preflight
///        admits a document by.
///
/// The counts mirror @ref sonare::util::json::Parser exactly: one node per JSON
/// value (object keys are not values), cumulative decoded bytes for every key
/// and string value, one entity per array element, and the decoded (not base64)
/// size of every sidecar / SysEx payload.
struct ProjectDocumentShape {
  std::size_t json_bytes = 0;
  std::size_t json_nodes = 0;
  std::size_t entities = 0;
  std::size_t string_bytes = 0;
  std::size_t decoded_payload_bytes = 0;
};

/// @brief Measures the document @ref project_to_json would emit for this model,
///        without applying any budget. Encodes the project, so callers on the
///        save path should use @ref project_to_json (which checks the budget
///        itself) rather than measuring first.
ProjectDocumentShape measure_project_document(const arrangement::Project& project,
                                              const arrangement::MidiContentStore& midi);

/// @brief Serializes a project (+ its MIDI content store) to a deterministic
///        JSON string. Stable key order, schema "version" field, round-trippable
///        float text. Pure: no I/O, no clock, no random.
/// @param limits Persistence budget the emitted document must fit; the default
///        is the budget @ref project_from_json reads under. Overridable so the
///        exact boundary can be exercised without building a document of the
///        production size.
/// @throws sonare::SonareException with @c ErrorCode::InvalidState when the
///         document would exceed @p limits, i.e. when it could not be read
///         back. Nothing is emitted in that case.
std::string project_to_json(const arrangement::Project& project,
                            const arrangement::MidiContentStore& midi,
                            const resource::ProjectImportResourceLimits& limits =
                                resource::kDefaultProjectImportResourceLimits);

/// @brief Non-throwing form of @ref project_to_json, for callers that must tell
///        "this document does not fit the persistence budget" apart from an
///        error and report it in their own terms (the MIDI import preflight,
///        which rejects such an import as invalid input rather than as a broken
///        project state).
/// @param out_json Receives the encoded document on success; cleared on failure.
/// @return False when the document would exceed @p limits, i.e. exactly when
///         @ref project_to_json would throw. Nothing is emitted in that case.
bool try_project_to_json(const arrangement::Project& project,
                         const arrangement::MidiContentStore& midi, std::string* out_json,
                         const resource::ProjectImportResourceLimits& limits =
                             resource::kDefaultProjectImportResourceLimits);

/// @brief Deserializes a project JSON produced by @ref project_to_json. Unknown
///        fields are ignored; AssistSidecars are preserved verbatim. Malformed /
///        truncated / garbage input never throws or reads OOB: it returns a
///        DeserializeResult with diagnostics and an empty Project.
DeserializeResult project_from_json(const std::string& json);

}  // namespace sonare::serialize
