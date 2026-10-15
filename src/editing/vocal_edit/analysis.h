#pragma once

/// @file analysis.h
/// @brief Validation, digesting and note extraction for vocal sessions.

#include <cmath>
#include <vector>

#include "core/audio.h"
#include "editing/vocal_edit/types.h"
#include "feature/pitch.h"
#include "util/constants.h"

namespace sonare::editing::vocal_edit {

/// @brief Fractional MIDI note number of @p hz (A4 = 440 Hz reference).
inline double midi_from_hz(double hz) {
  return sonare::constants::kMidiA4 +
         sonare::constants::kSemitonesPerOctave * std::log2(hz / sonare::constants::kA4Hz);
}

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
/// Notes are cut on the persisted segmentation settings and have disjoint,
/// source-clamped sample spans. Their IDs start at one; the session owns the
/// high-water mark after this function returns.
///
/// Segmentation and measurement read a per-frame F0 refined on the source
/// audio within a quarter semitone of the analysis value, so a note's centre is
/// not held to the analysis pitch grid; the analysis arrays themselves are
/// never rewritten.
std::vector<VocalNote> extract_vocal_notes(const Audio& source, const VocalAnalysisData& analysis);

/// @brief Measures one note through the same absolute-origin path used by
/// extraction, split and merge operations.
///
/// centre_midi is the mean of the refined contour over a whole number of
/// vibrato cycles, excluding the glide frames at either end; median_hz is the
/// same centre in hertz.
VocalNote measure_vocal_note(const Audio& source, const VocalAnalysisData& analysis, VocalNoteId id,
                             SampleRange source_range);

/// @brief Computes a digest for the analysis values and descriptors.
Sha256Digest digest_analysis(const VocalAnalysisData& analysis);

}  // namespace sonare::editing::vocal_edit
