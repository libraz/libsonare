/// @file suggester_test.cpp
/// @brief End-to-end behaviour of the mixing assistant's suggestion pipeline.

#include "mixing/assistant/suggester.h"

#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "mix_eval.h"
#include "mixing/api/scene.h"
#include "mixing/assistant/config_from_params.h"
#include "mixing/assistant/source_classifier.h"
#include "mixing/assistant/track_profile.h"
#include "support/schema_paths.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/json.h"

namespace {

using sonare::mixing::assistant::MixAssistantConfig;
using sonare::mixing::assistant::MixAssistantResult;
using sonare::mixing::assistant::TrackInput;
using sonare::mixing::assistant::TrackProfile;
using sonare::mixing::assistant::test::make_demo_tracks;

MixAssistantConfig all_domains_off() {
  MixAssistantConfig config;
  config.enable_structure = false;
  config.enable_gain = false;
  config.enable_balance = false;
  config.enable_eq = false;
  config.enable_dynamics = false;
  config.enable_image = false;
  return config;
}

struct CacheTracks {
  std::vector<std::vector<float>> left;
  std::vector<std::vector<float>> right;

  std::vector<TrackInput> inputs() const {
    std::vector<TrackInput> tracks;
    tracks.reserve(left.size());
    for (std::size_t index = 0; index < left.size(); ++index) {
      TrackInput track;
      track.id = index == 0 ? "cache-left" : "cache-right";
      track.left = left[index].data();
      track.right = right[index].data();
      track.frame_count = left[index].size();
      track.sample_rate = 48000;
      tracks.push_back(track);
    }
    return tracks;
  }
};

CacheTracks make_cache_tracks() {
  constexpr std::size_t kFrames = 24000;
  constexpr float kSampleRate = 48000.0f;
  CacheTracks fixture;
  fixture.left.assign(2, std::vector<float>(kFrames, 0.0f));
  fixture.right.assign(2, std::vector<float>(kFrames, 0.0f));
  for (std::size_t frame = 0; frame < kFrames; ++frame) {
    const float time = static_cast<float>(frame) / kSampleRate;
    const float tone = std::sin(sonare::constants::kTwoPi * 440.0f * time);
    fixture.left[0][frame] = 0.80f * tone;
    fixture.right[0][frame] = 0.10f * tone;
    fixture.left[1][frame] = 0.05f * tone;
    fixture.right[1][frame] = 0.35f * tone;
  }
  return fixture;
}

struct ToggleTracks {
  std::vector<float> wide_left;
  std::vector<float> wide_right;
  std::vector<float> reference_left;
  std::vector<float> reference_right;

  std::vector<TrackInput> inputs() const {
    return {
        {"toggle-wide", {}, wide_left.data(), wide_right.data(), wide_left.size(), 48000},
        {"toggle-reference",
         {},
         reference_left.data(),
         reference_right.data(),
         reference_left.size(),
         48000},
    };
  }
};

ToggleTracks make_toggle_tracks() {
  constexpr std::size_t kFrames = 48000;
  constexpr float kSampleRate = 48000.0f;
  ToggleTracks fixture;
  fixture.wide_left.resize(kFrames);
  fixture.wide_right.resize(kFrames);
  fixture.reference_left.resize(kFrames);
  fixture.reference_right.resize(kFrames);
  for (std::size_t frame = 0; frame < kFrames; ++frame) {
    const float time = static_cast<float>(frame) / kSampleRate;
    const float low = 0.70f * std::sin(sonare::constants::kTwoPi * 120.0f * time);
    const float high = 0.35f * std::sin(sonare::constants::kTwoPi * 440.0f * time);
    fixture.wide_left[frame] = low + high;
    fixture.wide_right[frame] = -low + high;
    fixture.reference_left[frame] = high;
    fixture.reference_right[frame] = high;
  }
  return fixture;
}

bool mix_measurements_differ(const sonare::mixing::assistant::MixProfile& lhs,
                             const sonare::mixing::assistant::MixProfile& rhs) {
  if (lhs.track_count != rhs.track_count || lhs.dominance.size() != rhs.dominance.size() ||
      lhs.image.histogram.size() != rhs.image.histogram.size()) {
    return true;
  }
  for (std::size_t index = 0; index < lhs.dominance.size(); ++index) {
    if (lhs.dominance[index].valid_frames != rhs.dominance[index].valid_frames ||
        std::abs(lhs.dominance[index].ratio - rhs.dominance[index].ratio) > 1.0e-5f) {
      return true;
    }
  }
  for (std::size_t index = 0; index < lhs.image.histogram.size(); ++index) {
    if (std::abs(lhs.image.histogram[index] - rhs.image.histogram[index]) > 1.0e-5f) return true;
  }
  return false;
}

template <typename Function>
void require_invalid_parameter(Function&& function) {
  try {
    function();
  } catch (const sonare::SonareException& error) {
    CHECK(error.code() == sonare::ErrorCode::InvalidParameter);
    return;
  } catch (...) {
    FAIL("expected SonareException with InvalidParameter");
    return;
  }
  FAIL("expected InvalidParameter");
}

}  // namespace

TEST_CASE("suggest_scene returns a scene, profiles and an explanation", "[mixing][assistant]") {
  const auto fixture = make_demo_tracks();
  const auto tracks = fixture.inputs();
  const MixAssistantResult result = sonare::mixing::assistant::suggest_scene(tracks);

  REQUIRE(result.tracks.size() == tracks.size());
  REQUIRE(result.mix.track_count == static_cast<int>(tracks.size()));
  REQUIRE_FALSE(result.scene.strips.empty());
  REQUIRE_FALSE(result.explanation.empty());
}

TEST_CASE("the pre-analysed overload returns the same scene as the full pipeline",
          "[mixing][assistant]") {
  // The pre-analysed path exists to skip re-measuring. If it disagreed with the
  // full pipeline it would be a faster function that returns a different
  // answer, which is worse than not having it.
  //
  // Written exactly as the header documents the split, with no step the header
  // does not mention: classification used to be one such step, and a caller
  // following the documented three calls got a scene missing structure,
  // balance, dynamics, image and most of the EQ, with no error to say so.
  const auto fixture = make_demo_tracks(48000, 0.5f);
  const auto tracks = fixture.inputs();
  const MixAssistantConfig config;

  const MixAssistantResult full = sonare::mixing::assistant::suggest_scene(tracks, config);

  sonare::mixing::assistant::TrackProfileConfig profile_config;
  profile_config.n_fft = config.n_fft;
  profile_config.hop_length = config.hop_length;
  const auto profiles = sonare::mixing::assistant::analyze_track_profiles(tracks, profile_config);
  const auto mix = sonare::mixing::assistant::analyze_mix_profile(tracks, profiles, config);
  const MixAssistantResult staged = sonare::mixing::assistant::suggest_scene(profiles, mix, config);

  REQUIRE(sonare::mixing::api::scene_to_json(staged.scene) ==
          sonare::mixing::api::scene_to_json(full.scene));
  REQUIRE(staged.explanation == full.explanation);
}

