#include <catch2/catch_test_macros.hpp>

#include "automation/automation_lane.h"
#include "util/exception.h"

using namespace sonare::automation;

#if defined(SONARE_WITH_ARRANGEMENT)
#include <memory>
#include <utility>

#include "arrangement/edit_command.h"
#include "arrangement/edit_compiler.h"
#include "arrangement/edit_history.h"

using namespace sonare::arrangement;

TEST_CASE("Typed automation lanes distinguish send and insert identities",
          "[arrangement][automation][lane_identity]") {
  Project project;
  const TrackId track = project.add_track(Track{});
  EditHistory history(std::move(project));
  const auto send = [](const char* send_id) {
    return AutomationLane(0, AutomationTargetKind::kTrackSendDb, {send_id, {}, {}, {}});
  };
  const auto insert = [](std::size_t index, const char* processor, const char* name) {
    return AutomationLane(0, AutomationTargetKind::kTrackInsertParam, {{}, index, processor, name});
  };
  REQUIRE(history.apply(std::make_unique<AddAutomationLane>(track, send("hall"))));
  REQUIRE(history.apply(std::make_unique<AddAutomationLane>(track, send("room"))));
  const auto room = history.project().find_track(track)->automation_lanes[1];
  REQUIRE(room.lane_id() != 0);
  CHECK_FALSE(history.apply(std::make_unique<AddAutomationLane>(track, send("hall"))));
  REQUIRE(history.apply(std::make_unique<AddAutomationLane>(track, insert(0, "eq", "gain"))));
  REQUIRE(history.apply(std::make_unique<AddAutomationLane>(track, insert(0, "eq", "frequency"))));
  REQUIRE(history.apply(std::make_unique<AddAutomationLane>(track, insert(1, "eq", "gain"))));
  REQUIRE(
      history.apply(std::make_unique<AddAutomationLane>(track, insert(0, "compressor", "gain"))));
  CHECK_FALSE(history.apply(std::make_unique<AddAutomationLane>(track, insert(0, "eq", "gain"))));

  CHECK_FALSE(
      history.apply(std::make_unique<EditAutomationLane>(track, room.lane_id(), send("hall"))));
  REQUIRE(
      history.apply(std::make_unique<EditAutomationLane>(track, room.lane_id(), send("delay"))));
  CHECK(history.project().find_track(track)->automation_lanes[1].target_identity().send_id ==
        "delay");
  REQUIRE(history.undo());
  CHECK(history.project().find_track(track)->automation_lanes[1] == room);
}
#endif

TEST_CASE("Automation target kinds require their own identity fields",
          "[automation][lane_identity]") {
  CHECK_THROWS_AS(AutomationLane(0, AutomationTargetKind::kTrackSendDb), sonare::SonareException);
  CHECK_THROWS_AS(AutomationLane(0, AutomationTargetKind::kTrackSendDb, {"hall", 0, {}, {}}),
                  sonare::SonareException);
  CHECK_THROWS_AS(AutomationLane(0, AutomationTargetKind::kTrackInsertParam, {{}, 0, "eq", {}}),
                  sonare::SonareException);
  CHECK_THROWS_AS(AutomationLane(0, AutomationTargetKind::kTrackWidth, {"hall", {}, {}, {}}),
                  sonare::SonareException);

  AutomationLane width(0, AutomationTargetKind::kTrackWidth);
  CHECK_NOTHROW(width.set_points({{0.0, 0.0f}, {1.0, 2.0f}}));
  CHECK_THROWS_AS(width.set_points({{0.0, 2.5f}}), sonare::SonareException);
}

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_MIXING)
TEST_CASE("Arrangement compilation checks duplicate typed automation identities",
          "[arrangement][automation][lane_identity]") {
  Project project;
  Track track;
  track.channel_strip_ref = "mix";
  auto& scene = project.scene();
  sonare::mixing::api::Strip strip;
  strip.id = track.channel_strip_ref;
  strip.sends.push_back({"hall", "hall-bus"});
  strip.sends.push_back({"room", "room-bus"});
  scene.strips.push_back(std::move(strip));
  scene.buses.emplace_back("hall-bus");
  scene.buses.emplace_back("room-bus");
  track.automation_lanes.emplace_back(0, AutomationTargetKind::kTrackSendDb,
                                      AutomationTargetIdentity{"hall", {}, {}, {}});
  track.automation_lanes.emplace_back(0, AutomationTargetKind::kTrackSendDb,
                                      AutomationTargetIdentity{"room", {}, {}, {}});
  const auto id = project.add_track(std::move(track));
  REQUIRE(compile(project, {}, {}).timeline.has_value());

  AutomationLane duplicate(0, AutomationTargetKind::kTrackSendDb, {"hall", {}, {}, {}});
  duplicate.set_lane_id(project.allocate_automation_lane_id());
  project.find_track_mutable(id)->automation_lanes.push_back(std::move(duplicate));
  const auto result = compile(project, {}, {});
  CHECK_FALSE(result.timeline.has_value());
  REQUIRE_FALSE(result.diagnostics.empty());
  CHECK(result.diagnostics.back().code == Diagnostic::Code::kAutomationLaneConflict);
}
#endif
