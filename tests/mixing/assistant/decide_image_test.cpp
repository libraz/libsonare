/// @file decide_image_test.cpp
/// @brief Contract of the mixing assistant's stereo placement, alignment and
///        mono-fold decisions.

#include "mixing/assistant/decide_image.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "util/json.h"

using Catch::Matchers::WithinAbs;
using sonare::mixing::assistant::decide_image;
using sonare::mixing::assistant::DeltaDomain;
using sonare::mixing::assistant::kBandCount;
using sonare::mixing::assistant::MixAssistantConfig;
using sonare::mixing::assistant::MixProfile;
using sonare::mixing::assistant::MonoRisk;
using sonare::mixing::assistant::PairAlignment;
using sonare::mixing::assistant::SceneDelta;
using sonare::mixing::assistant::SourceClass;
using sonare::mixing::assistant::TrackProfile;

namespace {

// The classifier gate is a confidence floor, so every hand-built profile that
// is meant to be placed carries an unambiguous classification.
constexpr float kConfidentClassification = 1.0f;
// Below the placement confidence floor, which is the only reason these tests
// need a second confidence value at all.
constexpr float kWeakClassification = 0.1f;
// Pan and width are float arithmetic over small factors, so a thousandth is far
// below anything a listener or a fader could resolve.
constexpr float kPositionTolerance = 1e-4f;
// Correlations of hand-built pairs. Only their ordering matters: the stronger
// one is the pair that must win when a track appears in two of them.
constexpr float kStrongCorrelation = 0.95f;
constexpr float kWeakerCorrelation = 0.80f;
// The mid band, used wherever a test needs one specific band to be the one a
// track occupies and the mix is crowded in.
constexpr int kCrowdedBand = 3;

std::array<float, kBandCount> even_occupancy() {
  std::array<float, kBandCount> bands{};
  bands.fill(1.0f / static_cast<float>(kBandCount));
  return bands;
}

// Puts most of the track's energy in one band so the crowding lookup has an
// unambiguous band to read.
std::array<float, kBandCount> occupancy_in(int band) {
  std::array<float, kBandCount> bands{};
  bands.fill(0.05f);
  bands[static_cast<std::size_t>(band)] = 1.0f - 0.05f * static_cast<float>(kBandCount - 1);
  return bands;
}

TrackProfile make_profile(const std::string& id, SourceClass source) {
  TrackProfile profile;
  profile.strip_id = id;
  profile.name = id;
  profile.source = source;
  profile.source_confidence = kConfidentClassification;
  profile.band_occupancy = even_occupancy();
  profile.channel_count = 2;
  profile.usable = true;
  return profile;
}

MixProfile make_mix(std::size_t track_count) {
  MixProfile mix;
  mix.track_count = static_cast<int>(track_count);
  // Sized but all false: no band is crowded unless a test says so.
  mix.image.crowded.assign(static_cast<std::size_t>(kBandCount), false);
  mix.image.crowding.assign(static_cast<std::size_t>(kBandCount), 0.0f);
  return mix;
}

PairAlignment make_pair(int reference, int target, int lag_samples, float correlation,
                        bool polarity_opposed) {
  PairAlignment pair;
  pair.reference_index = reference;
  pair.target_index = target;
  pair.lag_samples = lag_samples;
  pair.correlation = correlation;
  pair.polarity_opposed = polarity_opposed;
  pair.related = true;
  return pair;
}

MonoRisk make_risk(int track_index, const std::string& strip_id, bool wide_low_end) {
  MonoRisk risk;
  risk.track_index = track_index;
  risk.strip_id = strip_id;
  risk.correlation = 0.1f;
  risk.width = 1.6f;
  risk.wide_low_end = wide_low_end;
  return risk;
}

const SceneDelta* find_pan(const std::vector<SceneDelta>& deltas, const std::string& strip_id) {
  for (const SceneDelta& delta : deltas) {
    if (delta.strip_id == strip_id && delta.pan) return &delta;
  }
  return nullptr;
}

float pan_of(const std::vector<SceneDelta>& deltas, const std::string& strip_id) {
  const SceneDelta* delta = find_pan(deltas, strip_id);
  REQUIRE(delta != nullptr);
  return *delta->pan;
}

int count_delays(const std::vector<SceneDelta>& deltas, const std::string& strip_id) {
  int count = 0;
  for (const SceneDelta& delta : deltas) {
    if (delta.strip_id == strip_id && delta.channel_delay_samples) ++count;
  }
  return count;
}

int count_polarities(const std::vector<SceneDelta>& deltas, const std::string& strip_id) {
  int count = 0;
  for (const SceneDelta& delta : deltas) {
    if (delta.strip_id == strip_id && delta.polarity_invert_left) ++count;
  }
  return count;
}

const SceneDelta* find_delay(const std::vector<SceneDelta>& deltas, const std::string& strip_id) {
  for (const SceneDelta& delta : deltas) {
    if (delta.strip_id == strip_id && delta.channel_delay_samples) return &delta;
  }
  return nullptr;
}

const SceneDelta* find_polarity(const std::vector<SceneDelta>& deltas,
                                const std::string& strip_id) {
  for (const SceneDelta& delta : deltas) {
    if (delta.strip_id == strip_id && delta.polarity_invert_left) return &delta;
  }
  return nullptr;
}

const SceneDelta* find_insert(const std::vector<SceneDelta>& deltas, const std::string& strip_id) {
  for (const SceneDelta& delta : deltas) {
    if (delta.strip_id == strip_id && !delta.inserts.empty()) return &delta;
  }
  return nullptr;
}

// Every field a delta can carry, flattened. Two runs that agree here agree
// completely, which is what the determinism case has to show.
std::string describe(const std::vector<SceneDelta>& deltas) {
  std::ostringstream out;
  for (const SceneDelta& delta : deltas) {
    out << static_cast<int>(delta.domain) << '|' << delta.strip_id << '|' << delta.reason << '|'
        << (delta.pan ? std::to_string(*delta.pan) : "-") << '|'
        << (delta.width ? std::to_string(*delta.width) : "-") << '|'
        << (delta.polarity_invert_left ? (*delta.polarity_invert_left ? "1" : "0") : "-") << '|'
        << (delta.polarity_invert_right ? (*delta.polarity_invert_right ? "1" : "0") : "-") << '|'
        << (delta.channel_delay_samples ? std::to_string(*delta.channel_delay_samples) : "-")
        << '|';
    for (const auto& insert : delta.inserts) {
      out << static_cast<int>(insert.slot) << ':' << insert.processor_name << '='
          << insert.params_json << ';';
    }
    out << '\n';
  }
  return out.str();
}

bool every_delta_is_an_image_delta(const std::vector<SceneDelta>& deltas) {
  return std::all_of(deltas.begin(), deltas.end(),
                     [](const SceneDelta& delta) { return delta.domain == DeltaDomain::Image; });
}

bool reads_as_a_sentence(const std::string& reason) {
  return !reason.empty() && std::islower(static_cast<unsigned char>(reason.front())) != 0;
}

}  // namespace

