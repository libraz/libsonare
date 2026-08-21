/// @file decide_structure_test.cpp
/// @brief Contract of the mixing assistant's bus, VCA and effect-send topology.

#include "mixing/assistant/decide_structure.h"

#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "mastering/common/loudness_measure.h"
#include "mix_eval.h"
#include "mixing/api/scene.h"
#include "mixing/assistant/scene_delta.h"
#include "mixing/assistant/suggester.h"
#include "mixing/assistant/track_profile.h"
#include "util/json.h"

using sonare::mastering::api::insert_factory_names;
using sonare::mastering::api::insert_param_names;
using sonare::mixing::api::Scene;
using sonare::mixing::api::scene_from_json;
using sonare::mixing::api::scene_to_json;
using sonare::mixing::api::Strip;
using sonare::mixing::assistant::apply_deltas;
using sonare::mixing::assistant::decide_structure;
using sonare::mixing::assistant::DeltaDomain;
using sonare::mixing::assistant::kBandCount;
using sonare::mixing::assistant::kMinTracksPerSubgroup;
using sonare::mixing::assistant::MixAssistantConfig;
using sonare::mixing::assistant::MixProfile;
using sonare::mixing::assistant::SceneDelta;
using sonare::mixing::assistant::SourceClass;
using sonare::mixing::assistant::suggest_scene;
using sonare::mixing::assistant::TrackProfile;

