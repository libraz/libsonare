#include "editing/polyphony/masked_notes.h"

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "editing/note_model/note_renderer.h"
#include "editing/pitch_editor/f0_provider.h"
#include "util/exception.h"

namespace sonare::editing::polyphony {
namespace {

/// The track is full-length rather than cut to the ridge: make_note reads a
/// frame number as an absolute sample position, so a span-length track would
/// place every note at zero.
pitch_editor::F0Track ridge_track(const MultiF0Track& track, const F0Ridge& ridge,
                                  const NoteMaskSet& masks) {
  pitch_editor::F0Track out;
  const std::size_t n_frames = static_cast<std::size_t>(track.n_frames);
  out.f0_hz.assign(n_frames, 0.0f);
  out.voiced.assign(n_frames, false);

  // Every frame of a ridge is voiced, so the span's statistics are measured over
  // all of it and voiced_prob is never consulted.
  const std::size_t start = static_cast<std::size_t>(ridge.frame_start);
  for (std::size_t i = 0; i < ridge.f0_hz.size(); ++i) {
    out.f0_hz[start + i] = ridge.f0_hz[i];
    out.voiced[start + i] = true;
  }

  // frame_rate_hz stays 0: the cadence is the framing's, derived from the pair.
  out.hop_length = masks.hop_length;
  out.sample_rate = masks.sample_rate;
  return out;
}

}  // namespace

std::vector<note_model::NoteObject> make_masked_notes(
    const Spectrogram& spec, const MultiF0Track& track, const NoteMaskSet& masks, int length,
    const note_model::NoteExtractorConfig& config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  // A set and a track agreeing with each other can still describe another framing.
  SONARE_CHECK(masks.n_bins == spec.n_bins() && masks.n_frames == spec.n_frames() &&
                   masks.hop_length == spec.hop_length() && masks.sample_rate == spec.sample_rate(),
               ErrorCode::InvalidParameter);
  SONARE_CHECK(masks.n_frames == track.n_frames && masks.hop_length == track.hop_length &&
                   masks.sample_rate == track.sample_rate,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(masks.notes.size() == track.ridges.size(), ErrorCode::InvalidParameter);
  for (std::size_t i = 0; i < track.ridges.size(); ++i) {
    const F0Ridge& ridge = track.ridges[i];
    const NoteMask& mask = masks.notes[i];
    // A mask spanning frames the pitch curve does not describe would measure the
    // amplitude of one note over another's frames.
    SONARE_CHECK(mask.frame_start == ridge.frame_start &&
                     mask.n_frames == static_cast<int>(ridge.f0_hz.size()),
                 ErrorCode::InvalidParameter);
    SONARE_CHECK(ridge.frame_start >= 0 && ridge.frame_end() <= track.n_frames,
                 ErrorCode::InvalidParameter);
    // A pitch that is not positive and finite returns a note with no usable curve.
    for (const float hz : ridge.f0_hz) {
      SONARE_CHECK(hz > 0.0f && std::isfinite(hz), ErrorCode::InvalidParameter);
    }
  }
  // A non-positive reference_hz makes every median 0, the state a curve edit is rejected for.
  SONARE_CHECK(config.segmenter.reference_hz > 0.0f, ErrorCode::InvalidParameter);
  SONARE_CHECK(length >= 0, ErrorCode::InvalidParameter);

  std::vector<note_model::NoteObject> notes;
  notes.reserve(track.ridges.size());

  for (std::size_t i = 0; i < track.ridges.size(); ++i) {
    const F0Ridge& ridge = track.ridges[i];
    // The masked spectrogram is a temporary, so it dies with the statement that
    // inverts it and one note's full-length inverse is alive at a time.
    const Audio note_audio = apply_note_mask(spec, masks.notes[i]).to_audio(length);
    // A framing that reconstructs to nothing is a framing error, not a silent note.
    SONARE_CHECK(!note_audio.empty(), ErrorCode::InvalidParameter);
    note_model::NoteObject note = note_model::make_note(
        note_audio, ridge_track(track, ridge, masks), ridge.frame_start, ridge.frame_end(), config);
    // A length ending before the ridge clamps the span empty, which make_note accepts.
    note_model::validate_note_for_render(note);
    notes.push_back(std::move(note));
  }

  return notes;
}

}  // namespace sonare::editing::polyphony