TEST_CASE("the anchor classes are placed at the centre", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = {
      make_profile("kick", SourceClass::Kick),   make_profile("bass", SourceClass::Bass),
      make_profile("lead", SourceClass::Lead),   make_profile("vox", SourceClass::Vocal),
      make_profile("snare", SourceClass::Snare),
  };
  const std::vector<SceneDelta> deltas =
      decide_image(profiles, make_mix(profiles.size()), MixAssistantConfig{});

  REQUIRE(every_delta_is_an_image_delta(deltas));
  for (const TrackProfile& profile : profiles) {
    INFO("strip " << profile.strip_id);
    CHECK_THAT(pan_of(deltas, profile.strip_id), WithinAbs(0.0f, kPositionTolerance));
    CHECK(reads_as_a_sentence(find_pan(deltas, profile.strip_id)->reason));
  }
}

TEST_CASE("two tracks of one class are spread symmetrically", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = {
      make_profile("gtrA", SourceClass::Guitar),
      make_profile("gtrB", SourceClass::Guitar),
  };
  const std::vector<SceneDelta> deltas =
      decide_image(profiles, make_mix(profiles.size()), MixAssistantConfig{});

  const float first = pan_of(deltas, "gtrA");
  const float second = pan_of(deltas, "gtrB");
  CHECK(first < 0.0f);
  CHECK(second > 0.0f);
  CHECK_THAT(first, WithinAbs(-second, kPositionTolerance));
}

