/// @file gain_staging_test.cpp
/// @brief Contract of the mixing assistant's static input-trim staging.

#include "mixing/assistant/gain_staging.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cctype>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "support/audio_fixtures.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using sonare::mixing::assistant::analyze_track_profile;
using sonare::mixing::assistant::decide_gain_staging;
using sonare::mixing::assistant::DeltaDomain;
using sonare::mixing::assistant::kBandCount;
using sonare::mixing::assistant::kMaxSuggestedTrimDb;
using sonare::mixing::assistant::kMinSuggestedTrimDb;
using sonare::mixing::assistant::kSourceClassCount;
using sonare::mixing::assistant::MixAssistantConfig;
using sonare::mixing::assistant::SceneDelta;
using sonare::mixing::assistant::SourceClass;
using sonare::mixing::assistant::TrackInput;
using sonare::mixing::assistant::TrackProfile;

namespace {

// Comfortably longer than the 400 ms gating block, so duration is never the
// reason a hand-built profile is excluded.
constexpr float kMeasurableDurationSec = 2.0f;
// Shorter than one gating block.
constexpr float kUnmeasurableDurationSec = 0.1f;
// The staging target every test measures against; matches the config default.
constexpr float kTargetLufs = -18.0f;
// Loudness comparisons are dB values built from float arithmetic, so a
// thousandth of a dB is far below anything a mixer could hear or act on.
constexpr float kDbTolerance = 1e-3f;

// An even spread across the analysis bands, which is what band_occupancy looks
// like for any track with real spectral content: it sums to 1.
std::array<float, kBandCount> even_occupancy() {
  std::array<float, kBandCount> bands{};
  bands.fill(1.0f / static_cast<float>(kBandCount));
  return bands;
}

TrackProfile make_profile(const std::string& id, float integrated_lufs) {
  TrackProfile profile;
  profile.strip_id = id;
  profile.name = id;
  profile.base.loudness.integrated_lufs = integrated_lufs;
  profile.base.duration_sec = kMeasurableDurationSec;
  profile.duration_sec = kMeasurableDurationSec;
  profile.band_occupancy = even_occupancy();
  profile.usable = true;
  return profile;
}

const SceneDelta* find_delta(const std::vector<SceneDelta>& deltas, const std::string& strip_id) {
  for (const SceneDelta& delta : deltas) {
    if (delta.strip_id == strip_id) return &delta;
  }
  return nullptr;
}

float spread_db(const std::vector<float>& values) {
  const auto bounds = std::minmax_element(values.begin(), values.end());
  return *bounds.second - *bounds.first;
}

bool mentions_the_trim_range(const SceneDelta& delta) {
  return delta.reason.find("beyond") != std::string::npos;
}

}  // namespace

TEST_CASE("gain staging pulls every track onto the absolute target", "[mixing][assistant]") {
  const std::vector<float> measured{-30.0f, -18.0f, -6.0f};
  std::vector<TrackProfile> profiles;
  for (std::size_t i = 0; i < measured.size(); ++i) {
    profiles.push_back(make_profile("track" + std::to_string(i), measured[i]));
  }

  const std::vector<SceneDelta> deltas = decide_gain_staging(profiles, MixAssistantConfig{});
  REQUIRE(deltas.size() == profiles.size());

  std::vector<float> applied;
  for (std::size_t i = 0; i < deltas.size(); ++i) {
    const SceneDelta& delta = deltas[i];
    CHECK(delta.domain == DeltaDomain::Gain);
    CHECK(delta.strip_id == profiles[i].strip_id);
    REQUIRE(delta.input_trim_db.has_value());
    CHECK_FALSE(delta.reason.empty());
    // The reason becomes one line of the explanation verbatim, so it reads as a
    // lower-case declarative sentence rather than a heading.
    CHECK(delta.reason[0] == std::tolower(static_cast<unsigned char>(delta.reason[0])));
    applied.push_back(measured[i] + *delta.input_trim_db);
  }

  CHECK(spread_db(applied) < spread_db(measured));
  for (const float level : applied) {
    CHECK_THAT(level, WithinAbs(kTargetLufs, kDbTolerance));
  }
}

TEST_CASE("gain staging follows the configured target rather than the tracks present",
          "[mixing][assistant]") {
  const std::vector<TrackProfile> quiet{make_profile("a", -30.0f), make_profile("b", -26.0f)};
  std::vector<TrackProfile> mixed = quiet;
  mixed.push_back(make_profile("c", -4.0f));

  const std::vector<SceneDelta> quiet_deltas = decide_gain_staging(quiet, MixAssistantConfig{});
  const std::vector<SceneDelta> mixed_deltas = decide_gain_staging(mixed, MixAssistantConfig{});

  // Adding a loud track would move an average-following target, and with it the
  // trim suggested for every track that was already there.
  REQUIRE(quiet_deltas.size() == 2);
  REQUIRE(mixed_deltas.size() == 3);
  for (std::size_t i = 0; i < quiet_deltas.size(); ++i) {
    REQUIRE(quiet_deltas[i].input_trim_db.has_value());
    REQUIRE(mixed_deltas[i].input_trim_db.has_value());
    CHECK_THAT(*mixed_deltas[i].input_trim_db,
               WithinAbs(*quiet_deltas[i].input_trim_db, kDbTolerance));
  }
}

