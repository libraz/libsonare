#pragma once

/// @file note_object.h
/// @brief The unit of non-destructive note editing.
///
/// A NoteObject is what a segmented note becomes once it carries enough to be
/// edited on its own: its span, the F0 and amplitude curves measured over that
/// span, two measured quality figures, and a pending edit. The edit is a delta
/// against the source audio, which is never mutated -- rendering a set whose
/// edits are all identity reproduces the input exactly.
///
/// Monophonic: notes do not overlap in time. The polyphonic case needs a
/// per-note spectral mask and is not modelled here.

#include <cstdint>
#include <vector>

namespace sonare::editing::note_model {

/// @brief A per-frame curve over one note's span.
struct NoteCurve {
  std::vector<float> values;
  /// Cadence the values are sampled at. A curve an extractor produces carries
  /// the source track's own cadence.
  float frame_rate_hz = 0.0f;
  /// Frame index of values[0] in the source F0 track.
  int frame_offset = 0;
};

/// @brief A pending, non-destructive change to one note.
struct NoteEdit {
  float pitch_shift_semitones = 0.0f;
  float gain_db = 0.0f;
  /// Moves the note along the timeline. Negative moves it earlier.
  int64_t time_offset_samples = 0;
  /// >1 lengthens the note, <1 shortens it. Pitch is preserved.
  float time_stretch_ratio = 1.0f;
  bool muted = false;

  /// @brief True when the edit changes nothing.
  /// @details Exact comparison, so a non-finite field reads as non-identity and
  ///          reaches the renderer's validation rather than passing through.
  bool is_identity() const noexcept {
    return !muted && pitch_shift_semitones == 0.0f && gain_db == 0.0f && time_offset_samples == 0 &&
           time_stretch_ratio == 1.0f;
  }
};

/// @brief One editable note.
struct NoteObject {
  /// Span in source samples, [onset_sample, offset_sample).
  int64_t onset_sample = 0;
  int64_t offset_sample = 0;
  /// Span in the source F0 track's frames, [frame_start, frame_end).
  int frame_start = 0;
  int frame_end = 0;

  float median_hz = 0.0f;
  /// Median pitch in cents above the extractor's reference_hz.
  float median_cents = 0.0f;

  NoteCurve f0_hz;
  /// Per-frame RMS over the same frames as @ref f0_hz, linear.
  NoteCurve amplitude;

  /// Pitch steadiness in [0, 1], from the median absolute deviation of the
  /// span's cents against the segmentation threshold. 1 is perfectly steady.
  ///
  /// The only quality figure a note carries. A voiced fraction would be one
  /// too, but the segmenter emits maximal voiced runs, so it is 1 for every
  /// note it can produce and measures nothing.
  float f0_stability = 0.0f;

  NoteEdit edit{};

  int64_t length_samples() const noexcept { return offset_sample - onset_sample; }
};

}  // namespace sonare::editing::note_model
