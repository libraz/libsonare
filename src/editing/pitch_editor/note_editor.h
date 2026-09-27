#pragma once

/// @file note_editor.h
/// @brief Region-level monophonic note editing.

#include <vector>

#include "core/audio.h"
#include "editing/pitch_editor/note_segmenter.h"
#include "effects/time_stretch.h"

namespace sonare::editing::pitch_editor {

struct NoteEditorConfig {
  float fade_ms = 5.0f;
  StretchBackend stretch_backend = StretchBackend::NativeSpectral;
};

class NoteEditor {
 public:
  explicit NoteEditor(NoteEditorConfig config = {});

  Audio move_note(const Audio& audio, const NoteRegion& region, int target_onset_sample) const;
  /// Replaces the region with its time-stretched copy. Only the region's own
  /// length changes: the result is `audio.size() - region + stretched` samples
  /// and the audio after the region is shifted by exactly `stretched - region`,
  /// so a ratio of 1.0 keeps every later onset where it was.
  Audio stretch_note(const Audio& audio, const NoteRegion& region, float stretch_ratio) const;

 private:
  int fade_samples(int sample_rate, int region_length) const noexcept;
  static void apply_edge_fades(std::vector<float>& samples, int fade_samples);
  static NoteRegion clamp_region(const Audio& audio, const NoteRegion& region);

  NoteEditorConfig config_{};
};

}  // namespace sonare::editing::pitch_editor