TEST_CASE("a cached mix profile is reprojected for changed suggestion settings",
          "[mixing][assistant]") {
  const auto fixture = make_cache_tracks();
  const auto tracks = fixture.inputs();
  MixAssistantConfig measured_config;
  measured_config.suggestion_strength = 0.25f;
  const auto profiles = sonare::mixing::assistant::analyze_track_profiles(tracks);
  REQUIRE(profiles.size() == tracks.size());
  REQUIRE(std::all_of(profiles.begin(), profiles.end(),
                      [](const TrackProfile& profile) { return profile.usable; }));

  const auto cached =
      sonare::mixing::assistant::analyze_mix_profile(tracks, profiles, measured_config);
  REQUIRE_FALSE(cached.dominance.empty());
  REQUIRE_FALSE(cached.image.histogram.empty());

  MixAssistantConfig changed_strength = measured_config;
  changed_strength.suggestion_strength = 1.0f;
  MixAssistantConfig gain_disabled = measured_config;
  gain_disabled.enable_gain = false;
  MixAssistantConfig changed_target = measured_config;
  changed_target.target_track_lufs = -12.0f;
  const MixAssistantConfig variants[] = {changed_strength, gain_disabled, changed_target};

  bool saw_changed_measurement = false;
  for (const MixAssistantConfig& config : variants) {
    const auto fresh = sonare::mixing::assistant::suggest_scene(tracks, config);
    const auto staged = sonare::mixing::assistant::suggest_scene(profiles, cached, config);
    saw_changed_measurement |= mix_measurements_differ(cached, fresh.mix);
    CHECK(sonare::mixing::api::scene_to_json(staged.scene) ==
          sonare::mixing::api::scene_to_json(fresh.scene));
    CHECK(staged.explanation == fresh.explanation);
    CHECK(sonare::mixing::assistant::mix_assistant_result_to_json(staged) ==
          sonare::mixing::assistant::mix_assistant_result_to_json(fresh));
  }

  // The returned effective profile is itself a valid cache. Reusing it for A
  // after projecting to B must restore A's fresh scene and measurements rather
  // than comparing B's trim metadata against the original cache forever.
  const auto fresh_b = sonare::mixing::assistant::suggest_scene(tracks, changed_strength);
  const auto projected_b =
      sonare::mixing::assistant::suggest_scene(profiles, cached, changed_strength);
  CHECK(projected_b.mix.analysis_input_trim_db == fresh_b.mix.analysis_input_trim_db);
  const auto fresh_a = sonare::mixing::assistant::suggest_scene(tracks, measured_config);
  const auto projected_a =
      sonare::mixing::assistant::suggest_scene(profiles, projected_b.mix, measured_config);
  CHECK(sonare::mixing::api::scene_to_json(projected_a.scene) ==
        sonare::mixing::api::scene_to_json(fresh_a.scene));
  CHECK(projected_a.explanation == fresh_a.explanation);
  CHECK(sonare::mixing::assistant::mix_assistant_result_to_json(projected_a) ==
        sonare::mixing::assistant::mix_assistant_result_to_json(fresh_a));
  CHECK(projected_a.mix.analysis_input_trim_db == fresh_a.mix.analysis_input_trim_db);

  // This positive control proves the fixture exercises the reprojection rather
  // than passing because every cached cross-track value is scale-invariant.
  REQUIRE(saw_changed_measurement);

  const auto json = sonare::mixing::assistant::mix_assistant_result_to_json(
      sonare::mixing::assistant::suggest_scene(tracks, changed_strength));
  for (const char* cache_key :
       {"sourceStripIds", "dominanceMeasured", "channelEnergy", "analysisInputTrimDb"}) {
    CHECK(json.find(cache_key) == std::string::npos);
  }
}

TEST_CASE("a cached mix profile rejects reordered profile identities", "[mixing][assistant]") {
  const auto fixture = make_cache_tracks();
  const auto tracks = fixture.inputs();
  const MixAssistantConfig config;
  const auto profiles = sonare::mixing::assistant::analyze_track_profiles(tracks);
  const auto mix = sonare::mixing::assistant::analyze_mix_profile(tracks, profiles, config);

  auto swapped = profiles;
  std::swap(swapped[0].strip_id, swapped[1].strip_id);
  REQUIRE_THROWS_AS(sonare::mixing::assistant::suggest_scene(swapped, mix, config),
                    sonare::SonareException);
}

TEST_CASE("image measurements survive an on-off-on cached suggestion toggle",
          "[mixing][assistant]") {
  const auto fixture = make_toggle_tracks();
  const auto tracks = fixture.inputs();
  const auto profiles = sonare::mixing::assistant::analyze_track_profiles(tracks);
  REQUIRE(profiles.size() == tracks.size());
  REQUIRE(std::all_of(profiles.begin(), profiles.end(),
                      [](const TrackProfile& profile) { return profile.usable; }));

  MixAssistantConfig on_config;
  const auto fresh_on = sonare::mixing::assistant::suggest_scene(tracks, on_config);
  const auto cached_on =
      sonare::mixing::assistant::analyze_mix_profile(tracks, profiles, on_config);
  REQUIRE_FALSE(cached_on.alignment.empty());
  REQUIRE_FALSE(cached_on.mono_risks.empty());
  REQUIRE_FALSE(cached_on.image.histogram.empty());

  const auto on_result = sonare::mixing::assistant::suggest_scene(profiles, cached_on, on_config);
  REQUIRE_FALSE(on_result.mix.alignment.empty());
  REQUIRE_FALSE(on_result.mix.mono_risks.empty());
  CHECK(on_result.mix.cached_alignment.empty());
  CHECK(on_result.mix.cached_mono_risks.empty());

  MixAssistantConfig off_config = on_config;
  off_config.enable_image = false;
  const auto fresh_off = sonare::mixing::assistant::suggest_scene(tracks, off_config);
  const auto off_result =
      sonare::mixing::assistant::suggest_scene(profiles, on_result.mix, off_config);
  CHECK(off_result.mix.alignment.empty());
  CHECK(off_result.mix.image.histogram.empty());
  CHECK(off_result.mix.mono_risks.empty());
  REQUIRE_FALSE(off_result.mix.cached_alignment.empty());
  REQUIRE_FALSE(off_result.mix.cached_mono_risks.empty());
  CHECK(sonare::mixing::assistant::mix_assistant_result_to_json(off_result) ==
        sonare::mixing::assistant::mix_assistant_result_to_json(fresh_off));

  const auto off_again =
      sonare::mixing::assistant::suggest_scene(profiles, off_result.mix, off_config);
  CHECK(off_again.mix.alignment.empty());
  CHECK(off_again.mix.mono_risks.empty());
  CHECK_FALSE(off_again.mix.cached_alignment.empty());
  CHECK_FALSE(off_again.mix.cached_mono_risks.empty());

  const auto on_again =
      sonare::mixing::assistant::suggest_scene(profiles, off_again.mix, on_config);
  CHECK(sonare::mixing::api::scene_to_json(on_again.scene) ==
        sonare::mixing::api::scene_to_json(fresh_on.scene));
  CHECK(on_again.explanation == fresh_on.explanation);
  CHECK(sonare::mixing::assistant::mix_assistant_result_to_json(on_again) ==
        sonare::mixing::assistant::mix_assistant_result_to_json(fresh_on));
  CHECK_FALSE(on_again.mix.alignment.empty());
  CHECK_FALSE(on_again.mix.mono_risks.empty());
  CHECK(on_again.mix.cached_alignment.empty());
  CHECK(on_again.mix.cached_mono_risks.empty());

  const std::string json = sonare::mixing::assistant::mix_assistant_result_to_json(off_result);
  for (const char* cache_key : {"cachedAlignment", "cachedMonoRisks"}) {
    CHECK(json.find(cache_key) == std::string::npos);
  }
}

