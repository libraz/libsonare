/// @file decide_balance_test.cpp
/// @brief Contract of the mixing assistant's class-relative level balance.

#include "mixing/assistant/decide_balance.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "mastering/assistant/audio_profile.h"
#include "mixing/assistant/scene_delta.h"
#include "mixing/assistant/track_profile.h"

using Catch::Matchers::WithinAbs;
using sonare::mastering::assistant::GenreCandidate;
using sonare::mixing::assistant::balance_table_count;
using sonare::mixing::assistant::balance_table_index_for_genre;
using sonare::mixing::assistant::decide_balance;
using sonare::mixing::assistant::DeltaDomain;
using sonare::mixing::assistant::kBandCount;
using sonare::mixing::assistant::kDefaultBalanceTableIndex;
using sonare::mixing::assistant::kSourceClassCount;
using sonare::mixing::assistant::MixAssistantConfig;
using sonare::mixing::assistant::SceneDelta;
using sonare::mixing::assistant::source_class_to_string;
using sonare::mixing::assistant::SourceClass;
using sonare::mixing::assistant::TrackProfile;

namespace {

// A classification the decision table matched cleanly, so the class offset is
// applied in full and nothing in a test is scaled by an incidental confidence.
constexpr float kCertain = 1.0f;
// Confidently above the floor but clearly short of certain.
constexpr float kMarginal = 0.6f;
// Below the floor at which a class offset is acted on at all.
constexpr float kTooWeak = 0.2f;
// Comfortably longer than the 400 ms gating block, so duration is never the
// reason a hand-built profile is excluded.
constexpr float kMeasurableDurationSec = 2.0f;
// The offsets are dB values built from float arithmetic, so a thousandth of a
// dB is far below anything a mixer could hear or act on.
constexpr float kDbTolerance = 1e-3f;

// An even spread across the analysis bands, which is what band_occupancy looks
// like for any track with real spectral content: it sums to 1.
std::array<float, kBandCount> even_occupancy() {
  std::array<float, kBandCount> bands{};
  bands.fill(1.0f / static_cast<float>(kBandCount));
  return bands;
}

TrackProfile make_profile(const std::string& id, SourceClass source, float confidence) {
  TrackProfile profile;
  profile.strip_id = id;
  profile.name = id;
  profile.base.duration_sec = kMeasurableDurationSec;
  profile.duration_sec = kMeasurableDurationSec;
  profile.band_occupancy = even_occupancy();
  profile.source = source;
  profile.source_confidence = confidence;
  profile.usable = true;
  return profile;
}

// One confidently classified track per class, skipping Unknown, each named
// after its class so a failing assertion says which part moved.
std::vector<TrackProfile> profiles_for_every_class() {
  std::vector<TrackProfile> profiles;
  for (int index = 1; index < kSourceClassCount; ++index) {
    const auto source = static_cast<SourceClass>(index);
    profiles.push_back(make_profile(source_class_to_string(source), source, kCertain));
  }
  return profiles;
}

const SceneDelta* find_delta(const std::vector<SceneDelta>& deltas, const std::string& strip_id) {
  for (const SceneDelta& delta : deltas) {
    if (delta.strip_id == strip_id) return &delta;
  }
  return nullptr;
}

float fader_of(const std::vector<SceneDelta>& deltas, const std::string& strip_id) {
  const SceneDelta* delta = find_delta(deltas, strip_id);
  REQUIRE(delta != nullptr);
  REQUIRE(delta->fader_db.has_value());
  return *delta->fader_db;
}

}  // namespace

TEST_CASE("balance places forward parts above bed parts", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = profiles_for_every_class();
  const std::vector<SceneDelta> deltas = decide_balance(profiles, MixAssistantConfig{});
  REQUIRE(deltas.size() == profiles.size());

  // Only the ordering is asserted. The table holds studio convention rather than
  // derived values, so any row may be retuned; what may not change is which part
  // sits in front of which.
  CHECK(fader_of(deltas, "vocal") > fader_of(deltas, "lead"));
  CHECK(fader_of(deltas, "lead") > fader_of(deltas, "guitar"));
  CHECK(fader_of(deltas, "guitar") > fader_of(deltas, "strings"));
  CHECK(fader_of(deltas, "vocal") > fader_of(deltas, "backing"));
  CHECK(fader_of(deltas, "kick") > fader_of(deltas, "hiHat"));
  CHECK(fader_of(deltas, "kick") > fader_of(deltas, "cymbal"));
  CHECK(fader_of(deltas, "snare") > fader_of(deltas, "tom"));
  CHECK(fader_of(deltas, "bass") > fader_of(deltas, "keys"));
  CHECK(fader_of(deltas, "lead") > fader_of(deltas, "percussion"));

  // The anchors and the focus parts sit above the staged reference; the beds and
  // the garnish sit below it.
  CHECK(fader_of(deltas, "kick") > 0.0f);
  CHECK(fader_of(deltas, "bass") > 0.0f);
  CHECK(fader_of(deltas, "snare") > 0.0f);
  CHECK(fader_of(deltas, "vocal") > 0.0f);
  CHECK(fader_of(deltas, "keys") < 0.0f);
  CHECK(fader_of(deltas, "strings") < 0.0f);
  CHECK(fader_of(deltas, "cymbal") < 0.0f);
  CHECK(fader_of(deltas, "fx") < 0.0f);

  // The voice is the most forward element and an effect return the least, so
  // both are extremes rather than merely well placed.
  for (const SceneDelta& delta : deltas) {
    REQUIRE(delta.fader_db.has_value());
    CHECK(delta.domain == DeltaDomain::Gain);
    if (delta.strip_id != "vocal") CHECK(*delta.fader_db < fader_of(deltas, "vocal"));
    if (delta.strip_id != "fx") CHECK(*delta.fader_db > fader_of(deltas, "fx"));
  }
}

