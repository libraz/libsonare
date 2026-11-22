#pragma once

/// @file i_rhythm_generator.h
/// @brief assist seam: rhythm-pattern generator (frozen abstract).
///
/// An IRhythmGenerator produces rhythmic onset patterns (a sequence of PPQ
/// onsets + accents) over the requested scope, returned as an AssistResult of
/// EditCommands. It NEVER mutates the project. Same-build C++ abstract class.
/// Control/offline thread only.

#include "arrangement/project_view.h"
#include "midi/assist/assist_request.h"

namespace sonare::midi::assist {

/// @brief Generates rhythmic onset patterns over the requested scope.
class IRhythmGenerator {
 public:
  virtual ~IRhythmGenerator() = default;

  /// @brief Stable module id (namespacing key for sidecar ownership).
  virtual const char* module_id() const noexcept = 0;

  /// @brief Produces an AssistResult of EditCommands realizing a rhythm for
  ///        `request` against `view`. Deterministic for a fixed seed, request and
  ///        view when no time deadline is reached; a time-truncated boundary
  ///        depends on execution timing. Budget-cooperative; never mutates the
  ///        project.
  virtual AssistResult generate(const arrangement::ProjectView& view,
                                const AssistRequest& request) = 0;
};

}  // namespace sonare::midi::assist