TEST_CASE("mix analysis requires track and profile identities to stay aligned",
          "[mixing][assistant]") {
  const auto fixture = make_cache_tracks();
  const auto tracks = fixture.inputs();
  const MixAssistantConfig config;
  const auto profiles = sonare::mixing::assistant::analyze_track_profiles(tracks);

  auto missing = profiles;
  missing.pop_back();
  require_invalid_parameter(
      [&] { (void)sonare::mixing::assistant::analyze_mix_profile(tracks, missing, config); });

  auto reordered = profiles;
  std::swap(reordered[0], reordered[1]);
  require_invalid_parameter(
      [&] { (void)sonare::mixing::assistant::analyze_mix_profile(tracks, reordered, config); });

  auto duplicate = profiles;
  duplicate[1].strip_id = duplicate[0].strip_id;
  require_invalid_parameter(
      [&] { (void)sonare::mixing::assistant::analyze_mix_profile(tracks, duplicate, config); });

  auto empty = profiles;
  empty[0].strip_id.clear();
  require_invalid_parameter(
      [&] { (void)sonare::mixing::assistant::analyze_mix_profile(tracks, empty, config); });

  auto mismatched_tracks = tracks;
  mismatched_tracks[0].id = "different-source";
  require_invalid_parameter([&] {
    (void)sonare::mixing::assistant::analyze_mix_profile(mismatched_tracks, profiles, config);
  });
}

TEST_CASE("the split suggestion entry validates empty and duplicate ids before usability",
          "[mixing][assistant]") {
  const auto fixture = make_cache_tracks();
  const auto tracks = fixture.inputs();
  const MixAssistantConfig config;
  const auto profiles = sonare::mixing::assistant::analyze_track_profiles(tracks);
  const auto mix = sonare::mixing::assistant::analyze_mix_profile(tracks, profiles, config);

  auto empty = profiles;
  empty[0].usable = false;
  empty[0].strip_id.clear();
  require_invalid_parameter(
      [&] { (void)sonare::mixing::assistant::suggest_scene(empty, mix, config); });

  auto duplicate = profiles;
  duplicate[1].usable = false;
  duplicate[1].strip_id = duplicate[0].strip_id;
  require_invalid_parameter(
      [&] { (void)sonare::mixing::assistant::suggest_scene(duplicate, mix, config); });
}

TEST_CASE("all assistant entry points reject non-finite scalar configuration",
          "[mixing][assistant]") {
  const auto fixture = make_cache_tracks();
  const auto tracks = fixture.inputs();
  const auto profiles = sonare::mixing::assistant::analyze_track_profiles(tracks);
  const auto mix = sonare::mixing::assistant::analyze_mix_profile(tracks, profiles);
  struct Setting {
    const char* name;
    float MixAssistantConfig::*field;
  };
  const Setting settings[] = {
      {"target_track_lufs", &MixAssistantConfig::target_track_lufs},
      {"suggestion_strength", &MixAssistantConfig::suggestion_strength},
      {"eq_max_cut_db", &MixAssistantConfig::eq_max_cut_db},
      {"mix_bus_headroom_dbtp", &MixAssistantConfig::mix_bus_headroom_dbtp},
      {"tempo_bpm", &MixAssistantConfig::tempo_bpm},
  };
  const float invalid[] = {std::numeric_limits<float>::quiet_NaN(),
                           std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity()};

  for (const Setting& setting : settings) {
    for (const float value : invalid) {
      MixAssistantConfig config;
      config.*setting.field = value;
      INFO(setting.name << " = " << value);
      require_invalid_parameter(
          [&] { (void)sonare::mixing::assistant::suggest_scene(profiles, mix, config); });
    }
  }

  MixAssistantConfig raw_invalid;
  raw_invalid.eq_max_cut_db = std::numeric_limits<float>::quiet_NaN();
  require_invalid_parameter(
      [&] { (void)sonare::mixing::assistant::suggest_scene(tracks, raw_invalid); });

  MixAssistantConfig analysis_invalid;
  analysis_invalid.mix_bus_headroom_dbtp = std::numeric_limits<float>::infinity();
  require_invalid_parameter([&] {
    (void)sonare::mixing::assistant::analyze_mix_profile(tracks, profiles, analysis_invalid);
  });

  MixAssistantConfig finite;
  finite.target_track_lufs = -16.0f;
  finite.suggestion_strength = 0.5f;
  finite.eq_max_cut_db = 2.0f;
  finite.mix_bus_headroom_dbtp = -3.0f;
  finite.tempo_bpm = 120.0f;
  REQUIRE_NOTHROW(sonare::mixing::assistant::suggest_scene(tracks, finite));
  REQUIRE_NOTHROW(sonare::mixing::assistant::suggest_scene(profiles, mix, finite));
  REQUIRE_NOTHROW(sonare::mixing::assistant::analyze_mix_profile(tracks, profiles, finite));
}

