/// @file composition_assist.cpp
/// @brief CompositionAssist driver implementation.

#include "midi/assist/composition_assist.h"

#include <exception>
#include <utility>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_model.h"

namespace sonare::midi::assist {

namespace {

/// @brief Validates a module's command sequence by dry-applying it on a COPY of
///        the project + content store, so a live project is never mutated.
///
/// Returns true only when every command applies cleanly to the copy. An empty
/// command sequence is valid (a no-op result). The copy is discarded; the
/// caller is responsible for the real apply (via EditHistory).
bool commands_apply_cleanly(const arrangement::ProjectView& view,
                            const std::vector<arrangement::EditCommandPtr>& commands) {
  // Snapshot copies. ProjectView holds const refs into the host's project; the
  // copies here are scratch only and never observed by anyone else.
  arrangement::Project scratch = view.project();
  arrangement::MidiContentStore scratch_store;
  // Rebuild the scratch store from the view's underlying store. We copy every
  // clip's events so PatchMidiClip / ReplaceMidiClipEvents validate against the
  // real content rather than an empty store.
  for (const auto& clip : scratch.clips()) {
    if (const auto* ev = view.clip_events(clip.id)) {
      scratch_store.events[clip.id] = *ev;
    }
  }
  for (const auto& cmd : commands) {
    if (cmd == nullptr) return false;
    if (!cmd->apply(scratch, scratch_store)) return false;
  }
  return true;
}

/// @brief Runs one generative slot guarded by the error + budget contract.
///
/// On success, moves the module's commands into `out` and accumulates its
/// candidate payload / iteration count. On a module exception OR an invalid
/// command sequence the call is DISCARDED (nothing merged) and `discarded` is
/// set. Returns the module's reported iteration count (0 when discarded).
template <typename RunFn>
void dispatch_slot(const arrangement::ProjectView& view, RunFn&& run_module, AssistResult* out,
                   bool* discarded) {
  *discarded = false;
  AssistResult module_result;
  try {
    module_result = run_module();
  } catch (const std::exception&) {
    *discarded = true;
    return;
  } catch (...) {
    *discarded = true;
    return;
  }

  if (!commands_apply_cleanly(view, module_result.commands)) {
    *discarded = true;
    return;
  }

  out->diagnostics.iterations_consumed += module_result.diagnostics.iterations_consumed;
  if (!module_result.candidate_payload.empty()) {
    if (!out->candidate_payload.empty()) out->candidate_payload.push_back('\n');
    out->candidate_payload += module_result.candidate_payload;
  }
  for (auto& cmd : module_result.commands) {
    out->commands.push_back(std::move(cmd));
  }
}

}  // namespace

AssistResult CompositionAssist::run(const arrangement::ProjectView& view,
                                    const AssistRequest& request) const {
  AssistResult result;
  result.diagnostics.status = AssistStatus::kEmpty;

  // Iteration budget: count slots dispatched against the cap (cooperative — the
  // driver stops dispatching further slots once the cap is reached, and modules
  // additionally self-truncate via request.budget).
  const bool has_iter_cap = request.budget.has_iteration_cap();
  uint32_t dispatched = 0;
  bool truncated = false;
  bool any_discarded = false;
  bool any_ran = false;

  // A budget check helper: returns false (and marks truncated) when dispatching
  // another slot would exceed the iteration cap.
  const auto budget_ok = [&]() -> bool {
    if (has_iter_cap && dispatched >= request.budget.max_iterations) {
      truncated = true;
      return false;
    }
    return true;
  };

  // Fixed dispatch order: generator -> counterpoint -> rhythm. Query slots
  // (harmony/dissonance/judge) are consulted by modules, not by the driver.
  if (auto* gen = registry_.generator(); gen != nullptr && budget_ok()) {
    ++dispatched;
    any_ran = true;
    bool discarded = false;
    dispatch_slot(view, [&]() { return gen->generate(view, request); }, &result, &discarded);
    any_discarded = any_discarded || discarded;
  }

  if (auto* cp = registry_.counterpoint(); cp != nullptr && budget_ok()) {
    ++dispatched;
    any_ran = true;
    bool discarded = false;
    dispatch_slot(view, [&]() { return cp->derive(view, request, {}); }, &result, &discarded);
    any_discarded = any_discarded || discarded;
  }

  if (auto* rhythm = registry_.rhythm(); rhythm != nullptr && budget_ok()) {
    ++dispatched;
    any_ran = true;
    bool discarded = false;
    dispatch_slot(view, [&]() { return rhythm->generate(view, request); }, &result, &discarded);
    any_discarded = any_discarded || discarded;
  }

  // Compose the final status from what happened.
  if (truncated) {
    result.diagnostics.status = AssistStatus::kBudgetTruncated;
    result.diagnostics.reason = "iteration budget exhausted";
  } else if (any_discarded && result.commands.empty()) {
    result.diagnostics.status = AssistStatus::kDiscarded;
    result.diagnostics.reason = "module threw or returned an invalid patch";
  } else if (!any_ran || result.commands.empty()) {
    result.diagnostics.status = AssistStatus::kEmpty;
  } else {
    result.diagnostics.status = AssistStatus::kOk;
  }

  return result;
}

}  // namespace sonare::midi::assist
