/// @file decide_dynamics_test.cpp
/// @brief Contract of the mixing assistant's dynamics decisions.

#include "mixing/assistant/decide_dynamics.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "util/constants.h"
#include "util/json.h"

namespace json = sonare::util::json;

using sonare::mastering::api::insert_factory_names;
using sonare::mastering::api::insert_param_names;
using sonare::mixing::api::Insert;
using sonare::mixing::api::InsertSlot;
using sonare::mixing::assistant::BandDominance;
using sonare::mixing::assistant::decide_dynamics;
using sonare::mixing::assistant::DeltaDomain;
using sonare::mixing::assistant::kBandCount;
using sonare::mixing::assistant::MixAssistantConfig;
using sonare::mixing::assistant::MixProfile;
using sonare::mixing::assistant::SceneDelta;
using sonare::mixing::assistant::SourceClass;
using sonare::mixing::assistant::TrackProfile;

namespace {

// Comfortably longer than the 400 ms gating block, so duration is never the
// reason a hand-built profile is excluded.
constexpr float kMeasurableDurationSec = 2.0f;
// Well above the confidence the stage needs before it acts on a class, and
// above the higher bar the gate holds out for.
constexpr float kConfidentClass = 0.9f;
// Below the confidence the gate insists on, but above the one the shaping
// decisions need, so a profile carrying it is treated but never gated.
constexpr float kUncertainClass = 0.6f;
// A program level in the region a tracked part lands in. Every threshold the
// stage suggests is an offset from this, so its exact value never matters.
constexpr float kProgramLufs = -20.0f;
// A crest factor either side of the stage's reference point.
constexpr float kLowCrestDb = 4.0f;
constexpr float kHighCrestDb = 22.0f;
// A crest factor high enough for the gate to consider the material separable.
constexpr float kGateableCrestDb = 18.0f;
// Sustain ratios at both ends of the documented [0, 1] range.
constexpr float kTransientSustain = 0.05f;
constexpr float kSustainedSustain = 0.95f;
// Onsets per second for a part that is being played rather than held.
constexpr float kPlayedOnsetsPerSec = 4.0f;
// A part played densely enough that the gap between hits is shorter than the
// recovery its class's nominal release would take.
constexpr float kDenseOnsetsPerSec = 8.0f;
// And one played sparsely enough that the gap is never the binding constraint.
constexpr float kSparseOnsetsPerSec = 1.0f;

// An even spread across the analysis bands: it sums to 1 and puts enough energy
// in the top two bands for a voice to read as sibilant.
std::array<float, kBandCount> even_occupancy() {
  std::array<float, kBandCount> bands{};
  bands.fill(1.0f / static_cast<float>(kBandCount));
  return bands;
}

TrackProfile make_profile(const std::string& id, SourceClass source, float crest_db = 10.0f,
                          float sustain = 0.5f, float attack_density = 2.0f,
                          float confidence = kConfidentClass) {
  TrackProfile profile;
  profile.strip_id = id;
  profile.name = id;
  profile.source = source;
  profile.source_confidence = confidence;
  profile.base.loudness.integrated_lufs = kProgramLufs;
  profile.base.loudness.crest_factor_db = crest_db;
  profile.base.dynamics.sustain_ratio = sustain;
  profile.base.dynamics.attack_density = attack_density;
  profile.base.duration_sec = kMeasurableDurationSec;
  profile.duration_sec = kMeasurableDurationSec;
  profile.band_occupancy = even_occupancy();
  profile.usable = true;
  return profile;
}

MixProfile make_mix(std::size_t track_count) {
  MixProfile mix;
  mix.track_count = static_cast<int>(track_count);
  mix.dominance.assign(track_count * track_count * static_cast<std::size_t>(kBandCount),
                       BandDominance{});
  return mix;
}

void set_dominance(MixProfile& mix, std::size_t masker, std::size_t maskee, int band, float ratio,
                   int valid_frames) {
  const std::size_t index = (masker * static_cast<std::size_t>(mix.track_count) + maskee) *
                                static_cast<std::size_t>(kBandCount) +
                            static_cast<std::size_t>(band);
  REQUIRE(index < mix.dominance.size());
  mix.dominance[index] = BandDominance{ratio, valid_frames};
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

bool has_insert(const std::vector<SceneDelta>& deltas, const std::string& strip_id,
                const std::string& processor_name) {
  return find_insert(deltas, strip_id, processor_name) != nullptr;
}

float param_number(const Insert& insert, const std::string& key) {
  const json::Value params = json::parse(insert.params_json);
  const json::Value* field = params.find(key);
  REQUIRE(field != nullptr);
  REQUIRE(field->is_number());
  return field->as_float();
}

/// @brief The compressor release the stage suggests for one profile on its own.
float suggested_release_ms(const TrackProfile& profile) {
  const std::vector<TrackProfile> profiles{profile};
  const std::vector<SceneDelta> deltas =
      decide_dynamics(profiles, make_mix(profiles.size()), MixAssistantConfig{});
  const Insert* compressor = find_insert(deltas, profile.strip_id, "dynamics.compressor");
  REQUIRE(compressor != nullptr);
  return param_number(*compressor, "releaseMs");
}

/// @brief The longest release whose recovery fits between onsets at that rate.
/// @details Restates the derivation rather than reaching for the stage's own
///          constant: a third of the inter-onset interval, because the release
///          parameter is a one-pole time constant and the gain needs about three
///          of them to arrive. Written as the same sequence of float operations
///          the stage performs, so a release that sits exactly on the bound
///          compares equal rather than one ulp either side of it.
float recovery_bound_ms(float onsets_per_sec) { return (1000.0f / onsets_per_sec) * (1.0f / 3.0f); }

// One profile per class the stage can act on, plus the extremes of the dynamics
// profile, so a single run exercises every branch that emits an insert.
std::vector<TrackProfile> every_treatable_class() {
  return {
      // A gateable kit source: confident, transient and clearly played.
      make_profile("kick", SourceClass::Kick, kGateableCrestDb, kTransientSustain,
                   kPlayedOnsetsPerSec),
      make_profile("snare", SourceClass::Snare, kGateableCrestDb, kTransientSustain,
                   kPlayedOnsetsPerSec),
      make_profile("hat", SourceClass::HiHat),
      make_profile("tom", SourceClass::Tom),
      make_profile("ride", SourceClass::Cymbal),
      make_profile("bass", SourceClass::Bass, kLowCrestDb, kSustainedSustain),
      make_profile("gtr", SourceClass::Guitar),
      make_profile("keys", SourceClass::Keys),
      make_profile("strings", SourceClass::Strings, kLowCrestDb, kSustainedSustain),
      make_profile("lead", SourceClass::Lead),
      make_profile("vox", SourceClass::Vocal, kHighCrestDb),
      make_profile("bvox", SourceClass::Backing),
      make_profile("shaker", SourceClass::Percussion),
      make_profile("riser", SourceClass::Fx),
  };
}

}  // namespace

TEST_CASE("every suggested processor is one the insert factory can build", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = every_treatable_class();
  MixProfile mix = make_mix(profiles.size());
  // The kick sits at index 0 and the bass at index 5, so the canonical low-end
  // pair is present in this run too.
  set_dominance(mix, 0, 5, 1, 0.7f, 200);

  const std::vector<SceneDelta> deltas = decide_dynamics(profiles, mix, MixAssistantConfig{});
  REQUIRE_FALSE(deltas.empty());

  const std::vector<std::string> known = insert_factory_names();
  for (const SceneDelta& delta : deltas) {
    for (const Insert& insert : delta.inserts) {
      INFO("processor " << insert.processor_name);
      CHECK(std::find(known.begin(), known.end(), insert.processor_name) != known.end());
    }
  }
}

TEST_CASE("every suggested params object holds only numbers and booleans", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = every_treatable_class();
  MixProfile mix = make_mix(profiles.size());
  set_dominance(mix, 0, 5, 1, 0.7f, 200);

  const std::vector<SceneDelta> deltas = decide_dynamics(profiles, mix, MixAssistantConfig{});
  REQUIRE_FALSE(deltas.empty());

  for (const SceneDelta& delta : deltas) {
    for (const Insert& insert : delta.inserts) {
      INFO("processor " << insert.processor_name << " params " << insert.params_json);
      const json::Value params = json::parse(insert.params_json);
      REQUIRE(params.is_object());
      REQUIRE_FALSE(params.as_object().empty());
      for (const auto& [key, value] : params.as_object()) {
        INFO("key " << key);
        // A string or a null would reach the insert factory as a malformed
        // parameter rather than a wrong one.
        CHECK((value.is_number() || value.is_bool()));
      }
    }
  }
}

TEST_CASE("every suggested params key is one its processor reads", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles = every_treatable_class();
  MixProfile mix = make_mix(profiles.size());
  set_dominance(mix, 0, 5, 1, 0.7f, 200);

  const std::vector<SceneDelta> deltas = decide_dynamics(profiles, mix, MixAssistantConfig{});
  REQUIRE_FALSE(deltas.empty());

  for (const SceneDelta& delta : deltas) {
    for (const Insert& insert : delta.inserts) {
      const std::vector<std::string> accepted = insert_param_names(insert.processor_name);
      REQUIRE_FALSE(accepted.empty());
      const json::Value params = json::parse(insert.params_json);
      for (const auto& [key, value] : params.as_object()) {
        INFO("processor " << insert.processor_name << " key " << key);
        // A key the processor never reads is silently ignored, so the wrong
        // spelling looks exactly like a setting that had no effect.
        CHECK(std::find(accepted.begin(), accepted.end(), key) != accepted.end());
      }
    }
  }
}

TEST_CASE("a track with more crest factor gets a later compressor attack", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles{make_profile("even", SourceClass::Vocal, kLowCrestDb),
                                           make_profile("spiky", SourceClass::Vocal, kHighCrestDb)};

  const std::vector<SceneDelta> deltas =
      decide_dynamics(profiles, make_mix(profiles.size()), MixAssistantConfig{});

  const Insert* even = find_insert(deltas, "even", "dynamics.compressor");
  const Insert* spiky = find_insert(deltas, "spiky", "dynamics.compressor");
  REQUIRE(even != nullptr);
  REQUIRE(spiky != nullptr);

  // A transient standing further above the body needs longer to get past the
  // detector, or the compressor removes the leading edge it was sized for.
  CHECK(param_number(*spiky, "attackMs") > param_number(*even, "attackMs"));
  // And the same reading calls for a gentler ratio: the part is already
  // carrying its own dynamics.
  CHECK(param_number(*spiky, "ratio") < param_number(*even, "ratio"));
}

TEST_CASE("a more sustained track gets a longer compressor release", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles{
      make_profile("plucked", SourceClass::Keys, 10.0f, kTransientSustain),
      make_profile("held", SourceClass::Keys, 10.0f, kSustainedSustain)};

  const std::vector<SceneDelta> deltas =
      decide_dynamics(profiles, make_mix(profiles.size()), MixAssistantConfig{});

  const Insert* plucked = find_insert(deltas, "plucked", "dynamics.compressor");
  const Insert* held = find_insert(deltas, "held", "dynamics.compressor");
  REQUIRE(plucked != nullptr);
  REQUIRE(held != nullptr);

  // A sustained part has no gaps for the gain to recover in, so the recovery
  // has to sit beyond the note rather than inside it.
  CHECK(param_number(*held, "releaseMs") > param_number(*plucked, "releaseMs"));
}

TEST_CASE("a densely played part's release fits between its measured onsets",
          "[mixing][assistant]") {
  // The bound comes from the track's own onset rate rather than from an assumed
  // tempo, so the same class played twice as often gets a release that fits the
  // shorter gap.
  const float dense = suggested_release_ms(
      make_profile("dense", SourceClass::Kick, 10.0f, kTransientSustain, kDenseOnsetsPerSec));
  const float sparse = suggested_release_ms(
      make_profile("sparse", SourceClass::Kick, 10.0f, kTransientSustain, kSparseOnsetsPerSec));

  INFO("dense " << dense << " ms, sparse " << sparse << " ms");
  CHECK(dense < sparse);
  CHECK(dense <= recovery_bound_ms(kDenseOnsetsPerSec));
  // The sparse part's gap is wide enough that nothing binds, so it keeps the
  // release its class asked for. Without this the case above would also pass on
  // a stage that shortened every release regardless of the measurement.
  CHECK(sparse < recovery_bound_ms(kSparseOnsetsPerSec));
}

TEST_CASE("a track with no measurable onset rate keeps its class's release",
          "[mixing][assistant]") {
  // Zero and non-finite are the absence of a reading, not an infinitely slow
  // part. Dividing by either would give a release of zero or of infinity, and
  // both are worse than the value the cap was meant to refine.
  const float reference = suggested_release_ms(
      make_profile("reference", SourceClass::Kick, 10.0f, kTransientSustain, kSparseOnsetsPerSec));

  const std::array<float, 3> unmeasurable = {0.0f, std::numeric_limits<float>::quiet_NaN(),
                                             std::numeric_limits<float>::infinity()};
  for (const float density : unmeasurable) {
    INFO("attack density " << density);
    const float release = suggested_release_ms(
        make_profile("quiet", SourceClass::Kick, 10.0f, kTransientSustain, density));
    CHECK(std::isfinite(release));
    CHECK(release > 0.0f);
    // Identical to the uncapped value: the sparse reference is already below its
    // own bound, so both describe the untouched table release.
    CHECK(release == reference);
  }
}

TEST_CASE("the onset cap never stretches a sustained part's release", "[mixing][assistant]") {
  // The cap is one-sided. A part held rather than struck still earns the sustain
  // lengthening, and a wide gap between its few onsets must not be read as
  // permission to lengthen it further.
  const float transient = suggested_release_ms(make_profile(
      "struck", SourceClass::Percussion, 10.0f, kTransientSustain, kSparseOnsetsPerSec));
  const float sustained = suggested_release_ms(
      make_profile("held", SourceClass::Percussion, 10.0f, kSustainedSustain, kSparseOnsetsPerSec));

  INFO("transient " << transient << " ms, sustained " << sustained << " ms");
  CHECK(sustained > transient);
  // And it stays well inside the gap it was measured against, so the lengthening
  // has not walked past the bound either.
  CHECK(sustained <= recovery_bound_ms(kSparseOnsetsPerSec));
}

TEST_CASE("a class whose release outlasts one event is not capped", "[mixing][assistant]") {
  // A tom's release exists to outlast a single hit's decay, not to fit between
  // hits, so the onset rate has no say over it. Capping it would put the gain
  // recovery inside the note the setting was chosen to protect.
  const float dense = suggested_release_ms(
      make_profile("fill", SourceClass::Tom, 10.0f, kTransientSustain, kDenseOnsetsPerSec));
  const float sparse = suggested_release_ms(
      make_profile("accent", SourceClass::Tom, 10.0f, kTransientSustain, kSparseOnsetsPerSec));

  INFO("dense " << dense << " ms, sparse " << sparse << " ms");
  CHECK(dense == sparse);
  // And it is genuinely longer than the dense gap, so the case is not passing
  // because the cap would have been slack anyway.
  CHECK(dense > recovery_bound_ms(kDenseOnsetsPerSec));
}

TEST_CASE("a voice gets a rider or a de-esser", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles{make_profile("vox", SourceClass::Vocal),
                                           make_profile("bvox", SourceClass::Backing)};

  const std::vector<SceneDelta> deltas =
      decide_dynamics(profiles, make_mix(profiles.size()), MixAssistantConfig{});

  for (const TrackProfile& profile : profiles) {
    INFO("strip " << profile.strip_id);
    CHECK((has_insert(deltas, profile.strip_id, "dynamics.vocalRider") ||
           has_insert(deltas, profile.strip_id, "dynamics.deesser")));
  }
}

TEST_CASE("a de-esser follows the sibilant bands rather than the class alone",
          "[mixing][assistant]") {
  std::vector<TrackProfile> profiles{make_profile("bright", SourceClass::Vocal),
                                     make_profile("dark", SourceClass::Vocal)};
  // A dark voice, or one that has already been de-essed, carries nothing in the
  // top two bands; putting a de-esser on it would be acting on the class name.
  profiles[1].band_occupancy.fill(0.0f);
  profiles[1].band_occupancy[3] = 1.0f;

  const std::vector<SceneDelta> deltas =
      decide_dynamics(profiles, make_mix(profiles.size()), MixAssistantConfig{});

  CHECK(has_insert(deltas, "bright", "dynamics.deesser"));
  CHECK_FALSE(has_insert(deltas, "dark", "dynamics.deesser"));
}

TEST_CASE("a percussive track gets a transient shaper", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles{
      make_profile("kick", SourceClass::Kick), make_profile("snare", SourceClass::Snare),
      make_profile("shaker", SourceClass::Percussion), make_profile("pad", SourceClass::Keys)};

  const std::vector<SceneDelta> deltas =
      decide_dynamics(profiles, make_mix(profiles.size()), MixAssistantConfig{});

  CHECK(has_insert(deltas, "kick", "dynamics.transientShaper"));
  CHECK(has_insert(deltas, "snare", "dynamics.transientShaper"));
  CHECK(has_insert(deltas, "shaker", "dynamics.transientShaper"));
  // A sustained part has no leading edge to shape.
  CHECK_FALSE(has_insert(deltas, "pad", "dynamics.transientShaper"));
}

TEST_CASE("a kick and a bass contending in the low end get a sidechain duck",
          "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles{make_profile("kick", SourceClass::Kick),
                                           make_profile("bass", SourceClass::Bass)};
  MixProfile mix = make_mix(profiles.size());
  // kBands[1] is 60-250 Hz, and the kick owns most of it whenever both sound.
  set_dominance(mix, 0, 1, 1, 0.72f, 400);