TEST_CASE("a balance reason names the class and the amount", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles{make_profile("lead vox", SourceClass::Vocal, kCertain)};
  const std::vector<SceneDelta> deltas = decide_balance(profiles, MixAssistantConfig{});
  REQUIRE(deltas.size() == 1);

  const std::string& reason = deltas[0].reason;
  REQUIRE_FALSE(reason.empty());
  // The reason becomes one line of the explanation verbatim, so it reads as a
  // lower-case declarative sentence rather than a heading.
  CHECK(reason[0] == std::tolower(static_cast<unsigned char>(reason[0])));
  CHECK(reason.find("lead vox") != std::string::npos);
  CHECK(reason.find("vocal") != std::string::npos);
  CHECK(reason.find(" dB") != std::string::npos);
  // One decision is one line, so nothing in it wraps.
  CHECK(reason.find('\n') == std::string::npos);
}

TEST_CASE("an unclassified track gets no balance delta", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles{
      make_profile("kick", SourceClass::Kick, kCertain),
      // Confidently unclassified is still unclassified: there is no class whose
      // customary position could be applied to the track.
      make_profile("mystery", SourceClass::Unknown, kCertain)};

  const std::vector<SceneDelta> deltas = decide_balance(profiles, MixAssistantConfig{});

  CHECK(deltas.size() == 1);
  CHECK(find_delta(deltas, "kick") != nullptr);
  // Not a delta carrying a zero offset — no delta at all, so nothing downstream
  // can read the exclusion as a decision to leave the track where it is.
  CHECK(find_delta(deltas, "mystery") == nullptr);
}

TEST_CASE("a profile the profiler marked unusable gets no balance delta", "[mixing][assistant]") {
  std::vector<TrackProfile> profiles{make_profile("kick", SourceClass::Kick, kCertain),
                                     make_profile("rejected", SourceClass::Vocal, kCertain)};
  profiles[1].usable = false;
  profiles[1].exclusion_reason = "excluded by the profiler";

  const std::vector<SceneDelta> deltas = decide_balance(profiles, MixAssistantConfig{});

  CHECK(deltas.size() == 1);
  CHECK(find_delta(deltas, "kick") != nullptr);
  CHECK(find_delta(deltas, "rejected") == nullptr);
}

TEST_CASE("a weak classification moves the fader less than a confident one",
          "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles{make_profile("sure", SourceClass::Vocal, kCertain),
                                           make_profile("unsure", SourceClass::Vocal, kMarginal),
                                           make_profile("guess", SourceClass::Vocal, kTooWeak)};

  const std::vector<SceneDelta> deltas = decide_balance(profiles, MixAssistantConfig{});

  const float sure = fader_of(deltas, "sure");
  const float unsure = fader_of(deltas, "unsure");
  // Same class, same table row: the only difference is how sure the classifier
  // was, and a suggestion built on a misread class is worse than none.
  CHECK(std::fabs(unsure) < std::fabs(sure));
  CHECK(unsure > 0.0f);
  CHECK_THAT(unsure, WithinAbs(kMarginal * sure, kDbTolerance));

  // Below the floor the class is not acted on at all.
  CHECK(find_delta(deltas, "guess") == nullptr);
}

TEST_CASE("zero suggestion strength still decides every track, at zero offset",
          "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = profiles_for_every_class();
  MixAssistantConfig config;
  config.suggestion_strength = 0.0f;

  const std::vector<SceneDelta> deltas = decide_balance(profiles, config);

  // A zero offset decided on purpose is a decision, so the deltas are still
  // emitted; only an excluded track is absent altogether.
  REQUIRE(deltas.size() == profiles.size());
  for (const SceneDelta& delta : deltas) {
    REQUIRE(delta.fader_db.has_value());
    CHECK_THAT(*delta.fader_db, WithinAbs(0.0f, kDbTolerance));
  }
}

