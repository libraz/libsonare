#include "engine/metronome.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <iterator>

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

  // The accented click on beat 1 ramps up from silence (no hard 0 -> peak step)
  // and the regular click on the next beat is quieter than the accent peak.
  REQUIRE(left[0] > 0.0f);
  REQUIRE(left[0] < 0.75f);
  const float accent_peak = *std::max_element(left.begin(), left.begin() + 4);
  const float beat_peak = *std::max_element(left.begin() + 24000, left.begin() + 24004);
  REQUIRE(accent_peak > beat_peak);
  // Both clicks decay to exactly zero at their final sample (no tail step).
  REQUIRE(left[3] == 0.0f);
  REQUIRE(left[24003] == 0.0f);
}

TEST_CASE("Metronome click envelope ramps up and decays to zero", "[engine][metronome]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);

  constexpr int kClickLen = 64;
  sonare::engine::Metronome metro;
  metro.prepare(48000.0, &tempo);
  metro.set_config({true, 0.25f, 0.75f, kClickLen});

  std::array<float, 256> left{};
  float* channels[] = {left.data()};
  metro.process(channels, 1, static_cast<int>(left.size()), 0);

  // No instantaneous onset: the first sample starts near zero.
  REQUIRE(left[0] >= 0.0f);
  REQUIRE(left[0] < 0.1f);
  // The envelope ramps up to an interior peak (not at the very first sample).
  const auto peak_it = std::max_element(left.begin(), left.begin() + kClickLen);
  const float peak = *peak_it;
  const auto peak_index = std::distance(left.begin(), peak_it);
  REQUIRE(peak > 0.5f);
  REQUIRE(peak_index > 0);
  REQUIRE(peak <= 0.75f);
  // Strictly increasing through the attack: the first sample is below the peak.
  REQUIRE(left[0] < peak);
  // The click decays back to exactly zero at its final sample and stays silent
  // afterwards (no hard step at the tail boundary).
  REQUIRE(left[kClickLen - 1] == 0.0f);
  REQUIRE(left[kClickLen] == 0.0f);
  // The sample just before the end is small (decayed), proving the tail trends
  // to zero rather than ending mid-amplitude.
  REQUIRE(left[kClickLen - 2] < 0.1f);
}

TEST_CASE("Metronome click is continuous across sub-block boundaries", "[engine][metronome]") {
  // Regression: the click used to be truncated at each sub-block boundary, so a
  // click straddling a boundary lost its tail. Rendering one click split across
  // two consecutive sub-blocks must produce the same samples as one whole call.
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);

  constexpr int kClickLen = 64;
  constexpr int kTotal = 256;
  sonare::engine::Metronome metro;
  metro.prepare(48000.0, &tempo);
  metro.set_config({true, 0.25f, 0.75f, kClickLen});

  std::array<float, kTotal> whole{};
  float* whole_ch[] = {whole.data()};
  metro.process(whole_ch, 1, kTotal, 0);

  // Same render, split at sample 30 (mid-click): the second call starts at
  // block_start_sample 30 with the buffer pointer advanced to that offset.
  std::array<float, kTotal> split{};
  constexpr int kSplit = 30;
  float* head[] = {split.data()};
  metro.process(head, 1, kSplit, 0);
  float* tail[] = {split.data() + kSplit};
  metro.process(tail, 1, kTotal - kSplit, kSplit);

  for (int i = 0; i < kTotal; ++i) {
    REQUIRE(split[static_cast<size_t>(i)] == whole[static_cast<size_t>(i)]);
  }
  // The click genuinely straddled the split point (non-zero on both sides).
  REQUIRE(whole[kSplit - 1] > 0.0f);
  REQUIRE(whole[kSplit + 1] > 0.0f);
}

TEST_CASE("Metronome count-in ends at requested bar boundary", "[engine][metronome]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);

  sonare::engine::Metronome metro;
  metro.prepare(48000.0, &tempo);

  REQUIRE(metro.count_in_end_sample(0, 2) == 192000);
  REQUIRE(metro.count_in_end_sample(24000, 1) == 96000);
}
