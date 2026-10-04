#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "editing/vocal_edit/analysis.h"
#include "editing/vocal_edit/pitch_plan.h"
#include "editing/vocal_edit/types.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using sonare::constants::kA4Hz;
using sonare::constants::kMidiA4;
using sonare::constants::kSemitonesPerOctave;
using sonare::editing::vocal_edit::AnalysisGrid;
using sonare::editing::vocal_edit::compile_pitch_plan;
using sonare::editing::vocal_edit::compile_pitch_plans;
using sonare::editing::vocal_edit::PitchTargetMode;
using sonare::editing::vocal_edit::PitchTransition;
using sonare::editing::vocal_edit::RenderSettings;
using sonare::editing::vocal_edit::VocalAnalysisData;
using sonare::editing::vocal_edit::VocalEditState;
using sonare::editing::vocal_edit::VocalNote;

namespace {

VocalAnalysisData analysis(std::vector<float> f0, std::vector<uint8_t> voiced, double origin = 0.0,
                           double cadence = 100.0) {
  VocalAnalysisData result;
  result.grid.frame_origin_sample = origin;
  result.grid.samples_per_frame = 48000.0 / cadence;
  result.grid.frame_length_samples = 2048;
  result.f0_hz = std::move(f0);
  result.voiced = std::move(voiced);
  result.amplitude.assign(result.f0_hz.size(), 0.25f);
  result.algorithm_id = "host";
  result.algorithm_version = 1;
  return result;
}

VocalNote note(uint32_t start, uint32_t end, double centre_midi = 60.0) {
  VocalNote result;
  result.id = 1;
  result.source_range = {static_cast<int64_t>(start * 480), static_cast<int64_t>(end * 480)};
  result.analysis_frame_start = start;
  result.analysis_frame_end = end;
  result.has_pitch = true;
  result.centre_midi = centre_midi;
  result.median_hz = kA4Hz * std::pow(2.0, (centre_midi - kMidiA4) / kSemitonesPerOctave);
  result.f0_stability = 1.0;
  result.edit = sonare::editing::vocal_edit::VocalNoteEdit::identity_for(result.source_range);
  return result;
}

}  // namespace

TEST_CASE("pitch plan keeps a centred note identity bit exact", "[vocal_pitch_plan]") {
  const VocalAnalysisData track(
      analysis({261.62555f, 261.62555f, 0.0f, 261.62555f}, {1, 1, 0, 1}, 37.5, 100.0));
  const auto plan = compile_pitch_plan(track, note(0, 4), RenderSettings{}, 48000);

  REQUIRE(plan.points.size() == 4);
  CHECK(plan.pitch_identity);
  CHECK(plan.points[0].voiced);
  CHECK_FALSE(plan.points[2].voiced);
  CHECK_THAT(plan.points[0].delta_semitones, WithinAbs(0.0f, 1.0e-5f));
  CHECK_THAT(plan.points[2].delta_semitones, WithinAbs(0.0f, 1.0e-5f));
}

TEST_CASE("pitch plan centre target preserves measured drift and vibrato", "[vocal_pitch_plan]") {
  const double frame_rate = 100.0;
  const float c4 =
      static_cast<float>(kA4Hz * std::pow(2.0, (60.0 - kMidiA4) / kSemitonesPerOctave));
  const VocalAnalysisData track =
      analysis({c4, c4 * 1.029302f, c4 * 0.971532f}, {1, 1, 1}, 0.0, frame_rate);
  VocalNote edited = note(0, 3, 60.0);
  edited.edit.pitch.target.mode = PitchTargetMode::kCenter;
  edited.edit.pitch.target.center_midi = 62.0;
  edited.edit.pitch.amount = 1.0;
  const auto plan = compile_pitch_plan(track, edited, RenderSettings{}, 48000);

  REQUIRE(plan.points.size() == 3);
  CHECK_FALSE(plan.pitch_identity);
  CHECK_THAT(plan.points[0].target_midi, WithinAbs(62.0, 1.0e-4));
  CHECK_THAT(plan.points[0].effective_midi, WithinAbs(62.0, 1.0e-3));
  CHECK(plan.points[1].effective_midi != plan.points[0].effective_midi);
}

