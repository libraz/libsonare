#pragma once

/// @file masked_notes.h
/// @brief Turns tracked F0 ridges and their masks into editable note objects.
///
/// A ridge is a pitch followed across frames; a note object is that plus the
/// measured fields an edit is defined against -- a per-frame RMS curve, a median
/// pitch and a steadiness figure. Only the pitch carries over directly. The rest
/// has to be measured, and measuring it over the mixture would report every voice
/// sounding in the note's frames rather than the note.
///
/// So each note is measured over its own masked resynthesis, and measured by
/// @ref note_model::make_note -- the same function the monophonic extractor
/// derives its notes with. The polyphonic chain therefore spells `amplitude`,
/// `median_hz`, `median_cents` and `f0_stability` exactly as the monophonic one
/// does, by calling it rather than by agreeing with it. This is the companion of
/// @ref render_masked_notes and is gated with the pitch editor for the same
/// reason: it reaches outside the core's polyphony sources.

#include <vector>

#include "core/spectrum.h"
#include "editing/note_model/note_extractor.h"
#include "editing/note_model/note_object.h"
#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/note_mask.h"

namespace sonare::editing::polyphony {

/// @brief Builds one editable note per ridge of @p track.
/// @details Returns one note per ridge, in the track's order, so `notes[i]` is
///          the note of `masks.notes[i]` and the result feeds
///          @ref render_masked_notes without being reordered. Every returned
///          note has an identity edit.
///
///          Contract:
///          - The measured fields come from @ref note_model::make_note over that
///            note's isolated audio, with the ridge's own frames as the span. The
///            F0 curve is the ridge's values unchanged; `amplitude` is one RMS per
///            frame of the isolated note; `median_hz`, `median_cents` and
///            `f0_stability` are derived from the curve the same way an extracted
///            note's are.
///          - Every frame of a ridge is voiced by construction, so the statistics
///            are measured over the whole span. A voiced fraction would measure
///            nothing here for the same reason it measures nothing for a segmented
///            note.
///          - A returned note always carries a usable pitch curve, so a vibrato or
///            drift edit applies to it. That is the one edit
///            @ref note_model::validate_note_for_render rejects a note for being
///            unable to take, and a converted note cannot be in that state.
///
///            Holding that clause is what puts a positive `reference_hz` and
///            positive finite ridge pitches in the rejection list below. Neither is
///            checked further down: a `reference_hz` of 0 makes every frame's cents
///            0 and the note's `median_hz` 0, which is how a track spells no pitch,
///            and a ridge carrying a 0 or a NaN does the same. Both would return a
///            note that silently cannot take the edit this clause promises, which is
///            worse than a rejection.
///          - The spans are derived against @p length, not copied from the ridge.
///            A ridge is handed no length and one reaching the last frame ends
///            past the audio -- centre padding makes that the normal case -- while
///            @ref note_model::make_note clamps. So a returned note is always
///            sliceable over the audio it will be rendered against, and the same
///            @p length must be passed to @ref render_masked_notes or the two
///            disagree about where the note sits.
///          - `F0Ridge::median_hz` is not copied either. A ridge's is the median of
///            its Hz values; a note's is the median of the span's cents converted
///            back, which is the monophonic definition and is not the same number.
///            Taking the measured one everywhere is what makes the two chains
///            comparable, so the ridge's is read for nothing but the pitch values
///            it was computed from.
///          - One masked spectrogram is alive at a time, for the reason
///            @ref render_masked_notes holds to: a mask's inverse is full-length
///            whatever the note's length.
///          - A convert-then-render round inverts each mask twice, once here to
///            measure and once there to render. Accepted rather than folded:
///            conversion runs once per analysis and rendering once per edit round,
///            so the two are not in the same loop, and returning audio from here
///            to avoid it would hold every note's full-length inverse at once.
/// @param spec The STFT the masks index.
/// @param track The extraction the masks were built from, read for the per-ridge
///        F0 values and the framing.
/// @param masks Masks over @p spec, one per ridge, in @p track's ridge order.
/// @param length Source length in samples. 0 takes the framing's natural length.
/// @param config Only @c segmenter.reference_hz and
///        @c segmenter.segmentation_threshold_cents are read -- the segmenter
///        itself is not consulted, the spans being the ridges'. The remaining
///        fields are rejected when they are not finite, which is all
///        @ref note_model::make_note checks about them.
/// @throws SonareException(InvalidParameter) on an empty @p spec; a @p masks that
///         does not describe @p spec, or whose frame count, hop or sample rate
///         disagrees with @p track's -- both, because a set and a track agreeing
///         with each other while disagreeing with @p spec would otherwise pass, and
///         a hop that is not the spectrogram's puts every span somewhere else at the
///         cadence the track was believed to have. @ref render_masked_notes gets
///         this check from the residual it has to build; nothing here takes the set
///         as a whole, so it is made explicitly or not at all, and a set this
///         accepted but that call rejects would break the pair; a @p masks
///         with a different number of masks than @p track has ridges; a mask whose
///         `frame_start` or `n_frames` disagrees with its ridge's frames, which
///         would measure the amplitude over frames the pitch curve does not
///         describe; a ridge whose frames fall outside `track.n_frames`, which is
///         not implied by the two agreements above and is a write past the end of
///         the track this builds rather than an odd input; a ridge carrying a pitch
///         that is not positive and finite; a @c segmenter.reference_hz that is not
///         positive -- checked whatever @p track holds, including no ridge at all,
///         because it costs no inverse to check and a value that succeeds on an
///         empty track and throws on a populated one is a wiring bug that passed
///         quietly; a negative @p length; and anything
///         @ref note_model::make_note rejects about the framing or the config.
///
///         Each built note is then put through
///         @ref note_model::validate_note_for_render before it is returned, so the
///         clause above about a returned note being renderable is enforced rather
///         than reasoned. The case that needs it is a @p length ending before a
///         ridge's own span: both ends of the span clamp to it, leaving an empty
///         one that @ref note_model::make_note does not reject, since the bound it
///         checks is on frames. A note measured over nothing is a framing error
///         like an inverse carrying no samples, not a note to hand back.
///
///         A degenerate @p spec and @p length whose inverse carries no samples is
///         rejected per mask, where the inverse is taken, so a @p track with no
///         ridge returns an empty vector instead. That is deliberately unlike
///         @ref render_masked_notes, which has a residual to invert whatever the
///         notes are and so can reject the pair up front: here there is nothing to
///         invert, deciding in advance would cost a whole inverse of @p spec to
///         learn it, and no notes is the honest answer to no ridges.
std::vector<note_model::NoteObject> make_masked_notes(
    const Spectrogram& spec, const MultiF0Track& track, const NoteMaskSet& masks, int length = 0,
    const note_model::NoteExtractorConfig& config = {});

}  // namespace sonare::editing::polyphony