  const std::vector<SceneDelta> deltas = decide_dynamics(profiles, mix, MixAssistantConfig{});

  const Insert* duck = find_insert(deltas, "bass", "dynamics.duckingProcessor");
  REQUIRE(duck != nullptr);
  // The key is the track the detector listens to, not the one being ducked.
  CHECK(duck->sidechain_key == "kick");
  CHECK(duck->slot == InsertSlot::PreFader);
  CHECK(param_number(*duck, "rangeDb") > 0.0f);
  // Nothing is ducked under the bass in return.
  CHECK_FALSE(has_insert(deltas, "kick", "dynamics.duckingProcessor"));
}

TEST_CASE("a duck recovers between the key's hits, not between the ducked part's",
          "[mixing][assistant]") {
  // The detector listens to the key, so the gap the recovery has to fit inside
  // is the key's. Giving the two tracks opposite onset rates is what separates
  // the two readings: a release bounded by the ducked part's rate would read the
  // sparse figure and stay long.
  auto duck_release = [](float key_onsets_per_sec, float ducked_onsets_per_sec) {
    const std::vector<TrackProfile> profiles{
        make_profile("kick", SourceClass::Kick, 10.0f, kTransientSustain, key_onsets_per_sec),
        make_profile("bass", SourceClass::Bass, 10.0f, kSustainedSustain, ducked_onsets_per_sec)};
    MixProfile mix = make_mix(profiles.size());
    set_dominance(mix, 0, 1, 1, 0.72f, 400);
    const std::vector<SceneDelta> deltas = decide_dynamics(profiles, mix, MixAssistantConfig{});
    const Insert* duck = find_insert(deltas, "bass", "dynamics.duckingProcessor");
    REQUIRE(duck != nullptr);
    return param_number(*duck, "releaseMs");
  };

  const float dense_key = duck_release(kDenseOnsetsPerSec, kSparseOnsetsPerSec);
  const float sparse_key = duck_release(kSparseOnsetsPerSec, kDenseOnsetsPerSec);

  INFO("dense key " << dense_key << " ms, sparse key " << sparse_key << " ms");
  CHECK(dense_key < sparse_key);
  CHECK(dense_key <= recovery_bound_ms(kDenseOnsetsPerSec));
  // The sparse key leaves the starting point alone, so the case above cannot
  // pass on a stage that simply shortens every duck.
  CHECK(sparse_key < recovery_bound_ms(kSparseOnsetsPerSec));
}

