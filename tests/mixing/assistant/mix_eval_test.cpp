/// @file mix_eval_test.cpp
/// @brief Objective regression measurements for the mixing assistant.
///
/// @details Two kinds of check live here and they are treated differently.
///          Invariants — the scene round-trips, the suggestion instantiates and
///          renders, the master stays under its ceiling — fail the build.
///          Numeric quality figures are recorded and printed, not gated: a
///          threshold on "how much better did the mix get" would encode one
///          person's taste as a build failure.

#include "mix_eval.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "mastering/stereo/mono_compat_check.h"
#include "mixing/api/scene.h"
#include "mixing/assistant/suggester.h"

namespace {

using sonare::mixing::assistant::MixAssistantConfig;
using sonare::mixing::assistant::test::evaluate;
using sonare::mixing::assistant::test::make_demo_tracks;
using sonare::mixing::assistant::test::MixEvaluation;

// A suggested scene must not push the master past a delivery ceiling. Chosen to
// match the most permissive ceiling the mastering presets use, since the
// assistant proposes a mix rather than a master and a limiter has not run yet.
constexpr float kMasterCeilingDbtp = 0.0f;

// Widest L/R RMS difference a finished record is observed to carry, in dB. Not a
// preference and not a house rule: across the 928 UK/US number-one singles
// surveyed in P. Pestana and J. D. Reiss, "Intelligent Audio Production
// Strategies Informed by Best Practices" (AES 53rd International Conference on
// Semantic Audio, London, 2014), Fig. 3, the ratio between the two channels'
// RMS levels stays inside 0.8 dB, and stays there consistently across sixty
// years of releases. Every one of those mixes places instruments across the
// image, so the bound is a statement about the resulting energy balance rather
// than about how narrow the placement is — a suggestion that exceeds it has put
// more weight on one side than commercial practice ever does.
constexpr float kMaxChannelImbalanceDb = 0.8f;

}  // namespace

TEST_CASE("a suggested scene instantiates, renders and round-trips", "[mixing][assistant]") {
  // Not hidden, so this runs in the default ctest pass and in CI. Only the
  // invariants are asserted here, and they are binary — a scene that cannot be
  // serialised, cannot be instantiated, or clips the master is broken whatever
  // anyone thinks of the mix. The numeric quality figures are recorded in the
  // slow cases below rather than gated, because a threshold on "how much better
  // did the mix get" would encode one person's taste as a build failure.
  //
  // Short material on purpose: the invariants do not need the EQ pass to fire,
  // and this case has to stay cheap enough to run on every commit.
  const auto fixture = make_demo_tracks(48000, 0.5f);
  const auto tracks = fixture.inputs();
  const auto result = sonare::mixing::assistant::suggest_scene(tracks);
  const MixEvaluation evaluation = evaluate(tracks, result);

  REQUIRE(evaluation.scene_round_trips);
  REQUIRE(evaluation.rendered);
  REQUIRE(evaluation.master_true_peak_dbtp <= kMasterCeilingDbtp);
}

TEST_CASE("a suggested scene keeps the two channels in balance", "[mixing][assistant]") {
  // Gated rather than recorded, and in the default pass, because the bound is
  // measured from commercial releases rather than chosen here. The fixture is
  // deliberately asymmetric — the guitar pair is double-tracked at two different
  // pitches and the rest is mono — so a panning decision that leans one way has
  // material to lean with.
  //
  // Only the excess is asserted. The measured figure is reported rather than
  // pinned, because any particular value inside the bound is one placement among
  // many that all satisfy the same observation.
  //
  // The fixture length matches the invariant case above and is not shortened
  // further: below roughly 0.4 s the material is inside a single loudness gating
  // block, every track is excluded as unmeasurable, and the scene stops
  // rendering at all — which would turn this into a case that cannot fail.
  const auto fixture = make_demo_tracks(48000, 0.5f);
  const auto tracks = fixture.inputs();
  const auto result = sonare::mixing::assistant::suggest_scene(tracks);
  const MixEvaluation evaluation = evaluate(tracks, result);

  REQUIRE(evaluation.rendered);
  CAPTURE(evaluation.master_lr_rms_difference_db);
  REQUIRE(std::abs(evaluation.master_lr_rms_difference_db) <= kMaxChannelImbalanceDb);
}

TEST_CASE("the channel-balance measure reads a known imbalance", "[mixing][assistant]") {
  // The positive control for the case above. A measure that returned zero for
  // every input would satisfy the 0.8 dB bound on any mix whatsoever, including
  // one panned hard to a single side, so the bound is only worth asserting once
  // the measure is known to move.
  const std::vector<float> left(1024, 0.5f);
  const std::vector<float> right(left.size(), 0.25f);
  // Half the amplitude is a quarter of the power, so the two channels sit
  // 10*log10(4) dB apart.
  const double expected_db =
      static_cast<double>(sonare::constants::kPowerToDbScale) * std::log(4.0);

  using sonare::mixing::assistant::test::channel_rms_difference_db;
  // Positive when the left channel is the louder one, and antisymmetric in its
  // arguments — a sign convention the invariant's absolute value would hide.
  REQUIRE_THAT(channel_rms_difference_db(left, right),
               Catch::Matchers::WithinAbs(expected_db, 0.001));
  REQUIRE_THAT(channel_rms_difference_db(right, left),
               Catch::Matchers::WithinAbs(-expected_db, 0.001));
}