TEST_CASE("a paired spreadable class is placed genuinely wide", "[mixing][assistant]") {
  // A survey of professional practice finds wide placements accounting for about
  // a third of all panning decisions, so the main spreadable parts have to land
  // out near the edge rather than hedged towards the centre. A floor rather than
  // an equality: the extents are convention and may be retuned, but not back to
  // a timid spread.
  constexpr float kWideFloor = 0.75f;

  const std::vector<TrackProfile> profiles = {
      make_profile("gtrA", SourceClass::Guitar), make_profile("gtrB", SourceClass::Guitar),
      make_profile("fxA", SourceClass::Fx),      make_profile("fxB", SourceClass::Fx),
      make_profile("keysA", SourceClass::Keys),  make_profile("keysB", SourceClass::Keys),
  };
  const std::vector<SceneDelta> deltas =
      decide_image(profiles, make_mix(profiles.size()), MixAssistantConfig{});

  const float guitar = std::fabs(pan_of(deltas, "gtrA"));
  CHECK(guitar >= kWideFloor);
  CHECK(std::fabs(pan_of(deltas, "gtrB")) >= kWideFloor);
  // A colour part sits at least as far out as a main spreadable one...
  CHECK(std::fabs(pan_of(deltas, "fxA")) >= guitar);
  // ...while a support part stays inside them, because the finding is that wide
  // placements are common, not that every part belongs wide.
  CHECK(std::fabs(pan_of(deltas, "keysA")) < guitar);

  // Nothing reaches a full hard pan, which would leave the part absent from one
  // speaker altogether.
  for (const SceneDelta& delta : deltas) {
    REQUIRE(delta.pan);
    INFO("strip " << delta.strip_id);
    CHECK(std::fabs(*delta.pan) < 1.0f);
  }
}

TEST_CASE("a third track of one class is placed nearer the centre", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = {
      make_profile("gtrA", SourceClass::Guitar),
      make_profile("gtrB", SourceClass::Guitar),
      make_profile("gtrC", SourceClass::Guitar),
  };
  const std::vector<SceneDelta> deltas =
      decide_image(profiles, make_mix(profiles.size()), MixAssistantConfig{});

  const float first = pan_of(deltas, "gtrA");
  const float second = pan_of(deltas, "gtrB");
  const float third = pan_of(deltas, "gtrC");
  CHECK_THAT(first, WithinAbs(-second, kPositionTolerance));
  // The unpaired member steps inwards rather than piling onto an edge, and it
  // is the third one in input order that does so: the ladder follows the
  // caller's order and nothing else.
  CHECK(std::fabs(third) < std::fabs(first));
  CHECK(third != 0.0f);
}

TEST_CASE("a track whose class is unknown or barely classified is left unplaced",
          "[mixing][assistant]") {
  std::vector<TrackProfile> profiles = {
      make_profile("mystery", SourceClass::Unknown),
      make_profile("maybeGtr", SourceClass::Guitar),
      make_profile("silent", SourceClass::Guitar),
  };
  profiles[1].source_confidence = kWeakClassification;
  profiles[2].usable = false;

  const std::vector<SceneDelta> deltas =
      decide_image(profiles, make_mix(profiles.size()), MixAssistantConfig{});

  CHECK(deltas.empty());
}

TEST_CASE("the same input decides the same image twice", "[mixing][assistant]") {
  std::vector<TrackProfile> profiles = {
      make_profile("kick", SourceClass::Kick),   make_profile("gtrA", SourceClass::Guitar),
      make_profile("gtrB", SourceClass::Guitar), make_profile("gtrC", SourceClass::Guitar),
      make_profile("keys", SourceClass::Keys),   make_profile("fx", SourceClass::Fx),
  };
  profiles[1].band_occupancy = occupancy_in(kCrowdedBand);

  MixProfile mix = make_mix(profiles.size());
  mix.image.crowded[static_cast<std::size_t>(kCrowdedBand)] = true;
  mix.alignment = {
      make_pair(0, 1, 12, kStrongCorrelation, true),
      make_pair(1, 2, -7, kWeakerCorrelation, false),
      make_pair(2, 4, 0, kWeakerCorrelation, true),
  };
  mix.mono_risks = {make_risk(5, "fx", true)};

  const std::vector<SceneDelta> first = decide_image(profiles, mix, MixAssistantConfig{});
  const std::vector<SceneDelta> second = decide_image(profiles, mix, MixAssistantConfig{});

  REQUIRE_FALSE(first.empty());
  CHECK(describe(first) == describe(second));
}