TEST_CASE("a pair that does not contend in the low end gets no sidechain duck",
          "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles{make_profile("kick", SourceClass::Kick),
                                           make_profile("bass", SourceClass::Bass)};

  SECTION("the two never sound together in the band") {
    MixProfile mix = make_mix(profiles.size());
    set_dominance(mix, 0, 1, 1, 0.9f, 0);
    const std::vector<SceneDelta> deltas = decide_dynamics(profiles, mix, MixAssistantConfig{});
    CHECK_FALSE(has_insert(deltas, "bass", "dynamics.duckingProcessor"));
  }

  SECTION("the key source is the quieter of the two in the band") {
    MixProfile mix = make_mix(profiles.size());
    set_dominance(mix, 0, 1, 1, 0.3f, 400);
    const std::vector<SceneDelta> deltas = decide_dynamics(profiles, mix, MixAssistantConfig{});
    CHECK_FALSE(has_insert(deltas, "bass", "dynamics.duckingProcessor"));
  }

  SECTION("they only meet above the low end") {
    MixProfile mix = make_mix(profiles.size());
    // kBands[3] is 500-2000 Hz: shared harmonics, which is an EQ decision.
    set_dominance(mix, 0, 1, 3, 0.9f, 400);
    const std::vector<SceneDelta> deltas = decide_dynamics(profiles, mix, MixAssistantConfig{});
    CHECK_FALSE(has_insert(deltas, "bass", "dynamics.duckingProcessor"));
  }

  SECTION("the mix profile describes a different set of tracks") {
    MixProfile mix = make_mix(profiles.size() + 1);
    set_dominance(mix, 0, 1, 1, 0.9f, 400);
    const std::vector<SceneDelta> deltas = decide_dynamics(profiles, mix, MixAssistantConfig{});
    CHECK_FALSE(has_insert(deltas, "bass", "dynamics.duckingProcessor"));
  }
}

