#pragma once

/// @file note_tracking.h
/// @brief Which note a channel-addressed value belongs to when several are
///        sounding on one channel.
///
/// Layering: depends on nothing. Split out the way articulation_mode.h is, so
/// the profile that configures the rule and the zone model that applies it can
/// each name it without depending on the other.
///
/// The rule is chosen rather than derived, because MPE poses the question and
/// declines to answer it: "When there is more than one concurrent Active Note on
/// a Member Channel, implementation of how controllers affect the notes is up to
/// the Device" (M1-100-UM v1.1 section 2.2.4.1). It is chosen per dimension --
/// a host can track pressure to the newest note while bend reaches every one.

#include <cstdint>

namespace sonare::midi {

enum class NoteTracking : uint8_t {
  kLastNote = 0,  ///< The most recently started. The default.
  kLowestNote = 1,
  kHighestNote = 2,
  kAllNotes = 3,
};

inline constexpr uint8_t kNoteTrackingCount = 4;

}  // namespace sonare::midi