namespace {

// A classification the decision table matched cleanly, so nothing in a test is
// weakened by an incidental confidence.
constexpr float kCertain = 1.0f;
// Comfortably longer than the 400 ms gating block, so duration is never the
// reason a hand-built profile looks degenerate.
constexpr float kMeasurableDurationSec = 2.0f;
// Sample rate and block size the realised scene is instantiated at. Any legal
// pair does; these are the mixing suite's usual ones.
constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 512;

// The bus role the mixer resolves the summing destination by. Its id can be
// renamed around a track of the same name, so nothing here matches on the id.
constexpr const char* kMasterRole = "master";
constexpr const char* kSubgroupRole = "subgroup";

// An even spread across the analysis bands, which is what band_occupancy looks
// like for any track with real spectral content: it sums to 1.
std::array<float, kBandCount> even_occupancy() {
  std::array<float, kBandCount> bands{};
  bands.fill(1.0f / static_cast<float>(kBandCount));
  return bands;
}

TrackProfile make_profile(const std::string& id, SourceClass source, bool usable = true) {
  TrackProfile profile;
  profile.strip_id = id;
  profile.name = id;
  profile.base.duration_sec = kMeasurableDurationSec;
  profile.duration_sec = kMeasurableDurationSec;
  profile.band_occupancy = even_occupancy();
  profile.source = source;
  profile.source_confidence = kCertain;
  profile.usable = usable;
  return profile;
}

// Mirrors the scene the suggester builds the deltas against: one strip per
// profile and nothing else.
Scene base_scene(const std::vector<TrackProfile>& profiles) {
  Scene scene;
  scene.strips.reserve(profiles.size());
  for (const TrackProfile& profile : profiles) {
    Strip strip;
    strip.id = profile.strip_id;
    scene.strips.push_back(strip);
  }
  return scene;
}

std::vector<SceneDelta> decide(const std::vector<TrackProfile>& profiles,
                               const MixAssistantConfig& config = {}) {
  MixProfile mix;
  mix.track_count = static_cast<int>(profiles.size());
  return decide_structure(profiles, mix, config);
}

Scene realise(const std::vector<TrackProfile>& profiles, const MixAssistantConfig& config = {}) {
  return apply_deltas(base_scene(profiles), decide(profiles, config));
}

std::string master_bus_id(const Scene& scene) {
  for (const auto& bus : scene.buses) {
    if (bus.role == kMasterRole) return bus.id;
  }
  return {};
}

bool has_bus(const Scene& scene, const std::string& id) {
  return std::any_of(scene.buses.begin(), scene.buses.end(),
                     [&](const sonare::mixing::api::Bus& bus) { return bus.id == id; });
}

std::vector<std::string> subgroup_bus_ids(const Scene& scene) {
  std::vector<std::string> ids;
  for (const auto& bus : scene.buses) {
    if (bus.role == kSubgroupRole) ids.push_back(bus.id);
  }
  return ids;
}

// Walks the connection graph forward from @p from. Sends are deliberately not
// followed: a send is a parallel feed, and a strip whose only path to the
// master is through an effect return is a strip whose dry signal is missing.
bool reaches(const Scene& scene, const std::string& from, const std::string& target) {
  std::set<std::string> visited;
  std::vector<std::string> pending{from};
  while (!pending.empty()) {
    const std::string node = pending.back();
    pending.pop_back();
    if (node == target) return true;
    if (!visited.insert(node).second) continue;
    for (const auto& connection : scene.connections) {
      if (connection.source == node) pending.push_back(connection.destination);
    }
  }
  return false;
}

// Instantiates the scene through the C ABI, which is the only check that says
// the topology is usable rather than merely well-formed.
void require_instantiable(const Scene& scene) {
  const std::string json = scene_to_json(scene);
  SonareMixer* mixer = sonare_mixer_from_scene_json(json.c_str(), kSampleRate, kBlockSize);
  REQUIRE(mixer != nullptr);
  sonare_mixer_destroy(mixer);
}

// Every strip the assistant produced, including the effect returns it added.
void require_every_strip_reaches_master(const Scene& scene) {
  const std::string master = master_bus_id(scene);
  REQUIRE_FALSE(master.empty());
  for (const auto& strip : scene.strips) {
    INFO("strip " << strip.id);
    REQUIRE(reaches(scene, strip.id, master));
  }
}

// Id of the return strip carrying the plate reverb, or empty when the scene has
// no reverb bus at all.
std::string reverb_return_id(const Scene& scene) {
  for (const auto& strip : scene.strips) {
    for (const auto& insert : strip.inserts) {
      if (insert.processor_name == std::string("effects.reverb.plate")) return strip.id;
    }
  }
  return {};
}

// Id of the aux bus that feeds @p return_id. Read from the graph rather than
// assumed, because a track of the same name pushes the generated id to a suffix.
std::string bus_feeding(const Scene& scene, const std::string& return_id) {
  for (const auto& connection : scene.connections) {
    if (connection.destination != return_id) continue;
    if (has_bus(scene, connection.source)) return connection.source;
  }
  return {};
}

// The same scene with nothing feeding the reverb, so its return contributes
// silence. Every other path — the strips' own inserts, the delay return, the
// master trim — is left exactly as it was, and the whole signal path outside
// the reverb return is linear, so subtracting this render from the untouched
// one leaves the return's contribution and nothing else.
Scene without_reverb_sends(const Scene& scene, const std::string& reverb_bus_id) {
  Scene dry = scene;
  for (auto& strip : dry.strips) {
    strip.sends.erase(std::remove_if(strip.sends.begin(), strip.sends.end(),
                                     [&](const sonare::mixing::api::Send& send) {
                                       return send.destination_bus_id == reverb_bus_id;
                                     }),
                      strip.sends.end());
  }
  return dry;
}

std::vector<float> interleave(const std::vector<float>& left, const std::vector<float>& right) {
  std::vector<float> out(left.size() * 2, 0.0f);
  for (std::size_t frame = 0; frame < left.size(); ++frame) {
    out[frame * 2] = left[frame];
    out[frame * 2 + 1] = right[frame];
  }
  return out;
}

std::vector<TrackProfile> full_band_session() {
  return {
      make_profile("kickIn", SourceClass::Kick),     make_profile("snareTop", SourceClass::Snare),
      make_profile("hatClosed", SourceClass::HiHat), make_profile("bassDi", SourceClass::Bass),
      make_profile("gtrLeft", SourceClass::Guitar),  make_profile("gtrRight", SourceClass::Guitar),
      make_profile("leadVox", SourceClass::Vocal),   make_profile("bkgVox", SourceClass::Backing),
      make_profile("noise", SourceClass::Unknown),   make_profile("dead", SourceClass::Keys, false),
  };
}

}  // namespace

TEST_CASE("decide_structure always creates a master bus", "[mixing][assistant]") {
  SECTION("with tracks") {
    const Scene scene = realise(full_band_session());
    REQUIRE_FALSE(master_bus_id(scene).empty());
  }

  SECTION("with a single unclassified track") {
    const Scene scene = realise({make_profile("stem", SourceClass::Unknown)});
    REQUIRE(master_bus_id(scene) == "master");
  }
}

