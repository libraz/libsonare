#include <catch2/catch_test_macros.hpp>

#if defined(SONARE_WITH_ARRANGEMENT)

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "arrangement/edit_command.h"
#include "arrangement/edit_compiler.h"
#include "arrangement/edit_model.h"
#include "automation/automation_lane.h"
#include "automation/parameter.h"
#include "engine/realtime_engine.h"
#include "engine/track_mixer.h"
#include "mixing/api/scene.h"
#include "rt/command.h"

namespace {

namespace arr = sonare::arrangement;
using sonare::automation::AutomationLane;
using sonare::automation::AutomationTargetIdentity;
using sonare::automation::AutomationTargetKind;
using sonare::automation::Breakpoint;
using sonare::automation::CurveType;
using sonare::engine::RealtimeEngine;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;
constexpr int kRenderFrames = 8192;
constexpr char kSceneStripId[] = "lane-width-scene";
constexpr char kSceneSendId[] = "room-send";
constexpr char kSceneBusId[] = "room";

enum class LaneTarget { None, Width, Insert, Send };

arr::CompiledTimeline compile_timeline(bool antiphase, bool bind_scene_strip,
                                       LaneTarget lane_target, float lane_value) {
  arr::Project project;
  arr::MidiContentStore midi;
  arr::AudioContentStore audio;
  project.set_sample_rate(kSampleRate);
  project.set_tempo_segments({{0.0, 120.0, 0.0}});
  project.set_time_signatures({{0.0, {4, 4}}});

  arr::AudioSourceRef source_ref;
  source_ref.sample_rate_hint = kSampleRate;
  source_ref.channel_count = 2;
  const arr::SourceId source_id = project.add_audio_source(source_ref);
  arr::AudioSourceSamples samples;
  samples.sample_rate = kSampleRate;
  samples.channels.emplace_back(12000, 0.25f);
  samples.channels.emplace_back(12000, antiphase ? -0.25f : 0.25f);
  audio.sources.emplace(source_id, std::move(samples));

  arr::Track track;
  track.name = "track";
  track.kind = arr::Track::Kind::kAudio;
  if (bind_scene_strip) track.channel_strip_ref = kSceneStripId;
  const arr::TrackId track_id = project.add_track(std::move(track));

  if (bind_scene_strip) {
    sonare::mixing::api::Strip strip;
    strip.id = kSceneStripId;
    if (lane_target == LaneTarget::Send) {
      strip.sends.push_back({kSceneSendId, kSceneBusId});
      project.scene().buses.emplace_back(kSceneBusId);
    } else {
      strip.inserts.emplace_back(sonare::mixing::api::InsertSlot::PreFader, "utility.gain",
                                 R"({"levelDb":0})");
    }
    project.scene().strips.push_back(std::move(strip));
  }

  arr::EditClip clip;
  clip.track_id = track_id;
  clip.source_id = source_id;
  clip.start_ppq = 0.0;
  clip.length_ppq = 2.0;
  REQUIRE(project.add_clip(std::move(clip)) != 0);

  if (lane_target == LaneTarget::Width) {
    AutomationLane lane(0, AutomationTargetKind::kTrackWidth);
    lane.set_points({Breakpoint{0.0, lane_value, CurveType::Hold}});
    REQUIRE(arr::AddAutomationLane(track_id, std::move(lane)).apply(project, midi));
  } else if (lane_target == LaneTarget::Insert) {
    AutomationTargetIdentity identity;
    identity.insert_index = 0;
    identity.processor = "utility.gain";
    identity.param_name = "levelDb";
    AutomationLane lane(0, AutomationTargetKind::kTrackInsertParam, std::move(identity));
    lane.set_points({Breakpoint{0.0, lane_value, CurveType::Hold}});
    REQUIRE(arr::AddAutomationLane(track_id, std::move(lane)).apply(project, midi));
  } else if (lane_target == LaneTarget::Send) {
    AutomationTargetIdentity identity;
    identity.send_id = kSceneSendId;
    AutomationLane lane(0, AutomationTargetKind::kTrackSendDb, std::move(identity));
    lane.set_points({Breakpoint{0.0, lane_value, CurveType::Hold}});
    REQUIRE(arr::AddAutomationLane(track_id, std::move(lane)).apply(project, midi));
  }

  arr::CompileResult result = arr::compile(project, midi, audio);
  REQUIRE_FALSE(result.has_errors());
  REQUIRE(result.timeline.has_value());
  return std::move(*result.timeline);
}

std::vector<float> render(const arr::CompiledTimeline& timeline, bool bind_strips) {
  RealtimeEngine engine;
  engine.prepare(kSampleRate, kBlockSize);
  arr::ApplyOptions options;
  options.bind_strips = bind_strips;
  const arr::ApplyResult applied = arr::apply_to_engine(timeline, engine, options);
  REQUIRE(applied.ok());
  engine.prime_offline_parameters(2, kBlockSize);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::vector<float> left(static_cast<std::size_t>(kRenderFrames), 0.0f);
  std::vector<float> right(static_cast<std::size_t>(kRenderFrames), 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.render_offline(channels, 2, kRenderFrames, kBlockSize);

  std::vector<float> interleaved(static_cast<std::size_t>(kRenderFrames) * 2);
  for (int i = 0; i < kRenderFrames; ++i) {
    interleaved[static_cast<std::size_t>(i) * 2] = left[static_cast<std::size_t>(i)];
    interleaved[static_cast<std::size_t>(i) * 2 + 1] = right[static_cast<std::size_t>(i)];
  }
  return interleaved;
}

double channel_rms(const std::vector<float>& interleaved, std::size_t channel) {
  double sum = 0.0;
  for (std::size_t i = channel; i < interleaved.size(); i += 2) {
    sum += static_cast<double>(interleaved[i]) * interleaved[i];
  }
  return std::sqrt(sum / static_cast<double>(interleaved.size() / 2));
}

}  // namespace

TEST_CASE("track width automation keeps unity bit-identical and collapses antiphase audio",
          "[engine][arrangement][track-width]") {
  const arr::CompiledTimeline unity_timeline =
      compile_timeline(true, false, LaneTarget::Width, 1.0f);
  RealtimeEngine engine;
  engine.prepare(kSampleRate, kBlockSize);
  REQUIRE(arr::apply_to_engine(unity_timeline, engine).ok());
  const uint32_t track_id = unity_timeline.track_lanes.front().track_id;
  const int64_t width_id =
      engine.track_lane_automation_id(track_id, sonare::engine::TrackMixerRuntime::kWidth);
  REQUIRE(width_id >= 0);
  sonare::automation::ParameterDescription description;
  REQUIRE(engine.describe_reserved_parameter(static_cast<uint32_t>(width_id), &description));
  REQUIRE(description.name == "width");
  REQUIRE(description.min_value == 0.0f);
  REQUIRE(description.max_value == 2.0f);
  REQUIRE(description.default_value == 1.0f);
  REQUIRE(engine.track_mixer().set_track_parameter(
      track_id, sonare::engine::TrackMixerRuntime::kWidth, 0.0f));
  float width = -1.0f;
  REQUIRE(engine.track_mixer().track_lane_parameter_value(
      track_id, sonare::engine::TrackMixerRuntime::kWidth, &width));
  REQUIRE(width == 0.0f);
  REQUIRE(engine.track_mixer().restore_track_parameter(track_id,
                                                       sonare::engine::TrackMixerRuntime::kWidth));
  REQUIRE(engine.track_mixer().track_lane_parameter_value(
      track_id, sonare::engine::TrackMixerRuntime::kWidth, &width));
  REQUIRE(width == 1.0f);

  const std::vector<float> unity =
      render(compile_timeline(true, false, LaneTarget::None, 1.0f), false);
  const std::vector<float> width_unity = render(unity_timeline, false);
  const std::vector<float> width_zero =
      render(compile_timeline(true, false, LaneTarget::Width, 0.0f), false);

  REQUIRE(width_unity == unity);
  REQUIRE(width_zero != unity);
  REQUIRE(channel_rms(width_zero, 0) < 1.0e-6);
  REQUIRE(channel_rms(width_zero, 1) < 1.0e-6);
}

TEST_CASE("live timeline apply resolves track insert automation against its scene strip",
          "[engine][arrangement][track-insert]") {
  const arr::CompiledTimeline insert_lane =
      compile_timeline(false, true, LaneTarget::Insert, -12.0f);
  {
    RealtimeEngine engine;
    engine.prepare(kSampleRate, kBlockSize);
    const arr::ApplyResult applied = arr::apply_to_engine(insert_lane, engine);
    REQUIRE(applied.ok());
    REQUIRE(applied.installed_automation.empty());
    REQUIRE(engine.automation().lane_count() == 0);
  }

  const std::vector<float> static_gain =
      render(compile_timeline(false, true, LaneTarget::None, 0.0f), true);
  const std::vector<float> automated_gain = render(insert_lane, true);

  REQUIRE(automated_gain != static_gain);
  REQUIRE(channel_rms(automated_gain, 0) < channel_rms(static_gain, 0) * 0.5);
  REQUIRE(channel_rms(automated_gain, 1) < channel_rms(static_gain, 1) * 0.5);
}

TEST_CASE("live track insert automation validates the bound scene identity before mutation",
          "[engine][arrangement][track-insert]") {
  SECTION("processor identity") {
    arr::CompiledTimeline timeline = compile_timeline(false, true, LaneTarget::Insert, -12.0f);
    AutomationTargetIdentity identity;
    identity.insert_index = 0;
    identity.processor = "dynamics.compressor";
    identity.param_name = "levelDb";
    AutomationLane lane(0, AutomationTargetKind::kTrackInsertParam, std::move(identity));
    lane.set_points({Breakpoint{0.0, -12.0f, CurveType::Hold}});
    timeline.mixer.automation_bindings.front().lane = std::move(lane);
    RealtimeEngine engine;
    engine.prepare(kSampleRate, kBlockSize);
    arr::ApplyOptions options;
    options.bind_strips = true;
    const arr::ApplyResult applied = arr::apply_to_engine(timeline, engine, options);
    REQUIRE_FALSE(applied.ok());
    REQUIRE(applied.code == sonare::ErrorCode::InvalidParameter);
    REQUIRE(applied.outcome == arr::ApplyOutcome::kUnchanged);
  }

  SECTION("insert index") {
    arr::CompiledTimeline timeline = compile_timeline(false, true, LaneTarget::Insert, -12.0f);
    AutomationTargetIdentity identity;
    identity.insert_index = std::size_t{1};
    identity.processor = "utility.gain";
    identity.param_name = "levelDb";
    AutomationLane lane(0, AutomationTargetKind::kTrackInsertParam, std::move(identity));
    lane.set_points({Breakpoint{0.0, -12.0f, CurveType::Hold}});
    timeline.mixer.automation_bindings.front().lane = std::move(lane);
    RealtimeEngine engine;
    engine.prepare(kSampleRate, kBlockSize);
    arr::ApplyOptions options;
    options.bind_strips = true;
    const arr::ApplyResult applied = arr::apply_to_engine(timeline, engine, options);
    REQUIRE_FALSE(applied.ok());
    REQUIRE(applied.code == sonare::ErrorCode::InvalidParameter);
    REQUIRE(applied.outcome == arr::ApplyOutcome::kUnchanged);
  }
}

TEST_CASE("scene send automation is refused live and skipped by the stem engine",
          "[engine][arrangement][track-send]") {
  const auto timeline = compile_timeline(false, true, LaneTarget::Send, -6.0f);
  REQUIRE(timeline.mixer.automation_bindings.size() == 1);
  const auto track_id = timeline.mixer.automation_bindings.front().track_id;
  RealtimeEngine engine;
  engine.prepare(kSampleRate, kBlockSize);

  SECTION("live apply leaves the current timeline installed") {
    const auto original = compile_timeline(true, false, LaneTarget::Width, 1.0f);
    REQUIRE(arr::apply_to_engine(original, engine).ok());
    REQUIRE(engine.clip_count() == 1);
    REQUIRE(engine.automation().lane_count() == 1);

    arr::ApplyOptions options;
    options.bind_strips = true;
    const auto result = arr::apply_to_engine(timeline, engine, options);
    REQUIRE(result.code == sonare::ErrorCode::NotSupported);
    REQUIRE(result.outcome == arr::ApplyOutcome::kUnchanged);
    CHECK(result.message == "automation of scene send \"" + std::string(kSceneSendId) +
                                "\" on track " + std::to_string(track_id) +
                                " is realized only by the offline bounce");
    CHECK(result.installed_automation.empty());
    CHECK(engine.clip_count() == 1);
    CHECK(engine.automation().lane_count() == 1);
  }

  SECTION("stem apply leaves send automation to the scene mixer") {
    const auto result = arr::apply_to_engine(timeline, engine);
    REQUIRE(result.ok());
    CHECK(result.installed_automation.empty());
    CHECK(engine.automation().lane_count() == 0);
    CHECK(engine.clip_count() == 1);
  }
}

#endif  // SONARE_WITH_ARRANGEMENT
