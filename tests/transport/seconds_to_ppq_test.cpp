#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "engine/realtime_engine.h"
#include "transport/tempo_map.h"

namespace {

std::vector<sonare::transport::TempoSegment> tempo_segments() {
  sonare::transport::TempoSegment ramp{};
  ramp.start_ppq = 4.0;
  ramp.bpm = 120.0;
  ramp.end_bpm = 90.0;

  sonare::transport::TempoSegment tail{};
  tail.start_ppq = 8.0;
  tail.bpm = 90.0;
  tail.end_bpm = 60.0;
  return {{0.0, 120.0, 0.0}, ramp, tail};
}

double tolerance(double value, double relative) {
  return std::max(relative * std::abs(value), relative);
}

}  // namespace

TEST_CASE("TempoMap converts PPQ and seconds across constant and ramp segments",
          "[transport][tempo]") {
  const std::vector<double> ppq_positions{0.0, 0.25, 3.5, 4.0, 4.25, 6.0, 7.999, 8.0, 8.5, 16.0};
  std::vector<double> reference_seconds;

  for (double sample_rate : {44100.0, 48000.0}) {
    sonare::transport::TempoMap map;
    map.prepare(sample_rate);
    map.set_segments(tempo_segments());

    std::vector<double> seconds_at_positions;
    for (double ppq : ppq_positions) {
      const double seconds = map.ppq_to_seconds(ppq);
      const double restored_ppq = map.seconds_to_ppq(seconds);
      REQUIRE(std::abs(restored_ppq - ppq) <= tolerance(ppq, 1.0e-9));
      const double restored_seconds = map.ppq_to_seconds(map.seconds_to_ppq(seconds));
      REQUIRE(std::abs(restored_seconds - seconds) <= tolerance(seconds, 1.0e-9));
      seconds_at_positions.push_back(seconds);
    }

    const std::vector<double> seconds_positions{0.0, 0.5, 1.9, 2.0, 2.1, 3.0, 4.2, 4.4, 5.0, 10.0};
    for (double seconds : seconds_positions) {
      const double ppq = map.seconds_to_ppq(seconds);
      const double restored_seconds = map.ppq_to_seconds(ppq);
      REQUIRE(std::abs(restored_seconds - seconds) <= tolerance(seconds, 1.0e-9));
    }

    if (reference_seconds.empty()) {
      reference_seconds = seconds_at_positions;
    } else {
      for (size_t i = 0; i < seconds_at_positions.size(); ++i) {
        REQUIRE(std::abs(seconds_at_positions[i] - reference_seconds[i]) <=
                tolerance(reference_seconds[i], 1.0e-12));
      }
    }

    sonare::engine::RealtimeEngine engine;
    engine.prepare(sample_rate, 64);
    engine.set_tempo_segments(tempo_segments());
    for (size_t i = 0; i < ppq_positions.size(); ++i) {
      const double engine_seconds =
          static_cast<double>(engine.sample_at_ppq(ppq_positions[i])) / sample_rate;
      REQUIRE(std::abs(engine_seconds - seconds_at_positions[i]) <= 1.0 / sample_rate);
    }
  }
}

TEST_CASE("TempoMap reports effective tempo at default, pickup, and ramp positions",
          "[transport][tempo]") {
  sonare::transport::TempoMap default_map;
  default_map.prepare(48000.0);
  REQUIRE(default_map.bpm_at_ppq(0.0) == 120.0);
  REQUIRE(default_map.bpm_at_ppq(12.0) == 120.0);
  REQUIRE(default_map.bpm_at_ppq(std::numeric_limits<double>::quiet_NaN()) == 120.0);

  sonare::transport::TempoMap pickup_map;
  pickup_map.prepare(48000.0);
  pickup_map.set_segments({{4.0, 90.0, 0.0}});
  REQUIRE(pickup_map.bpm_at_ppq(0.0) == 90.0);
  REQUIRE(pickup_map.bpm_at_ppq(3.999) == 90.0);

  sonare::transport::TempoMap ramp_map;
  ramp_map.prepare(48000.0);
  ramp_map.set_segments(tempo_segments());
  REQUIRE(ramp_map.bpm_at_ppq(4.0) == 120.0);
  REQUIRE(std::abs(ramp_map.bpm_at_ppq(6.0) - 105.0) <= 1.0e-12);
  REQUIRE(ramp_map.bpm_at_ppq(8.0) == 90.0);
  REQUIRE(ramp_map.bpm_at_ppq(16.0) == 90.0);
}
