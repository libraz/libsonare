#pragma once

/// @file masked_renderer.h
/// @brief Renders edited notes through their spectral masks and sums them back.
///
/// The monophonic chain erases a note's span from the mixed audio and writes the
/// edited note over it. Polyphony cannot erase: another note is sounding in the
/// same samples. The mask is what replaces the erasure -- a note is inverted from
/// its own share of the STFT, edited in isolation, and added back to what no note
/// claimed.
///
/// The per-note chain is therefore not reimplemented here.
/// @ref note_model::render_notes runs once per note over that note's isolated
/// audio, so an edit means the same thing in both chains by construction rather
/// than by agreement, and the erase-and-overlay seam stays where it is -- aimed
/// at the note's own audio instead of at the mixture.
///
/// Unlike its siblings in this directory this reaches for the note edit chain, so
/// it is gated with the pitch editor rather than compiled into the core.

#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "editing/note_model/note_object.h"
#include "editing/note_model/note_renderer.h"
#include "editing/polyphony/note_mask.h"

namespace sonare::editing::polyphony {

/// @brief Renders @p notes over @p spec, each note through its own mask.
/// @details The output is the residual plus every note rendered over its own
///          masked resynthesis, at @p length samples and @p spec's sample rate.
///
///          Contract:
///          - `notes[i]` is the note of `masks.notes[i]`, so the two must be the
///            same length. A note's span and its edit are all that is read, so a
///            span disagreeing with its mask's frames cannot be detected here:
///            the mask decides what audio the note is, the span decides where the
///            edit applies to it.
///          - Overlapping spans are the normal case and are not checked. Each
///            note is rendered into its own isolated audio and the results are
///            summed, so there is no successor to push -- concatenating head,
///            stretched body and tail has no referent while several notes sound
///            over the same samples. Two notes moved onto each other add.
///          - Every note is validated, identity or not, before any inverse
///            transform runs, and so are the config fields
///            @ref note_model::validate_render_config covers. The only per-note
///            check that does not carry over from @ref note_model::render_notes is
///            disjointness. Those config fields are validated even with no note to
///            apply them to, for the same reason an identity note is: an
///            unrenderable argument is unrenderable whether or not this call would
///            reach it, and a `fade_ms` of NaN that succeeds on an empty set and
///            throws on a populated one is a wiring bug that passed quietly.
///          - `decomposition` is not in that set. It is validated where it is
///            read, which is a note carrying a vibrato or drift edit, so a call
///            with no such note never reaches it -- exactly as in the monophonic
///            chain, which is the agreement that matters more here than moving the
///            check would.
///          - A malformed mask is found later, where it is applied, so a set
///            unrenderable because of one of its masks pays the inverses of the
///            notes before it. Stated rather than fixed: the set-level shape is
///            checked up front and a mask reaching outside @p spec is not, which
///            costs a rejection work it did not need but cannot return a wrong
///            answer.
///          - With every edit identity the result is `spec.to_audio(length)` up
///            to the order of the additions. Not bit-for-bit the audio @p spec was
///            measured from: the round trip's own error is the STFT's and this
///            call neither adds to it nor removes it. The masks telescope exactly
///            because `to_audio`'s divisor, and the lanes where it declines to
///            divide, come from the geometry and the frame count alone and never
///            from the data -- so the inverses are one linear map applied to a
///            partition of @p spec.
///          - A muted note drops its span and keeps what its mask reaches outside
///            it, at most half a window either side. That material is the note's
///            share of bins its neighbours were sounding in too, so it is the
///            mask's leakage rather than the note, and dropping it would remove
///            energy no other note carries.
///          - One masked spectrogram is alive at a time. A mask's inverse is
///            full-length whatever the note's length, so holding all of them is
///            the input over again per note; the loop inverts one, folds it into
///            the accumulator, and releases it.
/// @param spec The STFT the masks index, measured from the audio being edited.
/// @param masks Masks over @p spec, in the order @ref build_note_masks returns.
/// @param notes One per mask, in the masks' order.
/// @param length Output length in samples, which is the source's own count. 0
///        takes the framing's natural length. Every inverse in the call is given
///        it, so the buffers align.
/// @param config Passed through to @ref note_model::render_notes unchanged.
/// @throws SonareException(InvalidParameter) on an empty @p spec, a @p masks that
///         does not describe it, a @p notes of a different length than
///         `masks.notes`, a negative @p length, anything
///         @ref note_model::render_notes rejects about a note or a config field,
///         or a @p spec and @p length whose inverse carries no samples at all --
///         which a single centred frame at `length` 0 does not, its whole
///         reconstruction being padding the trim removes. A call that can render
///         nothing is a framing error and not an empty result, and it is rejected
///         once against the residual rather than per note.
Audio render_masked_notes(const Spectrogram& spec, const NoteMaskSet& masks,
                          const std::vector<note_model::NoteObject>& notes, int length = 0,
                          const note_model::NoteRenderConfig& config = {});

}  // namespace sonare::editing::polyphony
