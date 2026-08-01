#pragma once

/// @file note_segmenter.h
/// @brief Monophonic note segmentation from F0 tracks.
///
/// This is a building block for the pitch-editing pipeline. Public facades use
/// it to turn a caller-supplied monophonic F0 track into stable note regions;
/// the internal pitch-correction pipeline also retains its direct use.

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
  std::vector<NoteRegion> segment(const F0Track& track) const;

 private:
  static float hz_to_cents(float hz, float reference_hz);
  NoteRegion make_region(const F0Track& track, int start, int end) const;

  NoteSegmenterConfig config_{};
};

}  // namespace sonare::editing::pitch_editor
