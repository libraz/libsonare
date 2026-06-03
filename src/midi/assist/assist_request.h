#pragma once

/// @file assist_request.h
/// @brief composition-assist request/result value types + the voice model.
///
/// These are the shared value types that flow across the assist seam. They
/// are CONTROL/OFFLINE-THREAD ONLY and are never touched by the RT/audio thread,
/// the compiler, or the engine. The seam links in the SAME build (it is not a
/// stable ABI): when SONARE_WITH_ASSIST is OFF none of these symbols exist on the
/// core/binding surface.
///
/// Contract highlights:
///   - Determinism: every request carries a uint32_t seed; the same seed + same
///     request must produce an identical AssistResult. Modules use a seeded
///     inline PRNG (no Date/now/std::rand).
///   - Budget: an optional time/iteration cap bounds a run; overrun yields a
///     partial/empty result, never an unbounded block.
///   - All output is a sequence of arrangement::EditCommand: assist NEVER returns
///     raw mutations and NEVER mutates a live Project. The caller applies the
///     returned commands via EditHistory so undo/redo/serialize/replay stay
///     uniform.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/harmonic_timeline.h"

namespace sonare::midi::assist {

/// @brief What part of the arrangement a run targets.
enum class GenerationMode : uint32_t {
  kWhole = 0,              ///< Generate / regenerate the whole scoped material.
  kVocalOnly = 1,          ///< Melody / lead-vocal part only.
  kAccompanimentOnly = 2,  ///< Backing / accompaniment parts only.
};

/// @brief Optional time + iteration budget for a run.
///
/// A module must check the budget cooperatively and stop early (returning a
/// partial result) when it is exhausted. A zero/unset field means "no limit on
/// that axis". The CompositionAssist driver also clamps to this budget.
struct AssistBudget {
  /// Wall-clock cap in milliseconds (0 = no time cap). Cooperative.
  uint32_t max_time_ms = 0;
  /// Iteration / candidate cap (0 = no iteration cap). Cooperative.
  uint32_t max_iterations = 0;

  bool has_time_cap() const noexcept { return max_time_ms != 0; }
  bool has_iteration_cap() const noexcept { return max_iterations != 0; }
};

/// @brief The scope a run operates over: track ids + optional PPQ range + mode.
struct AssistScope {
  /// Target track ids. Empty means "module decides / project-wide".
  std::vector<arrangement::TrackId> track_ids;

  /// Optional PPQ range [start_ppq, end_ppq). Unset (nullopt) = whole timeline.
  std::optional<double> start_ppq;
  std::optional<double> end_ppq;

  GenerationMode mode = GenerationMode::kWhole;

  /// @brief True when `track_id` is in scope (an empty track list = all in scope).
  bool covers_track(arrangement::TrackId track_id) const noexcept {
    if (track_ids.empty()) return true;
    for (auto id : track_ids) {
      if (id == track_id) return true;
    }
    return false;
  }
};

/// @brief A deterministic assist request handed to a module.
///
/// The same (seed, scope, params_json) must always produce the same result.
/// `params_json` is a module-specific opaque blob the core never interprets.
struct AssistRequest {
  /// Deterministic PRNG seed. Same seed + same request -> same result.
  uint32_t seed = 0;
  /// Target scope (tracks + PPQ range + generation mode).
  AssistScope scope;
  /// Optional time/iteration budget (default = no caps).
  AssistBudget budget;
  /// Module-specific opaque parameter blob (UTF-8 JSON or bytes). The core does
  /// not parse it; the module interprets it.
  std::string params_json;
};

/// @brief Why / how a run terminated (returned to the host for telemetry).
enum class AssistStatus : uint32_t {
  kOk = 0,               ///< Completed normally.
  kEmpty = 1,            ///< Nothing to do / no module registered for the slot.
  kBudgetTruncated = 2,  ///< Stopped early because the budget was exhausted.
  kDiscarded = 3,        ///< Module threw / returned an invalid patch: call discarded.
};

/// @brief Diagnostics describing a run outcome (no state, pure telemetry).
struct AssistDiagnostics {
  AssistStatus status = AssistStatus::kEmpty;
  /// Human-readable reason (owned). Empty when uninteresting.
  std::string reason;
  /// Iterations actually consumed by the module (for budget reporting).
  uint32_t iterations_consumed = 0;
};

/// @brief The result of a run: a uniform command sequence + opaque candidate
///        payload + diagnostics.
///
/// `commands` is the ONLY mutation channel: the caller applies it via
/// EditHistory. `candidate_payload` is an opaque blob (e.g. seed + chosen notes)
/// the host can keep as a candidate / history branch and re-apply for a
/// deterministic re-render; the core never interprets it.
struct AssistResult {
  std::vector<arrangement::EditCommandPtr> commands;
  std::string candidate_payload;
  AssistDiagnostics diagnostics;

  bool empty() const noexcept { return commands.empty(); }
};

/// @brief Minimal value type describing a voice / part's state.
///
/// A voice models one monophonic-ish line (a melody, a bass, an inner voice).
/// Generators / counterpoint engines read and produce these; the core stores
/// them as plain value data and never interprets the musical meaning.
struct VoiceModel {
  /// Stable voice id within a run (0 = unset).
  uint32_t id = 0;
  /// Track this voice renders to (0 = unbound).
  arrangement::TrackId track_id = 0;
  /// Role of the voice (free string, e.g. "lead", "bass"; not interpreted).
  std::string role;
  /// Lowest / highest allowed MIDI note for the voice's range (inclusive).
  uint8_t low_note = 0;
  uint8_t high_note = 127;
  /// Preferred polyphony (1 = monophonic). The core does not enforce it.
  uint8_t max_polyphony = 1;
};

}  // namespace sonare::midi::assist
