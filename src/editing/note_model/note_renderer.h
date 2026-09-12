#pragma once

/// @file note_renderer.h
/// @brief Renders edited note objects back over their source audio.
///
/// Non-destructive: only notes whose edit is non-identity are resynthesized,
/// and the source passes through everywhere else. A set whose edits are all
/// identity therefore reproduces the input bit for bit, which is the property
/// the whole editing model rests on.

#include <cstdint>
#include <vector>

#include "core/audio.h"
#include "editing/note_model/note_object.h"
#include "editing/note_model/pitch_decomposition.h"
#include "effects/time_stretch.h"

namespace sonare::editing::note_model {

struct NoteRenderConfig {
  /// Equal-power cross-fade at each edited note's edges.
  float fade_ms = 5.0f;
  StretchBackend stretch_backend = StretchBackend::NativeSpectral;
  /// Where the vibrato and drift edits cut the pitch curve. Copy it from the
  /// @ref PitchDecomposition a host drew from rather than restating the cutoff.
  PitchDecompositionConfig decomposition{};
};

/// @brief Ramps @p source out over [@p begin, @p end) of @p output rather than
///        cutting it, over @p fade samples at each edge.
/// @details A note that is muted, shortened or moved away leaves that range
///          behind, and a hard cut puts a step there. The taper is the
///          equal-power counterpart of the one the note cross-fades use, so the
///          seam keeps its level; @c fade of 0 is a hard cut.
///
///          Exposed because the pitch editor vacates a span the same way and a
///          second copy of the taper would drift out of step with the
///          cross-fade it is paired against.
void erase_span(std::vector<float>& output, const Audio& source, int64_t begin, int64_t end,
                int64_t fade);

/// @brief Validates one note's span and edit fields as @ref render_notes does.
/// @details Exposed because the polyphonic chain makes the same per-note checks
///          and has to make them before it inverts anything, while the
///          disjointness check @ref render_notes also makes does not apply to it.
/// @throws SonareException(InvalidParameter) on a span that is empty, reversed or
///         starts before zero; a non-finite or non-positive edit field; a
///         non-finite or negative envelope value; or a vibrato or drift edit on a
///         note that carries no usable pitch curve to apply it to.
void validate_note_for_render(const NoteObject& note);

/// @brief Validates the config fields @ref render_notes checks before rendering.
/// @details Covers only what that function checks up front. @c decomposition is
///          validated where it is read, which is a note carrying a curve edit, so
///          a call with no such note never reaches it.
///
///          The percussive event chain's identically shaped check is a different
///          rule -- it rejects a @c fade_ms of 0 where this accepts one as a hard
///          cut -- so the two cannot be folded together.
///
///          Exposed for the same reason as @ref validate_note_for_render, and for
///          one more: the polyphonic chain can be handed an empty note list, and a
///          config it never applies would otherwise go unchecked -- so a
///          @c fade_ms of NaN would succeed on an empty set and throw on a
///          populated one.
/// @throws SonareException(InvalidParameter) on a @c fade_ms that is not finite or
///         is negative.
void validate_render_config(const NoteRenderConfig& config);

/// @brief Renders @p notes over @p audio.
/// @details The output has the input's length and sample rate; an edit that
///          pushes a note past either end is truncated there. A muted note
///          silences its span and its other edit fields do not apply. Overlap
///          is checked on the source spans only -- where time_offset_samples
///          lands a note is not, so two moved notes may be written over each
///          other. A note lengthened past its own span writes into its
///          neighbours' samples for the same reason.
///
///          Per note the order is: pitch curve, time stretch, pitch shift,
///          formant warp, amplitude envelope, then gain. The pitch curve goes
///          first because the note's F0 track describes the source audio, and
///          nothing later in the chain preserves the frame-to-sample mapping it
///          is read through. The formant warp is an LPC analysis-resynthesis
///          round and runs only when the note asks for one, so an edit that
///          leaves formant_shift_semitones at 0 costs nothing and loses nothing
///          to it.
///
///          Validation covers every note, identity or not: an unrenderable set
///          is unrenderable whether or not this call would touch it.
/// @throws SonareException(InvalidParameter) on empty audio, a note whose span
///         is empty or reversed, overlapping source spans, a non-finite or
///         non-positive edit field, a non-finite or negative envelope value, a
///         non-finite config value, or a vibrato/drift edit on a note that
///         carries no usable pitch curve to apply it to.
Audio render_notes(const Audio& audio, const std::vector<NoteObject>& notes,
                   const NoteRenderConfig& config = {});

}  // namespace sonare::editing::note_model
