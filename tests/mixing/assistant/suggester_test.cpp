/// @file suggester_test.cpp
/// @brief End-to-end behaviour of the mixing assistant's suggestion pipeline.

#include "mixing/assistant/suggester.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "mix_eval.h"
#include "mixing/api/scene.h"
#include "mixing/assistant/source_classifier.h"
#include "mixing/assistant/track_profile.h"
#include "util/exception.h"
#include "util/json.h"

namespace {

using sonare::mixing::assistant::MixAssistantConfig;
using sonare::mixing::assistant::MixAssistantResult;
using sonare::mixing::assistant::TrackInput;
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