TEST_CASE("a polarity-opposed pair inverts the target and leaves the reference alone",
          "[mixing][assistant]") {
  // Deliberately unclassified: polarity is measured between two signals, so it
  // is decided without any classification at all.
  const std::vector<TrackProfile> profiles = {
      make_profile("snareTop", SourceClass::Unknown),
      make_profile("snareBottom", SourceClass::Unknown),
  };
  MixProfile mix = make_mix(profiles.size());
  mix.alignment = {make_pair(0, 1, 0, -kStrongCorrelation, true)};

  const std::vector<SceneDelta> deltas = decide_image(profiles, mix, MixAssistantConfig{});

  REQUIRE(deltas.size() == 1);
  CHECK(count_polarities(deltas, "snareTop") == 0);
  REQUIRE(count_polarities(deltas, "snareBottom") == 1);
  const SceneDelta* inverted = find_polarity(deltas, "snareBottom");
  REQUIRE(inverted != nullptr);
  REQUIRE(inverted->polarity_invert_left);
  REQUIRE(inverted->polarity_invert_right);
  CHECK(*inverted->polarity_invert_left);
  CHECK(*inverted->polarity_invert_right);
  CHECK(reads_as_a_sentence(inverted->reason));
}

TEST_CASE("a lag delays whichever side arrives first", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = {
      make_profile("close", SourceClass::Unknown),
      make_profile("room", SourceClass::Unknown),
  };
  MixProfile mix = make_mix(profiles.size());

  SECTION("a positive lag means the target arrives later, so the reference waits") {
    mix.alignment = {make_pair(0, 1, 40, kStrongCorrelation, false)};
    const std::vector<SceneDelta> deltas = decide_image(profiles, mix, MixAssistantConfig{});

    REQUIRE(deltas.size() == 1);
    const SceneDelta* delayed = find_delay(deltas, "close");
    REQUIRE(delayed != nullptr);
    CHECK(*delayed->channel_delay_samples == 40);
    CHECK(count_delays(deltas, "room") == 0);
  }

  SECTION("a negative lag means the target arrives first, so the target waits") {
    mix.alignment = {make_pair(0, 1, -40, kStrongCorrelation, false)};
    const std::vector<SceneDelta> deltas = decide_image(profiles, mix, MixAssistantConfig{});

    REQUIRE(deltas.size() == 1);
    const SceneDelta* delayed = find_delay(deltas, "room");
    REQUIRE(delayed != nullptr);
    CHECK(*delayed->channel_delay_samples == 40);
    CHECK(count_delays(deltas, "close") == 0);
  }
}

TEST_CASE("a pair already in time gets no delay delta", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = {
      make_profile("close", SourceClass::Unknown),
      make_profile("room", SourceClass::Unknown),
  };
  MixProfile mix = make_mix(profiles.size());
  mix.alignment = {make_pair(0, 1, 0, kStrongCorrelation, false)};

  const std::vector<SceneDelta> deltas = decide_image(profiles, mix, MixAssistantConfig{});

  CHECK(deltas.empty());
}

TEST_CASE("an unrelated pair is left alone entirely", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = {
      make_profile("piano", SourceClass::Unknown),
      make_profile("shaker", SourceClass::Unknown),
  };
  MixProfile mix = make_mix(profiles.size());
  PairAlignment pair = make_pair(0, 1, 55, kStrongCorrelation, true);
  pair.related = false;
  mix.alignment = {pair};

  const std::vector<SceneDelta> deltas = decide_image(profiles, mix, MixAssistantConfig{});

  CHECK(deltas.empty());
}

