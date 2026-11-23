#include "editing/polyphony/masked_notes.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "editing/note_model/note_renderer.h"
#include "editing/pitch_editor/f0_provider.h"
#include "util/exception.h"
#include "util/parameter_checks.h"

namespace sonare::editing::polyphony {
namespace {

namespace check = sonare::util::check;

/// The ridge's frames as a track window; the note is built with the ridge's absolute frame
/// numbers, so the window carries only the frames it reads.
pitch_editor::F0Track ridge_window(const F0Ridge& ridge, const NoteMaskSet& masks) {
  pitch_editor::F0Track out;
  // Every frame of a ridge is voiced, so the span's statistics are measured over
  // all of it and voiced_prob is never consulted.
  out.f0_hz = ridge.f0_hz;
  out.voiced.assign(ridge.f0_hz.size(), true);

  // frame_rate_hz stays 0: the cadence is the framing's, derived from the pair.
  out.hop_length = masks.hop_length;
  out.sample_rate = masks.sample_rate;
  return out;
}

/// The measurement over whatever @p invert(i) reads note @p i's isolated audio from.
template <typename Invert>
std::vector<note_model::NoteObject> make_notes_over(const StftGeometry& geometry,
                                                    const MultiF0Track& track,
                                                    const NoteMaskSet& masks, int length,
                                                    const note_model::NoteExtractorConfig& config,
                                                    const std::string& geometry_subject,
                                                    const Invert& invert) {
  if (geometry.empty()) {
    check::refuse(geometry_subject.c_str(), "must not be empty");
  }
  // A set and a track agreeing with each other can still describe another framing.
  if (masks.n_bins != geometry.n_bins) {
    check::refuse_relation({"masks.n_bins", geometry_subject + ".n_bins"}, "{0} must equal {1}");
  }
  if (masks.n_frames != geometry.n_frames) {
    check::refuse_relation({"masks.n_frames", geometry_subject + ".n_frames"},
                           "{0} must equal {1}");
  }
  if (masks.hop_length != geometry.hop_length) {
    check::refuse_relation({"masks.hop_length", geometry_subject + ".hop_length"},
                           "{0} must equal {1}");
  }
  if (masks.sample_rate != geometry.sample_rate) {
    check::refuse_relation({"masks.sample_rate", geometry_subject + ".sample_rate"},
                           "{0} must equal {1}");
  }
  if (masks.n_frames != track.n_frames) {
    check::refuse_relation({"masks.n_frames", "track.n_frames"}, "{0} must equal {1}");
  }
  if (masks.hop_length != track.hop_length) {
    check::refuse_relation({"masks.hop_length", "track.hop_length"}, "{0} must equal {1}");
  }
  if (masks.sample_rate != track.sample_rate) {
    check::refuse_relation({"masks.sample_rate", "track.sample_rate"}, "{0} must equal {1}");
  }
  if (masks.notes.size() != track.ridges.size()) {
    check::refuse_relation({"masks.notes", "track.ridges"},
                           "{0} must have the same number of entries as {1}");
  }
  for (std::size_t i = 0; i < track.ridges.size(); ++i) {
    const F0Ridge& ridge = track.ridges[i];
    const NoteMask& mask = masks.notes[i];
    const std::string ridge_path = "track.ridges[" + std::to_string(i) + "]";
    const std::string mask_path = "masks.notes[" + std::to_string(i) + "]";
    // A mask spanning frames the pitch curve does not describe would measure the
    // amplitude of one note over another's frames.
    if (mask.frame_start != ridge.frame_start) {
      check::refuse_relation({mask_path + ".frame_start", ridge_path + ".frame_start"},
                             "{0} must equal {1}");
    }
    if (mask.n_frames != static_cast<int>(ridge.f0_hz.size())) {
      check::refuse_relation({mask_path + ".n_frames", ridge_path + ".f0_hz"},
                             "{0} must equal the length of {1}");
    }
    check::integer_at_least(ridge.frame_start, 0, (ridge_path + ".frame_start").c_str());
    if (ridge.frame_end() > track.n_frames) {
      check::refuse_relation({ridge_path + ".frame_start", ridge_path + ".f0_hz", "track.n_frames"},
                             "{0} plus the length of {1} must be at most {2}");
    }
    // A pitch that is not positive and finite returns a note with no usable curve.
    for (std::size_t j = 0; j < ridge.f0_hz.size(); ++j) {
      const float hz = ridge.f0_hz[j];
      if (!(hz > 0.0f && std::isfinite(hz))) {
        check::refuse_outside(check::real_domain_above(0.0, false),
                              (ridge_path + ".f0_hz[" + std::to_string(j) + "]").c_str());
      }
    }
  }
  // A non-positive reference_hz makes every median 0, the state a curve edit is rejected for.
  if (!(config.segmenter.reference_hz > 0.0f)) {
    check::refuse("config.segmenter.reference_hz", "must be greater than 0");
  }
  check::integer_at_least(length, 0, "length");

  // The length the full-length inverse would have.
  const int64_t whole = length > 0
                            ? length
                            : static_cast<int64_t>(geometry.n_frames - 1) * geometry.hop_length +
                                  geometry.n_fft - (geometry.center ? 2 * (geometry.n_fft / 2) : 0);

  std::vector<note_model::NoteObject> notes;
  notes.reserve(track.ridges.size());

  for (std::size_t i = 0; i < track.ridges.size(); ++i) {
    const F0Ridge& ridge = track.ridges[i];
    // A framing that reconstructs to nothing is a framing error, not a silent note.
    SONARE_CHECK(whole > 0, ErrorCode::InvalidParameter);
    // Only the samples the note reaches are inverted, so the loop stays linear
    // in the input rather than paying a full-length inverse per note.
    NoteSegment segment;
    try {
      segment = invert(i);
    } catch (const SonareParameterRefusal& e) {
      const std::string prefix = "mask.";
      if (e.subject().compare(0, prefix.size(), prefix) != 0) throw;
      const std::string parent = "masks.notes[" + std::to_string(i) + "].";
      throw e.respelled(
          e.code(),
          [&prefix, &parent](const std::string& subject) {
            return subject.compare(0, prefix.size(), prefix) == 0
                       ? parent + subject.substr(prefix.size())
                       : subject;
          },
          e.caller_spelled());
    }
    note_model::NoteObject note = note_model::make_note(
        segment.audio, segment.first_sample, whole, ridge_window(ridge, masks), ridge.frame_start,
        track.n_frames, ridge.frame_start, ridge.frame_end(), config);
    // A length ending before the ridge clamps the span empty, which make_note accepts.
    note_model::validate_note_for_render(note);
    notes.push_back(std::move(note));
  }

  return notes;
}

}  // namespace

std::vector<note_model::NoteObject> make_masked_notes(
    const Spectrogram& spec, const MultiF0Track& track, const NoteMaskSet& masks, int length,
    const note_model::NoteExtractorConfig& config) {
  return make_notes_over(
      geometry_of(spec), track, masks, length, config, "spec",
      [&](std::size_t i) { return invert_note_mask(spec, masks.notes[i], length); });
}

std::vector<note_model::NoteObject> make_masked_notes(
    const MaskedCells& cells, const MultiF0Track& track, const NoteMaskSet& masks, int length,
    const note_model::NoteExtractorConfig& config) {
  if (cells.notes.size() != masks.notes.size()) {
    check::refuse_relation({"cells.notes", "masks.notes"},
                           "{0} must have the same number of entries as {1}");
  }
  return make_notes_over(cells.geometry, track, masks, length, config, "cells.geometry",
                         [&](std::size_t i) { return invert_note_mask(cells, masks, i, length); });
}

}  // namespace sonare::editing::polyphony
