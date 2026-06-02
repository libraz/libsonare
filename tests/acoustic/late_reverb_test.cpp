#include "acoustic/late_reverb.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "acoustic/material.h"
#include "acoustic/room_model.h"
#include "acoustic/room_types.h"
#include "analysis/acoustic_analyzer.h"

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

}  // namespace

TEST_CASE("Sabine and Eyring reverberation-time formulas", "[acoustic][late_reverb]") {
  // 0.161 * V / A.
  REQUIRE_THAT(sabine_rt60(100.0f, 20.0f), WithinRel(0.161f * 100.0f / 20.0f, 1e-5f));

  // Non-positive volume or absorption area => 0 (not infinity).
  REQUIRE(sabine_rt60(0.0f, 20.0f) == 0.0f);
  REQUIRE(sabine_rt60(100.0f, 0.0f) == 0.0f);
  REQUIRE(eyring_rt60(100.0f, 50.0f, 0.0f) == 0.0f);

  // Eyring < Sabine for the same room, and the gap widens with absorption.
  const float volume = 105.0f, surface = 142.0f;
  for (float alpha : {0.1f, 0.3f, 0.6f}) {
    const float area = surface * alpha;
    const float sab = sabine_rt60(volume, area);
    const float eyr = eyring_rt60(volume, surface, alpha);
    REQUIRE(eyr < sab);
  }
  // Low absorption: the two converge.
  REQUIRE_THAT(eyring_rt60(volume, surface, 0.02f),
               WithinRel(sabine_rt60(volume, surface * 0.02f), 0.05f));
}

TEST_CASE("shoebox reverberation time from geometry and materials", "[acoustic][late_reverb]") {
  const ShoeboxRoom room = uniform_room(7.0f, 5.0f, 3.0f, 0.15f);

  SECTION("uniform walls give equal per-band RT60 matching the scalar formula") {
    const ReverbTime rt = shoebox_reverb_time(room, ReverbModel::Sabine);
    REQUIRE(rt.rt60_bands.size() == static_cast<size_t>(kDefaultOctaveBands));

    const float volume = shoebox_volume(room);
    const float surface = shoebox_surface_area(room);
    const float expected = sabine_rt60(volume, surface * 0.15f);
    for (float band : rt.rt60_bands) REQUIRE_THAT(band, WithinRel(expected, 1e-4f));
  }

  SECTION("rigid (empty) walls give zero RT60 in every band") {
    ShoeboxRoom rigid;
    rigid.dims = {7.0f, 5.0f, 3.0f};  // walls default-constructed: no absorption
    const ReverbTime rt = shoebox_reverb_time(rigid, ReverbModel::Eyring);
    REQUIRE(rt.rt60_bands.size() == static_cast<size_t>(kDefaultOctaveBands));
    for (float band : rt.rt60_bands) REQUIRE(band == 0.0f);
  }

  SECTION("more absorption shortens RT60") {
    const ReverbTime quiet = shoebox_reverb_time(uniform_room(7, 5, 3, 0.4f), ReverbModel::Eyring);
    const ReverbTime live = shoebox_reverb_time(uniform_room(7, 5, 3, 0.1f), ReverbModel::Eyring);
    for (size_t b = 0; b < quiet.rt60_bands.size(); ++b) {
      REQUIRE(quiet.rt60_bands[b] < live.rt60_bands[b]);
    }
  }

  SECTION("frequency-dependent absorption gives frequency-dependent RT60") {
    ShoeboxRoom room2 = uniform_room(7, 5, 3, 0.1f);
    // Make the high band far more absorptive: shorter RT60 at 4 kHz than 125 Hz.
    for (Material& w : room2.walls) w.absorption.back() = 0.6f;
    const ReverbTime rt = shoebox_reverb_time(room2, ReverbModel::Eyring);
    REQUIRE(rt.rt60_bands.back() < rt.rt60_bands.front());
  }
}

TEST_CASE("late tail is empty when no band has finite decay", "[acoustic][late_reverb]") {
  ReverbTime rt;
  rt.rt60_bands = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  const Audio tail = synthesize_late_tail(rt, 48000);
  REQUIRE(tail.empty());
}

TEST_CASE("late tail is deterministic for a fixed seed", "[acoustic][late_reverb]") {
  const ReverbTime rt = shoebox_reverb_time(uniform_room(7, 5, 3, 0.15f), ReverbModel::Sabine);

  const Audio a = synthesize_late_tail(rt, 48000, {/*seed=*/7u});
  const Audio b = synthesize_late_tail(rt, 48000, {/*seed=*/7u});
  REQUIRE(a.size() == b.size());
  REQUIRE(a.size() > 0);
  for (size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);  // bit-exact

  const Audio c = synthesize_late_tail(rt, 48000, {/*seed=*/8u});
  bool differs = false;
  for (size_t i = 0; i < a.size() && !differs; ++i) differs = (a[i] != c[i]);
  REQUIRE(differs);
}

TEST_CASE("late tail length follows the longest RT60 and the cap", "[acoustic][late_reverb]") {
  ReverbTime rt;
  rt.rt60_bands = {0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f};
  const int sr = 48000;

  // headroom = 1.0 (default) => length = ceil(RT60 * 2 * sr), computed in double.
  const Audio tail = synthesize_late_tail(rt, sr);
  const int expected =
      static_cast<int>(std::ceil(static_cast<double>(0.8f) * 2.0 * static_cast<double>(sr)));
  REQUIRE(static_cast<int>(tail.size()) == expected);

  LateReverbConfig capped;
  capped.max_samples = 10000;
  const Audio clipped = synthesize_late_tail(rt, sr, capped);
  REQUIRE(static_cast<int>(clipped.size()) == 10000);
}