TEST_CASE("the gate is suggested only for confident, transient material", "[mixing][assistant]") {
  // The positive control. Without it the checks below would pass on a stage
  // that never suggests a gate at all.
  SECTION("a confidently classified transient kit source is gated") {
    const std::vector<TrackProfile> profiles{make_profile(
        "kick", SourceClass::Kick, kGateableCrestDb, kTransientSustain, kPlayedOnsetsPerSec)};
    const std::vector<SceneDelta> deltas =
        decide_dynamics(profiles, make_mix(profiles.size()), MixAssistantConfig{});
    const Insert* gate = find_insert(deltas, "kick", "dynamics.gate");
    REQUIRE(gate != nullptr);
    // A partial attenuation, never a mute: a gate that fires on the wrong
    // material has to dip the part rather than delete it.
    CHECK(param_number(*gate, "rangeDb") < 0.0f);
    // And the closing threshold sits below the opening one, so a signal resting
    // on the threshold cannot chatter the gate.
    CHECK(param_number(*gate, "closeThresholdDb") < param_number(*gate, "thresholdDb"));
  }

  SECTION("a class the classifier is unsure of is not gated") {
    const std::vector<TrackProfile> profiles{make_profile("kick", SourceClass::Kick,
                                                          kGateableCrestDb, kTransientSustain,
                                                          kPlayedOnsetsPerSec, kUncertainClass)};
    const std::vector<SceneDelta> deltas =
        decide_dynamics(profiles, make_mix(profiles.size()), MixAssistantConfig{});
    CHECK_FALSE(has_insert(deltas, "kick", "dynamics.gate"));
    // The track is still treated; only the destructive decision is withheld.
    CHECK(has_insert(deltas, "kick", "dynamics.compressor"));
  }

  SECTION("a sustained track is not gated") {
    const std::vector<TrackProfile> profiles{make_profile(
        "kick", SourceClass::Kick, kGateableCrestDb, kSustainedSustain, kPlayedOnsetsPerSec)};
    const std::vector<SceneDelta> deltas =
        decide_dynamics(profiles, make_mix(profiles.size()), MixAssistantConfig{});
    CHECK_FALSE(has_insert(deltas, "kick", "dynamics.gate"));
  }

  SECTION("a track whose hits do not stand above the bleed is not gated") {
    const std::vector<TrackProfile> profiles{make_profile("kick", SourceClass::Kick, kLowCrestDb,
                                                          kTransientSustain, kPlayedOnsetsPerSec)};
    const std::vector<SceneDelta> deltas =
        decide_dynamics(profiles, make_mix(profiles.size()), MixAssistantConfig{});
    CHECK_FALSE(has_insert(deltas, "kick", "dynamics.gate"));
  }

  SECTION("a class outside the close-miked kit is not gated") {
    const std::vector<TrackProfile> profiles{make_profile(
        "vox", SourceClass::Vocal, kGateableCrestDb, kTransientSustain, kPlayedOnsetsPerSec)};
    const std::vector<SceneDelta> deltas =
        decide_dynamics(profiles, make_mix(profiles.size()), MixAssistantConfig{});
    CHECK_FALSE(has_insert(deltas, "vox", "dynamics.gate"));
  }
}

