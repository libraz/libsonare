#pragma once

/// @file i_harmony_context.h
/// @brief assist seam: harmony-context query interface (frozen abstract).
///
/// An IHarmonyContext answers "what chord / scale applies at this PPQ?" over a
/// read-only ProjectView. It is the harmony oracle a future composition-theory
/// engine implements; the mock/test provides a trivial one. Same-build C++
/// abstract class (ABI stability NOT required). Control/offline thread only.

#include <cstdint>
#include <vector>

#include "arrangement/harmonic_timeline.h"
#include "arrangement/project_view.h"

namespace sonare::midi::assist {

/// @brief Read-only harmony oracle queried at PPQ granularity.
class IHarmonyContext {
 public:
  virtual ~IHarmonyContext() = default;

  /// @brief Returns the chord symbol active at `ppq` for the given view, or a
  ///        default-constructed (kUnknown) symbol when none applies.
  virtual arrangement::ChordSymbol chord_at(const arrangement::ProjectView& view,
                                            double ppq) const = 0;

  /// @brief Returns the key segment active at `ppq`, or a default (kUnknown) key.
  virtual arrangement::KeySegment key_at(const arrangement::ProjectView& view,
                                         double ppq) const = 0;

  /// @brief Returns the allowed scale pitch classes (0..11) at `ppq` — the set a
  ///        placement judge / generator may draw from. Empty = unconstrained.
  virtual std::vector<uint8_t> scale_pitch_classes(const arrangement::ProjectView& view,
                                                   double ppq) const = 0;
};

}  // namespace sonare::midi::assist
