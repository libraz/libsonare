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
///   - Determinism: every request carries a uint32_t seed; when no time cap is
///     reached, the same seed + same request produces an identical AssistResult.
///     Modules use a seeded inline PRNG (no Date/now/std::rand). A time-capped
///     boundary is cooperative and may stop at the monotonic deadline; content
///     already accepted before that boundary remains deterministic.
///   - Budget: modules cooperate with optional time/iteration caps and return
///     partial/empty results on exhaustion. The driver cannot preempt a module
///     that does not return.
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
/// The same request, including its budget, produces the same result when no
/// time deadline is reached. A time-truncated boundary depends on execution
/// time; the accepted candidates retain their deterministic content and order.
/// `params_json` is a module-specific opaque blob the core never interprets.
struct AssistRequest {
  /// Deterministic PRNG seed; deadline truncation can change the accepted prefix.
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
  /// The module REFUSED the request -- a parameter outside its domain, a blob it
  /// could not read. Distinct from kEmpty on purpose: "I will not answer this"
  /// and "I answered and had nothing to add" are different facts, and a caller
  /// that cannot tell them apart has to string-match the reason to find its own
  /// typo. The C ABI turns this one into an error return.
  kRejected = 4,
};

/// @brief Diagnostics describing a run outcome (no state, pure telemetry).
struct AssistDiagnostics {
  AssistStatus status = AssistStatus::kEmpty;
  /// Human-readable reason (owned). Empty when uninteresting.
  std::string reason;
  /// Reported work from normally returning modules, including work whose slot
  /// is later rejected, discarded, or truncated. Saturates at UINT32_MAX.
  uint32_t iterations_consumed = 0;
  /// Number of module slots discarded because they threw, exceeded the assigned
  /// budget, or returned commands that failed dry-run validation.
  uint32_t slots_discarded = 0;
};

/// @brief The result of a run: a uniform command sequence + opaque candidate
///        payload + diagnostics.
///
/// `commands` is the ONLY mutation channel: the caller applies it via
/// EditHistory. `candidate_payloads` preserves one opaque blob per contributing
/// slot; `candidate_payload` is the legacy newline-joined view kept for older
/// callers. The core never interprets either form.
struct AssistResult {
  std::vector<arrangement::EditCommandPtr> commands;
  std::vector<std::string> candidate_payloads;
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