TEST_CASE("profiling resolves the source class without a separate call", "[mixing][assistant]") {
  // The documented decomposed path never mentions classify_sources, so the
  // profiles it produces have to arrive already classified. Calling the
  // classifier a second time must then be a no-op rather than a correction.
  const auto fixture = make_demo_tracks(48000, 0.5f);
  const auto tracks = fixture.inputs();
  auto profiles = sonare::mixing::assistant::analyze_track_profiles(tracks);

  const bool any_classified = std::any_of(
      profiles.begin(), profiles.end(), [](const sonare::mixing::assistant::TrackProfile& profile) {
        return profile.source != sonare::mixing::assistant::SourceClass::Unknown;
      });
  REQUIRE(any_classified);

  const auto before = profiles;
  sonare::mixing::assistant::classify_sources(profiles);
  for (std::size_t index = 0; index < profiles.size(); ++index) {
    INFO("track " << profiles[index].strip_id);
    REQUIRE(profiles[index].source == before[index].source);
    REQUIRE_THAT(profiles[index].source_confidence,
                 Catch::Matchers::WithinAbs(before[index].source_confidence, 0.0f));
  }
}

TEST_CASE("two tracks sharing an id are rejected rather than absorbed", "[mixing][assistant]") {
  // Absorbing the duplicate ships a scene with two strips of the same id, which
  // the mixer refuses to load with a complaint that names the scene rather than
  // the two tracks that collided.
  std::vector<float> samples(24000, 0.0f);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    samples[index] = 0.2f * std::sin(0.05f * static_cast<float>(index));
  }
  std::vector<TrackInput> tracks;
  for (int index = 0; index < 2; ++index) {
    TrackInput track;
    track.id = "same";
    track.left = samples.data();
    track.frame_count = samples.size();
    track.sample_rate = 48000;
    tracks.push_back(track);
  }
  REQUIRE_THROWS_AS(sonare::mixing::assistant::suggest_scene(tracks), sonare::SonareException);

  tracks[1].id = "other";
  REQUIRE_NOTHROW(sonare::mixing::assistant::suggest_scene(tracks));
}

TEST_CASE("the mixing assistant rejects an empty track id on core and C surfaces",
          "[mixing][assistant][c_api][regression]") {
  const float sample = 0.0f;
  TrackInput track;
  track.left = &sample;
  track.frame_count = 1;
  track.sample_rate = 48000;
  try {
    (void)sonare::mixing::assistant::suggest_scene(std::vector<TrackInput>{track});
    FAIL("empty track id accepted");
  } catch (const sonare::SonareException& error) {
    CHECK(error.code() == sonare::ErrorCode::InvalidParameter);
    CHECK(std::string(error.what()).find("track id") != std::string::npos);
  }

  const float* channels[] = {&sample};
  const char* ids[] = {""};
  const size_t lengths[] = {1};
  char* json = nullptr;
  const auto error = sonare_mixing_assistant_suggest_scene_json(
      channels, nullptr, ids, nullptr, lengths, 1, 48000, nullptr, 0, &json);
  if (json != nullptr) sonare_free_string(json);
  CHECK(error == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(json == nullptr);

  track.id = "valid";
  CHECK_NOTHROW(sonare::mixing::assistant::suggest_scene(std::vector<TrackInput>{track}));
}

TEST_CASE("a non-finite sample excludes the track under its own reason", "[mixing][assistant]") {
  // One NaN reaches the integrated loudness as -inf, so without this the track
  // is reported excluded for being silent -- a diagnosis of the material rather
  // than of the buffer, and the one reading a caller cannot act on.
  std::vector<float> samples(24000);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    samples[index] = 0.2f * std::sin(0.05f * static_cast<float>(index));
  }
  samples[1000] = std::numeric_limits<float>::quiet_NaN();

  TrackInput track;
  track.id = "poisoned";
  track.left = samples.data();
  track.frame_count = samples.size();
  track.sample_rate = 48000;

  const auto result = sonare::mixing::assistant::suggest_scene({track});
  REQUIRE(result.tracks.size() == 1);
  REQUIRE_FALSE(result.tracks.front().usable);
  REQUIRE(result.tracks.front().exclusion_reason == "track has non-finite samples");

  samples[1000] = std::numeric_limits<float>::infinity();
  const auto infinite = sonare::mixing::assistant::suggest_scene({track});
  REQUIRE(infinite.tracks.front().exclusion_reason == "track has non-finite samples");
}

TEST_CASE("zero suggestion strength is not an empty suggestion", "[mixing][assistant]") {
  // Four doc sites used to promise an empty suggestion at strength 0. What the
  // pipeline actually does is take every level-like decision and set it to
  // zero, while the decisions that are not levels stay: this is the behaviour
  // those doc sites now describe.
  const auto fixture = make_demo_tracks(48000, 0.5f);
  const auto tracks = fixture.inputs();
  MixAssistantConfig silent;
  silent.suggestion_strength = 0.0f;
  const auto result = sonare::mixing::assistant::suggest_scene(tracks, silent);

  // Routing is not a level, so the bus topology survives.
  REQUIRE_FALSE(result.scene.buses.empty());
  REQUIRE_FALSE(result.scene.connections.empty());
  // Levels are decided, and decided to be zero.
  for (const auto& strip : result.scene.strips) {
    INFO("strip " << strip.id);
    REQUIRE_THAT(strip.fader_db, Catch::Matchers::WithinAbs(0.0f, 1e-4));
    REQUIRE_THAT(strip.pan, Catch::Matchers::WithinAbs(0.0f, 1e-4));
    // A send is a level that reached zero, so no effect bus is fed at all.
    REQUIRE(strip.sends.empty());
  }
  // Decided, therefore explained. An empty explanation here would mean the
  // decisions were never taken.
  REQUIRE_FALSE(result.explanation.empty());
}

TEST_CASE("suggest_scene is deterministic", "[mixing][assistant]") {
  // Determinism does not depend on the material's length, so this runs on the
  // short fixture to stay inside the default pass's time budget.
  const auto fixture = make_demo_tracks(48000, 0.5f);
  const auto tracks = fixture.inputs();
  const auto first = sonare::mixing::assistant::suggest_scene(tracks);
  const auto second = sonare::mixing::assistant::suggest_scene(tracks);
  REQUIRE(sonare::mixing::api::scene_to_json(first.scene) ==
          sonare::mixing::api::scene_to_json(second.scene));
  REQUIRE(first.explanation == second.explanation);
}

TEST_CASE("disabling every domain leaves the scene at its starting point", "[mixing][assistant]") {
  const auto fixture = make_demo_tracks();
  const auto tracks = fixture.inputs();
  const MixAssistantResult result =
      sonare::mixing::assistant::suggest_scene(tracks, all_domains_off());

  REQUIRE(result.explanation.empty());
  for (const auto& strip : result.scene.strips) {
    REQUIRE_THAT(strip.input_trim_db, Catch::Matchers::WithinAbs(0.0f, 1e-6));
    REQUIRE_THAT(strip.fader_db, Catch::Matchers::WithinAbs(0.0f, 1e-6));
    REQUIRE_THAT(strip.pan, Catch::Matchers::WithinAbs(0.0f, 1e-6));
    REQUIRE(strip.inserts.empty());
    REQUIRE(strip.sends.empty());
  }
  REQUIRE(result.scene.buses.empty());
  REQUIRE(result.scene.vca_groups.empty());
  REQUIRE(result.scene.connections.empty());
}

