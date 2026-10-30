#pragma once

// Helpers shared by the vocal edit session translation units.

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "editing/vocal_edit/session.h"

namespace sonare::editing::vocal_edit {

namespace detail {

inline constexpr double kMinFormantShiftSemitones = -10.34995771500078;  // 12*log2(0.55)
inline constexpr double kMaxFormantShiftSemitones = 8.669592293653093;   // 12*log2(1.65)

[[noreturn]] void invalid(const std::string& field, const std::string& message);

int64_t checked_end(int64_t start, int64_t length, const std::string& field);

bool contains_id(const std::set<VocalNoteId>& ids, VocalNoteId id);

void validate_note(const VocalNote& note, const VocalAnalysisData& analysis, int64_t source_length,
                   int64_t output_length);

size_t find_note(const VocalEditState& state, VocalNoteId id);

size_t find_note_or_throw(const VocalEditState& state, VocalNoteId id);

const VocalNote* find_note_or_null(const VocalEditState& state, VocalNoteId id);

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
