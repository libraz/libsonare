/// @file tempo_octave_evidence_outlier_test.cpp
/// @brief Octave evidence must not be scaled by one dominant onset frame.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string_view>
#include <vector>

#include "mir/tempo_estimator_bridge.h"

using sonare::Beat;
using sonare::mir::BeatAnalysisInput;
using sonare::mir::estimate_tempo;
using sonare::mir::TempoEstimate;

namespace {

/// @brief 20 beats at 120 BPM; beat frames carry 1.0, midpoints 0.5, and frame 2 carries @p loud.
BeatAnalysisInput fixture(float loud) {
  BeatAnalysisInput in;
  in.sample_rate = 22050;
  in.hop_length = 512;
  const double fps = static_cast<double>(in.sample_rate) / in.hop_length;
  in.onset_strength.assign(static_cast<size_t>(11.0 * fps), 0.0f);
  for (int i = 0; i < 20; ++i) {
    const double t = 1.0 + 0.5 * i;
    in.beats.push_back({static_cast<float>(t), 0, 1.0f});
    in.onset_strength[static_cast<size_t>(std::lround(t * fps))] = 1.0f;
    in.onset_strength[static_cast<size_t>(std::lround((t + 0.25) * fps))] = 0.5f;
  }
  in.onset_strength[2] = loud;
  return in;
}

/// @brief Double-tempo confidence relative to the primary one, i.e. the octave evidence.
double double_evidence(const BeatAnalysisInput& in) {
  double primary = 0.0;
  double doubled = 0.0;
  for (const TempoEstimate& est : estimate_tempo(in, {})) {
    if (std::string_view(est.label) == "primary") primary = est.confidence;
    if (std::string_view(est.label) == "double") doubled = est.confidence;
  }
  REQUIRE(primary > 0.0);
  return doubled / primary;
}

}  // namespace

TEST_CASE("double-tempo evidence reads the midpoint activation", "[mir][tempo][outlier]") {
  REQUIRE(std::abs(double_evidence(fixture(0.0f)) - 0.5) < 0.05);
}

TEST_CASE("double-tempo evidence ignores one dominant onset frame", "[mir][tempo][outlier]") {
  REQUIRE(std::abs(double_evidence(fixture(50.0f)) - double_evidence(fixture(0.0f))) < 0.05);
}
