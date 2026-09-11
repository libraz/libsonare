#pragma once

/// @file percussive_event.h
/// @brief The unit of non-destructive percussive editing.
///
/// A PercussiveEvent is a struck sound located in time: a span, three measured
/// figures, and a pending edit. It is deliberately not a note object -- it
/// carries no pitch, it is never associated with one, and the two models are
/// extracted by separate calls. A struck sound has no steady F0 to edit, so the
/// edit axes are time and amplitude and nothing else.
///
/// The signal an edit acts on is the percussive component of the span, not the
/// span itself. That is the whole reason the separation is in the model: moving
/// or silencing a hit leaves whatever was sounding underneath it in place.

#include <cstdint>

namespace sonare::editing::event_model {

/// @brief A pending, non-destructive change to one percussive event.
struct PercussiveEventEdit {
  /// Moves the hit along the timeline. Negative moves it earlier.
  int64_t time_offset_samples = 0;
  float gain_db = 0.0f;
  bool muted = false;

  /// @brief True when the edit changes nothing.
  /// @details Exact comparison, so a non-finite gain reads as non-identity and
  ///          reaches the renderer's validation rather than passing through.
  bool is_identity() const noexcept {
    return !muted && time_offset_samples == 0 && gain_db == 0.0f;
  }
};

/// @brief One editable percussive event.
struct PercussiveEvent {
  /// Span in source samples, [onset_sample, offset_sample). It runs from the
  /// detected onset to the next one, capped by the extractor.
  int64_t onset_sample = 0;
  int64_t offset_sample = 0;

  /// Detector strength at the onset, on the onset envelope's own scale. It
  /// orders events against each other and carries no absolute meaning.
  float strength = 0.0f;

  /// Peak absolute sample of the percussive component over the span, linear.
  /// This is what @c gain_db scales, so it is measured on the same signal the
  /// edit acts on rather than on the source.
  float peak_amplitude = 0.0f;

  /// Share of the span's energy the separation assigned to percussion, in
  /// [0, 1]. 0 when the span is silent.
  ///
  /// It describes the span rather than the onset that opened it. An isolated
  /// hit sits near 1, but a real hit over a loud sustain sits near 0, because
  /// the sustain owns the span's energy. So it separates a hit from a note
  /// attack only where nothing is sustaining through both, and it is not a
  /// test for whether a hit is there.
  float percussive_ratio = 0.0f;

  PercussiveEventEdit edit{};

  int64_t length_samples() const noexcept { return offset_sample - onset_sample; }
};

}  // namespace sonare::editing::event_model