namespace {

struct DomainSwitch {
  const char* label;
  bool MixAssistantConfig::*field;
};

// Split across two cases rather than one loop of six: the whole pipeline runs
// once per entry, and these belong in the default pass rather than behind the
// slow tag, so the run time is halved by halving the list.
void check_domain_switches(const DomainSwitch* switches, std::size_t count) {
  const auto fixture = make_demo_tracks(48000, 0.5f);
  const auto tracks = fixture.inputs();
  const auto full = sonare::mixing::assistant::suggest_scene(tracks);

  for (std::size_t index = 0; index < count; ++index) {
    MixAssistantConfig config;
    config.*switches[index].field = false;
    const auto reduced = sonare::mixing::assistant::suggest_scene(tracks, config);
    INFO("domain switched off: " << switches[index].label);
    // Switching a domain off must never add explanation lines, and the pipeline
    // must still produce a serialisable scene.
    REQUIRE(reduced.explanation.size() <= full.explanation.size());
    REQUIRE_NOTHROW(sonare::mixing::api::scene_to_json(reduced.scene));
  }
}

}  // namespace

TEST_CASE("the structure, gain and balance domains can be switched off on their own",
          "[mixing][assistant]") {
  const DomainSwitch switches[] = {
      {"structure", &MixAssistantConfig::enable_structure},
      {"gain", &MixAssistantConfig::enable_gain},
      {"balance", &MixAssistantConfig::enable_balance},
  };
  check_domain_switches(switches, std::size(switches));
}

TEST_CASE("the eq, dynamics and image domains can be switched off on their own",
          "[mixing][assistant]") {
  const DomainSwitch switches[] = {
      {"eq", &MixAssistantConfig::enable_eq},
      {"dynamics", &MixAssistantConfig::enable_dynamics},
      {"image", &MixAssistantConfig::enable_image},
  };
  check_domain_switches(switches, std::size(switches));
}

TEST_CASE("degenerate input is answered rather than thrown", "[mixing][assistant]") {
  SECTION("no tracks") {
    const auto result = sonare::mixing::assistant::suggest_scene({});
    REQUIRE(result.scene.strips.empty());
    REQUIRE(result.explanation.empty());
    REQUIRE(result.tracks.empty());
  }

  SECTION("all tracks silent") {
    std::vector<float> silence(4800, 0.0f);
    std::vector<TrackInput> tracks;
    for (int index = 0; index < 3; ++index) {
      TrackInput track;
      track.id = "silent" + std::to_string(index);
      track.left = silence.data();
      track.frame_count = silence.size();
      track.sample_rate = 48000;
      tracks.push_back(track);
    }
    const auto result = sonare::mixing::assistant::suggest_scene(tracks);
    REQUIRE(result.explanation.empty());
    REQUIRE(result.tracks.size() == 3);
    for (const auto& profile : result.tracks) {
      REQUIRE_FALSE(profile.usable);
    }
  }

  SECTION("non-positive sample rate") {
    std::vector<float> samples(4800, 0.1f);
    TrackInput track;
    track.id = "broken";
    track.left = samples.data();
    track.frame_count = samples.size();
    track.sample_rate = 0;
    const auto result = sonare::mixing::assistant::suggest_scene({track});
    REQUIRE_FALSE(result.tracks.front().usable);
    REQUIRE(result.explanation.empty());
  }

  SECTION("null buffer") {
    TrackInput track;
    track.id = "empty";
    track.sample_rate = 48000;
    REQUIRE_NOTHROW(sonare::mixing::assistant::suggest_scene({track}));
  }
}

TEST_CASE("a silent track receives no suggestion", "[mixing][assistant]") {
  const auto fixture = make_demo_tracks();
  const auto tracks = fixture.inputs();
  const auto result = sonare::mixing::assistant::suggest_scene(tracks);

  const auto silent =
      std::find_if(result.scene.strips.begin(), result.scene.strips.end(),
                   [](const sonare::mixing::api::Strip& strip) { return strip.id == "silent"; });
  REQUIRE(silent != result.scene.strips.end());
  REQUIRE_THAT(silent->input_trim_db, Catch::Matchers::WithinAbs(0.0f, 1e-6));
  REQUIRE(silent->inserts.empty());
}

TEST_CASE("the suggested scene survives a JSON round trip", "[mixing][assistant]") {
  const auto fixture = make_demo_tracks();
  const auto result = sonare::mixing::assistant::suggest_scene(fixture.inputs());
  const std::string json = sonare::mixing::api::scene_to_json(result.scene);
  const auto reparsed = sonare::mixing::api::scene_from_json(json);
  REQUIRE(sonare::mixing::api::scene_to_json(reparsed) == json);
}

TEST_CASE("the suggested scene's JSON carries no unknown scene key", "[mixing][assistant]") {
  const auto fixture = make_demo_tracks();
  const auto result = sonare::mixing::assistant::suggest_scene(fixture.inputs());
  std::vector<std::string> warnings;
  (void)sonare::mixing::api::scene_from_json(sonare::mixing::api::scene_to_json(result.scene),
                                             &warnings);
  REQUIRE(warnings.empty());
}

TEST_CASE("the result document is well-formed JSON with the expected shape",
          "[mixing][assistant]") {
  const auto fixture = make_demo_tracks();
  const auto result = sonare::mixing::assistant::suggest_scene(fixture.inputs());
  const std::string json = sonare::mixing::assistant::mix_assistant_result_to_json(result);

  const auto document = sonare::util::json::parse(json);
  REQUIRE(document.is_object());
  REQUIRE(document.contains("scene"));
  REQUIRE(document.contains("tracks"));
  REQUIRE(document.contains("mix"));
  REQUIRE(document.contains("explanation"));

  // The scene has to nest as a real object, not as an escaped string.
  REQUIRE(document["scene"].is_object());
  REQUIRE(document["tracks"].is_array());
  REQUIRE(document["explanation"].is_array());
}

TEST_CASE("result serialization skips dominance rows without profile identities",
          "[mixing][assistant]") {
  MixAssistantResult result;
  TrackProfile profile;
  profile.strip_id = "only";
  result.tracks = {profile};
  result.mix.track_count = 2;
  result.mix.dominance.assign(2 * 2 * sonare::mixing::assistant::kBandCount,
                              sonare::mixing::assistant::BandDominance{});
  result.mix.dominance[sonare::mixing::assistant::kBandCount].ratio = 0.75f;
  result.mix.dominance[sonare::mixing::assistant::kBandCount].valid_frames = 1;

  const auto document =
      sonare::util::json::parse(sonare::mixing::assistant::mix_assistant_result_to_json(result));
  REQUIRE(document["mix"]["trackCount"].as_int() == 2);
  REQUIRE(document["mix"]["bandDominance"].as_array().empty());
}

