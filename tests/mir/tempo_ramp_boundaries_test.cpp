#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "mir/tempo_estimator_bridge.h"
#include "transport/tempo_map.h"

namespace {
sonare::mir::BeatAnalysisInput piecewise_constant_beats(const std::vector<double>& bpms) {
  sonare::mir::BeatAnalysisInput input;
  input.bpm = static_cast<float>(bpms.front());
  input.sample_rate = 44100;
  input.hop_length = 512;
  double time = 0.0;
  input.beats.reserve(bpms.size() * 8 + 1);
  for (size_t interval = 0; interval < bpms.size() * 8; ++interval) {
    const size_t segment = interval / 8;
    sonare::Beat beat;
    beat.time = static_cast<float>(time);
    beat.frame = static_cast<int>(time * input.sample_rate / input.hop_length);
    beat.strength = 1.0f;
    input.beats.push_back(beat);
    time += 60.0 / bpms[segment];
  }
  sonare::Beat final_beat;
  final_beat.time = static_cast<float>(time);
  final_beat.frame = static_cast<int>(time * input.sample_rate / input.hop_length);
  final_beat.strength = 1.0f;
  input.beats.push_back(final_beat);
  return input;
}

void require_ramp_boundaries_match(const std::vector<double>& bpms, bool require_all_ramps = false,
                                   bool require_constant_fallback = false) {
  const auto input = piecewise_constant_beats(bpms);
  sonare::mir::TempoEstimatorConfig config;
  config.transition_weight = 0.0f;
  config.include_octave_candidates = false;
  const auto estimates = sonare::mir::estimate_tempo(input, config);
  REQUIRE(estimates.size() == 1);
  const auto& estimate = estimates.front();
  REQUIRE(estimate.segments.size() == bpms.size());

  sonare::transport::TempoMap map;
  map.prepare(input.sample_rate);
  map.set_segments(estimate.segments);
  for (size_t boundary = 0; boundary <= bpms.size(); ++boundary) {
    const size_t beat_index = boundary * 8;
    const double expected = input.beats[beat_index].time;
    CHECK(map.ppq_to_seconds(static_cast<double>(beat_index)) ==
          Catch::Approx(expected).margin(0.0001));
  }
  for (size_t i = 1; i < estimate.segments.size(); ++i) {
    const double end_bpm = estimate.segments[i - 1].end_bpm;
    if (require_all_ramps) REQUIRE(end_bpm > 0.0);
    if (end_bpm > 0.0) {
      CHECK(end_bpm == Catch::Approx(estimate.segments[i].bpm));
    }
  }
  if (require_constant_fallback) {
    size_t constant_fallbacks = 0;
    for (size_t i = 0; i + 1 < estimate.segments.size(); ++i) {
      if (estimate.segments[i].end_bpm == 0.0) ++constant_fallbacks;
    }
    CHECK(constant_fallbacks >= 1);
  }
}
}  // namespace

TEST_CASE("Multiple accelerating tempo ramps preserve every observed boundary",
          "[mir][tempo][ramp]") {
  require_ramp_boundaries_match({90.0, 120.0, 150.0}, true);
}

TEST_CASE("Multiple decelerating tempo ramps preserve every observed boundary",
          "[mir][tempo][ramp]") {
  require_ramp_boundaries_match({150.0, 120.0, 90.0}, true);
}

TEST_CASE("Extreme alternating ramps preserve every observed boundary", "[mir][tempo][ramp]") {
  require_ramp_boundaries_match({40.0, 240.0, 40.0});
  require_ramp_boundaries_match({60.0, 200.0, 60.0});
}

TEST_CASE("Long alternating ramps fall back to a constant segment when needed",
          "[mir][tempo][ramp]") {
  require_ramp_boundaries_match({40.0, 240.0, 40.0, 240.0, 40.0, 240.0, 40.0}, false, true);
}