TEST_CASE("suggestion strength scales the ratios and ranges", "[mixing][assistant]") {
  const std::vector<TrackProfile> profiles{make_profile("vox", SourceClass::Vocal)};
  MixAssistantConfig half;
  half.suggestion_strength = 0.5f;

  const std::vector<SceneDelta> full_deltas =
      decide_dynamics(profiles, make_mix(profiles.size()), MixAssistantConfig{});
  const std::vector<SceneDelta> half_deltas =
      decide_dynamics(profiles, make_mix(profiles.size()), half);

  const Insert* full = find_insert(full_deltas, "vox", "dynamics.compressor");
  const Insert* half_insert = find_insert(half_deltas, "vox", "dynamics.compressor");
  REQUIRE(full != nullptr);
  REQUIRE(half_insert != nullptr);

  // A ratio is a multiplier around unity, so a weaker suggestion moves it
  // towards 1 rather than scaling it.
  const float full_ratio = param_number(*full, "ratio");
  const float half_ratio = param_number(*half_insert, "ratio");
  CHECK(half_ratio < full_ratio);
  CHECK(half_ratio > 1.0f);
}

TEST_CASE("an untreatable track gets no dynamics delta", "[mixing][assistant]") {
  std::vector<TrackProfile> profiles{
      make_profile("music", SourceClass::Vocal), make_profile("mystery", SourceClass::Unknown),
      make_profile("rejected", SourceClass::Vocal), make_profile("guessed", SourceClass::Vocal)};
  profiles[2].usable = false;
  profiles[2].exclusion_reason = "excluded by the profiler";
  // Below the confidence any decision here acts on: the class is a guess, and
  // every suggestion this stage makes is selected by class.
  profiles[3].source_confidence = 0.1f;

  const std::vector<SceneDelta> deltas =
      decide_dynamics(profiles, make_mix(profiles.size()), MixAssistantConfig{});

  REQUIRE_FALSE(deltas.empty());
  for (const SceneDelta& delta : deltas) {
    // Not "a delta with a neutral processor" — no delta at all, so nothing
    // downstream can read the exclusion as a decision.
    CHECK(delta.strip_id == "music");
  }
}