TEST_CASE("decide_structure tolerates an empty track list", "[mixing][assistant]") {
  std::vector<SceneDelta> deltas;
  REQUIRE_NOTHROW(deltas = decide({}));

  const Scene scene = apply_deltas(Scene{}, deltas);
  REQUIRE(scene.strips.empty());
  REQUIRE(scene.connections.empty());
  // Either the master bus alone or nothing at all; both are answers a caller
  // can apply without a special case.
  REQUIRE(scene.buses.size() <= 1);
  if (!scene.buses.empty()) REQUIRE(scene.buses.front().role == kMasterRole);
}

TEST_CASE("decide_structure emits only structure deltas", "[mixing][assistant]") {
  for (const SceneDelta& delta : decide(full_band_session())) {
    INFO(delta.reason);
    REQUIRE(delta.domain == DeltaDomain::Structure);
    // A reason is one line of the caller-facing explanation, so an empty or
    // sentence-cased one would show up verbatim in the output.
    REQUIRE_FALSE(delta.reason.empty());
    REQUIRE(std::islower(static_cast<unsigned char>(delta.reason.front())) != 0);
  }
}

TEST_CASE("a subgroup needs more than one track", "[mixing][assistant]") {
  SECTION("two tracks of one class are grouped") {
    const Scene scene = realise({make_profile("gtrLeft", SourceClass::Guitar),
                                 make_profile("gtrRight", SourceClass::Guitar)});
    const std::vector<std::string> subgroups = subgroup_bus_ids(scene);
    REQUIRE(subgroups.size() == 1);
    REQUIRE(reaches(scene, "gtrLeft", subgroups.front()));
    REQUIRE(reaches(scene, "gtrRight", subgroups.front()));
    // One VCA group per subgroup, at unity, over exactly its members.
    REQUIRE(scene.vca_groups.size() == 1);
    REQUIRE(scene.vca_groups.front().gain_db == 0.0f);
    REQUIRE(scene.vca_groups.front().members.size() == 2);
  }

  SECTION("one track of a class is not") {
    const Scene scene = realise({make_profile("gtrLeft", SourceClass::Guitar)});
    REQUIRE(subgroup_bus_ids(scene).empty());
    REQUIRE(scene.vca_groups.empty());
    REQUIRE(reaches(scene, "gtrLeft", master_bus_id(scene)));
  }

  SECTION("classes recorded as one instrument share their subgroup") {
    // Kick, snare and hi-hat are close mics on the same kit, so two of them are
    // already enough for the drum subgroup even though no class has two tracks.
    const std::vector<TrackProfile> profiles = {make_profile("kickIn", SourceClass::Kick),
                                                make_profile("snareTop", SourceClass::Snare)};
    REQUIRE(profiles.size() >= static_cast<std::size_t>(kMinTracksPerSubgroup));
    const Scene scene = realise(profiles);
    REQUIRE(subgroup_bus_ids(scene).size() == 1);
  }
}

TEST_CASE("every strip has a path to master", "[mixing][assistant]") {
  const Scene scene = realise(full_band_session());
  require_every_strip_reaches_master(scene);

  SECTION("including the ones no subgroup would take") {
    // An unclassified track and one the profiler rejected are connected rather
    // than dropped: an unrouted strip is silent.
    const std::string master = master_bus_id(scene);
    REQUIRE(reaches(scene, "noise", master));
    REQUIRE(reaches(scene, "dead", master));
    // ...and neither joins a subgroup.
    for (const auto& group : scene.vca_groups) {
      REQUIRE(std::find(group.members.begin(), group.members.end(), "noise") ==
              group.members.end());
      REQUIRE(std::find(group.members.begin(), group.members.end(), "dead") == group.members.end());
    }
  }
}

