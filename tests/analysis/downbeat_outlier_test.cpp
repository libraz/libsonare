/// @file downbeat_outlier_test.cpp
/// @brief One dominant off-beat observation must not move the bar phase.

#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "analysis/downbeat_analyzer.h"

using namespace sonare;

namespace {

std::vector<Beat> bar_beats(int count) {
  std::vector<Beat> beats;
  for (int i = 0; i < count; ++i) {
    beats.push_back({static_cast<float>(i) * 0.5f, i * 10, 0.4f});
  }
  return beats;
}

/// @brief Low-frequency evidence [1, .3, .5, .3] per bar; beat @p loud_beat (if >= 0) is set to
///        @p loud. Harmonic change is flat, so the low band alone carries the phase.
DownbeatObservations observations_for(int count, int loud_beat, float loud) {
  const float pattern[4] = {1.0f, 0.3f, 0.5f, 0.3f};
  DownbeatObservations obs;
  for (int i = 0; i < count; ++i) {
    obs.lf.push_back(i == loud_beat ? loud : pattern[i % 4]);
  }
  obs.harmonic_change.assign(static_cast<size_t>(count), 0.0f);
  return obs;
}

}  // namespace

TEST_CASE("downbeat evidence picks the bar start", "[analysis][downbeat][outlier]") {
  const std::vector<Beat> beats = bar_beats(16);
  const DownbeatResult result =
      select_downbeat_phase(beats, TimeSignature{4, 4, 1.0f}, observations_for(16, -1, 0));

  REQUIRE(result.beat_indices == std::vector<int>{0, 4, 8, 12});
}

TEST_CASE("downbeat phase survives one dominant off-beat observation",
          "[analysis][downbeat][outlier]") {
  const std::vector<Beat> beats = bar_beats(16);
  const DownbeatResult result =
      select_downbeat_phase(beats, TimeSignature{4, 4, 1.0f}, observations_for(16, 5, 20.0f));

  REQUIRE(result.beat_indices == std::vector<int>{0, 4, 8, 12});
}