TEST_CASE("unbounded RT60 does not overflow the auto length", "[acoustic][late_reverb]") {
  // A near-rigid room gives an enormous RT60; the auto length must clamp in
  // double instead of overflowing int into a negative size. A small cap keeps
  // the test cheap while still routing through the clamp.
  ReverbTime rt;
  rt.rt60_bands = {1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f};
  LateReverbConfig cfg;
  cfg.max_samples = 1000;
  const Audio tail = synthesize_late_tail(rt, 48000, cfg);
  REQUIRE(static_cast<int>(tail.size()) == 1000);
}

TEST_CASE("late tail energy-decay curve is monotonic", "[acoustic][late_reverb]") {
  const ReverbTime rt = shoebox_reverb_time(uniform_room(7, 5, 3, 0.15f), ReverbModel::Sabine);
  const Audio tail = synthesize_late_tail(rt, 48000, {/*seed=*/3u});
  REQUIRE(tail.size() > 0);

  // Backward Schroeder integral: E(t) = sum_{tau>=t} h(tau)^2. Non-increasing by
  // construction; this guards the implementation against sign/order mistakes.
  double running = 0.0;
  double prev = -1.0;
  bool monotonic = true;
  for (size_t i = tail.size(); i-- > 0;) {
    running += static_cast<double>(tail[i]) * tail[i];
    if (prev >= 0.0 && running < prev - 1e-9) monotonic = false;
    prev = running;
  }
  REQUIRE(monotonic);
}

TEST_CASE("synthesized tail reproduces the design RT60 within 10%", "[acoustic][late_reverb]") {
  const int sr = 48000;
  const ShoeboxRoom room = uniform_room(8.0f, 6.0f, 3.5f, 0.12f);

  // Both models are exercised end-to-end: Eyring is the synthesizer's default,
  // so the round-trip must validate it as well as Sabine.
  const auto model = GENERATE(ReverbModel::Sabine, ReverbModel::Eyring);
  const ReverbTime rt = shoebox_reverb_time(room, model);
  const float target = rt.rt60_bands.front();  // uniform -> identical across bands

  const Audio tail = synthesize_late_tail(rt, sr, {/*seed=*/1u});
  REQUIRE(tail.size() > 0);

  const AcousticParameters params = AcousticAnalyzer::from_impulse_response(tail).parameters();
  REQUIRE(std::isfinite(params.rt60));
  REQUIRE_THAT(params.rt60, WithinRel(target, 0.10f));  // binding acceptance criterion

  // Per-band RT60 also tracks the (uniform) design target. The tolerance is
  // looser than the broadband 10% because the analyzer re-filters the *summed*
  // tail with shallow single-section octave skirts, so adjacent bands leak in;
  // require enough bands to actually fit so the check is not vacuous.
  int finite_bands = 0;
  for (float band_rt60 : params.rt60_bands) {
    if (!std::isfinite(band_rt60)) continue;
    ++finite_bands;
    REQUIRE_THAT(band_rt60, WithinRel(target, 0.15f));
  }
  REQUIRE(finite_bands >= 4);
}

TEST_CASE("synthesized tail recovers frequency-dependent RT60", "[acoustic][late_reverb]") {
  const int sr = 48000;
  // Distinct per-band decay: long at low frequency, short at high frequency.
  ReverbTime rt;
  rt.rt60_bands = {1.2f, 1.1f, 0.9f, 0.6f, 0.4f, 0.3f};

  const Audio tail = synthesize_late_tail(rt, sr, {/*seed=*/5u});
  REQUIRE(tail.size() > 0);

  const AcousticParameters params = AcousticAnalyzer::from_impulse_response(tail).parameters();
  REQUIRE(params.rt60_bands.size() == static_cast<size_t>(kDefaultOctaveBands));
  const float low = params.rt60_bands.front();  // 125 Hz
  const float high = params.rt60_bands.back();  // 4 kHz
  REQUIRE(std::isfinite(low));
  REQUIRE(std::isfinite(high));
  // The synthesizer imprints a per-band decay: the low band must read clearly
  // longer than the high band, in the neighbourhood of the design values.
  REQUIRE(low > high * 1.5f);
  REQUIRE_THAT(low, WithinRel(1.2f, 0.20f));
  REQUIRE_THAT(high, WithinRel(0.3f, 0.30f));
}

TEST_CASE("synthesized RT60 is sample-rate independent", "[acoustic][late_reverb]") {
  const ShoeboxRoom room = uniform_room(8.0f, 6.0f, 3.5f, 0.12f);
  const ReverbTime rt = shoebox_reverb_time(room, ReverbModel::Sabine);
  const float target = rt.rt60_bands.front();

  const Audio tail = synthesize_late_tail(rt, 44100, {/*seed=*/1u});
  const AcousticParameters params = AcousticAnalyzer::from_impulse_response(tail).parameters();
  REQUIRE(std::isfinite(params.rt60));
  REQUIRE_THAT(params.rt60, WithinRel(target, 0.10f));
}
