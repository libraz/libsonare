#include "acoustic/room_modes.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "acoustic/image_source.h"
#include "acoustic/late_reverb.h"
#include "acoustic/material.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "filters/iir.h"
#include "util/constants.h"
#include "util/splitmix64.h"

using Catch::Matchers::WithinRel;
using namespace sonare;
using namespace sonare::acoustic;

namespace {

ShoeboxRoom uniform_room(float length, float width, float height, float absorption) {
  ShoeboxRoom room;
  room.dims = {length, width, height};
  for (Material& w : room.walls) w = uniform_material(absorption, 0.0f);
  return room;
}

double energy(const std::vector<float>& x) {
  double e = 0.0;
  for (float v : x) e += static_cast<double>(v) * v;
  return e;
}

// |DFT| of @p x at @p hz.
double magnitude_at(const std::vector<float>& x, double hz, int sr) {
  double re = 0.0;
  double im = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    const double phase = constants::kTwoPiD * hz * static_cast<double>(i) / sr;
    re += x[i] * std::cos(phase);
    im -= x[i] * std::sin(phase);
  }
  return std::hypot(re, im);
}

// Point uniformly inside @p d from a SplitMix64 stream.
Vec3 random_point(SplitMix64& rng, const RoomDimensions& d) {
  return {static_cast<float>(rng.uniform()) * d.length, static_cast<float>(rng.uniform()) * d.width,
          static_cast<float>(rng.uniform()) * d.height};
}

}  // namespace

TEST_CASE("Schroeder frequency is 2000 sqrt(T60 / V)", "[acoustic][room_modes]") {
  REQUIRE_THAT(schroeder_frequency_hz(0.5f, 50.0f), WithinRel(2000.0f * std::sqrt(0.01f), 1e-5f));
  REQUIRE(schroeder_frequency_hz(0.0f, 50.0f) == 0.0f);
  REQUIRE(schroeder_frequency_hz(0.5f, 0.0f) == 0.0f);
}

TEST_CASE("a small room's modes start at its axial fundamentals and stop at twice the crossover",
          "[acoustic][room_modes]") {
  const int sr = 48000;
  const ShoeboxRoom room = uniform_room(7.0f, 5.0f, 3.0f, 0.2f);
  const ReverbTime rt = shoebox_reverb_time(room);
  const ModalPlan plan = plan_room_modes(room, rt, sr, sr);
  REQUIRE_FALSE(plan.modes.empty());
  const float volume = shoebox_volume(room);
  REQUIRE_THAT(plan.crossover_hz,
               WithinRel(schroeder_frequency_hz(rt60_at_hz(rt, plan.crossover_hz), volume), 0.05f));
  REQUIRE_THAT(plan.modes.front().freq_hz, WithinRel(kSoundSpeed / (2.0f * 7.0f), 1e-4f));
  REQUIRE(plan.modes.front().nx == 1);
  for (size_t i = 1; i < plan.modes.size(); ++i) {
    REQUIRE(plan.modes[i - 1].freq_hz <= plan.modes[i].freq_hz);
  }
  REQUIRE(plan.modes.back().freq_hz < 2.0f * plan.crossover_hz);
  for (const RoomMode& mode : plan.modes) {
    REQUIRE_THAT(
        mode.decay_per_s,
        WithinRel(static_cast<float>(constants::kLn1000D) / rt60_at_hz(rt, mode.freq_hz), 1e-5f));
  }
}

TEST_CASE("a room whose crossover sits under its fundamental has no modal region",
          "[acoustic][room_modes]") {
  // A near-anechoic 40 m cube: Schroeder at ~3.1 Hz, under the 4.3 Hz fundamental.
  const ShoeboxRoom hall = uniform_room(40.0f, 40.0f, 40.0f, 0.999f);
  const ModalPlan plan = plan_room_modes(hall, shoebox_reverb_time(hall), 48000, 48000);
  REQUIRE(plan.modes.empty());
  REQUIRE(plan.crossover_hz == 0.0f);
}

