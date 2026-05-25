#include "engine/metronome.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Metronome collects beat clicks at sample-accurate offsets", "[engine][metronome]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);

  sonare::engine::Metronome metro;
  metro.prepare(48000.0, &tempo);
  metro.set_config({true, 0.25f, 0.75f, 16});

  sonare::engine::MetronomeEventList events;
  metro.collect_events(0, 48000, &events);

  REQUIRE(events.size == 2);
  REQUIRE(events.events[0].offset == 0);
  REQUIRE(events.events[0].accent);
  REQUIRE(events.events[1].offset == 24000);
  REQUIRE_FALSE(events.events[1].accent);
}

TEST_CASE("Metronome follows tempo changes through TempoMap", "[engine][metronome]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);
  tempo.set_segments({{0.0, 120.0, 0.0}, {2.0, 60.0, 0.0}});

  sonare::engine::Metronome metro;
  metro.prepare(48000.0, &tempo);
  metro.set_config({true, 0.25f, 0.75f, 16});

  sonare::engine::MetronomeEventList events;
  metro.collect_events(0, 144001, &events);

  REQUIRE(events.size == 5);
  REQUIRE(events.events[0].timeline_sample == 0);
  REQUIRE(events.events[1].timeline_sample == 24000);
  REQUIRE(events.events[2].timeline_sample == 48000);
  REQUIRE(events.events[3].timeline_sample == 96000);
  REQUIRE(events.events[4].timeline_sample == 144000);
}

TEST_CASE("Metronome process renders accented and regular clicks", "[engine][metronome]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);

  sonare::engine::Metronome metro;
  metro.prepare(48000.0, &tempo);
  metro.set_config({true, 0.25f, 0.75f, 4});

  std::array<float, 24008> left{};
  float* channels[] = {left.data()};
  metro.process(channels, 1, static_cast<int>(left.size()), 0);

  REQUIRE(left[0] == 0.75f);
  REQUIRE(left[1] < left[0]);
  REQUIRE(left[24000] == 0.25f);
  REQUIRE(left[23999] == 0.0f);
}

TEST_CASE("Metronome count-in ends at requested bar boundary", "[engine][metronome]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);

  sonare::engine::Metronome metro;
  metro.prepare(48000.0, &tempo);

  REQUIRE(metro.count_in_end_sample(0, 2) == 192000);
  REQUIRE(metro.count_in_end_sample(24000, 1) == 96000);
}
