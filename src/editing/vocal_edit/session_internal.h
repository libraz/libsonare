#pragma once

// Helpers shared by the vocal edit session translation units.

#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "editing/vocal_edit/session.h"

namespace sonare::editing::vocal_edit {

namespace detail {

inline constexpr double kMinFormantShiftSemitones = -10.34995771500078;  // 12*log2(0.55)
inline constexpr double kMaxFormantShiftSemitones = 8.669592293653093;   // 12*log2(1.65)

// Slack, in samples, when comparing an analysis frame position against a window edge.
inline constexpr double kFrameWindowToleranceSamples = 1.0e-9;
// Allowance, in frames, subtracted before a fractional frame position is rounded up.
inline constexpr double kFrameCeilToleranceFrames = 1.0e-12;

[[noreturn]] inline void invalid(const std::string& field, const std::string& message) {
  throw VocalEditException(VocalReason::kInvalidInput, message, field);
}

[[noreturn]] inline void invalid_state(const std::string& field, const std::string& message) {
  throw VocalEditException(VocalReason::kInvalidState, message, field);
}

/// Sample position of analysis frame @p frame on @p grid.
inline double frame_sample_position(const AnalysisGrid& grid, uint32_t frame) {
  return grid.frame_origin_sample + static_cast<double>(frame) * grid.samples_per_frame;
}

/// Fractional analysis frame position of @p sample on @p grid.
inline double frame_raw_position(const AnalysisGrid& grid, int64_t sample) {
  return (static_cast<double>(sample) - grid.frame_origin_sample) / grid.samples_per_frame;
}

/// First frame at or after @p sample, with a tiny allowance for rounding in the position.
inline double frame_ceil(const AnalysisGrid& grid, int64_t sample) {
  return std::ceil(frame_raw_position(grid, sample) - kFrameCeilToleranceFrames);
}

int64_t checked_end(int64_t start, int64_t length, const std::string& field);

bool contains_id(const std::set<VocalNoteId>& ids, VocalNoteId id);

void validate_note(const VocalNote& note, const VocalAnalysisData& analysis, int64_t source_length,
                   int64_t output_length);

size_t find_note(const VocalEditState& state, VocalNoteId id);

const VocalNote* find_note_or_null(const VocalEditState& state, VocalNoteId id);

/// Refuses a transition whose windows, strength or curve do not fit the two notes it joins.
void validate_transition_values(const PitchTransition& transition, const VocalNote& left_note,
                                const VocalNote& right_note);

void sort_transitions(VocalEditState& state);

void apply_set_edit(VocalEditState& state, const SetNoteEditOp& operation);

void apply_set_source(VocalEditState& state, const SetNoteSourceSpanOp& operation,
                      const Audio& source, const VocalAnalysisData& analysis,
                      int64_t source_length);

void apply_split(VocalEditState& state, const SplitNoteOp& operation, const Audio& source,
                 const VocalAnalysisData& analysis, int64_t source_length, int64_t output_length,
                 uint64_t& next_id, std::vector<IdChange>& changes);

void apply_merge(VocalEditState& state, const MergeNotesOp& operation, const Audio& source,
                 const VocalAnalysisData& analysis, int64_t source_length, int64_t output_length,
                 const RenderSettings& render_settings, uint64_t& next_id,
                 std::vector<IdChange>& changes);

void apply_set_transition(VocalEditState& state, const SetTransitionOp& operation);

void apply_remove_transition(VocalEditState& state, const RemoveTransitionOp& operation);

}  // namespace detail

}  // namespace sonare::editing::vocal_edit
