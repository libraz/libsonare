#pragma once

/// @file harmony_context.h
/// @brief Built-in IHarmonyContext: serves the project's own harmonic timeline.
///
/// It measures nothing and decides nothing. The timeline is whatever was
/// annotated onto the project -- by the MIR bridge from detected chords and
/// keys, or by a host writing symbols directly -- and this module only answers
/// questions about it. A project carrying no timeline answers "unknown"
/// everywhere, which the modules read as "no harmonic constraint" rather than as
/// an error.
///
/// Control/offline thread only. Stateless, so one instance serves every call.

#include <cstdint>
#include <vector>

#include "arrangement/harmonic_timeline.h"
#include "arrangement/project_view.h"
#include "midi/assist/i_harmony_context.h"

namespace sonare::midi::assist::modules {

class TimelineHarmonyContext final : public IHarmonyContext {
 public:
  /// @brief The chord sounding at @p ppq, or an unknown-quality symbol when the
  ///        timeline has none there.
  arrangement::ChordSymbol chord_at(const arrangement::ProjectView& view,
                                    double ppq) const override;

  /// @brief The key segment covering @p ppq, or an unknown-mode segment when the
  ///        timeline has none there.
  arrangement::KeySegment key_at(const arrangement::ProjectView& view, double ppq) const override;

  /// @brief Pitch classes the key at @p ppq admits. Empty when the key is
  ///        unknown, which states no constraint rather than an empty one.
  std::vector<uint8_t> scale_pitch_classes(const arrangement::ProjectView& view,
                                           double ppq) const override;
};

}  // namespace sonare::midi::assist::modules