TEST_CASE("result serialization skips alignment pairs with invalid profile identities",
          "[mixing][assistant]") {
  MixAssistantResult result;
  TrackProfile profile;
  profile.strip_id = "only";
  result.tracks = {profile};
  result.mix.track_count = 1;
  sonare::mixing::assistant::PairAlignment invalid;
  invalid.reference_index = -1;
  invalid.target_index = 99;
  invalid.related = true;
  result.mix.alignment.push_back(invalid);

  const auto document =
      sonare::util::json::parse(sonare::mixing::assistant::mix_assistant_result_to_json(result));
  REQUIRE(document["mix"]["trackCount"].as_int() == 1);
  REQUIRE(document["mix"]["alignment"].as_array().empty());
}

TEST_CASE("result serialization keeps valid dominance and alignment rows", "[mixing][assistant]") {
  MixAssistantResult result;
  TrackProfile first;
  first.strip_id = "first";
  TrackProfile second;
  second.strip_id = "second";
  result.tracks = {first, second};
  result.mix.track_count = 2;
  result.mix.dominance.assign(2 * 2 * sonare::mixing::assistant::kBandCount,
                              sonare::mixing::assistant::BandDominance{});
  result.mix.dominance[sonare::mixing::assistant::kBandCount].ratio = 0.75f;
  result.mix.dominance[sonare::mixing::assistant::kBandCount].valid_frames = 4;
  sonare::mixing::assistant::PairAlignment aligned;
  aligned.reference_index = 0;
  aligned.target_index = 1;
  aligned.related = true;
  result.mix.alignment.push_back(aligned);

  const auto document =
      sonare::util::json::parse(sonare::mixing::assistant::mix_assistant_result_to_json(result));
  REQUIRE(document["mix"]["bandDominance"].as_array().size() == 1);
  REQUIRE(document["mix"]["alignment"].as_array().size() == 1);
  CHECK(document["mix"]["bandDominance"][0]["masker"].as_string() == "first");
  CHECK(document["mix"]["alignment"][0]["target"].as_string() == "second");
}

TEST_CASE("explanation lines follow the fixed application order", "[mixing][assistant]") {
  // The explanation is the deltas' own reasons in application order, so it must
  // not be empty when the scene was actually changed, and it must be stable.
  const auto fixture = make_demo_tracks();
  const auto tracks = fixture.inputs();
  const auto result = sonare::mixing::assistant::suggest_scene(tracks);
  REQUIRE_FALSE(result.explanation.empty());
  for (const auto& line : result.explanation) {
    REQUIRE_FALSE(line.empty());
    // Lower-case declarative sentences, per the module's writing rule.
    REQUIRE(line.front() == static_cast<char>(std::tolower(line.front())));
  }
}

TEST_CASE("a disabled domain's cross-track measurement is not taken", "[mixing][assistant]") {
  // The option's whole point is skipping the work, not discarding the result,
  // and the work is the cross-track measurement rather than the decision on top
  // of it. Asserted from the measurements themselves rather than from a clock:
  // each of these passes fills its field unconditionally when it runs, so an
  // empty field is the pass not having run. Only mono risks can legitimately
  // come back empty, which is why the fixture carries a track that is genuinely
  // at risk under a fold.
  const auto fixture = make_demo_tracks(48000, 0.5f);
  const auto tracks = fixture.inputs();
  const auto profiles = sonare::mixing::assistant::analyze_track_profiles(tracks);

  const auto everything =
      sonare::mixing::assistant::analyze_mix_profile(tracks, profiles, MixAssistantConfig{});
  REQUIRE_FALSE(everything.dominance.empty());
  REQUIRE_FALSE(everything.alignment.empty());
  REQUIRE_FALSE(everything.image.histogram.empty());
  REQUIRE_FALSE(everything.mono_risks.empty());

  SECTION("every domain off takes no cross-track measurement at all") {
    const auto measured =
        sonare::mixing::assistant::analyze_mix_profile(tracks, profiles, all_domains_off());
    CHECK(measured.dominance.empty());
    CHECK(measured.alignment.empty());
    CHECK(measured.image.histogram.empty());
    CHECK(measured.mono_risks.empty());
    // The profiles still describe the tracks: the per-track measurement is the
    // result's own payload, not a domain's private cost, so it is not skipped
    // with them and is the floor a caller budgets against.
    CHECK(measured.track_count == static_cast<int>(profiles.size()));
  }

  SECTION("the image domain owns alignment, occupancy and mono risk") {
    MixAssistantConfig config;
    config.enable_image = false;
    const auto measured = sonare::mixing::assistant::analyze_mix_profile(tracks, profiles, config);
    CHECK(measured.alignment.empty());
    CHECK(measured.image.histogram.empty());
    CHECK(measured.mono_risks.empty());
    // Band dominance belongs to the EQ and dynamics domains, which are still on.
    CHECK_FALSE(measured.dominance.empty());
  }

  SECTION("band dominance is shared, so it survives either of its two readers") {
    MixAssistantConfig eq_only;
    eq_only.enable_dynamics = false;
    CHECK_FALSE(sonare::mixing::assistant::analyze_mix_profile(tracks, profiles, eq_only)
                    .dominance.empty());

    MixAssistantConfig dynamics_only;
    dynamics_only.enable_eq = false;
    CHECK_FALSE(sonare::mixing::assistant::analyze_mix_profile(tracks, profiles, dynamics_only)
                    .dominance.empty());

    // And is taken only when at least one of them will read it.
    MixAssistantConfig neither;
    neither.enable_eq = false;
    neither.enable_dynamics = false;
    CHECK(sonare::mixing::assistant::analyze_mix_profile(tracks, profiles, neither)
              .dominance.empty());
  }
}

