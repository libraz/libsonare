#pragma once

/// @file note_renderer.h
/// @brief Renders edited note objects back over their source audio.
///
/// Non-destructive: only notes whose edit is non-identity are resynthesized,
/// and the source passes through everywhere else. A set whose edits are all
/// identity therefore reproduces the input bit for bit, which is the property
/// the whole editing model rests on.

#include <vector>

#include "core/audio.h"
#include "editing/note_model/note_object.h"
#include "effects/time_stretch.h"

namespace sonare::editing::note_model {

struct NoteRenderConfig {
  /// Equal-power cross-fade at each edited note's edges.
  float fade_ms = 5.0f;
  StretchBackend stretch_backend = StretchBackend::NativeSpectral;
};

/// @brief Renders @p notes over @p audio.
/// @details The output has the input's length and sample rate; an edit that
///          pushes a note past either end is truncated there. A muted note
///          silences its span and its other edit fields do not apply. Overlap
///          is checked on the source spans only -- where time_offset_samples
///          lands a note is not, so two moved notes may be written over each
///          other. A note lengthened past its own span writes into its
///          neighbours' samples for the same reason.
///
///          Validation covers every note, identity or not: an unrenderable set
///          is unrenderable whether or not this call would touch it.
/// @throws SonareException(InvalidParameter) on empty audio, a note whose span
///         is empty or reversed, overlapping source spans, a non-finite or
///         non-positive edit field, or a non-finite config value.
Audio render_notes(const Audio& audio, const std::vector<NoteObject>& notes,
                   const NoteRenderConfig& config = {});

}  // namespace sonare::editing::note_model
