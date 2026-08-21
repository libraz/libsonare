/// @file decide_eq_test.cpp
/// @brief Contract of the mixing assistant's static corrective EQ stage.

#include "mixing/assistant/decide_eq.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cctype>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "util/json.h"

using Catch::Matchers::WithinAbs;
using sonare::mastering::api::insert_factory_names;
using sonare::mixing::api::Insert;
using sonare::mixing::api::InsertSlot;
using sonare::mixing::assistant::BandDominance;
using sonare::mixing::assistant::decide_eq;
using sonare::mixing::assistant::DeltaDomain;
using sonare::mixing::assistant::kBandCount;
using sonare::mixing::assistant::MixAssistantConfig;
using sonare::mixing::assistant::MixProfile;
using sonare::mixing::assistant::SceneDelta;
using sonare::mixing::assistant::SourceClass;
using sonare::mixing::assistant::TrackProfile;

namespace {

// The two processor names the stage may suggest. Spelled out here rather than
// reused from the module so a rename has to be made deliberately in both.
const std::string kParametric = "eq.parametric";
const std::string kCutFilter = "eq.cutFilter";

// Analysis band indices, matching kBands. The mid pair is what most cases use:
// both sit above every high-pass corner in the module's table, so a cut there
// is never suppressed as redundant.
constexpr int kSubBand = 0;
constexpr int kMidBand = 3;
constexpr int kHighMidBand = 4;

// Comfortably above the classifier's acceptance floor, so confidence is never
// the reason a hand-built profile is skipped.
constexpr float kHighConfidence = 0.9f;
// Classified, but not confidently enough to carve a part on.
constexpr float kLowConfidence = 0.2f;

// Longer than one loudness gating block, matching the other assistant tests.
constexpr float kMeasurableDurationSec = 2.0f;

// Overlap long enough to read as a standing conflict rather than a coincidence.
constexpr int kOverlapFrames = 256;
// A handful of frames where the two happened to sound together.
constexpr int kBriefOverlapFrames = 4;

// Equal band energy: neither track is in the other's way.
constexpr float kEvenShare = 0.5f;
// Past the interference threshold, but not far enough to reach the ceiling.
constexpr float kContestedShare = 0.75f;
// Effectively one track owning the band outright.
constexpr float kBuriedShare = 0.99f;

// The default ceiling, mirrored so the expectations read as numbers rather than
// as config lookups.
constexpr float kDefaultMaxCutDb = 4.0f;
// dB values assembled from float arithmetic; a thousandth of a dB is far below
// anything audible.
constexpr float kDbTolerance = 1e-3f;
// Frequencies are written into JSON as doubles and read back; a tenth of a Hz
// is far below the resolution any of these corners are specified at.
constexpr float kHzTolerance = 0.1f;

TrackProfile make_profile(const std::string& id, SourceClass source) {
  TrackProfile profile;
  profile.strip_id = id;
  profile.name = id;
  profile.source = source;
  profile.source_confidence = kHighConfidence;
  profile.base.duration_sec = kMeasurableDurationSec;
  profile.duration_sec = kMeasurableDurationSec;
  profile.band_occupancy.fill(1.0f / static_cast<float>(kBandCount));
  profile.usable = true;
  return profile;
}

// Concentrates @p share of the track's energy in @p band and spreads the rest
// evenly, so the occupancy still sums to 1 the way a measured profile does.
void set_band_occupancy(TrackProfile& profile, int band, float share) {
  const float rest = (1.0f - share) / static_cast<float>(kBandCount - 1);
  profile.band_occupancy.fill(rest);
  profile.band_occupancy[static_cast<std::size_t>(band)] = share;
}

MixProfile make_mix(int track_count) {
  MixProfile mix;
  mix.track_count = track_count;
  mix.dominance.assign(static_cast<std::size_t>(track_count) *
                           static_cast<std::size_t>(track_count) *
                           static_cast<std::size_t>(kBandCount),
                       BandDominance{});
  return mix;
}

void set_dominance(MixProfile& mix, int masker, int maskee, int band, float ratio, int frames) {
  const std::size_t index =
      (static_cast<std::size_t>(masker) * static_cast<std::size_t>(mix.track_count) +
       static_cast<std::size_t>(maskee)) *
          static_cast<std::size_t>(kBandCount) +
      static_cast<std::size_t>(band);
  BandDominance entry;
  entry.ratio = ratio;
  entry.valid_frames = frames;
  mix.dominance[index] = entry;
  // The measure is a share of the pair's energy, so the opposite direction is
  // what is left over. Filling it keeps a hand-built matrix consistent with
  // what the masking pass would have produced.
  const std::size_t mirror =
      (static_cast<std::size_t>(maskee) * static_cast<std::size_t>(mix.track_count) +
       static_cast<std::size_t>(masker)) *
          static_cast<std::size_t>(kBandCount) +
      static_cast<std::size_t>(band);
  BandDominance opposite;
  opposite.ratio = 1.0f - ratio;
  opposite.valid_frames = frames;
  mix.dominance[mirror] = opposite;
}

const SceneDelta* find_delta(const std::vector<SceneDelta>& deltas, const std::string& strip_id,
                             const std::string& processor_name) {
  for (const SceneDelta& delta : deltas) {
    if (delta.strip_id != strip_id) continue;
    for (const Insert& insert : delta.inserts) {
      if (insert.processor_name == processor_name) return &delta;
    }
  }
  return nullptr;
}

const Insert* find_insert(const std::vector<SceneDelta>& deltas, const std::string& strip_id,
                          const std::string& processor_name) {
  for (const SceneDelta& delta : deltas) {
    if (delta.strip_id != strip_id) continue;
    for (const Insert& insert : delta.inserts) {
      if (insert.processor_name == processor_name) return &insert;
    }
  }
  return nullptr;
}

int count_inserts(const std::vector<SceneDelta>& deltas, const std::string& processor_name) {
  int count = 0;
  for (const SceneDelta& delta : deltas) {
    for (const Insert& insert : delta.inserts) {
      if (insert.processor_name == processor_name) ++count;
    }
  }
  return count;
}

// "band12.gainDb" splits into 12 and "gainDb"; anything else is not a key the
// parametric EQ reads.
bool split_band_key(const std::string& key, int* out_index, std::string* out_field) {
  if (key.rfind("band", 0) != 0) return false;
  std::size_t cursor = 4;
  int index = 0;
  bool any_digit = false;
  while (cursor < key.size() && std::isdigit(static_cast<unsigned char>(key[cursor])) != 0) {
    index = index * 10 + (key[cursor] - '0');
    any_digit = true;
    ++cursor;
  }
  if (!any_digit) return false;
  if (cursor >= key.size() || key[cursor] != '.') return false;
  if (cursor + 1 >= key.size()) return false;
  *out_index = index;
  *out_field = key.substr(cursor + 1);
  return true;
}

sonare::util::json::Value parse_params(const Insert& insert) {
  return sonare::util::json::parse(insert.params_json);
}

std::vector<TrackProfile> two_tracks() {
  return {make_profile("vox", SourceClass::Vocal), make_profile("pad", SourceClass::Strings)};
}

}  // namespace

