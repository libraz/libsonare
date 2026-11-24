#include "editing/note_model/note_target.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "core/convert.h"

namespace sonare::editing::note_model {

namespace {

/// Seconds of overlap between two intervals; negative when they are disjoint.
double overlap_seconds(double a_start, double a_end, double b_start, double b_end) {
  return std::min(a_end, b_end) - std::max(a_start, b_start);
}

/// Seconds between two disjoint intervals, 0 when they touch or overlap.
double gap_seconds(double a_start, double a_end, double b_start, double b_end) {
  return std::max(0.0, std::max(b_start - a_end, a_start - b_end));
}

bool has_measured_pitch(const NoteObject& note) {
  return std::isfinite(note.median_hz) && note.median_hz > 0.0f;
}

void apply_target(NoteObject& note, float target_midi, float max_correction) {
  const float current_midi = hz_to_midi(note.median_hz);
  const float shift = target_midi - current_midi;
  note.edit.pitch_shift_semitones = std::clamp(shift, -max_correction, max_correction);
}

}  // namespace

namespace detail {

void validate_note_target_assign_config(const NoteTargetAssignConfig& config) {
  util::check::within(config.min_overlap_ratio, kNoteTargetOverlapRatioDomain,
                      "config.min_overlap_ratio");
  util::check::within(config.max_correction_semitones, kNoteTargetMaxCorrectionDomain,
                      "config.max_correction_semitones");
}

void validate_note_target_note_spans(const std::vector<NoteObject>& notes) {
  for (std::size_t i = 0; i < notes.size(); ++i) {
    const NoteObject& note = notes[i];
    const std::string onset_subject = util::check::indexed_subject("notes", i, "onset_sample");
    const std::string offset_subject = util::check::indexed_subject("notes", i, "offset_sample");
    util::check::integer_within(note.onset_sample, kNoteTargetSamplePositionDomain,
                                onset_subject.c_str());
    util::check::integer_within(note.offset_sample, kNoteTargetSamplePositionDomain,
                                offset_subject.c_str());
    if (note.offset_sample <= note.onset_sample) {
      util::check::refuse_relation({offset_subject, onset_subject}, "{0} must be greater than {1}");
    }
  }
}

}  // namespace detail

size_t assign_note_targets(std::vector<NoteObject>& notes, int sample_rate,
                           const std::vector<NoteTarget>& targets,
                           const NoteTargetAssignConfig& config) {
  if (sample_rate <= 0) return 0;
  detail::validate_note_target_assign_config(config);
  detail::validate_note_target_note_spans(notes);
  const double rate = static_cast<double>(sample_rate);
  const float min_ratio = config.min_overlap_ratio;
  const float max_correction = config.max_correction_semitones;

  size_t assigned = 0;
  for (NoteObject& note : notes) {
    // A note with no measured pitch has nothing to correct from, so no policy
    // reaches it -- see the contract in the header.
    if (!has_measured_pitch(note)) continue;

    const double note_start = static_cast<double>(note.onset_sample) / rate;
    const double note_end = static_cast<double>(note.offset_sample) / rate;
    const double span = note_end - note_start;

    const NoteTarget* best = nullptr;
    double best_overlap = 0.0;
    if (span > 0.0) {
      const double required = static_cast<double>(min_ratio) * span;
      for (const NoteTarget& target : targets) {
        const double overlap =
            overlap_seconds(note_start, note_end, target.start_sec, target.end_sec);
        if (overlap <= 0.0 || overlap < required) continue;
        // Longest overlap wins; an exact tie goes to the earlier target, so the
        // answer does not depend on the order the targets arrived in.
        if (best == nullptr || overlap > best_overlap ||
            (overlap == best_overlap && target.start_sec < best->start_sec)) {
          best = &target;
          best_overlap = overlap;
        }
      }
    }

    if (best != nullptr) {
      apply_target(note, best->target_midi, max_correction);
      ++assigned;
      continue;
    }

    switch (config.unmatched_policy) {
      case UnmatchedTargetPolicy::Leave:
        break;
      case UnmatchedTargetPolicy::Mute:
        note.edit.muted = true;
        break;
      case UnmatchedTargetPolicy::Nearest: {
        const NoteTarget* nearest = nullptr;
        double nearest_gap = 0.0;
        for (const NoteTarget& target : targets) {
          const double gap = gap_seconds(note_start, note_end, target.start_sec, target.end_sec);
          if (nearest == nullptr || gap < nearest_gap ||
              (gap == nearest_gap && target.start_sec < nearest->start_sec)) {
            nearest = &target;
            nearest_gap = gap;
          }
        }
        if (nearest != nullptr) {
          apply_target(note, nearest->target_midi, max_correction);
          ++assigned;
        }
        break;
      }
    }
  }
  return assigned;
}

}  // namespace sonare::editing::note_model