TEST_CASE("suggestion strength scales the offset proportionally", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = profiles_for_every_class();
  MixAssistantConfig half;
  half.suggestion_strength = 0.5f;

  const std::vector<SceneDelta> full_deltas = decide_balance(profiles, MixAssistantConfig{});
  const std::vector<SceneDelta> half_deltas = decide_balance(profiles, half);
  REQUIRE(full_deltas.size() == profiles.size());
  REQUIRE(half_deltas.size() == full_deltas.size());

  for (const SceneDelta& full : full_deltas) {
    REQUIRE(full.fader_db.has_value());
    CHECK_THAT(fader_of(half_deltas, full.strip_id),
               WithinAbs(0.5f * *full.fader_db, kDbTolerance));
  }
}

TEST_CASE("an unrecognised genre falls back to the default balance table", "[mixing][assistant]") {
  REQUIRE(balance_table_count() >= 1);
  CHECK(kDefaultBalanceTableIndex < balance_table_count());

  // A genre the profiler has never emitted, and no genre at all, both resolve to
  // a table rather than to no balance.
  CHECK(balance_table_index_for_genre("") == kDefaultBalanceTableIndex);
  CHECK(balance_table_index_for_genre("notAGenre") == kDefaultBalanceTableIndex);

  // Every label the profiler can emit resolves to a table that exists.
  const std::vector<std::string> labels{"pop",      "edm",       "techno",  "trance", "drumAndBass",
                                        "hipHop",   "trap",      "rnb",     "metal",  "jazz",
                                        "acoustic", "classical", "ambient", "lofi",   "jpop",
                                        "kpop",     "gameOst",   "speech"};
  for (const std::string& label : labels) {
    CHECK(balance_table_index_for_genre(label) < balance_table_count());
  }
}

TEST_CASE("the genre read from the tracks selects one table for the whole call",
          "[mixing][assistant]") {
  std::vector<TrackProfile> pop = profiles_for_every_class();
  for (TrackProfile& profile : pop) {
    profile.base.genre_candidates.push_back(GenreCandidate{"pop", 0.9f});
  }
  std::vector<TrackProfile> classical = profiles_for_every_class();
  for (TrackProfile& profile : classical) {
    profile.base.genre_candidates.push_back(GenreCandidate{"classical", 0.9f});
  }

  const std::vector<SceneDelta> pop_deltas = decide_balance(pop, MixAssistantConfig{});
  const std::vector<SceneDelta> classical_deltas = decide_balance(classical, MixAssistantConfig{});
  REQUIRE(pop_deltas.size() == classical_deltas.size());

  // Both genres resolve to the same table while only one exists, so the two runs
  // must agree. Once a second table lands this case is what shows the switch
  // reaching the decision.
  for (const SceneDelta& delta : pop_deltas) {
    REQUIRE(delta.fader_db.has_value());
    CHECK_THAT(fader_of(classical_deltas, delta.strip_id),
               WithinAbs(*delta.fader_db, kDbTolerance));
  }
}

TEST_CASE("a mix with no genre at all is still balanced", "[mixing][assistant]") {
  // The profiler leaves genre_candidates empty for material it cannot place, and
  // that must not cost the mix its balance.
  const std::vector<TrackProfile> profiles = profiles_for_every_class();
  CHECK(decide_balance(profiles, MixAssistantConfig{}).size() == profiles.size());
}

TEST_CASE("degenerate input yields an empty balance suggestion", "[mixing][assistant]") {
  SECTION("no tracks") { CHECK(decide_balance({}, MixAssistantConfig{}).empty()); }

  SECTION("every track excluded") {
    std::vector<TrackProfile> profiles{make_profile("mystery", SourceClass::Unknown, kCertain),
                                       make_profile("guess", SourceClass::Vocal, kTooWeak),
                                       make_profile("rejected", SourceClass::Kick, kCertain)};
    profiles[2].usable = false;
    CHECK(decide_balance(profiles, MixAssistantConfig{}).empty());
  }

  SECTION("the balance domain is disabled") {
    const std::vector<TrackProfile> profiles{make_profile("vox", SourceClass::Vocal, kCertain)};
    MixAssistantConfig config;
    config.enable_balance = false;
    CHECK(decide_balance(profiles, config).empty());
  }
}

TEST_CASE("balance never sets an input trim", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = profiles_for_every_class();
  const std::vector<SceneDelta> deltas = decide_balance(profiles, MixAssistantConfig{});
  REQUIRE_FALSE(deltas.empty());

  // Staging owns the absolute-target trim and this stage owns the class-relative
  // fader offset. If balance ever starts writing a trim, the two stages are
  // making the same decision twice and the scene no longer says which is which.
  for (const SceneDelta& delta : deltas) {
    CHECK_FALSE(delta.input_trim_db.has_value());
    CHECK_FALSE(delta.vca_offset_db.has_value());
    CHECK_FALSE(delta.pan.has_value());
    CHECK(delta.inserts.empty());
    CHECK(delta.fader_db.has_value());
  }
}
