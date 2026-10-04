#pragma once

/// @file pitch_plan.h
/// @brief Pure compilation of an editable note into per-frame pitch deltas.

#include <cstdint>
#include <vector>

#include "editing/vocal_edit/types.h"

namespace sonare::editing::vocal_edit {

struct PitchEvaluationPoint {
  double source_sample = 0.0;
  double measured_midi = 0.0;
  double target_midi = 0.0;
  double effective_midi = 0.0;
  bool voiced = false;
  bool has_target = false;
  float delta_semitones = 0.0f;
};

struct PitchPlanDiagnostics {
  uint64_t limited_correction_frames = 0;
  uint64_t dry_passed_frames = 0;
};

struct CompiledPitchPlan {
  VocalNoteId note_id = kInvalidVocalNoteId;
  uint32_t analysis_frame_start = 0;
  std::vector<PitchEvaluationPoint> points;
  bool pitch_identity = true;
  PitchPlanDiagnostics diagnostics{};
};

using PitchPlan = CompiledPitchPlan;

double source_sample_to_destination_sample(const VocalNote& note, double source_sample);
double destination_sample_to_source_sample(const VocalNote& note, double destination_sample);

/// @brief Compiles the exact delta that a renderer should pass to its DSP.
///
/// This single-note form intentionally produces a pre-transition plan. The
/// multi-note overload below is the only entry point that composes neighbour
/// transitions because it has both raw endpoint deltas available.
CompiledPitchPlan compile_pitch_plan(const VocalAnalysisData& analysis, const VocalNote& note,
                                     const RenderSettings& settings, uint32_t source_sample_rate);

std::vector<CompiledPitchPlan> compile_pitch_plans(const VocalAnalysisData& analysis,
                                                   const VocalEditState& state,
                                                   const RenderSettings& settings,
                                                   uint32_t source_sample_rate);

}  // namespace sonare::editing::vocal_edit
