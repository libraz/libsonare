#pragma once

/// @file polyphonic_edit.h
/// @brief One call from audio to editable notes, and one back to audio.
///
/// The parts below this file each take the pieces they need and check that those
/// pieces describe each other. Assembling them by hand still leaves two things a
/// caller can get wrong and one it pays twice for: the STFT is computed by the
/// extraction and again for the masks, the track carries no spectrogram so the
/// framing has to be paired up by the caller, and the output length the note spans
/// were derived against has to be handed to the renderer unchanged -- which
/// @ref make_masked_notes can state and cannot enforce, because nothing it returns
/// records it.
///
/// So this carries the framing, the masks and the length alongside the notes. The
/// notes are the one part a host writes; everything else is what they were
/// measured against, and is validated again on the way out rather than trusted.

#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "editing/note_model/note_extractor.h"
#include "editing/note_model/note_object.h"
#include "editing/note_model/note_renderer.h"
#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/note_mask.h"
#include "editing/polyphony/shared_bins.h"

namespace sonare::editing::polyphony {

struct PolyphonicEditConfig {
  /// Carries the STFT geometry the whole chain runs in.
  MultiF0ExtractorConfig extraction{};
  NoteMaskConfig masks{};
  /// The refusal thresholds the apportionment runs under. Its defaults are the
  /// measured ones and lowering a threshold buys bins at the price of solving
  /// ones a fit cannot do, which @ref solve_shared_bins states costs more than it
  /// gains.
  SharedBinConfig shared_bins{};
  /// @ref make_masked_notes reads two of the segmenter's fields and checks the
  /// rest for finiteness, so an unrelated field that is not finite is still
  /// refused. Read by two is not the same as harmless to set.
  note_model::NoteExtractorConfig notes{};
};

/// @brief One analysis, plus the notes a host edits on it.
/// @details Everything but @ref notes is what the notes were measured against.
///          Holding it means holding the source's spectrum and every mask, which
///          is the input over again plus the claims -- the price of re-rendering
///          an edit without analysing the audio a second time.
struct PolyphonicAnalysis {
  /// The one STFT the extraction, the masks, the measurement and the render all
  /// run over.
  Spectrogram spectrum;
  MultiF0Track track;
  /// Apportioned, not equally split: a weight is the partial's fitted
  /// contribution over the observed bin, so it is complex and may exceed one
  /// where two partials partly cancel. Where a bin was refused, the equal share
  /// @ref build_note_masks gave it stands.
  NoteMaskSet masks;
  /// One per mask, in the masks' order. The only member a host writes, and only
  /// each note's @c edit: the spans and curves are measurements, and the order is
  /// the pairing with @ref masks.
  std::vector<note_model::NoteObject> notes;
  /// The source's own sample count, which every inverse in the chain is given.
  ///
  /// An analysis @ref analyze_polyphonic returns never carries 0: it refuses
  /// empty audio, so the count is at least one. A 0 reaching
  /// @ref render_polyphonic therefore means a hand-built or half-filled analysis,
  /// and it renders at the framing's natural length rather than being refused --
  /// because that is what the call it forwards to documents 0 to mean, and a
  /// wrapper stricter than the function it wraps is as wrong as a looser one.
  /// Stated rather than fixed: the member's own meaning is the source's count, and
  /// the default is 0 only because the struct is an aggregate.
  int length = 0;
};

/// @brief Finds the notes in @p audio and measures each over its own separation.
/// @details Runs the chain once: one STFT, the multi-F0 extraction over it, a mask
///          per tracked ridge, @ref solve_shared_bins to apportion the bins two
///          notes stand on, and @ref make_masked_notes for the measured fields.
///          Every returned note has an identity edit, so rendering the result
///          unchanged reproduces the analysis's own round trip.
///
///          The apportionment is in the chain rather than offered beside it,
///          because an equal split is not a neutral default: a bin is claimed
///          from a note's f0 and harmonic number, never from the spectrum, so a
///          note claims bins its own partials never reached, and an equal share
///          there takes half of whatever else is standing on them. What the fit
///          cannot do it refuses, and a refused bin keeps the equal share, so the
///          stage only ever replaces a split with something measured.
///
///          One consequence is not a refinement and is worth stating plainly.
///          A weight is the fitted component over the observation, so where two
///          partials nearly cancel it exceeds one, and @ref mask_total reaches
///          @ref SharedBinConfig::max_weight_modulus there. @ref residual_spectrum
///          is @c 1 - total unclamped, so at such a bin it carries several times
///          the input. **While every edit is identity this is inaudible** -- the
///          notes and the residual still sum to the input, which is the whole of
///          @ref render_polyphonic's round-trip claim. **The moment one note
///          moves, what is left there no longer cancels.** Lowering that ceiling
///          trades separation for a quieter residual.
///
///          A result with no notes is not an error. Silence, or material the
///          register of the framing cannot resolve, tracks no ridge; rendering
///          that is the residual alone, which is the whole round trip.
/// @param audio Source. Its length becomes @ref PolyphonicAnalysis::length, so the
///        spans and the render agree by construction rather than by the caller
///        passing one number twice.
/// @param config The three stages' configs. @c extraction.stft is the framing, and
///        is not restated anywhere else.
/// @throws SonareException(InvalidParameter) on empty @p audio, on an @p audio
///         longer than @ref PolyphonicAnalysis::length can hold -- the length is
///         an @c int because every stage below takes one, so a source past that
///         has to be refused here rather than wrapped into a shorter render --
///         plus every reason @ref extract_multi_f0, @ref build_note_masks,
///         @ref solve_shared_bins and @ref make_masked_notes throw about their
///         inputs or their configs.
PolyphonicAnalysis analyze_polyphonic(const Audio& audio, const PolyphonicEditConfig& config = {});

/// @brief Renders @p analysis back to audio, with whatever edits its notes carry.
/// @details @ref render_masked_notes over the carried framing, masks, notes and
///          length. With every edit identity the result is
///          `analysis.spectrum.to_audio(analysis.length)` up to the order of the
///          additions -- not the source bit for bit, the STFT round trip's own
///          error being neither added to nor removed here.
///
///          @p analysis is checked where the call below checks it, and the reach
///          of that is worth stating rather than summarising as "validated". The
///          masks are checked against the spectrum and the note count against the
///          mask count, so a dropped note or a mutated set shape is refused, and
///          each note is checked the way the monophonic chain checks one, so an
///          empty or reversed span is refused.
///
///          Two things are not checked, and both are stated rather than fixed. A
///          note's span moved elsewhere is rendered and not refused: the mask
///          decides what audio the note is and the span decides where the edit
///          applies to it, so the two are allowed to disagree, which
///          @ref render_masked_notes states for its own arguments and nothing
///          here narrows. And @ref PolyphonicAnalysis::track is not read on the
///          way out at all -- the render needs only the masks. It is carried
///          because a host deciding what to edit wants the per-ridge salience and
///          the per-frame polyphony, and re-deriving those means analysing the
///          audio again; so editing it changes nothing and is not reported.
///
///          One consequence is worth naming, because it is where the two halves
///          of this file part. A mask whose frames are no longer its ridge's is
///          refused by @ref make_masked_notes and rendered by this call: pairing a
///          mask with a ridge is a rule of the measurement, which reads both, and
///          not of the render, which reads only the masks. The same mutation is a
///          defect going in and a legitimate input coming out.
/// @throws SonareException(InvalidParameter) on an @p analysis whose @c spectrum
///         is empty, which a default-constructed one is; whose masks no longer
///         describe that spectrum; whose note count is no longer its mask count;
///         whose @c length is negative; whose @c spectrum and @c length have an
///         inverse carrying no samples at all; or that carries a note or a @p
///         config field @ref note_model::render_notes rejects.
Audio render_polyphonic(const PolyphonicAnalysis& analysis,
                        const note_model::NoteRenderConfig& config = {});

}  // namespace sonare::editing::polyphony