TEST_CASE("pitch plan curve uses absolute source samples and amount", "[vocal_pitch_plan]") {
  const float c4 =
      static_cast<float>(kA4Hz * std::pow(2.0, (60.0 - kMidiA4) / kSemitonesPerOctave));
  const VocalAnalysisData track = analysis({c4, c4, c4, c4}, {1, 1, 1, 1}, 12.5, 100.0);
  VocalNote edited = note(0, 4, 60.0);
  edited.edit.pitch.target.mode = PitchTargetMode::kCurve;
  edited.edit.pitch.target.points = {{0.0, 60.0}, {1920.0, 64.0}};
  edited.edit.pitch.amount = 0.5;
  edited.edit.pitch.max_correction_semitones = 12.0;
  const auto plan = compile_pitch_plan(track, edited, RenderSettings{}, 48000);

  REQUIRE(plan.points.size() == 4);
  CHECK_THAT(plan.points[0].target_midi, WithinAbs(60.0260417, 1.0e-5));
  CHECK_THAT(plan.points[3].target_midi, WithinAbs(63.0260417, 1.0e-5));
  CHECK_THAT(
      plan.points[3].effective_midi,
      WithinAbs(plan.points[3].measured_midi + 0.5 * (plan.points[3].target_midi - 60.0), 1.0e-3));
}

TEST_CASE("pitch plan amount zero disables target correction but keeps transpose",
          "[vocal_pitch_plan]") {
  const float c4 =
      static_cast<float>(kA4Hz * std::pow(2.0, (60.0 - kMidiA4) / kSemitonesPerOctave));
  const VocalAnalysisData track = analysis({c4, c4}, {1, 1});
  VocalNote edited = note(0, 2, 60.0);
  edited.edit.pitch.target.mode = PitchTargetMode::kCenter;
  edited.edit.pitch.target.center_midi = 67.0;
  edited.edit.pitch.amount = 0.0;
  edited.edit.pitch.transpose_semitones = 1.5;
  const auto plan = compile_pitch_plan(track, edited, RenderSettings{}, 48000);

  REQUIRE(plan.points.size() == 2);
  CHECK_THAT(plan.points[0].effective_midi, WithinAbs(61.5, 1.0e-3));
  CHECK_THAT(plan.points[0].delta_semitones, WithinAbs(1.5f, 1.0e-3f));
}

TEST_CASE("pitch plan speed resets at an unvoiced island", "[vocal_pitch_plan]") {
  const float c4 =
      static_cast<float>(kA4Hz * std::pow(2.0, (60.0 - kMidiA4) / kSemitonesPerOctave));
  const VocalAnalysisData track = analysis({c4, c4, 0.0f, c4, c4}, {1, 1, 0, 1, 1});
  VocalNote edited = note(0, 5, 60.0);
  edited.edit.pitch.target.mode = PitchTargetMode::kCenter;
  edited.edit.pitch.target.center_midi = 64.0;
  edited.edit.pitch.amount = 1.0;
  edited.edit.pitch.speed_ms = 100.0;
  const auto plan = compile_pitch_plan(track, edited, RenderSettings{}, 48000);

  REQUIRE(plan.points.size() == 5);
  CHECK(plan.points[1].effective_midi < 64.0);
  CHECK(plan.points[3].effective_midi < 64.0);
  CHECK_THAT(plan.points[2].delta_semitones, WithinAbs(0.0f, 1.0e-5f));
}

TEST_CASE("pitch plan applies correction limit and exposes diagnostics", "[vocal_pitch_plan]") {
  const float c4 =
      static_cast<float>(kA4Hz * std::pow(2.0, (60.0 - kMidiA4) / kSemitonesPerOctave));
  const VocalAnalysisData track = analysis({c4}, {1});
  VocalNote edited = note(0, 1, 60.0);
  edited.edit.pitch.target.mode = PitchTargetMode::kCenter;
  edited.edit.pitch.target.center_midi = 72.0;
  edited.edit.pitch.amount = 1.0;
  edited.edit.pitch.max_correction_semitones = 2.0;
  const auto plan = compile_pitch_plan(track, edited, RenderSettings{}, 48000);

  REQUIRE(plan.points.size() == 1);
  CHECK_THAT(plan.points[0].delta_semitones, WithinAbs(2.0f, 1.0e-5f));
  CHECK(plan.diagnostics.limited_correction_frames == 1);
}

