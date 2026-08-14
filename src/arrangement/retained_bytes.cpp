/// @file retained_bytes.cpp
/// @brief Saturating arithmetic for retained-memory accounting.

#include "arrangement/retained_bytes.h"

#include <limits>

#include "arrangement/edit_command.h"
#include "arrangement/edit_command_internal.h"

namespace sonare::arrangement::retained {

std::size_t saturating_add(std::size_t lhs, std::size_t rhs) noexcept {
  if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
    return std::numeric_limits<std::size_t>::max();
  }
  return lhs + rhs;
}

std::size_t saturating_multiply(std::size_t lhs, std::size_t rhs) noexcept {
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    return std::numeric_limits<std::size_t>::max();
  }
  return lhs * rhs;
}

}  // namespace sonare::arrangement::retained
namespace sonare::arrangement {

namespace {

inline std::size_t add(std::size_t lhs, std::size_t rhs) noexcept {
  return retained::saturating_add(lhs, rhs);
}

}  // namespace

// Track commands ------------------------------------------------------------

std::size_t AddTrack::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(track_));
}

std::size_t RemoveTrack::retained_bytes() const noexcept { return sizeof(*this); }

std::size_t RenameTrack::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(name_));
}

std::size_t SetTrackRoute::retained_bytes() const noexcept {
  return add(sizeof(*this), add(retained::dynamic_bytes(channel_strip_ref_),
                                retained::dynamic_bytes(output_target_)));
}

std::size_t SetTrackKind::retained_bytes() const noexcept { return sizeof(*this); }
std::size_t SetTrackMidiDestination::retained_bytes() const noexcept { return sizeof(*this); }
std::size_t SetTrackGain::retained_bytes() const noexcept { return sizeof(*this); }
std::size_t SetTrackMute::retained_bytes() const noexcept { return sizeof(*this); }
std::size_t SetTrackSolo::retained_bytes() const noexcept { return sizeof(*this); }
std::size_t SetTrackPan::retained_bytes() const noexcept { return sizeof(*this); }

// Clip commands -------------------------------------------------------------

std::size_t AddClip::retained_bytes() const noexcept {
  std::size_t total = add(sizeof(*this), retained::dynamic_bytes(clip_));
  total = add(total, retained::dynamic_bytes(restore_events_));
  return add(total, retained::dynamic_bytes(restore_sysex_payloads_));
}

std::size_t RemoveClip::retained_bytes() const noexcept { return sizeof(*this); }
std::size_t SplitClip::retained_bytes() const noexcept { return sizeof(*this); }
std::size_t TrimClip::retained_bytes() const noexcept { return sizeof(*this); }
std::size_t MoveClip::retained_bytes() const noexcept { return sizeof(*this); }
std::size_t DuplicateClip::retained_bytes() const noexcept { return sizeof(*this); }
std::size_t SetClipGain::retained_bytes() const noexcept { return sizeof(*this); }
std::size_t SetClipFade::retained_bytes() const noexcept { return sizeof(*this); }
std::size_t SetClipLoop::retained_bytes() const noexcept { return sizeof(*this); }
std::size_t SetClipWarpRef::retained_bytes() const noexcept { return sizeof(*this); }
std::size_t SetClipWarpMode::retained_bytes() const noexcept { return sizeof(*this); }

std::size_t SetClipTakes::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(takes_));
}

std::size_t SetClipCompSegments::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(segments_));
}

std::size_t SetWarpMap::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(map_));
}

std::size_t RemoveWarpMap::retained_bytes() const noexcept { return sizeof(*this); }

std::size_t RestoreWarpMap::retained_bytes() const noexcept {
  return add(sizeof(*this), add(retained::dynamic_bytes(map_), retained::dynamic_bytes(clip_ids_)));
}

std::size_t SetClipSource::retained_bytes() const noexcept { return sizeof(*this); }

// Source commands -----------------------------------------------------------

std::size_t AttachAudioSource::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(ref_));
}

std::size_t AttachMidiSource::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(ref_));
}

std::size_t ReplaceSource::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(replacement_));
}

// Timeline commands ---------------------------------------------------------

std::size_t SetSampleRate::retained_bytes() const noexcept { return sizeof(*this); }
std::size_t SetOverlapPolicy::retained_bytes() const noexcept { return sizeof(*this); }

std::size_t SetScene::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(scene_));
}

std::size_t SetMarker::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(name_));
}

std::size_t SetAnnotation::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(annotation_));
}

std::size_t SetTempoSegment::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(segments_));
}

std::size_t SetTimeSignatureSegment::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(segments_));
}

std::size_t SetHarmonySegment::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(chords_));
}

// Automation commands -------------------------------------------------------

std::size_t AddAutomationLane::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(lane_));
}

std::size_t RemoveAutomationLane::retained_bytes() const noexcept { return sizeof(*this); }

std::size_t EditAutomationLane::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(lane_));
}

// MIDI / assist commands ----------------------------------------------------

std::size_t ReplaceMidiClipEvents::retained_bytes() const noexcept {
  std::size_t total = add(sizeof(*this), retained::dynamic_bytes(events_));
  return add(total, retained::dynamic_bytes(sysex_payloads_));
}

std::size_t PatchMidiClip::retained_bytes() const noexcept {
  std::size_t total = add(sizeof(*this), retained::dynamic_bytes(patch_.add));
  return add(total, retained::dynamic_bytes(patch_.remove));
}

std::size_t SetAssistSidecar::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(sidecar_));
}

std::size_t RemoveSourceInternal::retained_bytes() const noexcept { return sizeof(*this); }
std::size_t RemoveMarkerInternal::retained_bytes() const noexcept { return sizeof(*this); }

std::size_t InsertAutomationLane::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(lane_));
}

std::size_t RemoveAssistSidecarInternal::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(key_));
}

std::size_t UnsplitClip::retained_bytes() const noexcept {
  std::size_t total = add(sizeof(*this), retained::dynamic_bytes(original_));
  return add(total, retained::dynamic_bytes(restore_events_));
}

// Inverse-only command implementations from edit_command_internal.h --------

std::size_t detail::RestoreClip::retained_bytes() const noexcept {
  return add(sizeof(*this), retained::dynamic_bytes(clip_));
}

std::size_t detail::RestoreTrackWithClips::retained_bytes() const noexcept {
  std::size_t total = add(sizeof(*this), retained::dynamic_bytes(track_));
  total = add(total,
              retained::saturating_multiply(clips_.capacity(), sizeof(RemovedTrackClipSnapshot)));
  for (const auto& snapshot : clips_) {
    std::size_t snapshot_bytes = retained::dynamic_bytes(snapshot.clip);
    snapshot_bytes = add(snapshot_bytes, retained::dynamic_bytes(snapshot.events));
    snapshot_bytes = add(snapshot_bytes, retained::dynamic_bytes(snapshot.sysex_payloads));
    total = add(total, snapshot_bytes);
  }
  return total;
}

}  // namespace sonare::arrangement
