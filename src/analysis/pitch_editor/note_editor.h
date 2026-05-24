#pragma once

/// @file note_editor.h
/// @brief Region-level monophonic note editing.

#include <vector>

#include "analysis/pitch_editor/note_segmenter.h"
#include "core/audio.h"
#include "effects/time_stretch.h"

namespace sonare::analysis::pitch_editor {

struct NoteEditorConfig {
  float fade_ms = 5.0f;
  StretchBackend stretch_backend = StretchBackend::NativeSpectral;
};

class NoteEditor {
 public:
  explicit NoteEditor(NoteEditorConfig config = {});

  Audio move_note(const Audio& audio, const NoteRegion& region, int target_onset_sample) const;
  Audio stretch_note(const Audio& audio, const NoteRegion& region, float stretch_ratio) const;

 private:
  int fade_samples(int sample_rate, int region_length) const noexcept;
  static void apply_edge_fades(std::vector<float>& samples, int fade_samples);
  static NoteRegion clamp_region(const Audio& audio, const NoteRegion& region);

  NoteEditorConfig config_{};
};

}  // namespace sonare::analysis::pitch_editor
