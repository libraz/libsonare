#pragma once

/// @file pitch_decomposition.h
/// @brief Splitting a note's pitch curve into centre, drift and vibrato.
///
/// A performed note's pitch is one curve carrying three things at once: the
/// note that was aimed at, a slow wander around it, and a periodic oscillation
/// on top. Editing any of them on its own needs them separated first, and the
/// only thing that decides where drift ends and vibrato begins is a cutoff.
///
/// The split reconstructs exactly, so nothing is lost between a host drawing
/// the curve and the renderer editing it.

#include <vector>

#include "editing/note_model/note_object.h"

namespace sonare::editing::note_model {

/// @brief Where drift ends and vibrato begins.
struct PitchDecompositionConfig {
  /// Boundary between the two curves, in Hz. Performed vibrato sits around
  /// 4-7 Hz and intonation drift below 2 Hz, so the default falls between them.
  float vibrato_cutoff_hz = 3.0f;
};

/// @brief One note's pitch curve, split in three.
struct PitchDecomposition {
  /// The note's steady pitch in Hz -- @ref NoteObject::median_hz. 0 when the
  /// note carries no usable pitch, and then both curves are empty.
  float centre_hz = 0.0f;
  /// Slow deviation from @ref centre_hz in cents, one entry per note frame.
  std::vector<float> drift;
  /// Fast deviation in cents, over the same frames.
  std::vector<float> vibrato;

  /// Cadence and source-track offset of both curves, copied from the note.
  float frame_rate_hz = 0.0f;
  int frame_offset = 0;

  /// The config this was produced with. Hand it to
  /// @ref NoteRenderConfig::decomposition so the renderer cuts the curve where
  /// the host drew it; a different cutoff there edits a vibrato nobody saw.
  PitchDecompositionConfig config{};
};

/// @brief Splits @p note's F0 curve into a centre, a drift and a vibrato.
/// @details @c drift[i] + @c vibrato[i] is the note's own pitch at frame i, in
///          cents above @c centre_hz, to within float rounding. Drift is a
///          zero-phase low pass, so neither curve is shifted in time against
///          the audio it was measured from.
///
///          Frames whose F0 is unusable (0 or non-finite, which is how a track
///          spells unvoiced) carry no measurement, so the curve is held at the
///          nearest usable neighbour across them. Both curves therefore have an
///          entry everywhere; a host marking the held ones reads them off the
///          note's own @c f0_hz, which is exact and so is not mirrored here.
///
///          A note carrying no usable pitch returns a zero centre and two empty
///          curves rather than throwing -- that is a measurement which came up
///          empty, not a bad argument.
/// @throws SonareException(InvalidParameter) on a non-finite or non-positive
///         cutoff, or on a note that has a pitch to decompose but a non-finite
///         or non-positive frame rate. A note with nothing to decompose returns
///         the empty result above without reaching that check -- its cadence
///         cannot matter.
PitchDecomposition decompose_pitch(const NoteObject& note,
                                   const PitchDecompositionConfig& config = {});

}  // namespace sonare::editing::note_model
