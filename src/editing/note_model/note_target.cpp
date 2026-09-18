#include "editing/note_model/note_target.h"

#include <algorithm>
#include <cmath>

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

/// A bad ratio reads as the strictest one rather than the loosest: an
/// under-matching run reports it in the assignment count, while a config that
/// silently matched everything would look like a successful one.
float resolve_min_overlap_ratio(float ratio) {
  if (!std::isfinite(ratio)) return 1.0f;
  return std::clamp(ratio, 0.0f, 1.0f);
}

/// Non-finite or negative saturates every correction to zero, which leaves the
/// take as recorded instead of shifting it by a value nothing bounded.
float resolve_max_correction(float semitones) {
  if (!std::isfinite(semitones) || semitones < 0.0f) return 0.0f;
  return semitones;
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

size_t assign_note_targets(std::vector<NoteObject>& notes, int sample_rate,
                           const std::vector<NoteTarget>& targets,
                           const NoteTargetAssignConfig& config) {
  if (sample_rate <= 0) return 0;
  const double rate = static_cast<double>(sample_rate);
  const float min_ratio = resolve_min_overlap_ratio(config.min_overlap_ratio);
  const float max_correction = resolve_max_correction(config.max_correction_semitones);

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