TEST_CASE("the modal band carries the removed diffuse band's energy on average over positions",
          "[acoustic][room_modes]") {
  const int sr = 48000;
  const ShoeboxRoom room = uniform_room(6.0f, 4.5f, 2.8f, 0.25f);
  const ReverbTime rt = shoebox_reverb_time(room);
  const Audio tail = synthesize_late_tail(rt, sr);
  std::vector<float> late(tail.data(), tail.data() + tail.size());
  const ModalPlan plan = plan_room_modes(room, rt, sr, late.size());
  REQUIRE(plan.modes.size() > 20);
  const double removed = remove_modal_band(late, plan, sr);
  REQUIRE(removed > 0.0);
  const float level = modal_level(plan, removed, late.size(), sr);

  SplitMix64 rng(7u);
  constexpr int kPlacements = 96;
  double mean = 0.0;
  for (int p = 0; p < kPlacements; ++p) {
    const SourceListener placement{random_point(rng, room.dims), random_point(rng, room.dims)};
    std::vector<float> modal(late.size(), 0.0f);
    add_room_modes(modal, plan, room, placement, level, sr);
    mean += energy(modal) / kPlacements;
  }
  CAPTURE(removed, mean, plan.modes.size(), plan.crossover_hz);
  REQUIRE_THAT(mean, WithinRel(removed, 0.25));
}

TEST_CASE("a listener on a mode's pressure node does not hear that mode",
          "[acoustic][room_modes]") {
  // 7 x 5 x 3 m: the (1,0,0) mode at 24.5 Hz is 10 Hz clear of its nearest neighbour.
  const int sr = 8000;
  const ShoeboxRoom room = uniform_room(7.0f, 5.0f, 3.0f, 0.2f);
  const ReverbTime rt = shoebox_reverb_time(room);
  const ModalPlan plan = plan_room_modes(room, rt, sr, 2 * sr);
  REQUIRE_FALSE(plan.modes.empty());
  const Vec3 source{0.3f, 1.2f, 1.1f};
  const auto render = [&](float listener_x) {
    std::vector<float> modal(static_cast<size_t>(2 * sr), 0.0f);
    add_room_modes(modal, plan, room, {source, {listener_x, 2.1f, 1.4f}}, 1.0f, sr);
    return magnitude_at(modal, plan.modes.front().freq_hz, sr);
  };
  const double at_node = render(3.5f);
  const double near_wall = render(0.3f);
  CAPTURE(at_node, near_wall);
  REQUIRE(at_node < near_wall * 0.1);
}

TEST_CASE("remove_modal_band and the lowpass-weighted modes are complementary at the crossover",
          "[acoustic][room_modes]") {
  const int sr = 48000;
  const ShoeboxRoom room = uniform_room(5.0f, 4.0f, 2.6f, 0.3f);
  const ModalPlan plan = plan_room_modes(room, shoebox_reverb_time(room), sr, sr);
  REQUIRE_FALSE(plan.modes.empty());
  for (const double hz :
       {0.5 * plan.crossover_hz, 1.0 * plan.crossover_hz, 2.0 * plan.crossover_hz}) {
    const double low = butterworth_zero_phase_gain(hz, plan.crossover_hz, sr, 4, false);
    const double high = butterworth_zero_phase_gain(hz, plan.crossover_hz, sr, 4, true);
    CAPTURE(hz, low, high);
    REQUIRE_THAT(low + high, WithinRel(1.0, 1e-6));
  }
}

TEST_CASE("the synthesized RIR's bass decays at the room's design RT60", "[acoustic][room_modes]") {
  const int sr = 48000;
  const ShoeboxRoom room = uniform_room(7.0f, 5.0f, 3.0f, 0.15f);
  const SourceListener placement{{1.1f, 1.3f, 1.2f}, {5.2f, 3.6f, 1.7f}};
  const RirSynthResult res = synthesize_rir(room, placement, sr);
  REQUIRE_FALSE(has_error(res.diagnostics));
  std::vector<float> band(res.rir.data(), res.rir.data() + res.rir.size());
  butterworth_zero_phase(band, 45.0f, sr, 4, true);
  butterworth_zero_phase(band, 90.0f, sr, 4, false);
  // Schroeder backward integral, -5 to -25 dB.
  std::vector<double> edc(band.size());
  double acc = 0.0;
  for (size_t i = band.size(); i-- > 0;) {
    acc += static_cast<double>(band[i]) * band[i];
    edc[i] = acc;
  }
  REQUIRE(acc > 0.0);
  const auto time_at = [&](double db) {
    for (size_t i = 0; i < edc.size(); ++i) {
      if (10.0 * std::log10(edc[i] / acc) <= db) return static_cast<double>(i) / sr;
    }
    return static_cast<double>(edc.size()) / sr;
  };
  const double t20 = 3.0 * (time_at(-25.0) - time_at(-5.0));
  const float design = rt60_at_hz(shoebox_reverb_time(room, ReverbModel::Eyring, nullptr), 63.0f);
  CAPTURE(t20, design);
  REQUIRE_THAT(t20, WithinRel(static_cast<double>(design), 0.25));
}