TEST_CASE("eq suggests no cut when no band is contested", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks();
  MixProfile mix = make_mix(2);
  for (int band = 0; band < kBandCount; ++band) {
    set_dominance(mix, 0, 1, band, kEvenShare, kOverlapFrames);
  }

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  CHECK(count_inserts(deltas, kParametric) == 0);
  for (const SceneDelta& delta : deltas) {
    CHECK(delta.domain == DeltaDomain::Eq);
  }
}

TEST_CASE("eq ignores a collision the two tracks barely shared", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks();
  MixProfile mix = make_mix(2);
  // One track owns the band outright, but only for a handful of frames, which
  // is a coincidence rather than a standing conflict.
  set_dominance(mix, 1, 0, kMidBand, kBuriedShare, kBriefOverlapFrames);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  CHECK(count_inserts(deltas, kParametric) == 0);
}

TEST_CASE("eq carves the lower-priority track, not the quieter one", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks();
  MixProfile mix = make_mix(2);
  // The pad is the one burying the vocal, so a rule that attenuated whichever
  // track lost the energy contest would carve the vocal here.
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  CHECK(find_insert(deltas, "pad", kParametric) != nullptr);
  CHECK(find_insert(deltas, "vox", kParametric) == nullptr);
  CHECK(count_inserts(deltas, kParametric) == 1);
}

