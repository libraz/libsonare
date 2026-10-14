#pragma once

/// @file clip_ppq.h
/// @brief Affine conversion between a clip's content and timeline PPQ.
///
/// MIDI event positions are stored in source-content PPQ. Assist harmony and
/// placement queries use timeline PPQ. The compiler applies this same start /
/// source-offset relationship when it schedules a clip, so keep the two
/// directions together here and leave loop, warp, and clipping policy to the
/// compiler.

#include "arrangement/edit_model.h"

namespace sonare::midi::assist {

/// Converts a source-content PPQ position to the clip timeline.
inline double clip_content_to_timeline_ppq(const arrangement::EditClip& clip,
                                           double content_ppq) noexcept {
  return clip.start_ppq + (content_ppq - clip.source_offset_ppq);
}

/// Converts a clip timeline PPQ position to source-content PPQ.
inline double timeline_to_clip_content_ppq(const arrangement::EditClip& clip,
                                           double timeline_ppq) noexcept {
  return clip.source_offset_ppq + (timeline_ppq - clip.start_ppq);
}

}  // namespace sonare::midi::assist
