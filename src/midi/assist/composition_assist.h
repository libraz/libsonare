#pragma once

/// @file composition_assist.h
/// @brief composition-assist driver: runs registered modules safely.
///
/// CompositionAssist is the explicit control/offline entry point that drives the
/// registered assist modules over a read-only ProjectView and collects their
/// EditCommands. It enforces the budget and the error contract:
///   - A module that throws or returns an invalid patch -> that call is
///     DISCARDED (empty result, no throw escaping), the project is untouched,
///     and the registry registration is preserved.
///   - The returned commands are applied by the CALLER via EditHistory; this
///     driver NEVER mutates a live Project.
///
/// CONTROL/OFFLINE-THREAD ONLY. Never called from RT / the audio callback / the
/// the compiler / the engine. When SONARE_WITH_ASSIST is OFF this type is absent.

#include "arrangement/project_view.h"
#include "midi/assist/assist_registry.h"
#include "midi/assist/assist_request.h"

namespace sonare::midi::assist {

/// @brief Safe driver over an AssistRegistry.
///
/// Holds a non-owning reference to the registry (the host owns it and the module
/// pointees). run() invokes the registered generative slots (generator,
/// counterpoint, rhythm) in a fixed order and merges their commands into one
/// AssistResult; unregistered slots are no-ops. Query slots (harmony,
/// dissonance, judge) are made available to modules through the registry but are
/// not invoked directly by the driver.
class CompositionAssist {
 public:
  explicit CompositionAssist(const AssistRegistry& registry) noexcept : registry_(registry) {}

  /// @brief Runs the registered modules for `request` against the read-only
  ///        `view` and returns the merged AssistResult.
  ///
  /// Guarantees:
  ///   - Never mutates `view` / the project.
  ///   - Never lets a module exception escape: a throwing/invalid module call is
  ///     discarded (its commands dropped) and reflected in diagnostics.
  ///   - Honors request.budget: stops dispatching further slots once the
  ///     iteration budget is exhausted and marks the result truncated.
  ///   - With no modules registered, returns an empty result (kEmpty).
  AssistResult run(const arrangement::ProjectView& view, const AssistRequest& request) const;

 private:
  const AssistRegistry& registry_;
};

}  // namespace sonare::midi::assist