TEST_CASE("eq breaks a priority tie on band occupancy", "[mixing][assistant]") {
  std::vector<TrackProfile> profiles{make_profile("gtrL", SourceClass::Guitar),
                                     make_profile("gtrR", SourceClass::Guitar)};
  // The dominant track is the one barely invested in the band, so the band
  // belongs to its neighbour and the dominant track is what gives way.
  set_band_occupancy(profiles[0], kMidBand, 0.1f);
  set_band_occupancy(profiles[1], kMidBand, 0.4f);

  MixProfile mix = make_mix(2);
  set_dominance(mix, 0, 1, kMidBand, kContestedShare, kOverlapFrames);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  CHECK(find_insert(deltas, "gtrL", kParametric) != nullptr);
  CHECK(find_insert(deltas, "gtrR", kParametric) == nullptr);
}

TEST_CASE("eq never cuts deeper than the configured ceiling", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks();
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kBuriedShare, kOverlapFrames);

  MixAssistantConfig config;
  config.eq_max_cut_db = kDefaultMaxCutDb;
  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, config);

  const Insert* insert = find_insert(deltas, "pad", kParametric);
  REQUIRE(insert != nullptr);
  const sonare::util::json::Value params = parse_params(*insert);
  REQUIRE(params.contains("band0.gainDb"));
  CHECK_THAT(params["band0.gainDb"].as_float(), WithinAbs(-kDefaultMaxCutDb, kDbTolerance));

  const SceneDelta* delta = find_delta(deltas, "pad", kParametric);
  REQUIRE(delta != nullptr);
  CHECK(delta->reason.find("ceiling") != std::string::npos);
}

TEST_CASE("eq reports the ceiling only when it actually bit", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks();
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);

  MixAssistantConfig config;
  config.eq_max_cut_db = kDefaultMaxCutDb;
  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, config);

  const Insert* insert = find_insert(deltas, "pad", kParametric);
  REQUIRE(insert != nullptr);
  const sonare::util::json::Value params = parse_params(*insert);
  REQUIRE(params.contains("band0.gainDb"));
  const float gain_db = params["band0.gainDb"].as_float();
  CHECK(gain_db < 0.0f);
  CHECK(gain_db > -kDefaultMaxCutDb);

  const SceneDelta* delta = find_delta(deltas, "pad", kParametric);
  REQUIRE(delta != nullptr);
  CHECK(delta->reason.find("ceiling") == std::string::npos);
}

TEST_CASE("eq raises the cut as the collision worsens, up to the ceiling", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks();

  const auto cut_for = [&profiles](float share) {
    MixProfile mix = make_mix(2);
    set_dominance(mix, 1, 0, kMidBand, share, kOverlapFrames);
    const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});
    const Insert* insert = find_insert(deltas, "pad", kParametric);
    REQUIRE(insert != nullptr);
    const sonare::util::json::Value params = parse_params(*insert);
    return -params["band0.gainDb"].as_float();
  };

  CHECK(cut_for(0.80f) > cut_for(0.70f));
  CHECK(cut_for(0.80f) <= kDefaultMaxCutDb);
}

TEST_CASE("eq writes params the parametric equalizer can read", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks();
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);
  set_dominance(mix, 1, 0, kHighMidBand, kContestedShare, kOverlapFrames);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  const Insert* insert = find_insert(deltas, "pad", kParametric);
  REQUIRE(insert != nullptr);
  const sonare::util::json::Value params = parse_params(*insert);
  REQUIRE(params.is_object());

  std::set<int> band_indices;
  for (const auto& [key, value] : params.as_object()) {
    int index = 0;
    std::string field;
    INFO("param key " << key);
    REQUIRE(split_band_key(key, &index, &field));
    band_indices.insert(index);
    // The insert factory's strict parse accepts nothing else.
    CHECK((value.is_number() || value.is_bool()));
  }

  // Bands are numbered contiguously from zero.
  REQUIRE(band_indices.size() == 2);
  CHECK(band_indices.count(0) == 1);
  CHECK(band_indices.count(1) == 1);
}

