#include "transport/transport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <limits>

#include "transport/musical_time.h"
#include "transport/tempo_map.h"

using Catch::Matchers::WithinAbs;

TEST_CASE("TempoMap::ppq_to_sample guards non-finite input", "[transport]") {
  sonare::transport::TempoMap map;
  map.prepare(48000.0);
  map.set_segments({{0.0, 120.0, 0.0}});
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  // Non-finite input must not reach llround (UB); it returns 0 instead.
  REQUIRE(map.ppq_to_sample(nan) == 0);
  REQUIRE(map.ppq_to_sample(-inf) == 0);
  REQUIRE(map.ppq_to_sample(inf) == 0);
  // A finite value still converts normally.
  REQUIRE(map.ppq_to_sample(1.0) == 24000);
}

TEST_CASE("Transport::set_loop rejects non-finite bounds", "[transport]") {
  sonare::transport::TempoMap map;
  map.prepare(48000.0);
  sonare::transport::Transport transport;
  transport.prepare(48000.0, &map);

  const double nan = std::numeric_limits<double>::quiet_NaN();
  transport.set_loop(2.0, nan, true);
  auto state = transport.snapshot();
  REQUIRE_FALSE(state.looping);

  // A valid loop still engages.
  transport.set_loop(2.0, 4.0, true);
  state = transport.snapshot();
  REQUIRE(state.looping);
}

TEST_CASE("TempoMap converts samples and PPQ across tempo segments", "[transport]") {
  sonare::transport::TempoMap map;
  map.prepare(48000.0);
  map.set_segments({{0.0, 120.0, 0.0}, {4.0, 60.0, 0.0}});

  REQUIRE(map.ppq_to_sample(0.0) == 0);
  REQUIRE(map.ppq_to_sample(1.0) == 24000);
  REQUIRE(map.ppq_to_sample(4.0) == 96000);
  REQUIRE(map.ppq_to_sample(5.0) == 144000);
  REQUIRE_THAT(map.sample_to_ppq(144000), WithinAbs(5.0, 1.0e-9));
  REQUIRE_THAT(map.sample_to_ppq(map.ppq_to_sample(7.25)), WithinAbs(7.25, 1.0e-9));
  REQUIRE_THAT(map.bpm_at_sample(96000), WithinAbs(60.0, 1.0e-9));
}

TEST_CASE("TempoMap linear tempo ramp round-trips and reduces to constant", "[transport][tempo]") {
  SECTION("ramped segment ppq<->sample conversions are inverses") {
    sonare::transport::TempoMap map;
    map.prepare(48000.0);
    // Ramp 120 -> 240 BPM over ppq [0, 4); a constant tail keeps the ramp span
    // finite (end_ppq is the next segment's start_ppq).
    sonare::transport::TempoSegment ramp{};
    ramp.start_ppq = 0.0;
    ramp.bpm = 120.0;
    ramp.end_bpm = 240.0;
    sonare::transport::TempoSegment tail{};
    tail.start_ppq = 4.0;
    tail.bpm = 240.0;
    map.set_segments({ramp, tail});

    for (double ppq : {0.0, 0.5, 1.0, 2.0, 3.5, 3.999}) {
      const int64_t sample = map.ppq_to_sample(ppq);
      // Sample resolution is integer, so allow a hair of round-trip tolerance.
      REQUIRE_THAT(map.sample_to_ppq(sample), WithinAbs(ppq, 5.0e-4));
    }
    // The instantaneous tempo rises monotonically across the ramp.
    REQUIRE(map.bpm_at_sample(0) < map.bpm_at_sample(map.ppq_to_sample(3.999)));
    REQUIRE_THAT(map.bpm_at_sample(0), WithinAbs(120.0, 1.0e-3));
  }

  SECTION("constant segment (end_bpm == 0) matches legacy constant-tempo math") {
    sonare::transport::TempoMap ramped;
    ramped.prepare(48000.0);
    sonare::transport::TempoSegment constant{};
    constant.start_ppq = 0.0;
    constant.bpm = 120.0;
    constant.end_bpm = 0.0;  // explicitly disabled ramp.
    ramped.set_segments({constant});

    sonare::transport::TempoMap legacy;
    legacy.prepare(48000.0);
    legacy.set_segments({{0.0, 120.0, 0.0}});

    for (double ppq : {0.0, 1.0, 4.0, 7.25, 16.0}) {
      REQUIRE(ramped.ppq_to_sample(ppq) == legacy.ppq_to_sample(ppq));
    }
    REQUIRE(ramped.ppq_to_sample(1.0) == 24000);
    for (int64_t sample : {int64_t{0}, int64_t{24000}, int64_t{96000}}) {
      REQUIRE_THAT(ramped.sample_to_ppq(sample), WithinAbs(legacy.sample_to_ppq(sample), 1.0e-12));
    }
  }
}

TEST_CASE("TempoMap reports bar and beat positions", "[transport]") {
  sonare::transport::TempoMap map;
  map.prepare(48000.0);
  map.set_time_signatures({{0.0, {4, 4}}, {8.0, {3, 4}}});

  auto start = map.ppq_to_bar_beat(0.0);
  REQUIRE(start.bar == 0);
  REQUIRE(start.beat == 1);
  REQUIRE_THAT(start.beat_fraction, WithinAbs(0.0, 1.0e-9));

  auto second_bar = map.ppq_to_bar_beat(5.5);
  REQUIRE(second_bar.bar == 1);
  REQUIRE(second_bar.beat == 2);
  REQUIRE_THAT(second_bar.beat_fraction, WithinAbs(0.5, 1.0e-9));

  auto three_four = map.ppq_to_bar_beat(10.0);
  REQUIRE(three_four.bar == 2);
  REQUIRE(three_four.beat == 3);
  REQUIRE_THAT(map.bar_start_ppq(10.0), WithinAbs(8.0, 1.0e-9));
}