TEST_CASE("generated identifiers never collide with a track", "[mixing][assistant]") {
  // Two kit tracks named after the identifiers this stage would otherwise
  // choose. Strips and buses share one node namespace in the mixer, so a clash
  // is a scene that refuses to load rather than a cosmetic problem.
  const std::vector<TrackProfile> profiles = {make_profile("master", SourceClass::Kick),
                                              make_profile("drumBus", SourceClass::Snare)};
  const Scene scene = realise(profiles);

  std::set<std::string> ids;
  for (const auto& strip : scene.strips) {
    INFO("strip " << strip.id);
    REQUIRE(ids.insert(strip.id).second);
  }
  for (const auto& bus : scene.buses) {
    INFO("bus " << bus.id);
    REQUIRE(ids.insert(bus.id).second);
  }
  for (const auto& group : scene.vca_groups) {
    INFO("vca group " << group.id);
    REQUIRE(ids.insert(group.id).second);
  }

  // The master keeps its role even when it had to give up its usual id.
  REQUIRE_FALSE(master_bus_id(scene).empty());
  REQUIRE(master_bus_id(scene) != "master");
  REQUIRE(has_bus(scene, master_bus_id(scene)));
  require_every_strip_reaches_master(scene);
  require_instantiable(scene);
}

TEST_CASE("a structure-only scene survives a JSON round trip", "[mixing][assistant]") {
  const Scene scene = realise(full_band_session());
  const std::string json = scene_to_json(scene);
  const Scene restored = scene_from_json(json);

  REQUIRE(scene_to_json(restored) == json);
  REQUIRE(restored.strips.size() == scene.strips.size());
  REQUIRE(restored.buses.size() == scene.buses.size());
  REQUIRE(restored.vca_groups.size() == scene.vca_groups.size());
  REQUIRE(restored.connections.size() == scene.connections.size());
}

TEST_CASE("the suggested scene instantiates a mixer", "[mixing][assistant]") {
  require_instantiable(realise(full_band_session()));
}

TEST_CASE("decide_structure is deterministic", "[mixing][assistant]") {
  // Two independently built but identical sessions, so the result cannot depend
  // on anything carried over from the first call.
  const std::string first = scene_to_json(realise(full_band_session()));
  const std::string second = scene_to_json(realise(full_band_session()));
  REQUIRE(first == second);
}

TEST_CASE("suggestion strength scales the effect sends", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = full_band_session();

  MixAssistantConfig silent;
  silent.suggestion_strength = 0.0f;
  const Scene without = realise(profiles, silent);
  for (const auto& strip : without.strips) {
    INFO("strip " << strip.id);
    REQUIRE(strip.sends.empty());
  }
  // The routing is not a level, so it survives a zero-strength suggestion.
  REQUIRE_FALSE(subgroup_bus_ids(without).empty());
  require_every_strip_reaches_master(without);
  require_instantiable(without);

  // A weakened suggestion never sends more than a full one. Compared per strip
  // and per destination so the check holds whether or not the effect buses are
  // compiled in at all.
  MixAssistantConfig half;
  half.suggestion_strength = 0.5f;
  const Scene weakened = realise(profiles, half);
  const Scene full = realise(profiles);
  for (const auto& strip : weakened.strips) {
    for (const auto& send : strip.sends) {
      const auto peer =
          std::find_if(full.strips.begin(), full.strips.end(),
                       [&](const Strip& candidate) { return candidate.id == strip.id; });
      REQUIRE(peer != full.strips.end());
      const auto match = std::find_if(
          peer->sends.begin(), peer->sends.end(), [&](const sonare::mixing::api::Send& candidate) {
            return candidate.destination_bus_id == send.destination_bus_id;
          });
      REQUIRE(match != peer->sends.end());
      INFO("strip " << strip.id << " -> " << send.destination_bus_id);
      REQUIRE(send.send_db < match->send_db);
    }
  }
}

