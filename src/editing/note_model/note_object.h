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
/// Whether two notes may share samples is a renderer's rule and not the type's:
/// the monophonic chain rejects overlapping spans, while the polyphonic one pairs
/// each note with a spectral mask and expects them. The mask is not modelled
/// here -- a note carries its span and its edit, and nothing about which of the
/// two is rendering it.

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

  /// Moves the spectral envelope, in semitones, on top of whatever the pitch
  /// shift already did to it.
  ///
  /// 0 runs no warp at all, so a pitch-only edit stays the clean phase-vocoder
  /// shift it is today rather than gaining an LPC analysis-resynthesis round it
  /// did not ask for. A pitch shift drags the formants with it, so holding them
  /// still is -pitch_shift_semitones and the chipmunk is the default.
  ///
  /// The warp is defined over a factor range of [0.55, 1.65], so a shift
  /// saturates near -10.3 and +8.7 semitones rather than being rejected.
  float formant_shift_semitones = 0.0f;

  /// Scales the vibrato measured over the note, stated as a change from it:
  /// 0 keeps it, -1 flattens it, +1 doubles it. Applying it needs a pitch
  /// curve, so a note carrying none is rejected rather than left alone.
  float vibrato_depth_change = 0.0f;
  /// The same, for the slow drift around the note's centre pitch.
  float drift_change = 0.0f;

  /// Per-frame linear gain over the note's span, on top of @ref gain_db.
  ///
  /// Resampled to whatever length the note is rendered at, so it survives a
  /// time stretch and does not have to match the source's frame count. Empty
  /// leaves the note's own envelope alone, which is the identity.
  std::vector<float> amplitude_envelope;

  /// @brief True when the edit changes nothing.
  /// @details Exact comparison, so a non-finite field reads as non-identity and
  ///          reaches the renderer's validation rather than passing through.
  bool is_identity() const noexcept {
    return !muted && pitch_shift_semitones == 0.0f && gain_db == 0.0f && time_offset_samples == 0 &&
           time_stretch_ratio == 1.0f && formant_shift_semitones == 0.0f &&
           vibrato_depth_change == 0.0f && drift_change == 0.0f && amplitude_envelope.empty();
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

  /// Median pitch of the span, or 0 when no frame of it carried a usable one --
  /// the same way an F0 track spells an unvoiced frame. A span the segmenter
  /// produced always has one; a span handed to @ref make_note need not.
  float median_hz = 0.0f;
  /// Median pitch in cents above the extractor's reference_hz. 0 both when the
  /// median sits on reference_hz and when @ref median_hz says there is none.
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
