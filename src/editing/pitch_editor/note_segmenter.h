#pragma once

/// @file note_segmenter.h
/// @brief Monophonic note segmentation from F0 tracks.
///
/// Core-internal: this is a building block for the pitch-editing pipeline and is
/// not exposed on any public surface (C ABI / Node / Python / WASM). Callers use
/// the higher-level pitch-correction entry points; segment boundaries are an
/// implementation detail, not part of the public contract.

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
