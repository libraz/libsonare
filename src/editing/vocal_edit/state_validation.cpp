// SONARE_WASM_EXCEPTION_UNWIND: release validation scratch containers when a check throws.
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "editing/vocal_edit/pitch_plan.h"
#include "editing/vocal_edit/session.h"
#include "editing/vocal_edit/session_internal.h"
#include "effects/formant_warp.h"
#include "util/db.h"
#include "util/insertion_sort.h"

namespace sonare::editing::vocal_edit {
using namespace detail;

namespace {

void validate_pitch_edit(const VocalNoteEdit& edit, const SampleRange source_range,
                         int64_t output_length) {
  const auto finite = [](double value) { return std::isfinite(value); };
  validate_pitch_parameters(edit.pitch, source_range);
  if (!finite(edit.destination_start_sample) || edit.destination_start_sample < 0 ||
      edit.destination_length_samples <= 0) {
    invalid("destination", "destination range is malformed");
  }
  const int64_t destination_end =
      checked_end(edit.destination_start_sample, edit.destination_length_samples, "destination");
  if (destination_end > output_length)
    invalid("destination", "destination range exceeds output length");
  if (!finite(edit.gain_db)) invalid("gain_db", "must be finite");
  const double gain = db_to_linear(edit.gain_db);
  if (!std::isfinite(gain)) invalid("gain_db", "linear gain is not finite");
  if (edit.formant.mode == FormantMode::kPreserve) {
    if (!finite(edit.formant.shift_semitones) || edit.formant.shift_semitones != 0.0) {
      invalid("formant.shift_semitones", "preserve mode requires a zero shift");
    }
  } else if (edit.formant.mode == FormantMode::kShift) {
    if (!finite(edit.formant.shift_semitones) ||
        !formant_factor_in_range(formant_factor_for_shift(edit.formant.shift_semitones))) {
      invalid("formant.shift_semitones", "is outside the renderer capability range");
    }
  } else {
    invalid("formant.mode", "unknown formant mode");
  }
  for (const float value : edit.amplitude_envelope) {
    if (!std::isfinite(value) || value < 0.0f)
      invalid("amplitude_envelope", "must be finite and non-negative");
    const double combined = static_cast<double>(value) * gain;
    if (!std::isfinite(combined) ||
        combined > static_cast<double>(std::numeric_limits<float>::max())) {
      invalid("amplitude_envelope", "combined gain is outside float range");
    }
  }
}

void validate_state(const VocalEditState& state, const VocalAnalysisData& analysis,
                    int64_t source_length, int64_t output_length) {
  std::set<VocalNoteId> ids;
  std::map<VocalNoteId, size_t> note_indices;
  for (size_t i = 0; i < state.notes.size(); ++i) {
    const auto& note = state.notes[i];
    validate_note(note, analysis, source_length, output_length);
    if (!ids.insert(note.id).second) invalid("notes.id", "note IDs must be unique");
    note_indices.emplace(note.id, i);
    if (i != 0) {
      if (state.notes[i - 1].source_range.end > note.source_range.start) {
        invalid("notes.source_range", "source note ranges must not overlap");
      }
    }
  }

  // Destination placement is an independent timeline. Source order is the
  // canonical note order, but valid edits may move disjoint notes past one
  // another, so check all intervals after sorting by destination start.
  std::vector<std::pair<int64_t, int64_t>> destinations;
  destinations.reserve(state.notes.size());
  for (const auto& note : state.notes) {
    destinations.emplace_back(note.edit.destination_start_sample,
                              checked_end(note.edit.destination_start_sample,
                                          note.edit.destination_length_samples, "destination"));
  }
  insertion_sort(destinations.begin(), destinations.end());
  for (size_t i = 1; i < destinations.size(); ++i) {
    if (destinations[i - 1].second > destinations[i].first) {
      invalid("notes.destination", "destination note ranges must not overlap");
    }
  }

  std::map<VocalNoteId, int64_t> incoming_windows;
  std::map<VocalNoteId, int64_t> outgoing_windows;
  std::pair<VocalNoteId, VocalNoteId> previous_key{0, 0};
  bool have_previous_key = false;
  const auto window_is_voiced = [&](const VocalNote& note, bool left_side, int64_t window) {
    const double destination_start = static_cast<double>(note.edit.destination_start_sample);
    const double destination_end =
        destination_start + static_cast<double>(note.edit.destination_length_samples);
    const double window_start =
        left_side ? destination_end - static_cast<double>(window) : destination_start;
    const double window_end =
        left_side ? destination_end : destination_start + static_cast<double>(window);
    const double source_start = static_cast<double>(note.source_range.start);
    const double source_window_start =
        source_start + (window_start - destination_start) /
                           static_cast<double>(note.edit.destination_length_samples) *
                           static_cast<double>(note.source_range.length());
    const double source_window_end =
        source_start + (window_end - destination_start) /
                           static_cast<double>(note.edit.destination_length_samples) *
                           static_cast<double>(note.source_range.length());
    bool saw_frame = false;
    for (uint32_t frame = note.analysis_frame_start; frame < note.analysis_frame_end; ++frame) {
      const double frame_sample = frame_sample_position(analysis.grid, frame);
      if (frame_sample + kFrameWindowToleranceSamples < source_window_start ||
          frame_sample - kFrameWindowToleranceSamples > source_window_end) {
        continue;
      }
      saw_frame = true;
      if (analysis.voiced[frame] == 0 || !(analysis.f0_hz[frame] > 0.0f) ||
          !std::isfinite(analysis.f0_hz[frame])) {
        return false;
      }
    }
    return saw_frame;
  };

  for (const auto& transition : state.transitions) {
    const auto left_it = note_indices.find(transition.left_note_id);
    const auto right_it = note_indices.find(transition.right_note_id);
    if (left_it == note_indices.end() || right_it == note_indices.end() ||
        transition.left_note_id == transition.right_note_id) {
      invalid("transition.note_id", "transition references unknown or identical notes");
    }
    if (left_it->second + 1 != right_it->second ||
        state.notes[left_it->second].source_range.end !=
            state.notes[right_it->second].source_range.start) {
      invalid("transition.source_range", "transition notes must be source-adjacent");
    }
    const auto& left_note = state.notes[left_it->second];
    const auto& right_note = state.notes[right_it->second];
    const int64_t left_destination_end =
        checked_end(left_note.edit.destination_start_sample,
                    left_note.edit.destination_length_samples, "destination");
    if (left_destination_end != right_note.edit.destination_start_sample || left_note.edit.muted ||
        right_note.edit.muted) {
      invalid("transition", "transition notes must be continuous and audible");
    }
    validate_transition_values(transition, left_note, right_note);
    if (!window_is_voiced(left_note, true, transition.left_window_samples) ||
        !window_is_voiced(right_note, false, transition.right_window_samples)) {
      invalid("transition", "transition windows must be fully voiced");
    }
    const std::pair<VocalNoteId, VocalNoteId> key{transition.left_note_id,
                                                  transition.right_note_id};
    if (have_previous_key && !(previous_key < key)) {
      invalid("transition", "transitions must be unique and sorted");
    }
    previous_key = key;
    have_previous_key = true;
    if (!outgoing_windows.emplace(transition.left_note_id, transition.left_window_samples).second ||
        !incoming_windows.emplace(transition.right_note_id, transition.right_window_samples)
             .second) {
      invalid("transition", "a note may have at most one incoming and outgoing transition");
    }
  }
  for (const auto& [note_id, incoming] : incoming_windows) {
    const auto outgoing = outgoing_windows.find(note_id);
    if (outgoing == outgoing_windows.end()) continue;
    const auto& note = state.notes[note_indices.at(note_id)];
    if (incoming > note.edit.destination_length_samples - outgoing->second) {
      invalid("transition", "incoming and outgoing transition windows overlap");
    }
  }
}

}  // namespace

namespace detail {

int64_t checked_end(int64_t start, int64_t length, const std::string& field) {
  if (start < 0 || length <= 0 || length > std::numeric_limits<int64_t>::max() - start) {
    invalid(field, "range is outside the non-negative int64 sample domain");
  }
  return start + length;
}

void validate_transition_values(const PitchTransition& transition, const VocalNote& left_note,
                                const VocalNote& right_note) {
  if (transition.left_window_samples <= 0 || transition.right_window_samples <= 0 ||
      transition.left_window_samples > left_note.edit.destination_length_samples ||
      transition.right_window_samples > right_note.edit.destination_length_samples ||
      !std::isfinite(transition.strength) || transition.strength < 0.0 ||
      transition.strength > 1.0 || transition.curve != TransitionCurve::kSmoothstep) {
    invalid("transition", "transition values are malformed");
  }
}

void validate_note(const VocalNote& note, const VocalAnalysisData& analysis, int64_t source_length,
                   int64_t output_length) {
  if (note.id == kInvalidVocalNoteId) invalid("note.id", "must be non-zero");
  if (note.source_range.start < 0 || note.source_range.end <= note.source_range.start ||
      note.source_range.end > source_length) {
    invalid("note.source_range", "must be a positive range inside source audio");
  }
  if (note.analysis_frame_start >= note.analysis_frame_end ||
      note.analysis_frame_end > analysis.f0_hz.size() ||
      note.analysis_frame_end > analysis.voiced.size()) {
    invalid("note.analysis_frame", "must be inside analysis");
  }
  const auto frame_sample = [&](uint32_t frame) {
    return frame_sample_position(analysis.grid, frame);
  };
  // A rounded absolute-origin boundary can land half a sample away from the
  // ideal frame position. Verify that each declared frame edge brackets the
  // source edge without requiring an exact inverse (which would reject valid
  // fractional origins and arbitrary crop points).
  const double start_frame = frame_sample(note.analysis_frame_start);
  const double previous_start_frame = note.analysis_frame_start == 0
                                          ? -std::numeric_limits<double>::infinity()
                                          : frame_sample(note.analysis_frame_start - 1);
  const double end_frame = frame_sample(note.analysis_frame_end);
  const double previous_end_frame = frame_sample(note.analysis_frame_end - 1);
  if (!std::isfinite(start_frame) || !std::isfinite(end_frame) ||
      start_frame < static_cast<double>(note.source_range.start) - 0.500001 ||
      previous_start_frame >= static_cast<double>(note.source_range.start) + 0.500001 ||
      end_frame < static_cast<double>(note.source_range.end) - 0.500001 ||
      previous_end_frame >= static_cast<double>(note.source_range.end) + 0.500001) {
    invalid("note.analysis_frame", "frame bounds do not bracket the source span");
  }
  bool has_voiced_frame = false;
  for (uint32_t frame = note.analysis_frame_start; frame < note.analysis_frame_end; ++frame) {
    if (analysis.voiced[frame] != 0 && analysis.f0_hz[frame] > 0.0f) {
      has_voiced_frame = true;
      break;
    }
  }
  if (note.has_pitch != has_voiced_frame) {
    invalid("note.has_pitch", "must agree with the authoritative analysis mask");
  }
  if (!std::isfinite(note.centre_midi) || note.centre_midi < 0.0 || note.centre_midi > 127.0 ||
      !std::isfinite(note.median_hz) || note.median_hz < 0.0 || !std::isfinite(note.f0_stability) ||
      note.f0_stability < 0.0 || note.f0_stability > 1.0) {
    invalid("note.measurement", "pitch measurements are malformed");
  }
  if (note.has_pitch) {
    if (!(note.median_hz > 0.0))
      invalid("note.median_hz", "pitched notes need a positive median F0");
  } else if (note.centre_midi != 0.0 || note.median_hz != 0.0 || note.f0_stability != 0.0) {
    invalid("note.measurement", "unpitched notes must carry canonical zero measurements");
  }
  validate_pitch_edit(note.edit, note.source_range, output_length);
}

}  // namespace detail

void validate_vocal_edit_state(const VocalEditState& state, const VocalAnalysisData& analysis,
                               int64_t source_length_samples, int64_t output_length_samples,
                               const RenderSettings& render_settings) {
  if (render_settings.profile != RenderProfile::kVocalPsolaV1 ||
      render_settings.algorithm_version != 1 || !std::isfinite(render_settings.edge_fade_ms) ||
      render_settings.edge_fade_ms < 0.0 || !std::isfinite(render_settings.vibrato_cutoff_hz) ||
      render_settings.vibrato_cutoff_hz <= 0.0) {
    invalid("render_settings", "render settings are malformed");
  }
  validate_state(state, analysis, source_length_samples, output_length_samples);
}

}  // namespace sonare::editing::vocal_edit