TEST_CASE("a track appearing in several pairs gets one delay and one polarity decision",
          "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = {
      make_profile("t0", SourceClass::Unknown),
      make_profile("t1", SourceClass::Unknown),
      make_profile("t2", SourceClass::Unknown),
  };
  MixProfile mix = make_mix(profiles.size());
  mix.alignment = {
      // Delays the reference t0 by 10 on the weaker evidence.
      make_pair(0, 1, 10, kWeakerCorrelation, false),
      // Delays the reference t0 by 30 on the stronger evidence, and inverts t2.
      make_pair(0, 2, 30, kStrongCorrelation, true),
      // Delays the target t2 by 20, and would invert t2 as well.
      make_pair(1, 2, -20, kWeakerCorrelation, true),
  };

  const std::vector<SceneDelta> deltas = decide_image(profiles, mix, MixAssistantConfig{});

  REQUIRE(count_delays(deltas, "t0") == 1);
  CHECK(*find_delay(deltas, "t0")->channel_delay_samples == 30);
  CHECK(count_delays(deltas, "t1") == 0);
  REQUIRE(count_delays(deltas, "t2") == 1);
  CHECK(*find_delay(deltas, "t2")->channel_delay_samples == 20);

  CHECK(count_polarities(deltas, "t0") == 0);
  CHECK(count_polarities(deltas, "t1") == 0);
  REQUIRE(count_polarities(deltas, "t2") == 1);
  // The strongest pair is the one that wins, so the reason names its reference.
  CHECK(find_polarity(deltas, "t2")->reason.find("t0") != std::string::npos);
}

TEST_CASE("a wide low end gets a mono maker the insert factory can build", "[mixing][assistant]") {
  namespace json = sonare::util::json;

  const std::vector<TrackProfile> profiles = {make_profile("padWide", SourceClass::Unknown)};
  MixProfile mix = make_mix(profiles.size());
  mix.mono_risks = {make_risk(0, "padWide", true)};

  const std::vector<SceneDelta> deltas = decide_image(profiles, mix, MixAssistantConfig{});

  const SceneDelta* fold = find_insert(deltas, "padWide");
  REQUIRE(fold != nullptr);
  REQUIRE(fold->inserts.size() == 1);
  const auto& insert = fold->inserts.front();

  const std::vector<std::string> factory_names = sonare::mastering::api::insert_factory_names();
  CHECK(std::find(factory_names.begin(), factory_names.end(), insert.processor_name) !=
        factory_names.end());

  const std::vector<std::string> param_names =
      sonare::mastering::api::insert_param_names(insert.processor_name);
  REQUIRE_FALSE(param_names.empty());

  const json::Value params = json::parse(insert.params_json);
  REQUIRE(params.is_object());
  REQUIRE_FALSE(params.as_object().empty());
  for (const auto& [key, value] : params.as_object()) {
    INFO("param " << key);
    // The scene carries params as JSON text, and the insert factory reads only
    // scalars: a string or a nested object here would be silently ignored.
    CHECK((value.is_number() || value.is_bool()));
    CHECK(std::find(param_names.begin(), param_names.end(), key) != param_names.end());
  }
  CHECK(reads_as_a_sentence(fold->reason));
}

TEST_CASE("a mono risk without a wide low end is narrowed but not folded", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = {make_profile("keysWide", SourceClass::Unknown)};
  MixProfile mix = make_mix(profiles.size());
  mix.mono_risks = {make_risk(0, "keysWide", false)};

  const std::vector<SceneDelta> deltas = decide_image(profiles, mix, MixAssistantConfig{});

  REQUIRE(deltas.size() == 1);
  REQUIRE(deltas.front().width);
  CHECK(*deltas.front().width < 1.0f);
  CHECK(deltas.front().inserts.empty());
}