namespace {

// Every field set, including the conditionally-omitted ones scene_json.cpp
// only writes for a non-default strip/bus, so the schema-path equality below
// actually exercises the whole writer rather than its unconditional subset.
// Every EqBand field off its default, so the writer emits every band path.
sonare::mastering::eq::EqBand make_fully_populated_eq_band() {
  using namespace sonare::mastering::eq;
  EqBand band(EqBandType::HighShelf, 4000.0f, 3.0f, 0.9f, true, BiquadCoeffMode::Vicanek);
  band.slope_db_oct = 24;
  band.placement = StereoPlacement::Mid;
  band.phase = PhaseMode::NaturalPhase;
  band.soloed = true;
  band.bypassed = true;
  band.proportional_q = true;
  band.proportional_q_strength = 0.1f;
  band.dyn.enabled = true;
  band.dyn.threshold_db = -18.0f;
  band.dyn.auto_threshold = true;
  band.dyn.ratio = 3.0f;
  band.dyn.range_db = -9.0f;
  band.dyn.attack_ms = 2.0f;
  band.dyn.release_ms = 80.0f;
  band.dyn.detector_delay_ms = 1.0f;
  band.dyn.sidechain_freq_hz = 2000.0f;
  band.dyn.sidechain_q = 2.0f;
  band.dyn.external_sidechain = true;
  return band;
}

sonare::mixing::api::Scene make_fully_populated_scene() {
  using sonare::mixing::api::Bus;
  using sonare::mixing::api::Insert;
  using sonare::mixing::api::InsertSlot;
  using sonare::mixing::api::Scene;
  using sonare::mixing::api::Send;
  using sonare::mixing::api::SendTiming;
  using sonare::mixing::api::Strip;
  using sonare::mixing::api::VcaGroup;

  Scene scene;

  Strip lead;
  lead.id = "lead";
  lead.input_trim_db = 1.5f;
  lead.fader_db = -2.0f;
  lead.vca_offset_db = 0.5f;
  lead.pan = 0.25f;
  lead.width = 1.2f;
  lead.muted = true;
  lead.soloed = false;
  lead.solo_safe = true;
  lead.pan_mode = 1;
  lead.dual_pan_left = -0.8f;
  lead.dual_pan_right = 0.8f;
  lead.polarity_invert_left = true;
  lead.polarity_invert_right = false;
  lead.pan_law = 1;
  lead.channel_delay_samples = 12;
  lead.source_layout = sonare::ChannelLayout::FivePointOne;
  lead.surround_pan.azimuth = 0.3f;
  lead.surround_pan.elevation = 0.1f;
  lead.surround_pan.divergence = 0.2f;
  lead.surround_pan.lfe = 0.05f;
  lead.surround_pan.distance = 1.5f;
  lead.metering.enabled = false;
  lead.metering.lufs = true;
  lead.metering.true_peak = true;
  lead.metering.true_peak_oversample = 8;
  lead.inserts.push_back(Insert(InsertSlot::PreFader, "eq.parametric", "{}", "lead-sidechain"));
  lead.sends.push_back(Send{"send1", "reverb", -6.0f, SendTiming::PreFader});
  lead.eq.enabled = false;
  lead.eq.bands.push_back(make_fully_populated_eq_band());
  scene.strips.push_back(lead);

  Strip backing;
  backing.id = "backing";
  scene.strips.push_back(backing);

  Bus master;
  master.id = "master";
  master.role = "master";
  master.layout = sonare::ChannelLayout::FivePointOne;
  master.input_trim_db = -1.0f;
  master.width = 0.8f;
  master.polarity_invert_left = true;
  master.polarity_invert_right = true;
  master.inserts.push_back(
      Insert(InsertSlot::PostFader, "dynamics.limiter", "{}", "bus-sidechain"));
  scene.buses.push_back(master);

  Bus reverb;
  reverb.id = "reverb";
  reverb.pan = -0.3f;
  reverb.pan_mode = 2;
  reverb.dual_pan_left = -0.6f;
  reverb.dual_pan_right = 0.4f;
  reverb.pan_law = 1;
  reverb.eq.bands.push_back(make_fully_populated_eq_band());
  scene.buses.push_back(reverb);

  VcaGroup group;
  group.id = "vca1";
  group.gain_db = 2.0f;
  group.members = {"lead", "backing"};
  scene.vca_groups.push_back(group);

  scene.connections.push_back({"lead", "master"});
  scene.connections.push_back({"backing", "master"});

  return scene;
}

std::vector<TrackProfile> make_fully_populated_track_profiles() {
  TrackProfile lead;
  lead.strip_id = "lead";
  lead.name = "Lead Vocal";
  lead.base.loudness.integrated_lufs = -14.0f;
  lead.base.loudness.true_peak_db = -1.0f;
  lead.base.loudness.crest_factor_db = 10.0f;
  lead.base.spectral.centroid_hz = 2200.0f;
  lead.base.spectral.flatness = 0.3f;
  lead.base.dynamics.attack_density = 1.5f;
  lead.base.dynamics.sustain_ratio = 0.6f;
  lead.source = sonare::mixing::assistant::SourceClass::Vocal;
  lead.source_confidence = 0.85f;
  lead.channel_count = 2;
  lead.duration_sec = 12.5f;
  lead.usable = true;

  // Distinct source/usable state so tracks[].usable and tracks[].exclusionReason
  // are exercised at both values, not just the unconditional keys.
  TrackProfile backing = lead;
  backing.strip_id = "backing";
  backing.name = "Backing Vocal";
  backing.usable = false;
  backing.exclusion_reason = "track has non-finite samples";

  return {lead, backing};
}

// Populated by hand rather than measured: mix.bandDominance[], mix.alignment[],
// mix.crowdedBands[] and mix.monoRisks[] are each conditional on a specific
// entry (see suggester.cpp), so a measured MixProfile is not guaranteed to
// trigger all four.
sonare::mixing::assistant::MixProfile make_fully_populated_mix_profile() {
  using sonare::mixing::assistant::BandDominance;
  using sonare::mixing::assistant::kBandCount;
  using sonare::mixing::assistant::MixProfile;
  using sonare::mixing::assistant::MonoRisk;
  using sonare::mixing::assistant::PairAlignment;

  MixProfile mix;
  mix.track_count = 2;

  mix.dominance.assign(static_cast<std::size_t>(mix.track_count * mix.track_count * kBandCount),
                       BandDominance{});
  BandDominance masking;
  masking.ratio = 0.7f;
  masking.valid_frames = 5;
  mix.dominance[static_cast<std::size_t>((0 * mix.track_count + 1) * kBandCount)] = masking;

  PairAlignment aligned;
  aligned.reference_index = 0;
  aligned.target_index = 1;
  aligned.lag_samples = 3;
  aligned.correlation = 0.6f;
  aligned.polarity_opposed = true;
  aligned.related = true;
  mix.alignment.push_back(aligned);

  mix.image.crowded.assign(static_cast<std::size_t>(kBandCount), false);
  mix.image.crowding.assign(static_cast<std::size_t>(kBandCount), 0.0f);
  mix.image.crowded[2] = true;
  mix.image.crowding[2] = 0.8f;

  MonoRisk risk;
  risk.track_index = 0;
  risk.strip_id = "lead";
  risk.correlation = 0.2f;
  risk.width = 1.5f;
  risk.wide_low_end = true;
  mix.mono_risks.push_back(risk);

  return mix;
}

MixAssistantResult make_fully_populated_assistant_result() {
  MixAssistantResult result;
  result.scene = make_fully_populated_scene();
  result.tracks = make_fully_populated_track_profiles();
  result.mix = make_fully_populated_mix_profile();
  result.explanation = {"pulled the lead down to make room for the backing vocal"};
  return result;
}

}  // namespace

