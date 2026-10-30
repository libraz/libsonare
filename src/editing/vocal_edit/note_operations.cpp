// SONARE_WASM_EXCEPTION_UNWIND: release staged note vectors when an edit operation throws.
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "editing/vocal_edit/analysis.h"
#include "editing/vocal_edit/pitch_plan.h"
#include "editing/vocal_edit/session.h"
#include "editing/vocal_edit/session_internal.h"
#include "util/insertion_sort.h"

namespace sonare::editing::vocal_edit {
using namespace detail;

namespace {

VocalNoteId issue_id(uint64_t& next_id) {
  if (next_id == 0 || next_id > static_cast<uint64_t>(std::numeric_limits<VocalNoteId>::max())) {
    throw VocalEditException(VocalReason::kCounterExhausted, "note ID counter exhausted",
                             "note_id");
  }
  const VocalNoteId issued = static_cast<VocalNoteId>(next_id);
  ++next_id;
  return issued;
}

void split_target(const VocalPitchTarget& target, double cut, VocalPitchTarget& left,
                  VocalPitchTarget& right) {
  left = target;
  right = target;
  if (target.mode != PitchTargetMode::kCurve) return;
  left.points.clear();
  right.points.clear();
  const auto value_at = [&](double sample) { return pitch_target_at(target, sample, 0.0); };
  for (const auto& point : target.points) {
    if (point.source_sample <= cut) left.points.push_back(point);
    if (point.source_sample >= cut) right.points.push_back(point);
  }
  if (left.points.empty() || left.points.back().source_sample != cut) {
    left.points.push_back({cut, value_at(cut)});
  }
  if (right.points.empty() || right.points.front().source_sample != cut) {
    right.points.insert(right.points.begin(), {cut, value_at(cut)});
  }
}

VocalPitchTarget respan_target(const VocalPitchTarget& target, SampleRange range) {
  if (target.mode != PitchTargetMode::kCurve) return target;
  VocalPitchTarget result = target;
  result.points.clear();
  const double start = static_cast<double>(range.start);
  const double end = static_cast<double>(range.end);
  const auto value_at = [&](double sample) { return pitch_target_at(target, sample, 0.0); };
  result.points.push_back({start, value_at(start)});
  for (const auto& point : target.points) {
    if (point.source_sample > start && point.source_sample < end) result.points.push_back(point);
  }
  result.points.push_back({end, value_at(end)});
  return result;
}

std::vector<float> split_envelope(const std::vector<float>& envelope, double fraction, bool left) {
  if (envelope.empty()) return {};
  const double cut = std::clamp(fraction, 0.0, 1.0);
  const auto value_at = [&](double position_value) {
    const double normalized = std::clamp(position_value, 0.0, 1.0);
    const double bounded = normalized * static_cast<double>(envelope.size() - 1);
    const size_t index = static_cast<size_t>(std::floor(bounded));
    if (index + 1 >= envelope.size()) return envelope.back();
    const double t = bounded - static_cast<double>(index);
    return static_cast<float>(envelope[index] + t * (envelope[index + 1] - envelope[index]));
  };
  std::vector<float> result(envelope.size());
  for (size_t index = 0; index < result.size(); ++index) {
    const double local = result.size() > 1
                             ? static_cast<double>(index) / static_cast<double>(result.size() - 1)
                             : 0.0;
    result[index] = value_at(left ? cut * local : cut + (1.0 - cut) * local);
  }
  return result;
}

uint32_t frame_for_sample_start(const AnalysisGrid& grid, uint32_t frame_count, int64_t sample) {
  const double raw =
      (static_cast<double>(sample) - grid.frame_origin_sample) / grid.samples_per_frame;
  const double frame = std::ceil(raw - 1.0e-12);
  if (!std::isfinite(frame) || frame < 0.0 || frame >= static_cast<double>(frame_count)) {
    invalid("source_range", "source span contains no analysis frame");
  }
  return static_cast<uint32_t>(frame);
}

uint32_t frame_for_sample_end(const AnalysisGrid& grid, uint32_t frame_count, int64_t sample) {
  const double raw =
      (static_cast<double>(sample) - grid.frame_origin_sample) / grid.samples_per_frame;
  const double frame = std::ceil(raw - 1.0e-12);
  if (!std::isfinite(frame) || frame <= 0.0 || frame > static_cast<double>(frame_count)) {
    invalid("source_range", "source span contains no analysis frame");
  }
  return static_cast<uint32_t>(frame);
}

void rewire_split_transitions(VocalEditState& state, VocalNoteId old_id, const VocalNote& left_note,
                              const VocalNote& right_note) {
  for (auto& transition : state.transitions) {
    if (transition.left_note_id == old_id) {
      transition.left_note_id = right_note.id;
    }
    if (transition.right_note_id == old_id) {
      transition.right_note_id = left_note.id;
    }
  }
  state.transitions.erase(std::remove_if(state.transitions.begin(), state.transitions.end(),
                                         [&](const PitchTransition& transition) {
                                           return transition.left_note_id ==
                                                  transition.right_note_id;
                                         }),
                          state.transitions.end());
}

bool constant_envelope(const std::vector<float>& envelope) {
  return envelope.empty() || envelope.size() == 1 ||
         std::all_of(envelope.begin() + 1, envelope.end(),
                     [&](float value) { return value == envelope.front(); });
}

bool same_constant_envelope(const std::vector<float>& lhs, const std::vector<float>& rhs) {
  if (!constant_envelope(lhs) || !constant_envelope(rhs)) return false;
  const float lhs_value = lhs.empty() ? 1.0f : lhs.front();
  const float rhs_value = rhs.empty() ? 1.0f : rhs.front();
  return lhs_value == rhs_value;
}

VocalPitchEdit canonical_merge_pitch(const std::vector<VocalNote>& selected,
                                     const VocalEditState& state, const VocalNote& merged,
                                     const VocalAnalysisData& analysis,
                                     const RenderSettings& render_settings,
                                     uint32_t source_sample_rate) {
  std::set<VocalNoteId> selected_ids;
  for (const auto& note : selected) selected_ids.insert(note.id);

  VocalEditState selected_state;
  selected_state.notes = selected;
  for (const auto& transition : state.transitions) {
    if (selected_ids.count(transition.left_note_id) != 0 &&
        selected_ids.count(transition.right_note_id) != 0) {
      selected_state.transitions.push_back(transition);
    }
  }
  const auto plans =
      compile_pitch_plans(analysis, selected_state, render_settings, source_sample_rate);

  struct EffectivePoint {
    double source_sample = 0.0;
    double effective_midi = 0.0;
  };
  std::vector<EffectivePoint> effective;
  for (const auto& plan : plans) {
    for (const auto& point : plan.points) {
      if (!point.voiced) continue;
      if (!std::isfinite(point.effective_midi) || point.effective_midi < 0.0 ||
          point.effective_midi > 127.0) {
        invalid("merge.edits", "effective pitch cannot be represented by a MIDI curve");
      }
      effective.push_back({point.source_sample, point.effective_midi});
    }
  }
  if (effective.empty()) {
    return VocalPitchEdit{};
  }
  std::sort(effective.begin(), effective.end(),
            [](const EffectivePoint& lhs, const EffectivePoint& rhs) {
              return lhs.source_sample < rhs.source_sample;
            });
  std::vector<EffectivePoint> unique_effective;
  unique_effective.reserve(effective.size());
  for (const auto point : effective) {
    if (!unique_effective.empty() && point.source_sample == unique_effective.back().source_sample) {
      if (std::abs(point.effective_midi - unique_effective.back().effective_midi) > 1.0e-5) {
        invalid("merge.edits", "internal pitch transition is discontinuous");
      }
      continue;
    }
    unique_effective.push_back(point);
  }

  const auto value_at = [&](double source_sample) {
    if (source_sample <= unique_effective.front().source_sample) {
      return unique_effective.front().effective_midi;
    }
    if (source_sample >= unique_effective.back().source_sample) {
      return unique_effective.back().effective_midi;
    }
    const auto upper = std::upper_bound(
        unique_effective.begin(), unique_effective.end(), source_sample,
        [](double sample, const EffectivePoint& point) { return sample < point.source_sample; });
    const auto& right = *upper;
    const auto& left = *(upper - 1);
    const double fraction =
        (source_sample - left.source_sample) / (right.source_sample - left.source_sample);
    return left.effective_midi + fraction * (right.effective_midi - left.effective_midi);
  };

  VocalPitchEdit canonical;
  canonical.target.mode = PitchTargetMode::kCurve;
  canonical.target.points.push_back({static_cast<double>(merged.source_range.start),
                                     value_at(static_cast<double>(merged.source_range.start))});
  for (const auto point : unique_effective) {
    canonical.target.points.push_back({point.source_sample, point.effective_midi});
  }
  canonical.target.points.push_back({static_cast<double>(merged.source_range.end),
                                     value_at(static_cast<double>(merged.source_range.end))});
  // The source frame grid can place a point exactly at a declared endpoint.
  // Collapse only exact duplicate positions after adding the required bounds.
  std::vector<VocalPitchPoint> unique_points;
  unique_points.reserve(canonical.target.points.size());
  for (const auto point : canonical.target.points) {
    if (!unique_points.empty() && point.source_sample == unique_points.back().source_sample) {
      if (std::abs(point.target_midi - unique_points.back().target_midi) > 1.0e-5) {
        invalid("merge.edits", "effective pitch has a discontinuous endpoint");
      }
      continue;
    }
    unique_points.push_back(point);
  }
  canonical.target.points = std::move(unique_points);
  canonical.amount = 1.0;
  canonical.speed_ms = 0.0;
  canonical.transpose_semitones = 0.0;
  canonical.drift_scale = 1.0;
  canonical.vibrato_scale = 1.0;

  // Start with the contract's measured-pitch bound, then grow it only if the
  // merged note's decomposition needs a larger correction. This keeps the
  // canonical curve uncapped while still rejecting a non-representable
  // effective trajectory.
  VocalNote probe = merged;
  probe.edit.pitch = canonical;
  VocalNote baseline = merged;
  baseline.edit.pitch = VocalPitchEdit{};
  const auto baseline_plan =
      compile_pitch_plan(analysis, baseline, render_settings, source_sample_rate);
  double max_required = 0.0;
  for (const auto& point : baseline_plan.points) {
    if (!point.voiced) continue;
    max_required =
        std::max(max_required, std::abs(value_at(point.source_sample) - point.measured_midi));
  }
  canonical.max_correction_semitones = std::max(1.0, max_required);
  bool uncapped = false;
  for (unsigned int attempt = 0; attempt < 64; ++attempt) {
    probe.edit.pitch = canonical;
    const auto probe_plan =
        compile_pitch_plan(analysis, probe, render_settings, source_sample_rate);
    if (probe_plan.diagnostics.limited_correction_frames == 0) {
      for (const auto& point : probe_plan.points) {
        if (point.voiced &&
            std::abs(point.effective_midi - value_at(point.source_sample)) > 1.0e-4) {
          invalid("merge.edits", "effective pitch curve cannot be represented");
        }
      }
      uncapped = true;
      break;
    }
    if (canonical.max_correction_semitones > std::numeric_limits<double>::max() * 0.5) {
      break;
    }
    canonical.max_correction_semitones *= 2.0;
  }
  if (!uncapped) invalid("merge.edits", "effective pitch curve exceeds correction limits");
  return canonical;
}

void rewire_merge_transitions(VocalEditState& state, const std::set<VocalNoteId>& retired,
                              VocalNoteId new_id) {
  std::optional<PitchTransition> incoming;
  std::optional<PitchTransition> outgoing;
  for (const auto& transition : state.transitions) {
    const bool left_retired = contains_id(retired, transition.left_note_id);
    const bool right_retired = contains_id(retired, transition.right_note_id);
    if (left_retired && !right_retired) {
      auto mapped = transition;
      mapped.left_note_id = new_id;
      outgoing = mapped;
    } else if (!left_retired && right_retired) {
      auto mapped = transition;
      mapped.right_note_id = new_id;
      incoming = mapped;
    }
  }
  state.transitions.erase(std::remove_if(state.transitions.begin(), state.transitions.end(),
                                         [&](const PitchTransition& transition) {
                                           return contains_id(retired, transition.left_note_id) ||
                                                  contains_id(retired, transition.right_note_id);
                                         }),
                          state.transitions.end());
  if (incoming && incoming->left_note_id != incoming->right_note_id)
    state.transitions.push_back(*incoming);
  if (outgoing && outgoing->left_note_id != outgoing->right_note_id)
    state.transitions.push_back(*outgoing);
}

}  // namespace

namespace detail {

size_t find_note(const VocalEditState& state, VocalNoteId id) {
  for (size_t i = 0; i < state.notes.size(); ++i) {
    if (state.notes[i].id == id) return i;
  }
  throw VocalEditException(VocalReason::kInvalidState, "note ID was not found", "note_id");
}

size_t find_note_or_throw(const VocalEditState& state, VocalNoteId id) {
  return find_note(state, id);
}

void sort_transitions(VocalEditState& state) {
  insertion_sort(state.transitions.begin(), state.transitions.end(),
                 [](const PitchTransition& lhs, const PitchTransition& rhs) {
                   if (lhs.left_note_id != rhs.left_note_id)
                     return lhs.left_note_id < rhs.left_note_id;
                   return lhs.right_note_id < rhs.right_note_id;
                 });
}

const VocalNote* find_note_or_null(const VocalEditState& state, VocalNoteId id) {
  for (const auto& note : state.notes) {
    if (note.id == id) return &note;
  }
  return nullptr;
}

void apply_set_edit(VocalEditState& state, const SetNoteEditOp& operation) {
  const size_t index = find_note_or_throw(state, operation.note_id);
  state.notes[index].edit = operation.edit;
}

void apply_set_source(VocalEditState& state, const SetNoteSourceSpanOp& operation,
                      const Audio& source, const VocalAnalysisData& analysis,
                      int64_t source_length) {
  const size_t index = find_note_or_throw(state, operation.note_id);
  if (operation.source_range.start < 0 ||
      operation.source_range.end <= operation.source_range.start ||
      operation.source_range.end > source_length) {
    invalid("source_range", "source span is malformed");
  }
  const uint32_t frame_start = frame_for_sample_start(
      analysis.grid, static_cast<uint32_t>(analysis.f0_hz.size()), operation.source_range.start);
  const uint32_t frame_end = frame_for_sample_end(
      analysis.grid, static_cast<uint32_t>(analysis.f0_hz.size()), operation.source_range.end);
  const VocalNote previous = state.notes[index];
  VocalNote candidate = measure_vocal_note(source, analysis, previous.id, operation.source_range);
  candidate.edit = previous.edit;
  if (previous.edit.pitch.target.mode == PitchTargetMode::kCurve &&
      (previous.edit.pitch.target.points.empty() ||
       previous.edit.pitch.target.points.front().source_sample !=
           static_cast<double>(operation.source_range.start) ||
       previous.edit.pitch.target.points.back().source_sample !=
           static_cast<double>(operation.source_range.end))) {
    candidate.edit.pitch.target = respan_target(previous.edit.pitch.target, operation.source_range);
  }
  candidate.source_range = operation.source_range;
  candidate.analysis_frame_start = frame_start;
  candidate.analysis_frame_end = frame_end;
  candidate.edit.destination_start_sample = operation.destination_start_sample;
  candidate.edit.destination_length_samples = operation.destination_length_samples;
  state.notes[index] = std::move(candidate);
}

void apply_split(VocalEditState& state, const SplitNoteOp& operation, const Audio& source,
                 const VocalAnalysisData& analysis, int64_t source_length, int64_t output_length,
                 uint64_t& next_id, std::vector<IdChange>& changes) {
  const size_t index = find_note_or_throw(state, operation.note_id);
  const VocalNote original = state.notes[index];
  if (operation.source_sample <= original.source_range.start ||
      operation.source_sample >= original.source_range.end) {
    invalid("source_sample", "split point must be inside the source note");
  }
  uint32_t split_frame = original.analysis_frame_start;
  while (split_frame < original.analysis_frame_end) {
    const double frame_sample = analysis.grid.frame_origin_sample +
                                static_cast<double>(split_frame) * analysis.grid.samples_per_frame;
    if (frame_sample >= static_cast<double>(operation.source_sample)) break;
    ++split_frame;
  }
  if (split_frame <= original.analysis_frame_start || split_frame >= original.analysis_frame_end) {
    invalid("source_sample", "split point must leave analysis frames on both sides");
  }

  const VocalNoteId left_id = issue_id(next_id);
  const VocalNoteId right_id = issue_id(next_id);
  const int64_t original_source_length = original.source_range.length();
  const int64_t left_source_length = operation.source_sample - original.source_range.start;
  const int64_t destination_length = original.edit.destination_length_samples;
  const int64_t left_destination_length = static_cast<int64_t>(std::floor(
      static_cast<long double>(destination_length) * left_source_length / original_source_length));
  if (left_destination_length <= 0 || left_destination_length >= destination_length) {
    invalid("destination", "split point produces an empty destination note");
  }

  const SampleRange left_source_range{original.source_range.start, operation.source_sample};
  const SampleRange right_source_range{operation.source_sample, original.source_range.end};
  VocalNote left = measure_vocal_note(source, analysis, left_id, left_source_range);
  VocalNote right = measure_vocal_note(source, analysis, right_id, right_source_range);
  left.edit = original.edit;
  right.edit = original.edit;
  left.analysis_frame_end = split_frame;
  right.analysis_frame_start = split_frame;
  left.edit.destination_start_sample = original.edit.destination_start_sample;
  left.edit.destination_length_samples = left_destination_length;
  right.edit.destination_start_sample =
      checked_end(original.edit.destination_start_sample, left_destination_length, "destination");
  right.edit.destination_length_samples = destination_length - left_destination_length;
  split_target(original.edit.pitch.target, static_cast<double>(operation.source_sample),
               left.edit.pitch.target, right.edit.pitch.target);
  const double fraction = static_cast<double>(left_source_length) / original_source_length;
  left.edit.amplitude_envelope = split_envelope(original.edit.amplitude_envelope, fraction, true);
  right.edit.amplitude_envelope = split_envelope(original.edit.amplitude_envelope, fraction, false);
  validate_note(left, analysis, source_length, output_length);
  validate_note(right, analysis, source_length, output_length);

  state.notes.erase(state.notes.begin() + static_cast<std::ptrdiff_t>(index));
  state.notes.insert(state.notes.begin() + static_cast<std::ptrdiff_t>(index), {left, right});
  rewire_split_transitions(state, original.id, left, right);
  changes.push_back({0, original.id, {left.id, right.id}});
}

void apply_merge(VocalEditState& state, const MergeNotesOp& operation, const Audio& source,
                 const VocalAnalysisData& analysis, int64_t source_length, int64_t output_length,
                 const RenderSettings& render_settings, uint64_t& next_id,
                 std::vector<IdChange>& changes) {
  if (operation.note_ids.size() < 2) invalid("note_ids", "merge requires at least two notes");
  std::vector<size_t> indices;
  indices.reserve(operation.note_ids.size());
  for (const auto id : operation.note_ids) indices.push_back(find_note_or_throw(state, id));
  insertion_sort(indices.begin(), indices.end());
  for (size_t i = 1; i < indices.size(); ++i) {
    if (indices[i] != indices[i - 1] + 1) invalid("note_ids", "merge notes must be adjacent");
  }
  std::vector<VocalNote> selected;
  selected.reserve(indices.size());
  for (const auto index : indices) selected.push_back(state.notes[index]);
  for (size_t i = 1; i < selected.size(); ++i) {
    if (selected[i - 1].source_range.end != selected[i].source_range.start) {
      invalid("source_range", "merge notes must be source-contiguous");
    }
  }
  const auto& first = selected.front();
  const auto& last = selected.back();
  if (operation.mode == MergeMode::kPreserveEdits) {
    for (size_t i = 1; i < selected.size(); ++i) {
      const long double first_source_length = static_cast<long double>(first.source_range.length());
      const long double current_source_length =
          static_cast<long double>(selected[i].source_range.length());
      const long double first_destination_length =
          static_cast<long double>(first.edit.destination_length_samples);
      const long double current_destination_length =
          static_cast<long double>(selected[i].edit.destination_length_samples);
      const bool same_stretch_ratio = first_destination_length * current_source_length ==
                                      current_destination_length * first_source_length;
      if (!same_stretch_ratio || first.edit.gain_db != selected[i].edit.gain_db ||
          first.edit.muted != selected[i].edit.muted ||
          first.edit.formant.mode != selected[i].edit.formant.mode ||
          first.edit.formant.shift_semitones != selected[i].edit.formant.shift_semitones ||
          !same_constant_envelope(first.edit.amplitude_envelope,
                                  selected[i].edit.amplitude_envelope)) {
        invalid("merge.edits", "preserving edits would silently discard an edit");
      }
    }
    const int64_t first_destination_end = checked_end(
        first.edit.destination_start_sample, first.edit.destination_length_samples, "destination");
    for (size_t i = 1; i < selected.size(); ++i) {
      const int64_t previous_end =
          checked_end(selected[i - 1].edit.destination_start_sample,
                      selected[i - 1].edit.destination_length_samples, "destination");
      if (previous_end != selected[i].edit.destination_start_sample) {
        invalid("merge.destination", "preserving edits requires contiguous destination ranges");
      }
    }
    static_cast<void>(first_destination_end);
  } else if (operation.mode != MergeMode::kResetEdits) {
    invalid("merge.mode", "unknown merge mode");
  }

  const VocalNoteId new_id = issue_id(next_id);
  VocalNote merged = measure_vocal_note(source, analysis, new_id,
                                        {first.source_range.start, last.source_range.end});
  merged.edit = operation.mode == MergeMode::kResetEdits
                    ? VocalNoteEdit::identity_for(merged.source_range)
                    : first.edit;
  if (operation.mode == MergeMode::kPreserveEdits) {
    merged.edit.destination_start_sample = first.edit.destination_start_sample;
    int64_t destination_end = checked_end(last.edit.destination_start_sample,
                                          last.edit.destination_length_samples, "destination");
    merged.edit.destination_length_samples = destination_end - merged.edit.destination_start_sample;
    merged.edit.pitch = canonical_merge_pitch(selected, state, merged, analysis, render_settings,
                                              static_cast<uint32_t>(source.sample_rate()));
    if (!merged.edit.amplitude_envelope.empty()) {
      auto& envelope = merged.edit.amplitude_envelope;
      envelope.erase(envelope.begin() + 1, envelope.end());
    }
  }
  validate_note(merged, analysis, source_length, output_length);
  std::set<VocalNoteId> retired;
  for (const auto& note : selected) retired.insert(note.id);
  const size_t first_index = indices.front();
  state.notes.erase(state.notes.begin() + static_cast<std::ptrdiff_t>(indices.front()),
                    state.notes.begin() + static_cast<std::ptrdiff_t>(indices.back() + 1));
  state.notes.insert(state.notes.begin() + static_cast<std::ptrdiff_t>(first_index), merged);
  rewire_merge_transitions(state, retired, new_id);
  changes.reserve(changes.size() + retired.size());
  for (const auto id : operation.note_ids) changes.push_back({0, id, {new_id}});
}

void apply_set_transition(VocalEditState& state, const SetTransitionOp& operation) {
  const auto& transition = operation.transition;
  const size_t left_index = find_note_or_throw(state, transition.left_note_id);
  const size_t right_index = find_note_or_throw(state, transition.right_note_id);
  if (left_index + 1 != right_index ||
      state.notes[left_index].edit.destination_start_sample +
              state.notes[left_index].edit.destination_length_samples !=
          state.notes[right_index].edit.destination_start_sample) {
    invalid("transition", "transition notes must be adjacent in destination time");
  }
  if (transition.left_window_samples <= 0 || transition.right_window_samples <= 0 ||
      transition.left_window_samples > state.notes[left_index].edit.destination_length_samples ||
      transition.right_window_samples > state.notes[right_index].edit.destination_length_samples ||
      !std::isfinite(transition.strength) || transition.strength < 0.0 ||
      transition.strength > 1.0 || transition.curve != TransitionCurve::kSmoothstep) {
    invalid("transition", "transition values are malformed");
  }
  state.transitions.erase(
      std::remove_if(state.transitions.begin(), state.transitions.end(),
                     [&](const PitchTransition& existing) {
                       return existing.left_note_id == transition.left_note_id &&
                              existing.right_note_id == transition.right_note_id;
                     }),
      state.transitions.end());
  for (const auto& existing : state.transitions) {
    if ((existing.left_note_id == transition.left_note_id &&
         existing.right_note_id != transition.right_note_id) ||
        (existing.right_note_id == transition.right_note_id &&
         existing.left_note_id != transition.left_note_id)) {
      invalid("transition", "an incoming or outgoing transition already exists");
    }
  }
  state.transitions.push_back(transition);
}

void apply_remove_transition(VocalEditState& state, const RemoveTransitionOp& operation) {
  const auto before = state.transitions.size();
  state.transitions.erase(
      std::remove_if(state.transitions.begin(), state.transitions.end(),
                     [&](const PitchTransition& transition) {
                       return transition.left_note_id == operation.left_note_id &&
                              transition.right_note_id == operation.right_note_id;
                     }),
      state.transitions.end());
  if (state.transitions.size() == before) invalid("transition", "transition was not found");
  static_cast<void>(find_note_or_throw(state, operation.left_note_id));
  static_cast<void>(find_note_or_throw(state, operation.right_note_id));
}

}  // namespace detail

}  // namespace sonare::editing::vocal_edit
