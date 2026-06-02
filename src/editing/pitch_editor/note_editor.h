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
  Audio stretch_note(const Audio& audio, const NoteRegion& region, float stretch_ratio) const;

 private:
  int fade_samples(int sample_rate, int region_length) const noexcept;
  static void apply_edge_fades(std::vector<float>& samples, int fade_samples);
  /// Equal-power cross-fade across a concatenation seam at @p splice, blending
  /// @p fade samples on each side. Keeps the seam continuous without the level
  /// dip a fade-to-zero introduces.
  static void crossfade_splice(std::vector<float>& samples, int splice, int fade);
  static NoteRegion clamp_region(const Audio& audio, const NoteRegion& region);

  NoteEditorConfig config_{};
};

}  // namespace sonare::editing::pitch_editor