TEST_CASE("eq suggests only processors the insert factory can build", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks();
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});
  REQUIRE_FALSE(deltas.empty());

  const std::vector<std::string> known = insert_factory_names();
  int inspected = 0;
  for (const SceneDelta& delta : deltas) {
    for (const Insert& insert : delta.inserts) {
      INFO("processor " << insert.processor_name);
      CHECK(std::find(known.begin(), known.end(), insert.processor_name) != known.end());
      ++inspected;
    }
  }
  CHECK(inspected > 0);
  // Both names the stage may emit are exercised by this arrangement.
  CHECK(count_inserts(deltas, kParametric) == 1);
  CHECK(count_inserts(deltas, kCutFilter) == 2);
}

TEST_CASE("eq folds every contested band into one parametric insert", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks();
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kContestedShare, kOverlapFrames);
  set_dominance(mix, 1, 0, kHighMidBand, kBuriedShare, kOverlapFrames);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  // A strip drops a second copy of the same processor in the same slot, so two
  // inserts would silently throw one of the two cuts away.
  CHECK(count_inserts(deltas, kParametric) == 1);

  const Insert* insert = find_insert(deltas, "pad", kParametric);
  REQUIRE(insert != nullptr);
  CHECK(insert->slot == InsertSlot::PreFader);
  const sonare::util::json::Value params = parse_params(*insert);
  REQUIRE(params.contains("band0.frequencyHz"));
  REQUIRE(params.contains("band1.frequencyHz"));
  CHECK_FALSE(params.contains("band2.frequencyHz"));
  // Ascending frequency order, so band0 is the lower of the two.
  CHECK(params["band0.frequencyHz"].as_float() < params["band1.frequencyHz"].as_float());

  const SceneDelta* delta = find_delta(deltas, "pad", kParametric);
  REQUIRE(delta != nullptr);
  // Both bands are named in the one reason the delta carries.
  CHECK(delta->reason.find("mid") != std::string::npos);
  CHECK(delta->reason.find("highMid") != std::string::npos);
}

TEST_CASE("eq high-passes each source class at its own corner", "[mixing][assistant]") {
  struct Expected {
    SourceClass source;
    std::string id;
    // 0 means the class keeps its low end.
    float corner_hz;
  };

  // Mirrors the module's table on purpose: a corner that moves without anyone
  // meaning to move it fails here rather than passing whatever the code says.
  const std::vector<Expected> expected{
      {SourceClass::Vocal, "vox", 80.0f},        {SourceClass::Lead, "lead", 80.0f},
      {SourceClass::Kick, "kick", 0.0f},         {SourceClass::Snare, "snare", 80.0f},
      {SourceClass::Bass, "bass", 0.0f},         {SourceClass::Guitar, "gtr", 75.0f},
      {SourceClass::Keys, "keys", 50.0f},        {SourceClass::Strings, "strings", 60.0f},
      {SourceClass::Tom, "tom", 60.0f},          {SourceClass::Backing, "bvox", 100.0f},
      {SourceClass::Percussion, "perc", 150.0f}, {SourceClass::HiHat, "hat", 400.0f},
      {SourceClass::Cymbal, "cym", 400.0f},      {SourceClass::Fx, "fx", 0.0f},
  };

  std::vector<TrackProfile> profiles;
  profiles.reserve(expected.size());
  for (const Expected& row : expected) profiles.push_back(make_profile(row.id, row.source));

  const MixProfile mix = make_mix(static_cast<int>(profiles.size()));
  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  for (const Expected& row : expected) {
    INFO("track " << row.id);
    const Insert* insert = find_insert(deltas, row.id, kCutFilter);
    if (row.corner_hz == 0.0f) {
      // The low end is what these classes are made of, so nothing is swept.
      CHECK(insert == nullptr);
      continue;
    }
    REQUIRE(insert != nullptr);
    CHECK(insert->slot == InsertSlot::PreFader);
    const sonare::util::json::Value params = parse_params(*insert);
    REQUIRE(params.contains("highPassFrequencyHz"));
    CHECK_THAT(params["highPassFrequencyHz"].as_float(), WithinAbs(row.corner_hz, kHzTolerance));
    REQUIRE(params.contains("highPassEnabled"));
    CHECK(params["highPassEnabled"].as_bool());
    // The stage has no opinion about the top end and must not imply one.
    CHECK_FALSE(params.contains("lowPassEnabled"));
    for (const auto& [key, value] : params.as_object()) {
      INFO("param key " << key);
      CHECK((value.is_number() || value.is_bool()));
    }
  }
}

