// SONARE_WASM_EXCEPTION_UNWIND: release compiled pitch curves when validation throws.
#include "editing/vocal_edit/pitch_plan.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "editing/note_model/note_object.h"
#include "editing/note_model/pitch_decomposition.h"
#include "util/constants.h"

namespace sonare::editing::vocal_edit {

using sonare::constants::kA4Hz;
using sonare::constants::kCentsPerSemitone;
using sonare::constants::kMidiA4;
using sonare::constants::kSemitonesPerOctave;

namespace {

[[noreturn]] void invalid(const std::string& field, const std::string& message) {
  throw VocalEditException(VocalReason::kInvalidInput, message, field);
}

double hz_to_midi(double hz) { return kMidiA4 + kSemitonesPerOctave * std::log2(hz / kA4Hz); }

void validate_pitch(const VocalPitchEdit& pitch, const VocalNote& note) {
  const auto finite = [](double value) { return std::isfinite(value); };
  if (!finite(pitch.amount) || pitch.amount < 0.0 || pitch.amount > 1.0) {
    invalid("pitch.amount", "must be finite and in [0, 1]");
  }
  if (!finite(pitch.speed_ms) || pitch.speed_ms < 0.0)
    invalid("pitch.speed_ms", "must be finite and >= 0");
  if (!finite(pitch.max_correction_semitones) || pitch.max_correction_semitones < 0.0) {
    invalid("pitch.max_correction_semitones", "must be finite and >= 0");
  }
  if (!finite(pitch.transpose_semitones)) invalid("pitch.transpose_semitones", "must be finite");
  if (!finite(pitch.drift_scale) || pitch.drift_scale < 0.0)
    invalid("pitch.drift_scale", "must be finite and >= 0");
  if (!finite(pitch.vibrato_scale) || pitch.vibrato_scale < 0.0) {
    invalid("pitch.vibrato_scale", "must be finite and >= 0");
  }
  if (pitch.target.mode == PitchTargetMode::kCenter) {
    if (!finite(pitch.target.center_midi) || pitch.target.center_midi < 0.0 ||
        pitch.target.center_midi > 127.0) {
      invalid("pitch.target.center_midi", "must be finite and in [0, 127]");
    }
  } else if (pitch.target.mode == PitchTargetMode::kCurve) {
    if (pitch.target.points.size() < 2)
      invalid("pitch.target.points", "a curve needs at least two points");
    double previous = -std::numeric_limits<double>::infinity();
    for (const auto& point : pitch.target.points) {
      if (!finite(point.source_sample) || !finite(point.target_midi) || point.target_midi < 0.0 ||
          point.target_midi > 127.0) {
        invalid("pitch.target.points", "curve points must be finite and MIDI must be in [0, 127]");
      }
      if (!(point.source_sample > previous))
        invalid("pitch.target.points", "source samples must be strictly increasing");
      previous = point.source_sample;
    }
    if (pitch.target.points.front().source_sample != static_cast<double>(note.source_range.start) ||
        pitch.target.points.back().source_sample != static_cast<double>(note.source_range.end)) {
      invalid("pitch.target.points", "curve endpoints must equal the source note range");
    }
  } else if (pitch.target.mode != PitchTargetMode::kNone) {
    invalid("pitch.target.mode", "unknown pitch target mode");
  }
}

}  // namespace

double pitch_target_at(const VocalPitchTarget& target, double source_sample, double fallback) {
  if (target.mode == PitchTargetMode::kNone) return fallback;
  if (target.mode == PitchTargetMode::kCenter) return target.center_midi;
  if (target.points.empty()) return fallback;
  return interpolate_sorted_points(target.points, source_sample,
                                   [](const VocalPitchPoint& point) { return point.target_midi; });
}

namespace {

CompiledPitchPlan compile_pitch_plan_at_rate(const VocalAnalysisData& analysis,
                                             const VocalNote& note, const RenderSettings& settings,
                                             uint32_t sample_rate) {
  if (note.id == kInvalidVocalNoteId) invalid("note.id", "must be non-zero");
  if (note.source_range.start < 0 || note.source_range.end <= note.source_range.start ||
      note.edit.destination_start_sample < 0 || note.edit.destination_length_samples <= 0) {
    invalid("note.range", "note source and destination ranges must be positive");
  }
  if (!std::isfinite(analysis.grid.frame_origin_sample) ||
      !std::isfinite(analysis.grid.samples_per_frame) || analysis.grid.samples_per_frame < 1.0 ||
      analysis.grid.frame_length_samples == 0 || analysis.f0_hz.empty() ||
      analysis.voiced.size() != analysis.f0_hz.size()) {
    invalid("analysis", "analysis grid and arrays are malformed");
  }
  if (note.analysis_frame_end <= note.analysis_frame_start ||
      note.analysis_frame_end > analysis.f0_hz.size() ||
      note.analysis_frame_end > analysis.voiced.size()) {
    invalid("note.analysis_frame", "note frame range is outside analysis");
  }
  if (sample_rate == 0) invalid("sample_rate", "must be non-zero");
  if (settings.profile != RenderProfile::kVocalPsolaV1 || settings.algorithm_version != 1) {
    invalid("render_settings", "render settings are unsupported");
  }
  bool has_voiced_frame = false;
  for (size_t index = note.analysis_frame_start; index < note.analysis_frame_end; ++index) {
    if (analysis.voiced[index] != 0 && analysis.voiced[index] != 1) {
      invalid("analysis.voiced", "voiced values must be exactly zero or one");
    }
    if (analysis.voiced[index] != 0 &&
        (!std::isfinite(analysis.f0_hz[index]) || analysis.f0_hz[index] <= 0.0f)) {
      invalid("analysis.f0_hz", "voiced frames need positive finite F0");
    }
    has_voiced_frame = has_voiced_frame || analysis.voiced[index] != 0;
  }
  if (note.has_pitch != has_voiced_frame)
    invalid("note.has_pitch", "does not match analysis voiced frames");
  if (note.has_pitch && (!std::isfinite(note.median_hz) || note.median_hz <= 0.0 ||
                         !std::isfinite(note.centre_midi))) {
    invalid("note.measurement", "pitched note measurements are malformed");
  }
  validate_pitch(note.edit.pitch, note);
  if (!std::isfinite(settings.vibrato_cutoff_hz) || settings.vibrato_cutoff_hz <= 0.0) {
    invalid("render_settings.vibrato_cutoff_hz", "must be finite and > 0");
  }

  CompiledPitchPlan plan;
  plan.note_id = note.id;
  plan.analysis_frame_start = note.analysis_frame_start;
  const size_t frame_count =
      static_cast<size_t>(note.analysis_frame_end - note.analysis_frame_start);
  plan.points.reserve(frame_count);

  std::vector<bool> voiced;
  voiced.reserve(frame_count);
  for (uint32_t frame = note.analysis_frame_start; frame < note.analysis_frame_end; ++frame) {
    const size_t index = static_cast<size_t>(frame);
    const bool is_voiced = analysis.voiced[index] != 0 && analysis.f0_hz[index] > 0.0f &&
                           std::isfinite(analysis.f0_hz[index]);
    voiced.push_back(is_voiced);
  }

  note_model::NoteObject decomposition_note;
  decomposition_note.median_hz = static_cast<float>(note.median_hz);
  decomposition_note.frame_start = static_cast<int>(note.analysis_frame_start);
  decomposition_note.frame_end = static_cast<int>(note.analysis_frame_end);
  decomposition_note.f0_hz.frame_rate_hz =
      static_cast<float>(static_cast<double>(sample_rate) / analysis.grid.samples_per_frame);
  decomposition_note.f0_hz.frame_offset = static_cast<int>(note.analysis_frame_start);
  decomposition_note.f0_hz.values.assign(
      analysis.f0_hz.begin() + static_cast<std::ptrdiff_t>(note.analysis_frame_start),
      analysis.f0_hz.begin() + static_cast<std::ptrdiff_t>(note.analysis_frame_end));
  const auto decomposition = note_model::decompose_pitch(
      decomposition_note,
      note_model::PitchDecompositionConfig{static_cast<float>(settings.vibrato_cutoff_hz)});
  std::vector<double> drift_cents(frame_count, 0.0);
  std::vector<double> vibrato_cents(frame_count, 0.0);
  if (decomposition.drift.size() == frame_count && decomposition.vibrato.size() == frame_count) {
    for (size_t i = 0; i < frame_count; ++i) {
      drift_cents[i] = decomposition.drift[i];
      vibrato_cents[i] = decomposition.vibrato[i];
    }
  }

  const double cadence_hz = static_cast<double>(sample_rate) / analysis.grid.samples_per_frame;
  const double frame_seconds = 1.0 / cadence_hz;
  double correction_state = 0.0;
  bool have_correction_state = false;
  bool previous_voiced = false;
  for (size_t offset = 0; offset < frame_count; ++offset) {
    const uint32_t frame = note.analysis_frame_start + static_cast<uint32_t>(offset);
    const size_t index = static_cast<size_t>(frame);
    const double source_sample = analysis.grid.frame_origin_sample +
                                 static_cast<double>(frame) * analysis.grid.samples_per_frame;
    if (!std::isfinite(source_sample)) invalid("analysis.grid", "frame position is not finite");
    const bool is_voiced = voiced[offset];
    PitchEvaluationPoint point;
    point.source_sample = source_sample;
    point.voiced = is_voiced;
    if (!is_voiced) {
      point.measured_midi = 0.0;
      point.target_midi = 0.0;
      point.effective_midi = 0.0;
      point.delta_semitones = 0.0f;
      point.has_target = false;
      correction_state = 0.0;
      have_correction_state = false;
      previous_voiced = false;
      plan.points.push_back(point);
      continue;
    }

    const double measured = hz_to_midi(static_cast<double>(analysis.f0_hz[index]));
    const double drift = drift_cents[offset] / kCentsPerSemitone;
    const double vibrato = vibrato_cents[offset] / kCentsPerSemitone;
    const double base = note.centre_midi + note.edit.pitch.drift_scale * drift +
                        note.edit.pitch.vibrato_scale * vibrato;
    const double target = pitch_target_at(note.edit.pitch.target, source_sample, base);
    point.measured_midi = measured;
    point.has_target = note.edit.pitch.target.mode != PitchTargetMode::kNone;
    point.target_midi = point.has_target ? target : 0.0;

    double raw_correction = 0.0;
    if (point.has_target && note.edit.pitch.amount > 0.0 &&
        note.edit.pitch.max_correction_semitones > 0.0) {
      // A centre target keeps drift/vibrato; an absolute curve corrects from the scaled base.
      const double requested = note.edit.pitch.target.mode == PitchTargetMode::kCenter
                                   ? target - note.centre_midi
                                   : target - base;
      const double limit = note.edit.pitch.max_correction_semitones;
      const double bounded = std::clamp(requested, -limit, limit);
      if (bounded != requested) ++plan.diagnostics.limited_correction_frames;
      raw_correction = note.edit.pitch.amount * bounded;
    }
    double smoothed = raw_correction;
    if (note.edit.pitch.speed_ms > 0.0) {
      const double alpha = 1.0 - std::exp(-frame_seconds / (note.edit.pitch.speed_ms * 0.001));
      if (!previous_voiced || !have_correction_state) correction_state = 0.0;
      correction_state += alpha * (raw_correction - correction_state);
      smoothed = correction_state;
      have_correction_state = true;
    } else {
      correction_state = raw_correction;
      have_correction_state = true;
    }
    previous_voiced = true;

    double delta = base - measured + smoothed + note.edit.pitch.transpose_semitones;
    // Float-cents decomposition and float F0 can differ by ulps; force the identity exactly.
    const bool target_correction_disabled =
        note.edit.pitch.target.mode == PitchTargetMode::kNone || note.edit.pitch.amount == 0.0;
    if (target_correction_disabled && note.edit.pitch.drift_scale == 1.0 &&
        note.edit.pitch.vibrato_scale == 1.0 && note.edit.pitch.transpose_semitones == 0.0) {
      delta = 0.0;
    }
    // Canonicalize -0.0 to the +0.0 bit pattern pitch_identity and the bypass compare.
    if (delta == 0.0) delta = 0.0;
    if (!std::isfinite(delta) || delta < -static_cast<double>(std::numeric_limits<float>::max()) ||
        delta > static_cast<double>(std::numeric_limits<float>::max())) {
      invalid("pitch.delta", "compiled pitch delta is not representable");
    }
    point.delta_semitones = static_cast<float>(delta);
    // Report the narrowed float the renderer consumes, not the pre-narrowing double.
    point.effective_midi = measured + static_cast<double>(point.delta_semitones);
    uint32_t delta_bits = 0;
    std::memcpy(&delta_bits, &point.delta_semitones, sizeof(delta_bits));
    if (delta_bits != 0u) plan.pitch_identity = false;
    plan.points.push_back(point);
  }
  return plan;
}

}  // namespace

double source_sample_to_destination_sample(const VocalNote& note, double source_sample) {
  if (note.source_range.length() <= 0 || note.edit.destination_length_samples <= 0) {
    invalid("note.range", "note ranges must be positive");
  }
  if (!std::isfinite(source_sample) ||
      source_sample < static_cast<double>(note.source_range.start) ||
      source_sample > static_cast<double>(note.source_range.end)) {
    invalid("source_sample", "outside note source range");
  }
  if (source_sample == static_cast<double>(note.source_range.start)) {
    return static_cast<double>(note.edit.destination_start_sample);
  }
  if (source_sample == static_cast<double>(note.source_range.end)) {
    return static_cast<double>(note.edit.destination_start_sample) +
           static_cast<double>(note.edit.destination_length_samples);
  }
  return static_cast<double>(note.edit.destination_start_sample) +
         (source_sample - static_cast<double>(note.source_range.start)) /
             static_cast<double>(note.source_range.length()) *
             static_cast<double>(note.edit.destination_length_samples);
}

double destination_sample_to_source_sample(const VocalNote& note, double destination_sample) {
  if (note.source_range.length() <= 0 || note.edit.destination_length_samples <= 0) {
    invalid("note.range", "note ranges must be positive");
  }
  const double destination_start = static_cast<double>(note.edit.destination_start_sample);
  const double destination_end =
      destination_start + static_cast<double>(note.edit.destination_length_samples);
  if (!std::isfinite(destination_sample) || destination_sample < destination_start ||
      destination_sample > destination_end) {
    invalid("destination_sample", "outside note destination range");
  }
  if (destination_sample == destination_start) return static_cast<double>(note.source_range.start);
  if (destination_sample == destination_end) return static_cast<double>(note.source_range.end);
  return static_cast<double>(note.source_range.start) +
         (destination_sample - destination_start) /
             static_cast<double>(note.edit.destination_length_samples) *
             static_cast<double>(note.source_range.length());
}

CompiledPitchPlan compile_pitch_plan(const VocalAnalysisData& analysis, const VocalNote& note,
                                     const RenderSettings& settings, uint32_t source_sample_rate) {
  return compile_pitch_plan_at_rate(analysis, note, settings, source_sample_rate);
}

std::vector<CompiledPitchPlan> compile_pitch_plans(const VocalAnalysisData& analysis,
                                                   const VocalEditState& state,
                                                   const RenderSettings& settings,
                                                   uint32_t source_sample_rate) {
  std::vector<CompiledPitchPlan> plans;
  plans.reserve(state.notes.size());
  for (const auto& note : state.notes) {
    plans.push_back(compile_pitch_plan_at_rate(analysis, note, settings, source_sample_rate));
  }

  // Bridge from the untouched plans so adjacent transitions stay order independent.
  const std::vector<CompiledPitchPlan> pre_transition_plans = plans;

  // Bridge the endpoint deltas across the boundary; scaling a correction toward zero would dip.
  const auto pre_plan_for = [&](VocalNoteId id) -> const CompiledPitchPlan* {
    for (const auto& plan : pre_transition_plans) {
      if (plan.note_id == id) return &plan;
    }
    return nullptr;
  };
  const auto output_plan_for = [&](VocalNoteId id) -> CompiledPitchPlan* {
    for (auto& plan : plans) {
      if (plan.note_id == id) return &plan;
    }
    return nullptr;
  };
  const auto note_for = [&](VocalNoteId id) -> const VocalNote* {
    for (const auto& note : state.notes) {
      if (note.id == id) return &note;
    }
    return nullptr;
  };
  const auto raw_delta_at_destination = [](const CompiledPitchPlan& plan, const VocalNote& note,
                                           double destination) {
    if (plan.points.empty()) return 0.0;
    const double source = destination_sample_to_source_sample(note, destination);
    return interpolate_sorted_points(plan.points, source, [](const PitchEvaluationPoint& point) {
      return static_cast<double>(point.delta_semitones);
    });
  };
  const auto narrow_delta = [](double value) {
    if (!std::isfinite(value) || value < -static_cast<double>(std::numeric_limits<float>::max()) ||
        value > static_cast<double>(std::numeric_limits<float>::max())) {
      invalid("pitch.delta", "transition delta is not representable");
    }
    const float narrowed = static_cast<float>(value);
    return narrowed == 0.0f ? 0.0f : narrowed;
  };

  for (const auto& transition : state.transitions) {
    const CompiledPitchPlan* left_plan = pre_plan_for(transition.left_note_id);
    const CompiledPitchPlan* right_plan = pre_plan_for(transition.right_note_id);
    CompiledPitchPlan* left_output = output_plan_for(transition.left_note_id);
    CompiledPitchPlan* right_output = output_plan_for(transition.right_note_id);
    const VocalNote* left_note = note_for(transition.left_note_id);
    const VocalNote* right_note = note_for(transition.right_note_id);
    if (!left_plan || !right_plan || !left_output || !right_output || !left_note || !right_note) {
      invalid("transition.note_id", "transition references a note without a pitch plan");
    }
    const double boundary = static_cast<double>(left_note->edit.destination_start_sample) +
                            static_cast<double>(left_note->edit.destination_length_samples);
    const double left_outer = boundary - static_cast<double>(transition.left_window_samples);
    const double right_outer = boundary + static_cast<double>(transition.right_window_samples);
    const double span = right_outer - left_outer;
    if (!(span > 0.0)) invalid("transition", "transition window has no duration");
    const double left_delta = raw_delta_at_destination(*left_plan, *left_note, left_outer);
    const double right_delta = raw_delta_at_destination(*right_plan, *right_note, right_outer);
    const double strength = transition.strength;
    const auto apply_bridge = [&](CompiledPitchPlan& plan, const VocalNote& note) {
      for (auto& point : plan.points) {
        if (!point.voiced) continue;
        // A frame within the half-sample bracketing tolerance can sit just outside the range.
        const double source =
            std::clamp(point.source_sample, static_cast<double>(note.source_range.start),
                       static_cast<double>(note.source_range.end));
        const double destination = source_sample_to_destination_sample(note, source);
        if (destination < left_outer || destination > right_outer) continue;
        const double t = std::clamp((destination - left_outer) / span, 0.0, 1.0);
        const double smooth = t * t * (3.0 - 2.0 * t);
        const double bridge = left_delta + (right_delta - left_delta) * smooth;
        const double final_delta = static_cast<double>(point.delta_semitones) +
                                   strength * (bridge - static_cast<double>(point.delta_semitones));
        point.delta_semitones = narrow_delta(final_delta);
        point.effective_midi = point.measured_midi + static_cast<double>(point.delta_semitones);
      }
    };
    apply_bridge(*left_output, *left_note);
    apply_bridge(*right_output, *right_note);
  }
  for (auto& plan : plans) {
    plan.pitch_identity = true;
    for (const auto& point : plan.points) {
      uint32_t bits = 0;
      std::memcpy(&bits, &point.delta_semitones, sizeof(bits));
      if (bits != 0u) {
        plan.pitch_identity = false;
        break;
      }
    }
  }
  return plans;
}

}  // namespace sonare::editing::vocal_edit