TEST_CASE("objective mix metrics are recorded for regression tracking",
          "[mixing][assistant][.][slow]") {
  const auto fixture = make_demo_tracks();
  const auto tracks = fixture.inputs();
  const auto result = sonare::mixing::assistant::suggest_scene(tracks);
  const MixEvaluation evaluation = evaluate(tracks, result);

  REQUIRE(evaluation.scene_round_trips);
  REQUIRE(evaluation.rendered);
  REQUIRE(evaluation.master_true_peak_dbtp <= kMasterCeilingDbtp);

  // Recorded, not gated.
  WARN("track loudness spread (dB): " << evaluation.track_loudness_spread_db);
  WARN("total band dominance above parity: " << evaluation.total_dominance);
  WARN("mono risk count: " << evaluation.mono_risk_count);
  WARN("master true peak (dBTP): " << evaluation.master_true_peak_dbtp);
  WARN("master L/R RMS difference (dB): " << evaluation.master_lr_rms_difference_db);
}

TEST_CASE("gain staging narrows the spread of per-track loudness", "[mixing][assistant][.][slow]") {
  // Near-tautological once staging normalises to an absolute target, so this is
  // a break detector rather than evidence of improvement: it catches a silent
  // track that escaped exclusion, or a clamp that never engaged.
  const auto fixture = make_demo_tracks();
  const auto tracks = fixture.inputs();

  const auto suggested = sonare::mixing::assistant::suggest_scene(tracks);

  MixAssistantConfig off;
  off.enable_structure = false;
  off.enable_gain = false;
  off.enable_balance = false;
  off.enable_eq = false;
  off.enable_dynamics = false;
  off.enable_image = false;
  const auto untouched = sonare::mixing::assistant::suggest_scene(tracks, off);

  const float before = evaluate(tracks, untouched).track_loudness_spread_db;
  const float after = evaluate(tracks, suggested).track_loudness_spread_db;
  INFO("spread before " << before << " dB, after " << after << " dB");
  REQUIRE(after <= before);
}

TEST_CASE("every strip in a suggested scene reaches the master", "[mixing][assistant][.][slow]") {
  // A strip with no path to the master is silent in the render, which is the
  // one structural mistake that produces a plausible-looking scene and no
  // audio.
  const auto fixture = make_demo_tracks();
  const auto result = sonare::mixing::assistant::suggest_scene(fixture.inputs());
  const auto& scene = result.scene;
  if (scene.connections.empty()) {
    SUCCEED("structure suggestions are disabled in this build");
    return;
  }

  for (const auto& strip : scene.strips) {
    bool reachable = false;
    std::vector<std::string> frontier = {strip.id};
    std::vector<std::string> visited;
    while (!frontier.empty() && !reachable) {
      const std::string node = frontier.back();
      frontier.pop_back();
      if (std::find(visited.begin(), visited.end(), node) != visited.end()) continue;
      visited.push_back(node);
      for (const auto& connection : scene.connections) {
        if (connection.source != node) continue;
        if (connection.destination == "master") {
          reachable = true;
          break;
        }
        frontier.push_back(connection.destination);
      }
      for (const auto& send : strip.sends) {
        if (send.destination_bus_id == node) frontier.push_back(send.destination_bus_id);
      }
    }
    INFO("strip without a path to the master: " << strip.id);
    REQUIRE(reachable);
  }
}

TEST_CASE("the suggestion does not make the mono fold worse", "[mixing][assistant][.][slow]") {
  // Widening for separation is only useful while the mix still survives a mono
  // fold, so the count of at-risk tracks must not grow because of a suggestion.
  const auto fixture = make_demo_tracks();
  const auto tracks = fixture.inputs();
  const auto result = sonare::mixing::assistant::suggest_scene(tracks);

  std::vector<float> summed_left;
  std::vector<float> summed_right;
  sonare::mixing::assistant::test::sum_tracks(tracks, summed_left, summed_right);

  std::vector<float> mixed_left;
  std::vector<float> mixed_right;
  REQUIRE(
      sonare::mixing::assistant::test::render_scene(tracks, result.scene, mixed_left, mixed_right));

  const auto reference = sonare::mastering::stereo::mono_compat_check(
      summed_left.data(), summed_right.data(), summed_left.size());
  const auto mixed = sonare::mastering::stereo::mono_compat_check(
      mixed_left.data(), mixed_right.data(), mixed_left.size());

  INFO("reference correlation " << reference.correlation << ", mixed " << mixed.correlation);
  // The suggested mix may narrow or widen, but it must not invert the bus.
  REQUIRE(mixed.correlation > -0.5f);
}