TEST_CASE("Musical time helpers convert note values", "[transport]") {
  using sonare::transport::NoteModifier;

  REQUIRE_THAT(sonare::transport::note_length_ppq(4), WithinAbs(1.0, 1.0e-9));
  REQUIRE_THAT(sonare::transport::note_length_ppq(8, NoteModifier::kDotted),
               WithinAbs(0.75, 1.0e-9));
  REQUIRE_THAT(sonare::transport::note_length_ppq(4, NoteModifier::kTriplet),
               WithinAbs(2.0 / 3.0, 1.0e-9));
  REQUIRE(sonare::transport::ppq_duration_to_samples(1.0, 120.0, 48000.0) == 24000);
  REQUIRE_THAT(sonare::transport::samples_to_ppq_duration(24000, 120.0, 48000.0),
               WithinAbs(1.0, 1.0e-9));
}

TEST_CASE("Transport play stop seek and render clock are independent", "[transport]") {
  sonare::transport::TempoMap map;
  map.prepare(48000.0);

  sonare::transport::Transport transport;
  transport.prepare(48000.0, &map);

  transport.advance(128);
  auto state = transport.snapshot();
  REQUIRE(state.render_frame == 128);
  REQUIRE(state.sample_position == 0);
  REQUIRE_FALSE(state.playing);

  transport.play();
  transport.advance(256);
  state = transport.snapshot();
  REQUIRE(state.render_frame == 384);
  REQUIRE(state.sample_position == 256);
  REQUIRE(state.playing);

  transport.seek_ppq(2.0);
  state = transport.snapshot();
  REQUIRE(state.sample_position == 48000);
  REQUIRE_THAT(state.ppq_position, WithinAbs(2.0, 1.0e-9));

  transport.stop();
  transport.advance(64);
  state = transport.snapshot();
  REQUIRE(state.render_frame == 448);
  REQUIRE(state.sample_position == 48000);
  REQUIRE_FALSE(state.playing);
}

TEST_CASE("Transport loop boundaries and wrap are sample accurate", "[transport]") {
  sonare::transport::TempoMap map;
  map.prepare(48000.0);

  sonare::transport::Transport transport;
  transport.prepare(48000.0, &map);
  transport.seek_ppq(3.99);
  transport.set_loop(2.0, 4.0, true);
  transport.play();

  sonare::transport::BoundaryList boundaries;
  REQUIRE(transport.collect_loop_boundaries(512, &boundaries));
  REQUIRE(boundaries.size() == 1);
  REQUIRE(boundaries[0].offset == 240);
  REQUIRE(boundaries[0].timeline_sample == 96000);

  transport.advance(512);
  const auto state = transport.snapshot();
  REQUIRE(state.render_frame == 512);
  REQUIRE(state.sample_position == 48272);
  REQUIRE_THAT(state.ppq_position, WithinAbs(2.0113333333333334, 1.0e-9));
}

TEST_CASE("Transport playhead exactly on loop_end wraps consistently", "[transport]") {
  // A playhead seeked exactly onto loop_end while looping must be reported as a
  // wrap at offset 0 by collect_loop_boundaries, matching advance()'s wrap, so
  // the sub-block splitter and the post-advance snapshot agree.
  sonare::transport::TempoMap map;
  map.prepare(48000.0);

  sonare::transport::Transport transport;
  transport.prepare(48000.0, &map);
  transport.set_loop(2.0, 4.0, true);  // loop_start=48000, loop_end=96000
  transport.play();
  transport.seek_ppq(4.0);  // sample_position == loop_end

  sonare::transport::BoundaryList boundaries;
  REQUIRE(transport.collect_loop_boundaries(512, &boundaries));
  REQUIRE(boundaries.size() == 1);
  REQUIRE(boundaries[0].offset == 0);
  REQUIRE(boundaries[0].timeline_sample == 96000);

  transport.advance(512);
  const auto state = transport.snapshot();
  REQUIRE(state.sample_position == 48512);  // loop_start + 512
}

TEST_CASE("Transport surfaces a loop-boundary overflow counter", "[transport]") {
  // A loop far shorter than the block can wrap more than BoundaryList::kCapacity
  // times in a single block. Those extra wraps are dropped; the drop must be
  // observable via loop_overflow_count() (and BoundaryList::overflowed())
  // instead of being silently swallowed.
  sonare::transport::TempoMap map;
  map.prepare(48000.0);
  map.set_segments({{0.0, 120.0, 0.0}});  // 1 ppq == 24000 samples

  sonare::transport::Transport transport;
  transport.prepare(48000.0, &map);
  REQUIRE(transport.loop_overflow_count() == 0u);

  // loop_len = 100 samples (end_ppq = 100/24000). A 4096-sample block wraps ~40
  // times, well beyond kCapacity (16).
  const double end_ppq = 100.0 / 24000.0;
  transport.set_loop(0.0, end_ppq, true);
  transport.play();

  sonare::transport::BoundaryList boundaries;
  REQUIRE(transport.collect_loop_boundaries(4096, &boundaries));
  REQUIRE(boundaries.size() == sonare::transport::BoundaryList::kCapacity);
  REQUIRE(boundaries.overflowed());
  REQUIRE(transport.loop_overflow_count() == 1u);

  // A subsequent overflow increments the counter again.
  boundaries.clear();
  REQUIRE(transport.collect_loop_boundaries(4096, &boundaries));
  REQUIRE(transport.loop_overflow_count() == 2u);

  // prepare() resets the diagnostic stamp.
  transport.prepare(48000.0, &map);
  REQUIRE(transport.loop_overflow_count() == 0u);
}
