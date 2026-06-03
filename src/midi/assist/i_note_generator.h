#pragma once

/// @file i_note_generator.h
/// @brief assist seam: melodic/harmonic note generator (frozen abstract).
///
/// An INoteGenerator is the primary generative slot: given a read-only
/// ProjectView and a deterministic AssistRequest it RETURNS an AssistResult
/// (a sequence of EditCommands + an opaque candidate payload). It NEVER mutates
/// the project. A future midi-sketch-style pop generator implements this slot.
/// Same-build C++ abstract class. Control/offline thread only.

#include "arrangement/project_view.h"
#include "midi/assist/assist_request.h"

namespace sonare::midi::assist {

/// @brief Generates notes (as EditCommands) over the requested scope.
class INoteGenerator {
 public:
  virtual ~INoteGenerator() = default;

  /// @brief Stable module id (namespacing key for sidecar ownership, e.g.
  ///        "midi-sketch"). Used by ProjectView::module_sidecar scoping.
  virtual const char* module_id() const noexcept = 0;

  /// @brief Produces an AssistResult for `request` against the read-only `view`.
  ///
  /// Determinism: same seed + same request -> identical result. The generator
  /// must honor request.budget cooperatively (partial result on overrun) and
  /// must NOT mutate any live project. On an internal failure it may throw or
  /// return an invalid result; the CompositionAssist driver discards such calls.
  virtual AssistResult generate(const arrangement::ProjectView& view,
                                const AssistRequest& request) = 0;
};

}  // namespace sonare::midi::assist