TEST_CASE("eq skips a band the high-pass has already removed", "[mixing][assistant]") {
  std::vector<TrackProfile> profiles{make_profile("hat", SourceClass::HiHat),
                                     make_profile("perc", SourceClass::Percussion)};
  MixProfile mix = make_mix(2);
  // The sub band tops out at 60 Hz, well under both classes' corners.
  set_dominance(mix, 0, 1, kSubBand, kBuriedShare, kOverlapFrames);

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  CHECK(count_inserts(deltas, kParametric) == 0);
  CHECK(count_inserts(deltas, kCutFilter) == 2);
}

TEST_CASE("eq leaves unidentified, unusable and low-confidence tracks alone",
          "[mixing][assistant]") {
  std::vector<TrackProfile> profiles{make_profile("mystery", SourceClass::Unknown),
                                     make_profile("silent", SourceClass::Vocal),
                                     make_profile("maybeLead", SourceClass::Lead)};
  profiles[1].usable = false;
  profiles[2].source_confidence = kLowConfidence;

  MixProfile mix = make_mix(3);
  for (int masker = 0; masker < 3; ++masker) {
    for (int maskee = 0; maskee < 3; ++maskee) {
      if (masker == maskee) continue;
      set_dominance(mix, masker, maskee, kMidBand, kBuriedShare, kOverlapFrames);
    }
  }

  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, MixAssistantConfig{});

  // Not a zero-valued suggestion; no suggestion at all.
  CHECK(deltas.empty());
}

TEST_CASE("eq makes no peaking cut when the ceiling is zero", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks();
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kBuriedShare, kOverlapFrames);

  MixAssistantConfig config;
  config.eq_max_cut_db = 0.0f;
  const std::vector<SceneDelta> deltas = decide_eq(profiles, mix, config);

  CHECK(count_inserts(deltas, kParametric) == 0);
  // Tidying the low end is not a cut governed by the ceiling.
  CHECK(count_inserts(deltas, kCutFilter) == 2);
}

TEST_CASE("eq returns nothing when the domain is switched off", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = two_tracks();
  MixProfile mix = make_mix(2);
  set_dominance(mix, 1, 0, kMidBand, kBuriedShare, kOverlapFrames);

  MixAssistantConfig config;
  config.enable_eq = false;

  CHECK(decide_eq(profiles, mix, config).empty());
}

TEST_CASE("eq handles no tracks and a single track", "[mixing][assistant]") {
  SECTION("no tracks") {
    const std::vector<TrackProfile> profiles;
    const MixProfile mix;
    std::vector<SceneDelta> deltas;
    REQUIRE_NOTHROW(deltas = decide_eq(profiles, mix, MixAssistantConfig{}));
    CHECK(deltas.empty());
  }

  SECTION("one track") {
    const std::vector<TrackProfile> profiles{make_profile("vox", SourceClass::Vocal)};
    const MixProfile mix = make_mix(1);
    std::vector<SceneDelta> deltas;
    REQUIRE_NOTHROW(deltas = decide_eq(profiles, mix, MixAssistantConfig{}));
    // Nothing to collide with, so only the low-end tidy remains.
    CHECK(count_inserts(deltas, kParametric) == 0);
    CHECK(count_inserts(deltas, kCutFilter) == 1);
  }

  SECTION("a mix profile that never got measured") {
    const std::vector<TrackProfile> profiles = two_tracks();
    const MixProfile mix;
    std::vector<SceneDelta> deltas;
    REQUIRE_NOTHROW(deltas = decide_eq(profiles, mix, MixAssistantConfig{}));
    CHECK(count_inserts(deltas, kParametric) == 0);
  }
}
