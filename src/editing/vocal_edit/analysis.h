#pragma once

/// @file analysis.h
/// @brief Validation, digesting and note extraction for vocal sessions.

#include <vector>

#include "core/audio.h"
#include "editing/vocal_edit/types.h"
#include "feature/pitch.h"

namespace sonare::editing::vocal_edit {

struct VocalAnalysisOptions {
  PitchConfig pitch{};
  double voiced_threshold = 0.5;
  double segmentation_threshold_cents = 50.0;
  double minimum_note_ms = 30.0;
  double reference_hz = 440.0;
  std::string algorithm_id = "libsonare.pyin";
  uint32_t algorithm_version = 1;
};

/// @brief Computes the source descriptor from canonical float32 sample bytes.
SourceDescriptor describe_source(const Audio& source);

/// @brief Validates and canonicalizes caller-supplied analysis.
///
/// Unvoiced F0 values are normalized to zero. The input arrays are copied, so
/// an analysis object may safely outlive the caller's buffers.
VocalAnalysisData validate_analysis(const Audio& source, VocalAnalysisData analysis);

/// @brief Runs the default pYIN analysis and adds position metadata.
VocalAnalysisData analyze_vocal(const Audio& source, const VocalAnalysisOptions& options = {});

/// @brief Builds the initial note state from the immutable analysis track.
///
/// Notes follow the persisted NoteSegmenter settings and have disjoint,
/// source-clamped sample spans. Their IDs start at one; the session owns the
/// high-water mark after this function returns.
std::vector<VocalNote> extract_vocal_notes(const Audio& source, const VocalAnalysisData& analysis);

/// @brief Measures one note through the same absolute-origin path used by
/// extraction, split and merge operations.
VocalNote measure_vocal_note(const Audio& source, const VocalAnalysisData& analysis, VocalNoteId id,
                             SampleRange source_range);

/// @brief Computes a digest for the analysis values and descriptors.
Sha256Digest digest_analysis(const VocalAnalysisData& analysis);

}  // namespace sonare::editing::vocal_edit
