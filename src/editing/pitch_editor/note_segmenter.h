#pragma once

/// @file note_segmenter.h
/// @brief Monophonic note segmentation from F0 tracks.
///
/// Turns a monophonic F0 track into stable note regions. Used by the public
/// note-segment API, the monophonic transcriber and vocal-edit analysis.

#include <vector>

#include "editing/pitch_editor/f0_provider.h"
#include "util/constants.h"

namespace sonare::editing::pitch_editor {

struct NoteRegion {
  int onset_sample = 0;
  int offset_sample = 0;
  float median_cents = 0.0f;
  int frame_start = 0;
  int frame_end = 0;
};

struct NoteSegmenterConfig {
  float segmentation_threshold_cents = 50.0f;
  float min_note_ms = 30.0f;
  float reference_hz = constants::kA4Hz;
};

class NoteSegmenter {
 public:
  explicit NoteSegmenter(NoteSegmenterConfig config = {});

  /// @brief Cuts the voiced runs of @p track into notes.
  ///
  /// A frame further than segmentation_threshold_cents from the note's pitch
  /// for min_note_ms starts a new note there. The note's pitch is the midpoint
  /// of the span its frames covered while settling (a frame fits while it is
  /// within the threshold of that midpoint and the span stays within twice the
  /// threshold), frozen once the span has held for half
  /// a 250 ms cycle, two cycles at the latest; a midpoint is within the
  /// threshold of every frame it was built from whatever phase a vibrato started
  /// on. After a split the new note follows the glide while the pitch keeps
  /// moving the same way, so it begins where the glide ends. Each region's
  /// median_cents is the median of its voiced frames.
  std::vector<NoteRegion> segment(const F0Track& track) const;

 private:
  static float hz_to_cents(float hz, float reference_hz);
  NoteRegion make_region(const F0Track& track, int start, int end) const;

  NoteSegmenterConfig config_{};
};

}  // namespace sonare::editing::pitch_editor
