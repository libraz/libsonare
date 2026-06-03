#pragma once

/// @file i_counterpoint_engine.h
/// @brief assist seam: counterpoint / voice-derivation engine (frozen).
///
/// An ICounterpointEngine derives additional voices (counter-melodies, inner
/// voices, harmonization) from existing material in a read-only ProjectView and
/// RETURNS an AssistResult of EditCommands. It NEVER mutates the project.
/// Same-build C++ abstract class. Control/offline thread only.

#include <vector>

#include "arrangement/project_view.h"
#include "midi/assist/assist_request.h"

namespace sonare::midi::assist {

/// @brief Derives counterpoint / additional voices against existing material.
class ICounterpointEngine {
 public:
  virtual ~ICounterpointEngine() = default;

  /// @brief Stable module id (namespacing key for sidecar ownership).
  virtual const char* module_id() const noexcept = 0;

  /// @brief Derives voices for `request` against `view`, given the existing
  ///        `cantus` voices to harmonize/counterpoint against.
  /// @return an AssistResult (EditCommands + candidate payload). Deterministic
  ///         for a fixed seed; budget-cooperative; never mutates the project.
  virtual AssistResult derive(const arrangement::ProjectView& view, const AssistRequest& request,
                              const std::vector<VoiceModel>& cantus) = 0;
};

}  // namespace sonare::midi::assist
