/// @file composition_assist.cpp
/// @brief CompositionAssist driver implementation.

#include "midi/assist/composition_assist.h"

#include <chrono>
#include <exception>
#include <utility>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_model.h"

namespace sonare::midi::assist {

namespace {

/// @brief Copies the ProjectView content store into a scratch store.
///
/// Rebuilds every clip's events so PatchMidiClip / ReplaceMidiClipEvents
/// validate against the real content rather than an empty store.
arrangement::MidiContentStore copy_content_store(const arrangement::ProjectView& view) {
  arrangement::MidiContentStore scratch_store;
  // Rebuild the scratch store from the view's underlying store. We copy every
  // clip's events so PatchMidiClip / ReplaceMidiClipEvents validate against the
  // real content rather than an empty store.
  for (const auto& clip : view.clips()) {
    if (const auto* ev = view.clip_events(clip.id)) {
      scratch_store.events[clip.id] = *ev;
    }
  }
  return scratch_store;
}

/// @brief Validates a module's command sequence by dry-applying it on cumulative
///        scratch state, so earlier accepted slots are visible to later slots.
///
/// Returns true only when every command applies cleanly. On failure, the scratch
/// state is left unchanged. On success, scratch advances to include the module's
/// patch. The live project is never mutated.
bool commands_apply_cleanly(arrangement::Project* scratch,
                            arrangement::MidiContentStore* scratch_store,
                            const std::vector<arrangement::EditCommandPtr>& commands,
                            const AssistScope& scope);

bool clip_equal(const arrangement::EditClip& a, const arrangement::EditClip& b) {
  return a.id == b.id && a.track_id == b.track_id && a.source_id == b.source_id &&
         a.start_ppq == b.start_ppq && a.length_ppq == b.length_ppq &&
         a.source_offset_ppq == b.source_offset_ppq && a.gain == b.gain &&
         a.fade_in.length_ppq == b.fade_in.length_ppq && a.fade_in.curve == b.fade_in.curve &&
         a.fade_out.length_ppq == b.fade_out.length_ppq && a.fade_out.curve == b.fade_out.curve &&
         a.loop_mode == b.loop_mode && a.loop_length_ppq == b.loop_length_ppq &&
         a.warp_ref_id == b.warp_ref_id;
}

bool track_equal(const arrangement::Track& a, const arrangement::Track& b) {
  return a.id == b.id && a.name == b.name && a.kind == b.kind &&
         a.channel_strip_ref == b.channel_strip_ref && a.output_target == b.output_target &&
         a.midi_destination_id == b.midi_destination_id;
}

bool scoped_tracks_unchanged(const arrangement::Project& before,
                             const arrangement::MidiContentStore& before_store,
                             const arrangement::Project& after,
                             const arrangement::MidiContentStore& after_store,
                             const AssistScope& scope) {
  if (scope.track_ids.empty()) return true;

  for (const auto& track : after.tracks()) {
    const arrangement::Track* before_track = before.find_track(track.id);
    if (before_track == nullptr) {
      return false;
    }
    if (!scope.covers_track(track.id) && !track_equal(*before_track, track)) {
      return false;
    }
  }
  for (const auto& track : before.tracks()) {
    if (after.find_track(track.id) == nullptr && !scope.covers_track(track.id)) {
      return false;
    }
  }

  for (const auto& clip : after.clips()) {
    const arrangement::EditClip* before_clip = before.find_clip(clip.id);
    if (before_clip == nullptr) {
      if (!scope.covers_track(clip.track_id)) return false;
      continue;
    }
    if (!scope.covers_track(before_clip->track_id) && !clip_equal(*before_clip, clip)) {
      return false;
    }
  }

  for (const auto& clip : before.clips()) {
    if (scope.covers_track(clip.track_id)) continue;
    const arrangement::EditClip* after_clip = after.find_clip(clip.id);
    if (after_clip == nullptr) return false;
    const auto before_events = before_store.events.find(clip.id);
    const auto after_events = after_store.events.find(clip.id);
    const bool before_has = before_events != before_store.events.end();
    const bool after_has = after_events != after_store.events.end();
    if (before_has != after_has) return false;
    if (before_has && before_events->second != after_events->second) return false;
  }

  return true;
}

bool commands_apply_cleanly(arrangement::Project* scratch,
                            arrangement::MidiContentStore* scratch_store,
                            const std::vector<arrangement::EditCommandPtr>& commands,
                            const AssistScope& scope) {
  arrangement::Project candidate = *scratch;
  arrangement::MidiContentStore candidate_store = *scratch_store;
  for (const auto& cmd : commands) {
    if (cmd == nullptr) return false;
    if (!cmd->apply(candidate, candidate_store)) return false;
  }
  if (!scoped_tracks_unchanged(*scratch, *scratch_store, candidate, candidate_store, scope)) {
    return false;
  }
  *scratch = std::move(candidate);
  *scratch_store = std::move(candidate_store);
  return true;
}

/// @brief Runs one generative slot guarded by the error + budget contract.
///
/// On success, moves the module's commands into `out` and accumulates its
/// candidate payload / iteration count. On a module exception OR an invalid
/// command sequence the call is DISCARDED (nothing merged) and `discarded` is
/// set. Returns the module's reported iteration count (0 when discarded).
template <typename RunFn>
void dispatch_slot(arrangement::Project* scratch, arrangement::MidiContentStore* scratch_store,
                   const AssistScope& scope, RunFn&& run_module, AssistResult* out,
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

  if (!commands_apply_cleanly(scratch, scratch_store, module_result.commands, scope)) {
    *discarded = true;
    return;
  }

  out->diagnostics.iterations_consumed += module_result.diagnostics.iterations_consumed;
  if (module_result.candidate_payloads.empty()) {
    if (!module_result.candidate_payload.empty()) {
      module_result.candidate_payloads.push_back(std::move(module_result.candidate_payload));
    }
  }
  for (auto& payload : module_result.candidate_payloads) {
    if (payload.empty()) continue;
    if (!out->candidate_payload.empty()) out->candidate_payload.push_back('\n');
    out->candidate_payload += payload;
    out->candidate_payloads.push_back(std::move(payload));
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
  const auto start_time = std::chrono::steady_clock::now();

  // Iteration budget: count slots dispatched against the cap (cooperative — the
  // driver stops dispatching further slots once the cap is reached, and modules
  // additionally self-truncate via request.budget).
  const bool has_iter_cap = request.budget.has_iteration_cap();
  const bool has_time_cap = request.budget.has_time_cap();
  uint32_t dispatched = 0;
  bool truncated = false;
  bool any_discarded = false;
  bool any_ran = false;
  arrangement::Project scratch = view.project();
  arrangement::MidiContentStore scratch_store = copy_content_store(view);

  const AssistQueryContext queries{registry_.harmony_context(), registry_.dissonance_analyzer(),
                                   registry_.judge()};

  // A budget check helper: returns false (and marks truncated) when dispatching
  // another slot would exceed the iteration cap.
  const auto budget_ok = [&]() -> bool {
    if (has_iter_cap && dispatched >= request.budget.max_iterations) {
      truncated = true;
      return false;
    }
    if (has_time_cap) {
      const auto elapsed = std::chrono::steady_clock::now() - start_time;
      const auto elapsed_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
      if (elapsed_ms >= static_cast<int64_t>(request.budget.max_time_ms)) {
        truncated = true;
        return false;
      }
    }
    return true;
  };

  // Fixed dispatch order: generator -> counterpoint -> rhythm. Query slots
  // (harmony/dissonance/judge) are consulted by modules, not by the driver.
  if (auto* gen = registry_.generator(); gen != nullptr && budget_ok()) {
    ++dispatched;
    any_ran = true;
    bool discarded = false;
    dispatch_slot(
        &scratch, &scratch_store, request.scope, [&]() { return gen->generate(view, request); },
        &result, &discarded);
    any_discarded = any_discarded || discarded;
    if (discarded) ++result.diagnostics.slots_discarded;
  }

  if (auto* cp = registry_.counterpoint(); cp != nullptr && budget_ok()) {
    ++dispatched;
    any_ran = true;
    bool discarded = false;
    dispatch_slot(
        &scratch, &scratch_store, request.scope,
        [&]() { return cp->derive(view, request, {}, queries); }, &result, &discarded);
    any_discarded = any_discarded || discarded;
    if (discarded) ++result.diagnostics.slots_discarded;
  }

  if (auto* rhythm = registry_.rhythm(); rhythm != nullptr && budget_ok()) {
    ++dispatched;
    any_ran = true;
    bool discarded = false;
    dispatch_slot(
        &scratch, &scratch_store, request.scope, [&]() { return rhythm->generate(view, request); },
        &result, &discarded);
    any_discarded = any_discarded || discarded;
    if (discarded) ++result.diagnostics.slots_discarded;
  }

  // Compose the final status from what happened.
  if (truncated) {
    result.diagnostics.status = AssistStatus::kBudgetTruncated;
    result.diagnostics.reason = has_iter_cap && dispatched >= request.budget.max_iterations
                                    ? "iteration budget exhausted"
                                    : "time budget exhausted";
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
