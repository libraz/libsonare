#include "engine/tempo_sync.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

TEST_CASE("tempo sync converts musical values to PPQ", "[engine][tempo_sync]") {
  REQUIRE_THAT(sonare::engine::tempo_sync_ppq({4, sonare::transport::NoteModifier::kStraight}),
               WithinAbs(1.0, 1.0e-9));
  REQUIRE_THAT(sonare::engine::tempo_sync_ppq({8, sonare::transport::NoteModifier::kDotted}),
               WithinAbs(0.75, 1.0e-9));
  REQUIRE_THAT(sonare::engine::tempo_sync_ppq({4, sonare::transport::NoteModifier::kTriplet}),
               WithinAbs(2.0 / 3.0, 1.0e-9));
}

TEST_CASE("tempo sync follows TransportState bpm and sample rate", "[engine][tempo_sync]") {
  sonare::transport::TransportState state{};
  state.bpm = 120.0;
  state.sample_rate = 48000.0;
  REQUIRE(sonare::engine::tempo_sync_samples({4, sonare::transport::NoteModifier::kStraight},
                                             state) == 24000);

  state.bpm = 60.0;
  REQUIRE(sonare::engine::tempo_sync_samples({4, sonare::transport::NoteModifier::kStraight},
                                             state) == 48000);
}

TEST_CASE("tempo sync warp bake smooths segment joins without changing target length",
          "[engine][tempo_sync]") {
  constexpr int kSamplesPerSegment = 2048;
  std::vector<float> source(static_cast<size_t>(kSamplesPerSegment * 2));
  std::fill(source.begin(), source.begin() + kSamplesPerSegment, 0.75f);
  std::fill(source.begin() + kSamplesPerSegment, source.end(), -0.75f);

  const std::vector<sonare::engine::TempoSyncWarpSegment> segments{
      {0, kSamplesPerSegment, kSamplesPerSegment},
      {kSamplesPerSegment, kSamplesPerSegment, kSamplesPerSegment},
  };
  sonare::engine::TempoSyncWarpBakeConfig config;
  config.sample_rate = 48000;
  config.n_fft = 512;
  config.hop_length = 128;

  config.join_crossfade_samples = 0;
  const std::vector<float> hard =
      sonare::engine::bake_tempo_sync_warp_channel(source.data(), source.size(), segments, config);

  config.join_crossfade_samples = 64;
  const std::vector<float> smoothed =
      sonare::engine::bake_tempo_sync_warp_channel(source.data(), source.size(), segments, config);

  REQUIRE(hard.size() == source.size());
  REQUIRE(smoothed.size() == source.size());
  const size_t boundary = static_cast<size_t>(kSamplesPerSegment);
  const float hard_jump = std::abs(hard[boundary] - hard[boundary - 1]);
  const float smoothed_jump = std::abs(smoothed[boundary] - smoothed[boundary - 1]);
  REQUIRE(smoothed_jump < hard_jump);
}