TEST_CASE("no strip is given the same processor twice in the same slot", "[mixing][assistant]") {
  std::vector<TrackProfile> profiles = every_treatable_class();
  // A second kick keying the same bass would otherwise be a second copy of the
  // ducking processor in the same slot, which apply_deltas drops.
  profiles.push_back(make_profile("kick-sub", SourceClass::Kick, kGateableCrestDb,
                                  kTransientSustain, kPlayedOnsetsPerSec));
  MixProfile mix = make_mix(profiles.size());
  set_dominance(mix, 0, 5, 1, 0.72f, 400);
  set_dominance(mix, profiles.size() - 1, 5, 1, 0.80f, 400);

  const std::vector<SceneDelta> deltas = decide_dynamics(profiles, mix, MixAssistantConfig{});
  REQUIRE_FALSE(deltas.empty());

  std::set<std::string> seen;
  for (const SceneDelta& delta : deltas) {
    CHECK(delta.domain == DeltaDomain::Dynamics);
    CHECK_FALSE(delta.reason.empty());
    // The reason becomes one line of the explanation verbatim, so it reads as a
    // lower-case declarative sentence rather than a heading.
    CHECK(delta.reason[0] == std::tolower(static_cast<unsigned char>(delta.reason[0])));
    for (const Insert& insert : delta.inserts) {
      const std::string key = delta.strip_id + "/" +
                              (insert.slot == InsertSlot::PreFader ? "pre" : "post") + "/" +
                              insert.processor_name;
      INFO("insert " << key);
      CHECK(seen.insert(key).second);
    }
  }
}

TEST_CASE("degenerate dynamics input yields an empty suggestion", "[mixing][assistant]") {
  SECTION("no tracks") { CHECK(decide_dynamics({}, MixProfile{}, MixAssistantConfig{}).empty()); }

  SECTION("the dynamics domain is disabled") {
    const std::vector<TrackProfile> profiles{make_profile("vox", SourceClass::Vocal)};
    MixAssistantConfig config;
    config.enable_dynamics = false;
    CHECK(decide_dynamics(profiles, make_mix(profiles.size()), config).empty());
  }

  SECTION("every track is silent") {
    std::vector<TrackProfile> profiles{make_profile("vox", SourceClass::Vocal),
                                       make_profile("kick", SourceClass::Kick)};
    for (TrackProfile& profile : profiles) {
      profile.base.loudness.integrated_lufs = sonare::constants::kFloorDb;
    }
    CHECK(decide_dynamics(profiles, make_mix(profiles.size()), MixAssistantConfig{}).empty());
  }
}