TEST_CASE("pitch transitions bridge neighbouring raw deltas in destination time",
          "[vocal_pitch_plan]") {
  const float c4 =
      static_cast<float>(kA4Hz * std::pow(2.0, (60.0 - kMidiA4) / kSemitonesPerOctave));
  const VocalAnalysisData track =
      analysis({c4, c4, c4, c4, c4, c4, c4, c4}, {1, 1, 1, 1, 1, 1, 1, 1});
  VocalNote left = note(0, 4, 60.0);
  VocalNote right = note(4, 8, 60.0);
  right.id = 2;
  right.edit = sonare::editing::vocal_edit::VocalNoteEdit::identity_for(right.source_range);
  left.edit.pitch.transpose_semitones = 2.0;
  right.edit.pitch.transpose_semitones = 4.0;

  VocalEditState state;
  state.notes = {left, right};
  state.transitions.push_back(PitchTransition{1, 2, 480, 480, 1.0});
  const auto plans =
      sonare::editing::vocal_edit::compile_pitch_plans(track, state, RenderSettings{}, 48000);

  REQUIRE(plans.size() == 2);
  // At the declared boundary t=1/2, the bridge is halfway between the two
  // outer endpoint corrections. It must retain the neighbouring correction,
  // rather than attenuating either note toward zero.
  CHECK(plans[0].points[3].delta_semitones >= 1.99f);
  CHECK(plans[1].points[0].delta_semitones > 2.0f);
  CHECK(plans[1].points[0].delta_semitones < 4.0f);
}

TEST_CASE("pitch evaluation reports the narrowed float delta", "[vocal_pitch_plan]") {
  const float c4 =
      static_cast<float>(kA4Hz * std::pow(2.0, (60.0 - kMidiA4) / kSemitonesPerOctave));
  const VocalAnalysisData track = analysis({c4}, {1});
  VocalNote edited = note(0, 1, 60.0);
  edited.edit.pitch.transpose_semitones = 0.1;
  const auto plan = compile_pitch_plan(track, edited, RenderSettings{}, 48000);

  REQUIRE(plan.points.size() == 1);
  CHECK(plan.points[0].effective_midi ==
        plan.points[0].measured_midi + static_cast<double>(plan.points[0].delta_semitones));
}

TEST_CASE("identity pitch deltas use positive zero after transitions", "[vocal_pitch_plan]") {
  const float c4 =
      static_cast<float>(kA4Hz * std::pow(2.0, (60.0 - kMidiA4) / kSemitonesPerOctave));
  const VocalAnalysisData track = analysis(std::vector<float>(8, c4), std::vector<uint8_t>(8, 1));
  VocalNote left = note(0, 4, 60.0);
  VocalNote right = note(4, 8, 60.0);
  right.id = 2;
  VocalEditState state;
  state.notes = {left, right};
  state.transitions.push_back(PitchTransition{1, 2, 480, 480, 0.0});

  const auto plans = compile_pitch_plans(track, state, RenderSettings{}, 48000);
  REQUIRE(plans.size() == 2);
  for (const auto& plan : plans) {
    CHECK(plan.pitch_identity);
    for (const auto& point : plan.points) {
      if (point.voiced) CHECK_FALSE(std::signbit(point.delta_semitones));
    }
  }
}

TEST_CASE("successive transitions use immutable pre-transition neighbour deltas",
          "[vocal_pitch_plan]") {
  const float c4 =
      static_cast<float>(kA4Hz * std::pow(2.0, (60.0 - kMidiA4) / kSemitonesPerOctave));
  const VocalAnalysisData track = analysis(std::vector<float>(12, c4), std::vector<uint8_t>(12, 1));
  VocalNote left = note(0, 4, 60.0);
  VocalNote middle = note(4, 8, 60.0);
  VocalNote right = note(8, 12, 60.0);
  right.id = 3;
  middle.id = 2;
  left.edit.pitch.transpose_semitones = 2.0;
  middle.edit.pitch.transpose_semitones = 4.0;
  right.edit.pitch.transpose_semitones = 8.0;

  VocalEditState state;
  state.notes = {left, middle, right};
  // The windows exactly abut inside the middle note. The incoming window
  // therefore changes a preceding sample point, but must not change the
  // outgoing transition's outer raw endpoint after interpolation.
  state.transitions.push_back(PitchTransition{1, 2, 840, 840, 1.0});
  state.transitions.push_back(PitchTransition{2, 3, 1080, 840, 1.0});
  const auto plans = compile_pitch_plans(track, state, RenderSettings{}, 48000);

  REQUIRE(plans.size() == 3);
  // Outgoing window starts at destination 2760. The raw middle delta is 4,
  // the right delta is 8, and the middle point at 2880 is t=1/16.
  const double t = 1.0 / 16.0;
  const double expected = 4.0 + (8.0 - 4.0) * (t * t * (3.0 - 2.0 * t));
  CHECK_THAT(plans[1].points[2].delta_semitones, WithinAbs(expected, 1.0e-5));
}