TEST_CASE("zero strength removes the spread but keeps the physical corrections",
          "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = {
      make_profile("gtrA", SourceClass::Guitar),
      make_profile("gtrB", SourceClass::Guitar),
  };
  MixProfile mix = make_mix(profiles.size());
  mix.alignment = {make_pair(0, 1, 24, kStrongCorrelation, true)};
  mix.mono_risks = {make_risk(1, "gtrB", true)};

  MixAssistantConfig config;
  config.suggestion_strength = 0.0f;
  const std::vector<SceneDelta> deltas = decide_image(profiles, mix, config);

  // Placement is a taste decision, so it collapses to the centre.
  CHECK_THAT(pan_of(deltas, "gtrA"), WithinAbs(0.0f, kPositionTolerance));
  CHECK_THAT(pan_of(deltas, "gtrB"), WithinAbs(0.0f, kPositionTolerance));

  // Polarity, delay and the low-end fold correct a measured cancellation, which
  // is not half-wrong at half strength, so they survive a zero strength intact.
  REQUIRE(count_polarities(deltas, "gtrB") == 1);
  CHECK(*find_polarity(deltas, "gtrB")->polarity_invert_left);
  REQUIRE(count_delays(deltas, "gtrA") == 1);
  CHECK(*find_delay(deltas, "gtrA")->channel_delay_samples == 24);
  REQUIRE(find_insert(deltas, "gtrB") != nullptr);
}

TEST_CASE("crowding changes how far a track is spread, never which side", "[mixing][assistant]") {
  std::vector<TrackProfile> profiles = {
      make_profile("gtrA", SourceClass::Guitar),
      make_profile("gtrB", SourceClass::Guitar),
  };
  profiles[0].band_occupancy = occupancy_in(kCrowdedBand);
  profiles[1].band_occupancy = occupancy_in(kCrowdedBand);

  const MixProfile open = make_mix(profiles.size());
  MixProfile crowded = make_mix(profiles.size());
  crowded.image.crowded[static_cast<std::size_t>(kCrowdedBand)] = true;
  crowded.image.crowding[static_cast<std::size_t>(kCrowdedBand)] = 0.9f;

  const std::vector<SceneDelta> open_deltas = decide_image(profiles, open, MixAssistantConfig{});
  const std::vector<SceneDelta> crowded_deltas =
      decide_image(profiles, crowded, MixAssistantConfig{});

  for (const char* strip_id : {"gtrA", "gtrB"}) {
    INFO("strip " << strip_id);
    const float open_pan = pan_of(open_deltas, strip_id);
    const float crowded_pan = pan_of(crowded_deltas, strip_id);
    // The side is the class rule's decision and a spectral measurement must not
    // be able to move it. Only the distance out may change.
    CHECK(std::signbit(open_pan) == std::signbit(crowded_pan));
    CHECK(std::fabs(crowded_pan) > std::fabs(open_pan));
  }
}

TEST_CASE("degenerate track counts are handled without throwing", "[mixing][assistant]") {
  SECTION("no tracks at all") {
    const std::vector<TrackProfile> profiles;
    std::vector<SceneDelta> deltas;
    REQUIRE_NOTHROW(deltas = decide_image(profiles, make_mix(0), MixAssistantConfig{}));
    CHECK(deltas.empty());
  }

  SECTION("a single track of a spreadable class") {
    const std::vector<TrackProfile> profiles = {make_profile("gtrA", SourceClass::Guitar)};
    std::vector<SceneDelta> deltas;
    REQUIRE_NOTHROW(deltas =
                        decide_image(profiles, make_mix(profiles.size()), MixAssistantConfig{}));
    REQUIRE(deltas.size() == 1);
    // No partner to balance against, so a lone member steps only part of the
    // way out rather than taking the class's full extent.
    CHECK(std::fabs(pan_of(deltas, "gtrA")) < 1.0f);
  }

  SECTION("a pair index that falls outside the profiles") {
    const std::vector<TrackProfile> profiles = {make_profile("only", SourceClass::Unknown)};
    MixProfile mix = make_mix(profiles.size());
    mix.alignment = {make_pair(0, 7, 30, kStrongCorrelation, true)};
    mix.mono_risks = {make_risk(9, "ghost", true)};
    std::vector<SceneDelta> deltas;
    REQUIRE_NOTHROW(deltas = decide_image(profiles, mix, MixAssistantConfig{}));
    CHECK(deltas.empty());
  }
}

TEST_CASE("a disabled image domain decides nothing", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = {
      make_profile("kick", SourceClass::Kick),
      make_profile("gtrA", SourceClass::Guitar),
  };
  MixProfile mix = make_mix(profiles.size());
  mix.alignment = {make_pair(0, 1, 16, kStrongCorrelation, true)};

  MixAssistantConfig config;
  config.enable_image = false;

  CHECK(decide_image(profiles, mix, config).empty());
}