TEST_CASE("the assistant result schema list matches what the writer emits", "[mixing][assistant]") {
  const auto result = make_fully_populated_assistant_result();
  const auto actual = sonare::test::schema_paths_of(
      sonare::mixing::assistant::mix_assistant_result_to_json(result));
  const auto& expected_paths = sonare::mixing::assistant::mix_assistant_result_schema_paths();
  const std::set<std::string> expected(expected_paths.begin(), expected_paths.end());
  REQUIRE(actual == expected);
}

TEST_CASE("the assistant result's scene interior matches the scene document schema",
          "[mixing][assistant]") {
  // Both lists are written out literally so a reader outside this language can
  // parse them, which is exactly what lets the two copies drift; this is what
  // stops them.
  std::set<std::string> prefixed;
  for (const auto& path : sonare::mixing::api::scene_schema_paths()) {
    prefixed.insert("scene." + path);
  }
  std::set<std::string> interior;
  for (const auto& path : sonare::mixing::assistant::mix_assistant_result_schema_paths()) {
    if (path.rfind("scene.", 0) == 0) interior.insert(path);
  }
  REQUIRE(interior == prefixed);
}

TEST_CASE("the mixing assistant param builders refuse an unknown key", "[mixing][assistant]") {
  namespace assistant = sonare::mixing::assistant;
  const sonare::mastering::api::Param known[] = {{"suggestionStrength", 0.5}};
  CHECK(assistant::mix_assistant_config_from_params(known, 1).suggestion_strength == 0.5f);
  const sonare::mastering::api::Param snake[] = {{"min_duration_sec", 2.0}};
  CHECK(assistant::track_profile_config_from_params(snake, 1).min_duration_sec == 2.0f);

  // A misspelt key would otherwise leave its setting at the default unannounced.
  const sonare::mastering::api::Param misspelt[] = {{"suggestionStrenght", 0.5}};
  CHECK_THROWS_AS(assistant::mix_assistant_config_from_params(misspelt, 1),
                  sonare::SonareException);
  CHECK_THROWS_AS(assistant::track_profile_config_from_params(misspelt, 1),
                  sonare::SonareException);
}

// A track excluded before BS.1770 ever ran has no loudness, and reports it the
// way a measured-silent track does: integratedLufs is null, never a finite 0.
TEST_CASE("an unmeasured track reports its integrated loudness as null", "[mixing][assistant]") {
  std::vector<float> samples(4800, 0.1f);
  std::vector<float> poisoned = samples;
  poisoned[100] = std::numeric_limits<float>::quiet_NaN();
  const auto make_track = [](const std::string& id, const float* data, std::size_t frames,
                             int sample_rate) {
    TrackInput track;
    track.id = id;
    track.left = data;
    track.frame_count = frames;
    track.sample_rate = sample_rate;
    return track;
  };
  const std::vector<TrackInput> tracks = {
      make_track("empty", nullptr, 0, 48000),
      make_track("norate", samples.data(), samples.size(), 0),
      make_track("nonfinite", poisoned.data(), poisoned.size(), 48000),
  };
  const auto result = sonare::mixing::assistant::suggest_scene(tracks);
  REQUIRE(result.tracks.size() == tracks.size());
  const sonare::util::json::Value parsed =
      sonare::util::json::parse(sonare::mixing::assistant::mix_assistant_result_to_json(result));
  const sonare::util::json::Value* rows = parsed.find("tracks");
  REQUIRE(rows != nullptr);
  REQUIRE(rows->is_array());
  REQUIRE(rows->as_array().size() == tracks.size());
  for (const sonare::util::json::Value& row : rows->as_array()) {
    const sonare::util::json::Value* id = row.find("stripId");
    REQUIRE(id != nullptr);
    INFO(id->as_string());
    const sonare::util::json::Value* reason = row.find("exclusionReason");
    REQUIRE(reason != nullptr);
    CHECK_FALSE(reason->as_string().empty());
    const sonare::util::json::Value* lufs = row.find("integratedLufs");
    REQUIRE(lufs != nullptr);
    CHECK(lufs->is_null());
  }
}

// Zero is the only sentinel for the transport fallback; a negative tempo is a
// mistake to name, not a second spelling of zero.
TEST_CASE("the mixing assistant param builder refuses a negative tempo", "[mixing][assistant]") {
  namespace assistant = sonare::mixing::assistant;
  const sonare::mastering::api::Param fallback[] = {{"tempoBpm", 0.0}};
  CHECK(assistant::mix_assistant_config_from_params(fallback, 1).tempo_bpm == 0.0f);
  const sonare::mastering::api::Param stated[] = {{"tempoBpm", 120.0}};
  CHECK(assistant::mix_assistant_config_from_params(stated, 1).tempo_bpm == 120.0f);
  for (const double tempo : {-120.0, -0.5}) {
    INFO(tempo);
    const sonare::mastering::api::Param negative[] = {{"tempoBpm", tempo}};
    try {
      (void)assistant::mix_assistant_config_from_params(negative, 1);
      FAIL("negative tempo accepted");
    } catch (const sonare::SonareException& error) {
      CHECK(error.code() == sonare::ErrorCode::InvalidParameter);
      CHECK(std::string(error.what()).find("tempoBpm") != std::string::npos);
    }
  }
}

// The master is a structure decision, so with structure off there is no bus to
// carry the headroom trim; the correction must then be reported, not dropped.
TEST_CASE("a headroom correction with no master bus is reported", "[mixing][assistant]") {
  const auto fixture = make_demo_tracks(48000, 0.5f);
  const auto tracks = fixture.inputs();
  const auto mentions = [](const MixAssistantResult& result, const std::string& text) {
    return std::any_of(
        result.explanation.begin(), result.explanation.end(),
        [&text](const std::string& line) { return line.find(text) != std::string::npos; });
  };

  // Non-vacuity: with a master bus the same tracks do need pulling down.
  const auto with_master = sonare::mixing::assistant::suggest_scene(tracks);
  REQUIRE(mentions(with_master, "pulled the master bus down"));

  MixAssistantConfig config;
  config.enable_structure = false;
  const auto without_master = sonare::mixing::assistant::suggest_scene(tracks, config);
  REQUIRE(std::none_of(without_master.scene.buses.begin(), without_master.scene.buses.end(),
                       [](const sonare::mixing::api::Bus& bus) { return bus.role == "master"; }));
  CHECK(mentions(without_master, "headroom"));
}