TEST_CASE("a silent track gets no gain staging delta", "[mixing][assistant]") {
  std::vector<TrackProfile> profiles{make_profile("music", -24.0f), make_profile("silent", 0.0f),
                                     make_profile("floored", 0.0f)};
  // Silence reaches the profile either as the BS.1770 -inf sentinel or pinned
  // at the numerical dB floor; both mean the same thing here.
  profiles[1].base.loudness.integrated_lufs = -std::numeric_limits<float>::infinity();
  profiles[2].base.loudness.integrated_lufs = sonare::constants::kFloorDb;

  const std::vector<SceneDelta> deltas = decide_gain_staging(profiles, MixAssistantConfig{});

  CHECK(deltas.size() == 1);
  CHECK(find_delta(deltas, "music") != nullptr);
  // Not "a delta with a zero trim" — no delta at all, so nothing downstream can
  // read the exclusion as a decision to leave the track where it is.
  CHECK(find_delta(deltas, "silent") == nullptr);
  CHECK(find_delta(deltas, "floored") == nullptr);
}

TEST_CASE("a track shorter than one gating block gets no gain staging delta",
          "[mixing][assistant]") {
  std::vector<TrackProfile> profiles{make_profile("music", -24.0f), make_profile("stab", -12.0f)};
  profiles[1].duration_sec = kUnmeasurableDurationSec;

  const std::vector<SceneDelta> deltas = decide_gain_staging(profiles, MixAssistantConfig{});

  CHECK(deltas.size() == 1);
  CHECK(find_delta(deltas, "music") != nullptr);
  CHECK(find_delta(deltas, "stab") == nullptr);
}

TEST_CASE("a track with no band energy gets no gain staging delta", "[mixing][assistant]") {
  std::vector<TrackProfile> profiles{make_profile("music", -24.0f), make_profile("dc", -24.0f)};
  // DC sits below the lowest analysis band, so a DC-only track reports no
  // occupancy anywhere even though its loudness is not at the floor.
  profiles[1].band_occupancy.fill(0.0f);

  const std::vector<SceneDelta> deltas = decide_gain_staging(profiles, MixAssistantConfig{});

  CHECK(deltas.size() == 1);
  CHECK(find_delta(deltas, "music") != nullptr);
  CHECK(find_delta(deltas, "dc") == nullptr);
}

TEST_CASE("a profile the profiler marked unusable gets no gain staging delta",
          "[mixing][assistant]") {
  std::vector<TrackProfile> profiles{make_profile("music", -24.0f),
                                     make_profile("rejected", -24.0f)};
  profiles[1].usable = false;
  profiles[1].exclusion_reason = "excluded by the profiler";

  const std::vector<SceneDelta> deltas = decide_gain_staging(profiles, MixAssistantConfig{});

  CHECK(deltas.size() == 1);
  CHECK(find_delta(deltas, "rejected") == nullptr);
}

TEST_CASE("a track further from the target than the trim range says so", "[mixing][assistant]") {
  // Far enough either side of the target that the correction cannot fit in the
  // suggested trim range.
  const std::vector<TrackProfile> profiles{make_profile("whisper", -70.0f),
                                           make_profile("blaring", 12.0f),
                                           make_profile("normal", -21.0f)};

  const std::vector<SceneDelta> deltas = decide_gain_staging(profiles, MixAssistantConfig{});
  REQUIRE(deltas.size() == 3);

  const SceneDelta* whisper = find_delta(deltas, "whisper");
  const SceneDelta* blaring = find_delta(deltas, "blaring");
  const SceneDelta* normal = find_delta(deltas, "normal");
  REQUIRE(whisper != nullptr);
  REQUIRE(blaring != nullptr);
  REQUIRE(normal != nullptr);

  CHECK(mentions_the_trim_range(*whisper));
  CHECK(mentions_the_trim_range(*blaring));
  CHECK_FALSE(mentions_the_trim_range(*normal));

  // The value itself is left unclamped: apply_deltas clamps the summed total
  // once, and clamping here as well would hide how far the total overshot.
  REQUIRE(whisper->input_trim_db.has_value());
  REQUIRE(blaring->input_trim_db.has_value());
  CHECK(*whisper->input_trim_db > kMaxSuggestedTrimDb);
  CHECK(*blaring->input_trim_db < kMinSuggestedTrimDb);
  CHECK_THAT(*whisper->input_trim_db, WithinAbs(52.0f, kDbTolerance));
  CHECK_THAT(*blaring->input_trim_db, WithinAbs(-30.0f, kDbTolerance));
}

