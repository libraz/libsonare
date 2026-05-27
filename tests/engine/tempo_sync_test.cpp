#include "engine/tempo_sync.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

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
