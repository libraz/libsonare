// SONARE_WASM_EXCEPTION_UNWIND: release staged edits and history on exceptions before publication.
#include "editing/vocal_edit/session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>

#include "editing/vocal_edit/pitch_plan.h"
#include "editing/vocal_edit/state_codec.h"
#include "util/insertion_sort.h"
#include "util/sha256.h"

namespace sonare::editing::vocal_edit {
namespace {

std::atomic<uint64_t> g_next_session_epoch{1};

VocalSessionId make_session_id(const SourceDescriptor& descriptor, VocalSessionEpoch epoch) {
  VocalSessionId id{};
  // Entropy-seeded so lineage ids differ across processes even if random_device is deterministic.
  std::random_device entropy;
  for (size_t offset = 0; offset < id.size();) {
    const auto word = entropy();
    for (size_t byte = 0; byte < sizeof(word) && offset < id.size(); ++byte, ++offset) {
      id[offset] = static_cast<uint8_t>(word >> (byte * 8));
    }
  }
  for (size_t i = 0; i < id.size(); ++i) id[i] ^= descriptor.digest[i];
  for (size_t i = 0; i < sizeof(epoch) && i < id.size(); ++i) {
    id[i] ^= static_cast<uint8_t>(epoch >> (i * 8));
  }
  const auto timestamp =
      static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  for (size_t i = 0; i < sizeof(timestamp) && i + sizeof(epoch) < id.size(); ++i) {
    id[i + sizeof(epoch)] ^= static_cast<uint8_t>(timestamp >> (i * 8));
  }
  bool all_zero = true;
  for (const auto byte : id) all_zero = all_zero && byte == 0;
  if (all_zero) id.back() = 1;
  return id;
}

VocalSessionEpoch issue_session_epoch() {
  uint64_t current = g_next_session_epoch.load(std::memory_order_relaxed);
  for (;;) {
    if (current == 0 || current == std::numeric_limits<uint64_t>::max()) {
      throw VocalEditException(VocalReason::kCounterExhausted, "session epoch exhausted", "epoch");
    }
    if (g_next_session_epoch.compare_exchange_weak(current, current + 1, std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
      return current;
    }
  }
}

[[noreturn]] void invalid(const std::string& field, const std::string& message) {
  throw VocalEditException(VocalReason::kInvalidInput, message, field);
}

[[noreturn]] void invalid_state(const std::string& field, const std::string& message) {
  throw VocalEditException(VocalReason::kInvalidState, message, field);
}

[[noreturn]] void revision_conflict(const std::string& field, uint64_t expected, uint64_t actual) {
  throw VocalEditException(VocalReason::kRevisionConflict, "revision or generation conflict", field,
                           std::to_string(expected), std::to_string(actual));
}

int64_t checked_end(int64_t start, int64_t length, const std::string& field) {
  if (start < 0 || length <= 0 || length > std::numeric_limits<int64_t>::max() - start) {
    invalid(field, "range is outside the non-negative int64 sample domain");
  }
  return start + length;
}

bool same_target(const VocalPitchTarget& lhs, const VocalPitchTarget& rhs) {
  if (lhs.mode != rhs.mode || lhs.center_midi != rhs.center_midi ||
      lhs.points.size() != rhs.points.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.points.size(); ++i) {
    if (lhs.points[i].source_sample != rhs.points[i].source_sample ||
        lhs.points[i].target_midi != rhs.points[i].target_midi) {
      return false;
    }
  }
  return true;
}

bool same_pitch_except_target(const VocalPitchEdit& lhs, const VocalPitchEdit& rhs) {
  return lhs.amount == rhs.amount && lhs.speed_ms == rhs.speed_ms &&
         lhs.max_correction_semitones == rhs.max_correction_semitones &&
         lhs.transpose_semitones == rhs.transpose_semitones && lhs.drift_scale == rhs.drift_scale &&
         lhs.vibrato_scale == rhs.vibrato_scale;
}

constexpr double kMinFormantShiftSemitones = -10.34995771500078;  // 12*log2(0.55)
constexpr double kMaxFormantShiftSemitones = 8.669592293653093;   // 12*log2(1.65)

void validate_pitch_edit(const VocalNoteEdit& edit, const SampleRange source_range,
                         int64_t output_length) {
  const auto finite = [](double value) { return std::isfinite(value); };
  const auto& pitch = edit.pitch;
  if (!finite(pitch.amount) || pitch.amount < 0.0 || pitch.amount > 1.0)
    invalid("pitch.amount", "must be in [0, 1]");
  if (!finite(pitch.speed_ms) || pitch.speed_ms < 0.0) invalid("pitch.speed_ms", "must be >= 0");
  if (!finite(pitch.max_correction_semitones) || pitch.max_correction_semitones < 0.0) {
    invalid("pitch.max_correction_semitones", "must be >= 0");
  }
  if (!finite(pitch.transpose_semitones)) invalid("pitch.transpose_semitones", "must be finite");
  if (!finite(pitch.drift_scale) || pitch.drift_scale < 0.0)
    invalid("pitch.drift_scale", "must be >= 0");
  if (!finite(pitch.vibrato_scale) || pitch.vibrato_scale < 0.0)
    invalid("pitch.vibrato_scale", "must be >= 0");
  if (pitch.target.mode == PitchTargetMode::kCenter) {
    if (!finite(pitch.target.center_midi) || pitch.target.center_midi < 0.0 ||
        pitch.target.center_midi > 127.0) {
      invalid("pitch.target.center_midi", "must be in [0, 127]");
    }
    if (!pitch.target.points.empty())
      invalid("pitch.target.points", "centre targets cannot carry curve points");
  } else if (pitch.target.mode == PitchTargetMode::kCurve) {
    if (pitch.target.points.size() < 2)
      invalid("pitch.target.points", "curve needs at least two points");
    double previous = -std::numeric_limits<double>::infinity();
    for (const auto& point : pitch.target.points) {
      if (!finite(point.source_sample) || !finite(point.target_midi) || point.target_midi < 0.0 ||
          point.target_midi > 127.0 || !(point.source_sample > previous)) {
        invalid("pitch.target.points", "curve points are malformed");
      }
      previous = point.source_sample;
    }
    if (pitch.target.points.front().source_sample != static_cast<double>(source_range.start) ||
        pitch.target.points.back().source_sample != static_cast<double>(source_range.end)) {
      invalid("pitch.target.points", "curve endpoints must equal the source range");
    }
  } else if (pitch.target.mode == PitchTargetMode::kNone) {
    if (!finite(pitch.target.center_midi) || pitch.target.center_midi != 0.0 ||
        !pitch.target.points.empty()) {
      invalid("pitch.target", "none targets must carry canonical empty values");
    }
  } else {
    invalid("pitch.target.mode", "unknown target mode");
  }
  if (!finite(edit.destination_start_sample) || edit.destination_start_sample < 0 ||
      edit.destination_length_samples <= 0) {
    invalid("destination", "destination range is malformed");
  }
  const int64_t destination_end =
      checked_end(edit.destination_start_sample, edit.destination_length_samples, "destination");
  if (destination_end > output_length)
    invalid("destination", "destination range exceeds output length");
  if (!finite(edit.gain_db)) invalid("gain_db", "must be finite");
  const double gain = std::pow(10.0, edit.gain_db / 20.0);
  if (!std::isfinite(gain)) invalid("gain_db", "linear gain is not finite");
  if (edit.formant.mode == FormantMode::kPreserve) {
    if (!finite(edit.formant.shift_semitones) || edit.formant.shift_semitones != 0.0) {
      invalid("formant.shift_semitones", "preserve mode requires a zero shift");
    }
  } else if (edit.formant.mode == FormantMode::kShift) {
    if (!finite(edit.formant.shift_semitones) ||
        edit.formant.shift_semitones < kMinFormantShiftSemitones ||
        edit.formant.shift_semitones > kMaxFormantShiftSemitones) {
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
    return analysis.grid.frame_origin_sample +
           static_cast<double>(frame) * analysis.grid.samples_per_frame;
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
      const double frame_sample = analysis.grid.frame_origin_sample +
                                  static_cast<double>(frame) * analysis.grid.samples_per_frame;
      if (frame_sample + 1.0e-9 < source_window_start ||
          frame_sample - 1.0e-9 > source_window_end) {
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
    if (transition.left_window_samples <= 0 || transition.right_window_samples <= 0 ||
        transition.left_window_samples > left_note.edit.destination_length_samples ||
        transition.right_window_samples > right_note.edit.destination_length_samples ||
        !std::isfinite(transition.strength) || transition.strength < 0.0 ||
        transition.strength > 1.0 || transition.curve != TransitionCurve::kSmoothstep) {
      invalid("transition", "transition values are malformed");
    }
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

size_t find_note(const VocalEditState& state, VocalNoteId id) {
  for (size_t i = 0; i < state.notes.size(); ++i) {
    if (state.notes[i].id == id) return i;
  }
  throw VocalEditException(VocalReason::kInvalidState, "note ID was not found", "note_id");
}

size_t find_note_or_throw(const VocalEditState& state, VocalNoteId id) {
  return find_note(state, id);
}

bool contains_id(const std::set<VocalNoteId>& ids, VocalNoteId id) {
  return ids.find(id) != ids.end();
}

VocalNoteId issue_id(uint64_t& next_id) {
  if (next_id == 0 || next_id > static_cast<uint64_t>(std::numeric_limits<VocalNoteId>::max())) {
    throw VocalEditException(VocalReason::kCounterExhausted, "note ID counter exhausted",
                             "note_id");
  }
  const VocalNoteId issued = static_cast<VocalNoteId>(next_id);
  ++next_id;
  return issued;
}

uint64_t next_generation(uint64_t current) {
  if (current == std::numeric_limits<uint64_t>::max()) {
    throw VocalEditException(VocalReason::kCounterExhausted, "content generation exhausted",
                             "generation");
  }
  return current + 1;
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
  if (envelope.empty() || envelope.size() == 1) return envelope;
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
    const double local = static_cast<double>(index) / static_cast<double>(result.size() - 1);
    result[index] = value_at(left ? cut * local : cut + (1.0 - cut) * local);
  }
  return result;
}

void append_transition_dirty(const VocalEditState& state, VocalNoteId id,
                             std::vector<SampleRange>& dirty) {
  for (const auto& transition : state.transitions) {
    if (transition.left_note_id != id && transition.right_note_id != id) continue;
    for (const auto& note : state.notes) {
      if (note.id == transition.left_note_id || note.id == transition.right_note_id) {
        dirty.push_back(note.source_range);
        dirty.push_back({note.edit.destination_start_sample,
                         checked_end(note.edit.destination_start_sample,
                                     note.edit.destination_length_samples, "destination")});
      }
    }
  }
}

void normalize_ranges(std::vector<SampleRange>& ranges) {
  ranges.erase(std::remove_if(ranges.begin(), ranges.end(),
                              [](const SampleRange& range) { return range.start >= range.end; }),
               ranges.end());
  insertion_sort(ranges.begin(), ranges.end(), [](const SampleRange& lhs, const SampleRange& rhs) {
    return lhs.start < rhs.start || (lhs.start == rhs.start && lhs.end < rhs.end);
  });
  std::vector<SampleRange> merged;
  for (const auto& range : ranges) {
    if (!merged.empty() && range.start <= merged.back().end) {
      merged.back().end = std::max(merged.back().end, range.end);
    } else {
      merged.push_back(range);
    }
  }
  ranges = std::move(merged);
}

void sort_transitions(VocalEditState& state) {
  insertion_sort(state.transitions.begin(), state.transitions.end(),
                 [](const PitchTransition& lhs, const PitchTransition& rhs) {
                   if (lhs.left_note_id != rhs.left_note_id)
                     return lhs.left_note_id < rhs.left_note_id;
                   return lhs.right_note_id < rhs.right_note_id;
                 });
}

bool same_edit_content(const VocalNoteEdit& lhs, const VocalNoteEdit& rhs) {
  return same_pitch_except_target(lhs.pitch, rhs.pitch) &&
         same_target(lhs.pitch.target, rhs.pitch.target) &&
         lhs.destination_start_sample == rhs.destination_start_sample &&
         lhs.destination_length_samples == rhs.destination_length_samples &&
         lhs.gain_db == rhs.gain_db && lhs.muted == rhs.muted &&
         lhs.amplitude_envelope == rhs.amplitude_envelope && lhs.formant.mode == rhs.formant.mode &&
         lhs.formant.shift_semitones == rhs.formant.shift_semitones;
}

bool same_note_content(const VocalNote& lhs, const VocalNote& rhs) {
  return lhs.id == rhs.id && lhs.source_range.start == rhs.source_range.start &&
         lhs.source_range.end == rhs.source_range.end &&
         lhs.analysis_frame_start == rhs.analysis_frame_start &&
         lhs.analysis_frame_end == rhs.analysis_frame_end && lhs.has_pitch == rhs.has_pitch &&
         lhs.centre_midi == rhs.centre_midi && lhs.median_hz == rhs.median_hz &&
         lhs.f0_stability == rhs.f0_stability && same_edit_content(lhs.edit, rhs.edit);
}

bool same_transition(const PitchTransition& lhs, const PitchTransition& rhs) {
  return lhs.left_note_id == rhs.left_note_id && lhs.right_note_id == rhs.right_note_id &&
         lhs.left_window_samples == rhs.left_window_samples &&
         lhs.right_window_samples == rhs.right_window_samples && lhs.strength == rhs.strength &&
         lhs.curve == rhs.curve;
}

const VocalNote* find_note_or_null(const VocalEditState& state, VocalNoteId id) {
  for (const auto& note : state.notes) {
    if (note.id == id) return &note;
  }
  return nullptr;
}

void append_transition_range(const VocalEditState& state, const PitchTransition& transition,
                             std::vector<SampleRange>& ranges) {
  const auto* left = find_note_or_null(state, transition.left_note_id);
  const auto* right = find_note_or_null(state, transition.right_note_id);
  if (!left || !right) return;
  const int64_t left_end = checked_end(left->edit.destination_start_sample,
                                       left->edit.destination_length_samples, "destination");
  if (transition.left_window_samples > left_end - left->edit.destination_start_sample ||
      transition.right_window_samples >
          std::numeric_limits<int64_t>::max() - right->edit.destination_start_sample) {
    return;
  }
  const int64_t begin = left_end - transition.left_window_samples;
  const int64_t end = right->edit.destination_start_sample + transition.right_window_samples;
  if (begin < end) ranges.push_back({begin, end});
}

std::vector<SampleRange> dirty_ranges_for_states(const VocalEditState& old_state,
                                                 const VocalEditState& new_state,
                                                 int64_t output_length) {
  std::vector<SampleRange> ranges;
  std::map<VocalNoteId, const VocalNote*> old_notes;
  std::map<VocalNoteId, const VocalNote*> new_notes;
  for (const auto& note : old_state.notes) old_notes.emplace(note.id, &note);
  for (const auto& note : new_state.notes) new_notes.emplace(note.id, &note);
  std::set<VocalNoteId> all_ids;
  for (const auto& entry : old_notes) all_ids.insert(entry.first);
  for (const auto& entry : new_notes) all_ids.insert(entry.first);
  for (const auto id : all_ids) {
    const auto old_it = old_notes.find(id);
    const auto new_it = new_notes.find(id);
    if (old_it != old_notes.end() && new_it != new_notes.end() &&
        same_note_content(*old_it->second, *new_it->second)) {
      continue;
    }
    if (old_it != old_notes.end()) {
      ranges.push_back(old_it->second->source_range);
      ranges.push_back(
          {old_it->second->edit.destination_start_sample,
           checked_end(old_it->second->edit.destination_start_sample,
                       old_it->second->edit.destination_length_samples, "destination")});
    }
    if (new_it != new_notes.end()) {
      ranges.push_back(new_it->second->source_range);
      ranges.push_back(
          {new_it->second->edit.destination_start_sample,
           checked_end(new_it->second->edit.destination_start_sample,
                       new_it->second->edit.destination_length_samples, "destination")});
    }
  }
  const auto transition_in = [](const std::vector<PitchTransition>& transitions,
                                const PitchTransition& wanted) {
    return std::find_if(transitions.begin(), transitions.end(), [&](const PitchTransition& value) {
             return same_transition(value, wanted);
           }) != transitions.end();
  };
  for (const auto& transition : old_state.transitions) {
    if (!transition_in(new_state.transitions, transition))
      append_transition_range(old_state, transition, ranges);
  }
  for (const auto& transition : new_state.transitions) {
    if (!transition_in(old_state.transitions, transition))
      append_transition_range(new_state, transition, ranges);
  }
  for (auto& range : ranges) {
    range.start = std::clamp<int64_t>(range.start, 0, output_length);
    range.end = std::clamp<int64_t>(range.end, 0, output_length);
  }
  normalize_ranges(ranges);
  return ranges;
}

uint64_t state_storage_bytes(const VocalEditState& state) {
  uint64_t bytes = sizeof(VocalEditState);
  const auto add = [&bytes](uint64_t value) {
    if (value > std::numeric_limits<uint64_t>::max() - bytes) {
      throw VocalEditException(VocalReason::kCounterExhausted, "state size counter exhausted",
                               "history");
    }
    bytes += value;
  };
  const auto add_array = [&add](size_t count, size_t element_size) {
    if (element_size != 0 && count > std::numeric_limits<uint64_t>::max() / element_size) {
      throw VocalEditException(VocalReason::kCounterExhausted, "state size counter exhausted",
                               "history");
    }
    add(static_cast<uint64_t>(count) * static_cast<uint64_t>(element_size));
  };
  for (const auto& note : state.notes) {
    add_array(note.edit.pitch.target.points.capacity(), sizeof(VocalPitchPoint));
    add_array(note.edit.amplitude_envelope.capacity(), sizeof(float));
  }
  add_array(state.notes.capacity(), sizeof(VocalNote));
  add_array(state.transitions.capacity(), sizeof(PitchTransition));
  return bytes;
}

uint64_t retained_history_bytes(const std::vector<std::shared_ptr<const VocalEditState>>& history,
                                size_t current_index) {
  if (current_index >= history.size()) {
    throw VocalEditException(VocalReason::kInvalidState,
                             "history index is outside the retained state list", "history");
  }
  uint64_t bytes = 0;
  for (size_t index = 0; index < history.size(); ++index) {
    // The live state is not retained history, so it does not count against the budget.
    if (index == current_index) continue;
    const uint64_t state_bytes = state_storage_bytes(*history[index]);
    if (state_bytes > std::numeric_limits<uint64_t>::max() - bytes) {
      throw VocalEditException(VocalReason::kCounterExhausted, "history size counter exhausted",
                               "history");
    }
    bytes += state_bytes;
  }
  return bytes;
}

}  // namespace

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

struct VocalEditSession::Impl {
  Audio source;
  VocalSessionId session_id{};
  SourceDescriptor source_descriptor{};
  std::shared_ptr<const VocalAnalysisData> analysis;
  VocalCapabilities capabilities{};
  RenderSettings render_settings{};
  int64_t output_length_samples = 0;
  VocalSessionLimits limits{};
  VocalSessionEpoch epoch = 0;
  VocalRevision revision = 0;
  VocalDraftId next_draft_id = 1;
  uint64_t next_note_id = 1;
  std::shared_ptr<std::atomic<uint32_t>> render_job_count =
      std::make_shared<std::atomic<uint32_t>>(0);
  std::shared_ptr<VocalRenderCache> cache;
  std::shared_ptr<const VocalEditState> committed;
  std::vector<std::shared_ptr<const VocalEditState>> history;
  size_t history_index = 0;
  bool draft_open = false;
  bool session_alive = true;
};

namespace {

std::vector<CompiledPitchPlan> evaluate_state(const VocalAnalysisData& analysis,
                                              const VocalEditState& state,
                                              const RenderSettings& render_settings,
                                              uint32_t source_sample_rate,
                                              const std::vector<VocalNoteId>& requested) {
  std::set<VocalNoteId> wanted(requested.begin(), requested.end());
  const bool all = requested.empty();
  const auto compiled = compile_pitch_plans(analysis, state, render_settings, source_sample_rate);
  if (!all && compiled.size() < wanted.size()) {
    throw VocalEditException(VocalReason::kInvalidState,
                             "pitch evaluation references an unknown note", "note_id");
  }
  if (all) return compiled;
  std::vector<CompiledPitchPlan> plans;
  plans.reserve(wanted.size());
  for (const auto& plan : compiled) {
    if (contains_id(wanted, plan.note_id)) plans.push_back(plan);
  }
  if (plans.size() != wanted.size()) {
    throw VocalEditException(VocalReason::kInvalidState,
                             "pitch evaluation references an unknown note", "note_id");
  }
  return plans;
}

void mark_note_changed(VocalNote& note) {
  note.content_generation = next_generation(note.content_generation);
}

void apply_set_edit(VocalEditState& state, const SetNoteEditOp& operation,
                    const VocalAnalysisData& analysis, int64_t output_length,
                    std::vector<SampleRange>& dirty) {
  const size_t index = find_note_or_throw(state, operation.note_id);
  dirty.push_back(state.notes[index].source_range);
  dirty.push_back({state.notes[index].edit.destination_start_sample,
                   checked_end(state.notes[index].edit.destination_start_sample,
                               state.notes[index].edit.destination_length_samples, "destination")});
  state.notes[index].edit = operation.edit;
  mark_note_changed(state.notes[index]);
  append_transition_dirty(state, operation.note_id, dirty);
  static_cast<void>(analysis);
  static_cast<void>(output_length);
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

void apply_set_source(VocalEditState& state, const SetNoteSourceSpanOp& operation,
                      const Audio& source, const VocalAnalysisData& analysis, int64_t source_length,
                      int64_t output_length, std::vector<SampleRange>& dirty) {
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
  candidate.content_generation = previous.content_generation;
  candidate.edit = previous.edit;
  if (previous.edit.pitch.target.mode == PitchTargetMode::kCurve &&
      (previous.edit.pitch.target.points.empty() ||
       previous.edit.pitch.target.points.front().source_sample !=
           static_cast<double>(operation.source_range.start) ||
       previous.edit.pitch.target.points.back().source_sample !=
           static_cast<double>(operation.source_range.end))) {
    candidate.edit.pitch.target = respan_target(previous.edit.pitch.target, operation.source_range);
  }
  dirty.push_back(candidate.source_range);
  dirty.push_back(operation.source_range);
  dirty.push_back({candidate.edit.destination_start_sample,
                   checked_end(candidate.edit.destination_start_sample,
                               candidate.edit.destination_length_samples, "destination")});
  candidate.source_range = operation.source_range;
  candidate.analysis_frame_start = frame_start;
  candidate.analysis_frame_end = frame_end;
  candidate.edit.destination_start_sample = operation.destination_start_sample;
  candidate.edit.destination_length_samples = operation.destination_length_samples;
  static_cast<void>(output_length);
  state.notes[index] = std::move(candidate);
  mark_note_changed(state.notes[index]);
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

void apply_split(VocalEditState& state, const SplitNoteOp& operation, const Audio& source,
                 const VocalAnalysisData& analysis, int64_t source_length, int64_t output_length,
                 uint64_t& next_id, std::vector<SampleRange>& dirty,
                 std::vector<IdChange>& changes) {
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

  dirty.push_back(original.source_range);
  dirty.push_back({original.edit.destination_start_sample,
                   checked_end(original.edit.destination_start_sample,
                               original.edit.destination_length_samples, "destination")});
  state.notes.erase(state.notes.begin() + static_cast<std::ptrdiff_t>(index));
  state.notes.insert(state.notes.begin() + static_cast<std::ptrdiff_t>(index), {left, right});
  rewire_split_transitions(state, original.id, left, right);
  changes.push_back({0, original.id, {left.id, right.id}});
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

void apply_merge(VocalEditState& state, const MergeNotesOp& operation, const Audio& source,
                 const VocalAnalysisData& analysis, int64_t source_length, int64_t output_length,
                 const RenderSettings& render_settings, uint64_t& next_id,
                 std::vector<SampleRange>& dirty, std::vector<IdChange>& changes) {
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
      merged.edit.amplitude_envelope = {merged.edit.amplitude_envelope.front()};
    }
  }
  validate_note(merged, analysis, source_length, output_length);
  std::set<VocalNoteId> retired;
  for (const auto& note : selected) {
    retired.insert(note.id);
    dirty.push_back(note.source_range);
    dirty.push_back({note.edit.destination_start_sample,
                     checked_end(note.edit.destination_start_sample,
                                 note.edit.destination_length_samples, "destination")});
  }
  const size_t first_index = indices.front();
  state.notes.erase(state.notes.begin() + static_cast<std::ptrdiff_t>(indices.front()),
                    state.notes.begin() + static_cast<std::ptrdiff_t>(indices.back() + 1));
  state.notes.insert(state.notes.begin() + static_cast<std::ptrdiff_t>(first_index), merged);
  rewire_merge_transitions(state, retired, new_id);
  changes.reserve(changes.size() + retired.size());
  for (const auto id : operation.note_ids) changes.push_back({0, id, {new_id}});
}

void apply_set_transition(VocalEditState& state, const SetTransitionOp& operation,
                          std::vector<SampleRange>& dirty) {
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
  dirty.push_back(state.notes[left_index].source_range);
  dirty.push_back(state.notes[right_index].source_range);
}

void apply_remove_transition(VocalEditState& state, const RemoveTransitionOp& operation,
                             std::vector<SampleRange>& dirty) {
  const auto before = state.transitions.size();
  state.transitions.erase(
      std::remove_if(state.transitions.begin(), state.transitions.end(),
                     [&](const PitchTransition& transition) {
                       return transition.left_note_id == operation.left_note_id &&
                              transition.right_note_id == operation.right_note_id;
                     }),
      state.transitions.end());
  if (state.transitions.size() == before) invalid("transition", "transition was not found");
  dirty.push_back(state.notes[find_note_or_throw(state, operation.left_note_id)].source_range);
  dirty.push_back(state.notes[find_note_or_throw(state, operation.right_note_id)].source_range);
}

}  // namespace

VocalEditSession VocalEditSession::create(Audio source, const VocalSessionCreateOptions& options) {
  const SourceDescriptor descriptor = describe_source(source);
  VocalAnalysisData analysis = options.analysis.has_value()
                                   ? validate_analysis(source, *options.analysis)
                                   : analyze_vocal(source, options.analysis_options);
  const int64_t output_length =
      options.output_length_samples == 0 ? descriptor.sample_count : options.output_length_samples;
  if (output_length < descriptor.sample_count || output_length <= 0) {
    invalid("output_length_samples", "must cover the source audio");
  }
  if (!std::isfinite(options.render_settings.edge_fade_ms) ||
      options.render_settings.edge_fade_ms < 0.0 ||
      !std::isfinite(options.render_settings.vibrato_cutoff_hz) ||
      options.render_settings.vibrato_cutoff_hz <= 0.0) {
    invalid("render_settings", "render settings are malformed");
  }
  if (options.limits.max_render_jobs == 0) invalid("max_render_jobs", "must be non-zero");
  const uint64_t epoch = issue_session_epoch();

  auto impl = std::make_shared<Impl>();
  impl->source = std::move(source);
  impl->source_descriptor = descriptor;
  impl->session_id = make_session_id(descriptor, epoch);
  impl->analysis = std::make_shared<const VocalAnalysisData>(std::move(analysis));
  impl->capabilities.min_formant_shift_semitones = kMinFormantShiftSemitones;
  impl->capabilities.max_formant_shift_semitones = kMaxFormantShiftSemitones;
  impl->render_settings = options.render_settings;
  impl->output_length_samples = output_length;
  impl->limits = options.limits;
  impl->cache = make_vocal_render_cache(options.limits.max_cache_bytes);
  impl->epoch = epoch;
  auto initial = std::make_shared<VocalEditState>();
  initial->notes = extract_vocal_notes(impl->source, *impl->analysis);
  if (initial->notes.size() > std::numeric_limits<VocalNoteId>::max()) {
    throw VocalEditException(VocalReason::kCounterExhausted, "note ID counter exhausted",
                             "note_id");
  }
  impl->next_note_id = static_cast<uint64_t>(initial->notes.size()) + 1;
  validate_vocal_edit_state(*initial, *impl->analysis, descriptor.sample_count, output_length,
                            impl->render_settings);
  impl->committed = std::shared_ptr<const VocalEditState>(initial);
  impl->history.push_back(impl->committed);
  return VocalEditSession(std::move(impl));
}

VocalEditSession VocalEditSession::restore(Audio source, const std::vector<uint8_t>& bytes,
                                           const VocalSessionLimits& runtime_limits) {
  if (runtime_limits.max_render_jobs == 0) invalid("max_render_jobs", "must be non-zero");
  const VocalPersistedState persisted = decode_vocal_state(bytes);
  const SourceDescriptor descriptor = describe_source(source);
  const bool source_matches = descriptor.sample_rate == persisted.source.sample_rate &&
                              descriptor.sample_count == persisted.source.sample_count &&
                              descriptor.digest == persisted.source.digest;
  if (!source_matches) {
    throw VocalEditException(VocalReason::kSourceMismatch,
                             "state source does not match supplied audio", "source",
                             util::sha256_digest_hex(persisted.source.digest),
                             util::sha256_digest_hex(descriptor.digest));
  }
  const uint64_t maximum_next_id =
      static_cast<uint64_t>(std::numeric_limits<VocalNoteId>::max()) + 1u;
  if (persisted.next_note_id == 0 || persisted.next_note_id > maximum_next_id) {
    throw VocalEditException(VocalReason::kCounterExhausted,
                             "note ID counter is outside its valid high-water range", "note_id");
  }
  if (persisted.output_length_samples < descriptor.sample_count ||
      persisted.output_length_samples <= 0) {
    invalid("output_length_samples", "must cover the source audio");
  }
  const Sha256Digest stored_analysis_digest = persisted.analysis.digest;
  const std::vector<float> stored_amplitude = persisted.analysis.amplitude;
  VocalAnalysisData analysis_input = persisted.analysis;
  analysis_input.amplitude.clear();
  VocalAnalysisData analysis = validate_analysis(source, std::move(analysis_input));
  if (!stored_amplitude.empty() && stored_amplitude.size() != analysis.amplitude.size()) {
    throw VocalEditException(VocalReason::kInvalidState,
                             "analysis amplitude length does not match its grid",
                             "analysis.amplitude");
  }
  if (!stored_amplitude.empty() &&
      !std::equal(stored_amplitude.begin(), stored_amplitude.end(), analysis.amplitude.begin(),
                  [](float lhs, float rhs) {
                    uint32_t left_bits = 0;
                    uint32_t right_bits = 0;
                    std::memcpy(&left_bits, &lhs, sizeof(left_bits));
                    std::memcpy(&right_bits, &rhs, sizeof(right_bits));
                    return left_bits == right_bits;
                  })) {
    throw VocalEditException(VocalReason::kInvalidState,
                             "analysis amplitude does not match supplied source",
                             "analysis.amplitude");
  }
  if (stored_analysis_digest != analysis.digest) {
    throw VocalEditException(
        VocalReason::kInvalidState, "analysis digest does not match its payload", "analysis.digest",
        util::sha256_digest_hex(stored_analysis_digest), util::sha256_digest_hex(analysis.digest));
  }
  validate_vocal_edit_state(persisted.edit_state, analysis, descriptor.sample_count,
                            persisted.output_length_samples, persisted.render_settings);
  VocalNoteId maximum_note_id = 0;
  for (const auto& note : persisted.edit_state.notes)
    maximum_note_id = std::max(maximum_note_id, note.id);
  if (persisted.next_note_id <= static_cast<uint64_t>(maximum_note_id)) {
    throw VocalEditException(VocalReason::kInvalidState,
                             "note ID high-water is behind persisted notes", "note_id");
  }
  const uint64_t epoch = issue_session_epoch();
  auto impl = std::make_shared<Impl>();
  impl->source = std::move(source);
  impl->source_descriptor = descriptor;
  impl->session_id = persisted.session_id;
  impl->analysis = std::make_shared<const VocalAnalysisData>(std::move(analysis));
  impl->render_settings = persisted.render_settings;
  impl->output_length_samples = persisted.output_length_samples;
  impl->limits = runtime_limits;
  impl->cache = make_vocal_render_cache(runtime_limits.max_cache_bytes);
  impl->epoch = epoch;
  impl->revision = persisted.committed_revision;
  impl->next_note_id = persisted.next_note_id;
  impl->capabilities.min_formant_shift_semitones = kMinFormantShiftSemitones;
  impl->capabilities.max_formant_shift_semitones = kMaxFormantShiftSemitones;
  impl->committed = std::make_shared<const VocalEditState>(persisted.edit_state);
  impl->history.push_back(impl->committed);
  return VocalEditSession(std::move(impl));
}

VocalEditSession::~VocalEditSession() {
  if (impl_) {
    impl_->session_alive = false;
    impl_->draft_open = false;
  }
}

VocalEditSession::VocalEditSession(VocalEditSession&& other) noexcept
    : impl_(std::move(other.impl_)) {}

VocalEditSession& VocalEditSession::operator=(VocalEditSession&& other) noexcept {
  if (this == &other) return *this;
  if (impl_) {
    impl_->session_alive = false;
    impl_->draft_open = false;
  }
  impl_ = std::move(other.impl_);
  return *this;
}

std::vector<VocalNote> VocalEditSession::notes() const {
  if (!impl_ || !impl_->session_alive) invalid_state("session", "session is not alive");
  return impl_->committed->notes;
}

VocalAnalysisData VocalEditSession::analysis() const {
  if (!impl_ || !impl_->session_alive) invalid_state("session", "session is not alive");
  return *impl_->analysis;
}

SourceDescriptor VocalEditSession::source_descriptor() const {
  if (!impl_ || !impl_->session_alive) invalid_state("session", "session is not alive");
  return impl_->source_descriptor;
}

VocalCapabilities VocalEditSession::capabilities() const noexcept {
  if (!impl_ || !impl_->session_alive) return {};
  return impl_->capabilities;
}

std::vector<PitchTransition> VocalEditSession::transitions() const {
  if (!impl_ || !impl_->session_alive) invalid_state("session", "session is not alive");
  return impl_->committed->transitions;
}

VocalStateToken VocalEditSession::token() const {
  if (!impl_ || !impl_->session_alive) invalid_state("session", "session is not alive");
  return {impl_->epoch, impl_->revision, 0, 0};
}

int64_t VocalEditSession::output_length_samples() const {
  if (!impl_ || !impl_->session_alive) invalid_state("session", "session is not alive");
  return impl_->output_length_samples;
}

const RenderSettings& VocalEditSession::render_settings() const {
  if (!impl_ || !impl_->session_alive) invalid_state("session", "session is not alive");
  return impl_->render_settings;
}

std::unique_ptr<VocalEditDraft> VocalEditSession::begin_edit(VocalRevision expected_revision) {
  if (!impl_ || !impl_->session_alive) invalid_state("session", "session is not alive");
  if (expected_revision != impl_->revision)
    revision_conflict("revision", expected_revision, impl_->revision);
  if (impl_->draft_open) invalid_state("draft", "only one draft may be open");
  if (impl_->next_draft_id == 0) {
    throw VocalEditException(VocalReason::kCounterExhausted, "draft ID counter exhausted",
                             "draft_id");
  }
  auto draft = std::unique_ptr<VocalEditDraft>(new VocalEditDraft(impl_));
  draft->candidate_ = std::make_shared<VocalEditState>(*impl_->committed);
  draft->draft_token_ = {impl_->epoch, impl_->revision, impl_->next_draft_id, 1};
  draft->local_next_note_id_ = impl_->next_note_id;
  impl_->next_draft_id = impl_->next_draft_id == std::numeric_limits<VocalDraftId>::max()
                             ? 0
                             : impl_->next_draft_id + 1;
  impl_->draft_open = true;
  return draft;
}

std::shared_ptr<const VocalRenderSnapshot> VocalEditSession::capture_render_snapshot() const {
  if (!impl_ || !impl_->session_alive) invalid_state("session", "session is not alive");
  auto data = std::make_shared<VocalRenderSnapshotData>();
  data->source = impl_->source;
  data->source_descriptor = impl_->source_descriptor;
  data->analysis = impl_->analysis;
  data->state = impl_->committed;
  data->output_length_samples = impl_->output_length_samples;
  data->render_settings = impl_->render_settings;
  data->token = token();
  data->cache = impl_->cache;
  data->render_job_count = impl_->render_job_count;
  data->max_render_jobs = impl_->limits.max_render_jobs;
  return std::make_shared<VocalRenderSnapshot>(std::move(data));
}

std::vector<CompiledPitchPlan> VocalEditSession::evaluate_pitch(
    const std::vector<VocalNoteId>& note_ids) const {
  if (!impl_ || !impl_->session_alive) invalid_state("session", "session is not alive");
  return evaluate_state(*impl_->analysis, *impl_->committed, impl_->render_settings,
                        impl_->source_descriptor.sample_rate, note_ids);
}

double VocalEditSession::source_sample_to_destination_sample(VocalNoteId note_id,
                                                             double source_sample) const {
  if (!impl_ || !impl_->session_alive) invalid_state("session", "session is not alive");
  const auto& note = impl_->committed->notes[find_note(*impl_->committed, note_id)];
  return ::sonare::editing::vocal_edit::source_sample_to_destination_sample(note, source_sample);
}

double VocalEditSession::destination_sample_to_source_sample(VocalNoteId note_id,
                                                             double destination_sample) const {
  if (!impl_ || !impl_->session_alive) invalid_state("session", "session is not alive");
  const auto& note = impl_->committed->notes[find_note(*impl_->committed, note_id)];
  return ::sonare::editing::vocal_edit::destination_sample_to_source_sample(note,
                                                                            destination_sample);
}

StateChangeResult VocalEditSession::undo(VocalRevision expected_revision,
                                         const StateResultPreparation& before_publish) {
  if (!impl_ || !impl_->session_alive) invalid_state("session", "session is not alive");
  if (impl_->draft_open) invalid_state("draft", "undo is not allowed while a draft is open");
  if (expected_revision != impl_->revision)
    revision_conflict("revision", expected_revision, impl_->revision);
  if (impl_->history_index == 0) invalid_state("history", "nothing to undo");
  if (impl_->revision == std::numeric_limits<VocalRevision>::max()) {
    throw VocalEditException(VocalReason::kCounterExhausted, "revision exhausted", "revision");
  }
  const auto old_state = impl_->committed;
  const size_t target_index = impl_->history_index - 1;
  const auto target_state = impl_->history[target_index];
  // Compute all potentially-throwing work before publishing the new model.
  // An allocation failure must leave both the visible state and history
  // cursor untouched.
  auto dirty = dirty_ranges_for_states(*old_state, *target_state, impl_->output_length_samples);
  const VocalRevision next_revision = impl_->revision + 1;
  StateChangeResult result{{impl_->epoch, next_revision, 0, 0}, std::move(dirty)};
  if (before_publish) before_publish(result);
  impl_->history_index = target_index;
  impl_->committed = target_state;
  impl_->revision = next_revision;
  return result;
}

StateChangeResult VocalEditSession::redo(VocalRevision expected_revision,
                                         const StateResultPreparation& before_publish) {
  if (!impl_ || !impl_->session_alive) invalid_state("session", "session is not alive");
  if (impl_->draft_open) invalid_state("draft", "redo is not allowed while a draft is open");
  if (expected_revision != impl_->revision)
    revision_conflict("revision", expected_revision, impl_->revision);
  if (impl_->history_index + 1 >= impl_->history.size())
    invalid_state("history", "nothing to redo");
  if (impl_->revision == std::numeric_limits<VocalRevision>::max()) {
    throw VocalEditException(VocalReason::kCounterExhausted, "revision exhausted", "revision");
  }
  const auto old_state = impl_->committed;
  const size_t target_index = impl_->history_index + 1;
  const auto target_state = impl_->history[target_index];
  // See undo(): build the dirty result before changing the cursor or model.
  auto dirty = dirty_ranges_for_states(*old_state, *target_state, impl_->output_length_samples);
  const VocalRevision next_revision = impl_->revision + 1;
  StateChangeResult result{{impl_->epoch, next_revision, 0, 0}, std::move(dirty)};
  if (before_publish) before_publish(result);
  impl_->history_index = target_index;
  impl_->committed = target_state;
  impl_->revision = next_revision;
  return result;
}

bool VocalEditSession::can_undo() const noexcept {
  return impl_ && impl_->session_alive && !impl_->draft_open && impl_->history_index > 0;
}

bool VocalEditSession::can_redo() const noexcept {
  return impl_ && impl_->session_alive && !impl_->draft_open &&
         impl_->history_index + 1 < impl_->history.size();
}

std::vector<uint8_t> VocalEditSession::export_state() const {
  if (!impl_ || !impl_->session_alive) invalid_state("session", "session is not alive");
  if (impl_->draft_open) invalid_state("draft", "cannot export while a draft is open");
  VocalPersistedState persisted;
  persisted.session_id = impl_->session_id;
  persisted.next_note_id = impl_->next_note_id;
  persisted.committed_revision = impl_->revision;
  persisted.source = impl_->source_descriptor;
  persisted.output_length_samples = impl_->output_length_samples;
  persisted.analysis = *impl_->analysis;
  persisted.edit_state = *impl_->committed;
  persisted.render_settings = impl_->render_settings;
  return encode_vocal_state(persisted);
}

VocalEditDraft::VocalEditDraft(std::shared_ptr<VocalEditSession::Impl> impl)
    : impl_(std::move(impl)), active_(true) {}

VocalEditDraft::~VocalEditDraft() { cancel(); }

VocalEditDraft::VocalEditDraft(VocalEditDraft&& other) noexcept
    : impl_(std::move(other.impl_)),
      candidate_(std::move(other.candidate_)),
      draft_token_(other.draft_token_),
      local_next_note_id_(other.local_next_note_id_),
      active_(other.active_) {
  other.active_ = false;
}

VocalEditDraft& VocalEditDraft::operator=(VocalEditDraft&& other) noexcept {
  if (this == &other) return *this;
  cancel();
  impl_ = std::move(other.impl_);
  candidate_ = std::move(other.candidate_);
  draft_token_ = other.draft_token_;
  local_next_note_id_ = other.local_next_note_id_;
  active_ = other.active_;
  other.active_ = false;
  return *this;
}

std::vector<VocalNote> VocalEditDraft::notes() const {
  if (!active_ || !impl_ || !impl_->session_alive || !candidate_)
    invalid_state("draft", "draft is not active");
  return candidate_->notes;
}

VocalAnalysisData VocalEditDraft::analysis() const {
  if (!active_ || !impl_ || !impl_->session_alive) invalid_state("draft", "draft is not active");
  return *impl_->analysis;
}

std::vector<PitchTransition> VocalEditDraft::transitions() const {
  if (!active_ || !impl_ || !impl_->session_alive || !candidate_)
    invalid_state("draft", "draft is not active");
  return candidate_->transitions;
}

VocalStateToken VocalEditDraft::token() const {
  if (!active_ || !impl_ || !impl_->session_alive) invalid_state("draft", "draft is not active");
  return draft_token_;
}

DraftApplyResult VocalEditDraft::apply(VocalGeneration expected_generation,
                                       const std::vector<Operation>& operations,
                                       const DraftResultPreparation& before_publish) {
  if (!active_ || !impl_ || !impl_->session_alive || !candidate_)
    invalid_state("draft", "draft is not active");
  if (expected_generation != draft_token_.draft_generation) {
    revision_conflict("generation", expected_generation, draft_token_.draft_generation);
  }
  if (operations.empty()) invalid("operations", "at least one operation is required");
  if (draft_token_.draft_generation == std::numeric_limits<VocalGeneration>::max()) {
    throw VocalEditException(VocalReason::kCounterExhausted, "draft generation exhausted",
                             "generation");
  }

  auto candidate = std::make_shared<VocalEditState>(*candidate_);
  uint64_t next_id = local_next_note_id_;
  std::vector<SampleRange> dirty;
  std::vector<IdChange> changes;
  std::set<VocalNoteId> expanded_curve_ids;
  std::set<VocalNoteId> expanded_envelope_ids;
  // Record expansions against the state at the start of the atomic batch as
  // well as while applying operations below. This closes an ordering hole:
  // a SetNoteEdit that clears a curve before SetNoteSourceSpan must not make
  // the later expansion look as though it never had a curve. The contract
  // requires the final candidate to carry an explicit curve (and envelope)
  // whenever the authored state being expanded had one.
  for (const auto& operation : operations) {
    if (!std::holds_alternative<SetNoteSourceSpanOp>(operation)) continue;
    const auto& source_operation = std::get<SetNoteSourceSpanOp>(operation);
    const auto* original = find_note_or_null(*candidate_, source_operation.note_id);
    if (!original) continue;
    const bool expands = source_operation.source_range.start < original->source_range.start ||
                         source_operation.source_range.end > original->source_range.end;
    if (!expands) continue;
    if (original->edit.pitch.target.mode == PitchTargetMode::kCurve) {
      expanded_curve_ids.insert(source_operation.note_id);
    }
    if (!original->edit.amplitude_envelope.empty()) {
      expanded_envelope_ids.insert(source_operation.note_id);
    }
  }
  for (size_t operation_index = 0; operation_index < operations.size(); ++operation_index) {
    const auto& operation = operations[operation_index];
    const size_t changes_before = changes.size();
    std::visit(
        [&](const auto& typed) {
          using T = std::decay_t<decltype(typed)>;
          if constexpr (std::is_same_v<T, SetNoteEditOp>) {
            apply_set_edit(*candidate, typed, *impl_->analysis, impl_->output_length_samples,
                           dirty);
          } else if constexpr (std::is_same_v<T, SetNoteSourceSpanOp>) {
            const size_t current_index = find_note_or_throw(*candidate, typed.note_id);
            const auto& current = candidate->notes[current_index];
            if (current.edit.pitch.target.mode == PitchTargetMode::kCurve &&
                (typed.source_range.start < current.source_range.start ||
                 typed.source_range.end > current.source_range.end)) {
              expanded_curve_ids.insert(typed.note_id);
            }
            if (!current.edit.amplitude_envelope.empty() &&
                (typed.source_range.start < current.source_range.start ||
                 typed.source_range.end > current.source_range.end)) {
              expanded_envelope_ids.insert(typed.note_id);
            }
            apply_set_source(*candidate, typed, impl_->source, *impl_->analysis,
                             impl_->source_descriptor.sample_count, impl_->output_length_samples,
                             dirty);
          } else if constexpr (std::is_same_v<T, SplitNoteOp>) {
            apply_split(*candidate, typed, impl_->source, *impl_->analysis,
                        impl_->source_descriptor.sample_count, impl_->output_length_samples,
                        next_id, dirty, changes);
          } else if constexpr (std::is_same_v<T, MergeNotesOp>) {
            apply_merge(*candidate, typed, impl_->source, *impl_->analysis,
                        impl_->source_descriptor.sample_count, impl_->output_length_samples,
                        impl_->render_settings, next_id, dirty, changes);
          } else if constexpr (std::is_same_v<T, SetTransitionOp>) {
            apply_set_transition(*candidate, typed, dirty);
          } else if constexpr (std::is_same_v<T, RemoveTransitionOp>) {
            apply_remove_transition(*candidate, typed, dirty);
          } else if constexpr (std::is_same_v<T, ResetNotesOp>) {
            for (const auto id : typed.note_ids) {
              const size_t index = find_note_or_throw(*candidate, id);
              dirty.push_back(candidate->notes[index].source_range);
              candidate->notes[index].edit =
                  VocalNoteEdit::identity_for(candidate->notes[index].source_range);
              mark_note_changed(candidate->notes[index]);
            }
          }
        },
        operation);
    for (size_t i = changes_before; i < changes.size(); ++i) {
      changes[i].operation_index = static_cast<uint32_t>(operation_index);
    }
  }
  for (const auto note_id : expanded_curve_ids) {
    const bool has_companion_edit =
        std::any_of(operations.begin(), operations.end(), [note_id](const Operation& operation) {
          return std::holds_alternative<SetNoteEditOp>(operation) &&
                 std::get<SetNoteEditOp>(operation).note_id == note_id;
        });
    if (!has_companion_edit) {
      invalid("source_range", "expanding a curve source span requires a companion SetNoteEdit");
    }
    const auto& final_note = candidate->notes[find_note(*candidate, note_id)];
    if (final_note.edit.pitch.target.mode != PitchTargetMode::kCurve ||
        final_note.edit.pitch.target.points.size() < 2 ||
        final_note.edit.pitch.target.points.front().source_sample !=
            static_cast<double>(final_note.source_range.start) ||
        final_note.edit.pitch.target.points.back().source_sample !=
            static_cast<double>(final_note.source_range.end)) {
      invalid("source_range", "the companion SetNoteEdit must provide the expanded pitch curve");
    }
  }
  for (const auto note_id : expanded_envelope_ids) {
    const bool has_companion_edit =
        std::any_of(operations.begin(), operations.end(), [note_id](const Operation& operation) {
          return std::holds_alternative<SetNoteEditOp>(operation) &&
                 std::get<SetNoteEditOp>(operation).note_id == note_id;
        });
    if (!has_companion_edit ||
        candidate->notes[find_note(*candidate, note_id)].edit.amplitude_envelope.empty()) {
      invalid("source_range",
              "the companion SetNoteEdit must provide the expanded amplitude envelope");
    }
  }
  sort_transitions(*candidate);
  validate_vocal_edit_state(*candidate, *impl_->analysis, impl_->source_descriptor.sample_count,
                            impl_->output_length_samples, impl_->render_settings);
  dirty = dirty_ranges_for_states(*candidate_, *candidate, impl_->output_length_samples);
  normalize_ranges(dirty);
  auto next_token = draft_token_;
  ++next_token.draft_generation;
  DraftApplyResult result{next_token, std::move(dirty), std::move(changes)};
  if (before_publish) before_publish(result);
  candidate_ = std::move(candidate);
  local_next_note_id_ = next_id;
  // IDs issued by a successful apply are consumed even if this draft is
  // cancelled later. Failed batches never reach this assignment.
  impl_->next_note_id = next_id;
  draft_token_ = next_token;
  return result;
}

std::vector<CompiledPitchPlan> VocalEditDraft::evaluate_pitch(
    const std::vector<VocalNoteId>& note_ids) const {
  if (!active_ || !impl_ || !impl_->session_alive || !candidate_)
    invalid_state("draft", "draft is not active");
  return evaluate_state(*impl_->analysis, *candidate_, impl_->render_settings,
                        impl_->source_descriptor.sample_rate, note_ids);
}

std::shared_ptr<const VocalRenderSnapshot> VocalEditDraft::capture_render_snapshot() const {
  if (!active_ || !impl_ || !impl_->session_alive || !candidate_)
    invalid_state("draft", "draft is not active");
  auto data = std::make_shared<VocalRenderSnapshotData>();
  data->source = impl_->source;
  data->source_descriptor = impl_->source_descriptor;
  data->analysis = impl_->analysis;
  data->state = std::shared_ptr<const VocalEditState>(candidate_);
  data->output_length_samples = impl_->output_length_samples;
  data->render_settings = impl_->render_settings;
  data->token = draft_token_;
  data->cache = impl_->cache;
  data->render_job_count = impl_->render_job_count;
  data->max_render_jobs = impl_->limits.max_render_jobs;
  return std::make_shared<VocalRenderSnapshot>(std::move(data));
}

double VocalEditDraft::source_sample_to_destination_sample(VocalNoteId note_id,
                                                           double source_sample) const {
  if (!active_ || !impl_ || !impl_->session_alive || !candidate_)
    invalid_state("draft", "draft is not active");
  const auto& note = candidate_->notes[find_note(*candidate_, note_id)];
  return ::sonare::editing::vocal_edit::source_sample_to_destination_sample(note, source_sample);
}

double VocalEditDraft::destination_sample_to_source_sample(VocalNoteId note_id,
                                                           double destination_sample) const {
  if (!active_ || !impl_ || !impl_->session_alive || !candidate_)
    invalid_state("draft", "draft is not active");
  const auto& note = candidate_->notes[find_note(*candidate_, note_id)];
  return ::sonare::editing::vocal_edit::destination_sample_to_source_sample(note,
                                                                            destination_sample);
}

StateChangeResult VocalEditDraft::commit(VocalRevision expected_revision,
                                         const StateResultPreparation& before_publish) {
  if (!active_ || !impl_ || !impl_->session_alive || !candidate_)
    invalid_state("draft", "draft is not active");
  if (expected_revision != impl_->revision)
    revision_conflict("revision", expected_revision, impl_->revision);
  if (impl_->revision == std::numeric_limits<VocalRevision>::max()) {
    throw VocalEditException(VocalReason::kCounterExhausted, "revision exhausted", "revision");
  }
  // Stage the complete history transition locally. In particular, do not
  // publish the candidate before vector growth, byte accounting, or dirty
  // range construction has succeeded; those operations may allocate or
  // report a counter overflow.
  auto dirty =
      dirty_ranges_for_states(*impl_->committed, *candidate_, impl_->output_length_samples);
  const auto new_committed = std::shared_ptr<const VocalEditState>(candidate_);
  auto new_history = impl_->history;
  if (impl_->history_index + 1 < new_history.size()) {
    new_history.erase(new_history.begin() + static_cast<std::ptrdiff_t>(impl_->history_index + 1),
                      new_history.end());
  }
  new_history.push_back(new_committed);
  size_t new_history_index = new_history.size() - 1;

  size_t max_history_entries = static_cast<size_t>(impl_->limits.max_undo_depth);
  if (max_history_entries < std::numeric_limits<size_t>::max()) ++max_history_entries;
  while (new_history.size() > max_history_entries) {
    new_history.erase(new_history.begin());
    --new_history_index;
  }
  while (new_history.size() > 1 &&
         retained_history_bytes(new_history, new_history_index) > impl_->limits.max_history_bytes) {
    new_history.erase(new_history.begin());
    --new_history_index;
  }
  const VocalRevision next_revision = impl_->revision + 1;
  StateChangeResult result{{impl_->epoch, next_revision, 0, 0}, std::move(dirty)};
  if (before_publish) before_publish(result);

  // These publications are all noexcept after the staged work above.
  impl_->committed = new_committed;
  impl_->history.swap(new_history);
  impl_->history_index = new_history_index;
  impl_->next_note_id = local_next_note_id_;
  impl_->revision = next_revision;
  impl_->draft_open = false;
  active_ = false;
  draft_token_.draft_id = 0;
  draft_token_.draft_generation = 0;
  return result;
}

void VocalEditDraft::cancel() noexcept {
  if (active_ && impl_) {
    impl_->draft_open = false;
    active_ = false;
  }
}

}  // namespace sonare::editing::vocal_edit
