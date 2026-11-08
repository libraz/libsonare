#pragma once

/// @file note_state_decoder.h
/// @brief Monophonic note segmentation by Viterbi decoding of a joint note-state model.
///
/// Each pitch bin of a third of a semitone carries an Attack and a Stable state, and one
/// Silent state is shared by all bins. A frame's F0, its RMS level and nearby spectral-flux
/// onset peaks are the observations; the best path is cut into notes where it enters an
/// Attack state. Emission costs scale with the frame period and leaving a state costs the
/// same at any period, so a take segments alike at every sample rate.
///
/// Scores are unnormalised log costs accumulated in double. Ties are broken by fixed
/// candidate order and toward the lower bin, so the result is deterministic.

#include <vector>

#include "core/audio.h"
#include "editing/pitch_editor/f0_provider.h"

namespace sonare::editing::note_model {

/// Observations of one take, one entry per track frame unless stated otherwise.
struct NoteDecoderInput {
  /// F0 and voicing per frame; `voiced_prob` is not read.
  const pitch_editor::F0Track* track = nullptr;
  /// Linear RMS per track frame; a non-finite value reads as 0.
  std::vector<float> frame_rms;
  /// Onset peak frames at the monophonic onset evidence rate (200 fps).
  std::vector<int> onset_frames;
  /// False when no onset evidence could be computed; every frame then has no cue.
  bool onsets_available = false;
};

/// One decoded note as a half-open range `[frame_start, frame_end)` of track frames.
struct DecodedSpan {
  int frame_start;
  int frame_end;
};

/// @brief Decodes the note spans of a monophonic take.
/// @param input Track, frame RMS and onset frames of the take
/// @param fmin_hz Lowest pitch of the bin range (Hz, > 0)
/// @param fmax_hz Highest pitch of the bin range (Hz, > fmin_hz)
/// @param reference_hz Frequency of 0 cents, which bin centres are laid out from
/// @param segmentation_threshold_cents Scale of the Stable pitch width (1.8 times this value)
/// @param min_note_ms Shortest note kept, raised to two frames when below that
/// @return Spans in ascending order; empty for an empty track.
/// @details A note starts where the path enters an Attack state and ends at the next note
///          start or the first Silent frame. A note that re-enters the bin it leaves is
///          joined to its predecessor when either is shorter than the minimum; any other
///          note shorter than the minimum is dropped.
/// @throw sonare::SonareException (InvalidParameter) on a null track, per-frame arrays of
///        unequal length, a track with no frame cadence, or an invalid pitch range.
std::vector<DecodedSpan> decode_note_states(const NoteDecoderInput& input, float fmin_hz,
                                            float fmax_hz, float reference_hz,
                                            float segmentation_threshold_cents, float min_note_ms);

/// @brief RMS of each track frame of @p audio.
/// @details Frame `i` covers samples `[floor(i * spf), floor((i + 1) * spf))` with
///          `spf = track.samples_per_frame()`, cut at the end of the audio. An empty range
///          or a non-finite result gives 0.
std::vector<float> frame_rms_for_track(const Audio& audio, const pitch_editor::F0Track& track);

}  // namespace sonare::editing::note_model