TEST_CASE("effect returns name a processor the factory can build", "[mixing][assistant]") {
  const Scene scene = realise(full_band_session());
  const std::vector<std::string> names = insert_factory_names();

  // The effect buses are only proposed where the optional FX suite is compiled
  // in, so the scene is inspected for what it actually carries rather than for
  // what a particular build configuration would produce.
  std::size_t checked = 0;
  for (const auto& strip : scene.strips) {
    for (const auto& insert : strip.inserts) {
      INFO("insert " << insert.processor_name << " on " << strip.id);
      REQUIRE(std::find(names.begin(), names.end(), insert.processor_name) != names.end());

      const auto params = sonare::util::json::parse(insert.params_json);
      REQUIRE(params.is_object());
      const std::vector<std::string> accepted = insert_param_names(insert.processor_name);
      for (const auto& [key, value] : params.as_object()) {
        INFO("param " << key);
        REQUIRE((value.is_number() || value.is_bool()));
        REQUIRE(std::find(accepted.begin(), accepted.end(), key) != accepted.end());
      }
      ++checked;
    }
  }

  if (checked > 0) {
    // An effect return exists only to be fed, so something has to send to it.
    std::set<std::string> destinations;
    for (const auto& strip : scene.strips) {
      for (const auto& send : strip.sends) destinations.insert(send.destination_bus_id);
    }
    REQUIRE_FALSE(destinations.empty());
    for (const auto& destination : destinations) {
      INFO("send destination " << destination);
      REQUIRE(has_bus(scene, destination));
      // The aux bus feeds a return strip, which in turn reaches the master.
      REQUIRE(reaches(scene, destination, master_bus_id(scene)));
    }
  }
}

TEST_CASE("the reverb return sits well below the direct sound", "[mixing][assistant][.][slow]") {
  // The send table's reverb column is anchored on a survey of professional
  // practice, which measures the reverb return about 9 LU below the direct
  // sound. A send amount is not that relative loudness, so the only way to see
  // where the column actually puts the return is to render the suggested mix
  // twice — once as suggested and once with nothing feeding the reverb — and
  // measure the difference of the two renders, which is the return's whole
  // contribution.
  //
  // A band rather than a value. The figure is a population statistic over a song
  // corpus and this is one synthetic session, so what is defended is that the
  // return is a clearly subordinate layer rather than either an inaudible one or
  // a second mix.
  constexpr float kTargetRelativeLu = -9.0f;
  constexpr float kToleranceLu = 3.0f;

  namespace eval = sonare::mixing::assistant::test;

  const auto fixture = eval::make_demo_tracks(48000, 1.2f);
  const auto tracks = fixture.inputs();
  const auto result = suggest_scene(tracks);

  // Required, not probed. The fixture carries a lead vocal and a guitar, both of
  // which the send table feeds, so a scene without a reverb return means the
  // effect-bus stage did not run — and a case that quietly passed in that state
  // would hide exactly the thing it exists to measure.
  const std::string return_id = reverb_return_id(result.scene);
  REQUIRE_FALSE(return_id.empty());
  const std::string reverb_bus = bus_feeding(result.scene, return_id);
  REQUIRE_FALSE(reverb_bus.empty());

  std::vector<float> mixed_left;
  std::vector<float> mixed_right;
  REQUIRE(eval::render_scene(tracks, result.scene, mixed_left, mixed_right));

  std::vector<float> dry_left;
  std::vector<float> dry_right;
  REQUIRE(eval::render_scene(tracks, without_reverb_sends(result.scene, reverb_bus), dry_left,
                             dry_right));
  REQUIRE(mixed_left.size() == dry_left.size());

  // In place: the mixed render becomes the wet component once the dry one is
  // taken out of it.
  std::vector<float>& wet_left = mixed_left;
  std::vector<float>& wet_right = mixed_right;
  for (std::size_t frame = 0; frame < wet_left.size(); ++frame) {
    wet_left[frame] -= dry_left[frame];
    wet_right[frame] -= dry_right[frame];
  }

  const std::vector<float> wet = interleave(wet_left, wet_right);
  const std::vector<float> dry = interleave(dry_left, dry_right);
  const float wet_lufs = sonare::mastering::common::measure_lufs_interleaved(
      wet.data(), wet_left.size(), 2, fixture.sample_rate);
  const float dry_lufs = sonare::mastering::common::measure_lufs_interleaved(
      dry.data(), dry_left.size(), 2, fixture.sample_rate);
  const float relative_lu = wet_lufs - dry_lufs;
  INFO("dry " << dry_lufs << " LUFS, wet " << wet_lufs << " LUFS, relative " << relative_lu
              << " LU");
  CHECK(relative_lu <= kTargetRelativeLu + kToleranceLu);
  CHECK(relative_lu >= kTargetRelativeLu - kToleranceLu);
}
