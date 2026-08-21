/// @file scene_delta_test.cpp
/// @brief Delta composition rules for the mixing assistant.

#include "mixing/assistant/scene_delta.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>
#include <vector>

using sonare::mixing::api::Insert;
using sonare::mixing::api::InsertSlot;
using sonare::mixing::api::Scene;
using sonare::mixing::api::Send;
using sonare::mixing::api::SendTiming;
using sonare::mixing::assistant::apply_deltas;
using sonare::mixing::assistant::DeltaDomain;
using sonare::mixing::assistant::kMaxSuggestedTrimDb;
using sonare::mixing::assistant::SceneDelta;

namespace {

SceneDelta gain_delta(const std::string& strip_id, float trim_db) {
  SceneDelta delta;
  delta.domain = DeltaDomain::Gain;
  delta.strip_id = strip_id;
  delta.input_trim_db = trim_db;
  delta.reason = "trim staged towards the loudness target";
  return delta;
}

const sonare::mixing::api::Strip* find(const Scene& scene, const std::string& id) {
  for (const auto& strip : scene.strips) {
    if (strip.id == id) return &strip;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("apply_deltas leaves the base scene untouched", "[mixing][assistant]") {
  Scene base;
  base.strips.push_back({});
  base.strips.front().id = "kick";

  const std::vector<SceneDelta> deltas = {gain_delta("kick", -6.0f)};
  const Scene result = apply_deltas(base, deltas);

  REQUIRE(base.strips.front().input_trim_db == 0.0f);
  REQUIRE(find(result, "kick") != nullptr);
  REQUIRE_THAT(find(result, "kick")->input_trim_db, Catch::Matchers::WithinAbs(-6.0f, 1e-6));
}

TEST_CASE("apply_deltas creates a strip a delta names but the scene lacks", "[mixing][assistant]") {
  const Scene result = apply_deltas(Scene{}, {gain_delta("bass", 3.0f)});
  REQUIRE(result.strips.size() == 1);
  REQUIRE(result.strips.front().id == "bass");
}

TEST_CASE("gain contributions sum rather than overwrite", "[mixing][assistant]") {
  std::vector<SceneDelta> deltas = {gain_delta("vox", -4.0f), gain_delta("vox", 1.5f)};
  const Scene result = apply_deltas(Scene{}, deltas);
  REQUIRE_THAT(find(result, "vox")->input_trim_db, Catch::Matchers::WithinAbs(-2.5f, 1e-6));
}

TEST_CASE("spatial fields are decided by the last delta in application order",
          "[mixing][assistant]") {
  SceneDelta earlier;
  earlier.domain = DeltaDomain::Structure;
  earlier.strip_id = "gtr";
  earlier.pan = -0.8f;
  earlier.reason = "provisional position";

  SceneDelta later;
  later.domain = DeltaDomain::Image;
  later.strip_id = "gtr";
  later.pan = 0.4f;
  later.reason = "spread against the other rhythm parts";

  // Deliberately supplied out of order: the domain ordering, not the caller's
  // ordering, has to decide the winner.
  const Scene result = apply_deltas(Scene{}, {later, earlier});
  REQUIRE_THAT(find(result, "gtr")->pan, Catch::Matchers::WithinAbs(0.4f, 1e-6));
}

TEST_CASE("a delta that does not set a field leaves it alone", "[mixing][assistant]") {
  SceneDelta positioned;
  positioned.domain = DeltaDomain::Image;
  positioned.strip_id = "gtr";
  positioned.pan = 0.6f;
  positioned.reason = "spread";

  // Emitted after the pan decision and touching nothing spatial. If untouched
  // fields were carried as plain values this would recentre the strip.
  SceneDelta unrelated;
  unrelated.domain = DeltaDomain::Image;
  unrelated.strip_id = "gtr";
  unrelated.polarity_invert_left = true;
  unrelated.reason = "polarity corrected";

  const Scene result = apply_deltas(Scene{}, {positioned, unrelated});
  REQUIRE_THAT(find(result, "gtr")->pan, Catch::Matchers::WithinAbs(0.6f, 1e-6));
  REQUIRE(find(result, "gtr")->polarity_invert_left);
}

TEST_CASE("a duplicate insert in the same slot is dropped and reported", "[mixing][assistant]") {
  SceneDelta first;
  first.domain = DeltaDomain::Eq;
  first.strip_id = "snare";
  first.inserts.push_back(Insert{InsertSlot::PreFader, "eq.parametric", "{}"});
  first.reason = "carve the boxy region";

  SceneDelta second;
  second.domain = DeltaDomain::Dynamics;
  second.strip_id = "snare";
  second.inserts.push_back(Insert{InsertSlot::PreFader, "eq.parametric", "{}"});
  second.reason = "another eq";

  std::vector<std::string> notes;
  const Scene result = apply_deltas(Scene{}, {first, second}, &notes);
  REQUIRE(find(result, "snare")->inserts.size() == 1);
  REQUIRE_FALSE(notes.empty());
}

TEST_CASE("the same processor in a different slot is not a duplicate", "[mixing][assistant]") {
  SceneDelta pre;
  pre.domain = DeltaDomain::Eq;
  pre.strip_id = "snare";
  pre.inserts.push_back(Insert{InsertSlot::PreFader, "eq.parametric", "{}"});
  pre.reason = "pre-fader carve";

  SceneDelta post;
  post.domain = DeltaDomain::Eq;
  post.strip_id = "snare";
  post.inserts.push_back(Insert{InsertSlot::PostFader, "eq.parametric", "{}"});
  post.reason = "post-fader shape";

  const Scene result = apply_deltas(Scene{}, {pre, post});
  REQUIRE(find(result, "snare")->inserts.size() == 2);
}

TEST_CASE("duplicate buses, vca groups and sends are dropped", "[mixing][assistant]") {
  SceneDelta structure;
  structure.domain = DeltaDomain::Structure;
  structure.buses.push_back(sonare::mixing::api::Bus{"drum-bus", "subgroup"});
  structure.buses.push_back(sonare::mixing::api::Bus{"drum-bus", "subgroup"});
  structure.vca_groups.push_back({"drums", 0.0f, {"kick"}});
  structure.vca_groups.push_back({"drums", 0.0f, {"snare"}});
  structure.reason = "drum subgroup";

  SceneDelta sends;
  sends.domain = DeltaDomain::Structure;
  sends.strip_id = "kick";
  sends.sends.push_back(Send{"kick-to-bus", "drum-bus", 0.0f, SendTiming::PostFader});
  sends.sends.push_back(Send{"kick-to-bus-again", "drum-bus", -3.0f, SendTiming::PostFader});
  sends.reason = "route the kick";

  std::vector<std::string> notes;
  const Scene result = apply_deltas(Scene{}, {structure, sends}, &notes);
  REQUIRE(result.buses.size() == 1);
  REQUIRE(result.vca_groups.size() == 1);
  REQUIRE(find(result, "kick")->sends.size() == 1);
  REQUIRE(notes.size() >= 3);
}

TEST_CASE("the clamp is applied once to the total and is reported", "[mixing][assistant]") {
  // Two contributions that are each in range but overshoot together. Clamping
  // per delta would silently accept the pair.
  std::vector<SceneDelta> deltas = {gain_delta("quiet", 20.0f), gain_delta("quiet", 20.0f)};
  std::vector<std::string> notes;
  const Scene result = apply_deltas(Scene{}, deltas, &notes);

  REQUIRE_THAT(find(result, "quiet")->input_trim_db,
               Catch::Matchers::WithinAbs(kMaxSuggestedTrimDb, 1e-6));
  REQUIRE_FALSE(notes.empty());
}

TEST_CASE("an empty delta list returns the base scene unchanged", "[mixing][assistant]") {
  Scene base;
  base.strips.push_back({});
  base.strips.front().id = "kick";
  base.strips.front().pan = 0.25f;

  const Scene result = apply_deltas(base, {});
  REQUIRE(sonare::mixing::api::scene_to_json(result) == sonare::mixing::api::scene_to_json(base));
}

TEST_CASE("a purely structural delta touches no strip", "[mixing][assistant]") {
  SceneDelta structure;
  structure.domain = DeltaDomain::Structure;
  structure.buses.push_back(sonare::mixing::api::Bus{"master", "master"});
  structure.reason = "master bus created";

  const Scene result = apply_deltas(Scene{}, {structure});
  REQUIRE(result.strips.empty());
  REQUIRE(result.buses.size() == 1);
}

TEST_CASE("clamped spatial values stay inside the schema's accepted range", "[mixing][assistant]") {
  SceneDelta wild;
  wild.domain = DeltaDomain::Image;
  wild.strip_id = "fx";
  wild.pan = 4.0f;
  wild.width = 99.0f;
  wild.channel_delay_samples = -50;
  wild.reason = "deliberately out of range";

  const Scene result = apply_deltas(Scene{}, {wild});
  const auto* strip = find(result, "fx");
  REQUIRE(strip->pan <= 1.0f);
  REQUIRE(strip->width <= sonare::mixing::assistant::kMaxSuggestedWidth);
  REQUIRE(strip->channel_delay_samples >= 0);

  // The schema validates the same bounds on the way out, so a clamped scene
  // must survive serialisation.
  REQUIRE_NOTHROW(sonare::mixing::api::scene_to_json(result));
}