TEST_CASE("zero suggestion strength still decides every track, at zero trim",
          "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles{make_profile("a", -30.0f), make_profile("b", -6.0f)};
  MixAssistantConfig config;
  config.suggestion_strength = 0.0f;

  const std::vector<SceneDelta> deltas = decide_gain_staging(profiles, config);

  // A zero trim decided on purpose is a decision, so the deltas are still
  // emitted; only an excluded track is absent altogether.
  REQUIRE(deltas.size() == profiles.size());
  for (const SceneDelta& delta : deltas) {
    REQUIRE(delta.input_trim_db.has_value());
    CHECK_THAT(*delta.input_trim_db, WithinAbs(0.0f, kDbTolerance));
  }
}

TEST_CASE("suggestion strength scales the trim proportionally", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles{make_profile("a", -30.0f)};
  MixAssistantConfig half;
  half.suggestion_strength = 0.5f;

  const std::vector<SceneDelta> full_deltas = decide_gain_staging(profiles, MixAssistantConfig{});
  const std::vector<SceneDelta> half_deltas = decide_gain_staging(profiles, half);
  REQUIRE(full_deltas.size() == 1);
  REQUIRE(half_deltas.size() == 1);
  REQUIRE(full_deltas[0].input_trim_db.has_value());
  REQUIRE(half_deltas[0].input_trim_db.has_value());

  CHECK_THAT(*half_deltas[0].input_trim_db,
             WithinAbs(0.5f * *full_deltas[0].input_trim_db, kDbTolerance));
}

TEST_CASE("degenerate input yields an empty suggestion", "[mixing][assistant]") {
  SECTION("no tracks") { CHECK(decide_gain_staging({}, MixAssistantConfig{}).empty()); }

  SECTION("every track excluded") {
    std::vector<TrackProfile> profiles{make_profile("silent", 0.0f), make_profile("stab", -12.0f),
                                       make_profile("dc", -12.0f)};
    profiles[0].base.loudness.integrated_lufs = -std::numeric_limits<float>::infinity();
    profiles[1].duration_sec = kUnmeasurableDurationSec;
    profiles[2].band_occupancy.fill(0.0f);
    CHECK(decide_gain_staging(profiles, MixAssistantConfig{}).empty());
  }

  SECTION("the gain domain is disabled") {
    const std::vector<TrackProfile> profiles{make_profile("a", -30.0f)};
    MixAssistantConfig config;
    config.enable_gain = false;
    CHECK(decide_gain_staging(profiles, config).empty());
  }
}

TEST_CASE("the source class does not change the staging decision", "[mixing][assistant]") {
  const TrackProfile base = make_profile("track", -27.5f);
  const std::vector<SceneDelta> reference = decide_gain_staging({base}, MixAssistantConfig{});
  REQUIRE(reference.size() == 1);
  REQUIRE(reference[0].input_trim_db.has_value());

  // A class-relative offset is the balance pass's decision. If staging ever
  // starts reading the class, the two stages are making the same decision twice.
  for (int index = 0; index < kSourceClassCount; ++index) {
    TrackProfile profile = base;
    profile.source = static_cast<SourceClass>(index);
    profile.source_confidence = 1.0f;

    const std::vector<SceneDelta> deltas = decide_gain_staging({profile}, MixAssistantConfig{});
    REQUIRE(deltas.size() == 1);
    REQUIRE(deltas[0].input_trim_db.has_value());
    CHECK_THAT(*deltas[0].input_trim_db, WithinAbs(*reference[0].input_trim_db, kDbTolerance));
    CHECK(deltas[0].reason == reference[0].reason);
  }
}

TEST_CASE("a measured track lands on the target once its trim is applied", "[mixing][assistant]") {
  // Long enough to clear the gating block, short enough to stay out of the slow
  // tier.
  constexpr int kSampleRate = 48000;
  constexpr float kDurationSec = 0.5f;
  constexpr float kToneHz = 440.0f;
  constexpr float kAmplitude = 0.25f;
  const int frames = static_cast<int>(kSampleRate * kDurationSec);
  const std::vector<float> samples =
      sonare::test::generate_sine_samples(kToneHz, kSampleRate, frames, kAmplitude);

  TrackInput track;
  track.id = "tone";
  track.name = "tone";
  track.left = samples.data();
  track.frame_count = samples.size();
  track.sample_rate = kSampleRate;

  const TrackProfile profile = analyze_track_profile(track);
  REQUIRE(profile.usable);

  const std::vector<SceneDelta> deltas = decide_gain_staging({profile}, MixAssistantConfig{});
  REQUIRE(deltas.size() == 1);
  CHECK(deltas[0].strip_id == "tone");
  REQUIRE(deltas[0].input_trim_db.has_value());

  const float applied = profile.base.loudness.integrated_lufs + *deltas[0].input_trim_db;
  CHECK_THAT(applied, WithinAbs(kTargetLufs, kDbTolerance));
}
