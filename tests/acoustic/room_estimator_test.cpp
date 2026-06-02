#include "analysis/room_estimator.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "acoustic/late_reverb.h"
#include "acoustic/material.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "core/audio.h"

using Catch::Matchers::WithinRel;
using namespace sonare;
using sonare::acoustic::Material;
using sonare::acoustic::ReverbModel;
using sonare::acoustic::ReverbTime;
using sonare::acoustic::RirSynthResult;
using sonare::acoustic::shoebox_reverb_time;
using sonare::acoustic::ShoeboxRoom;
using sonare::acoustic::SourceListener;
using sonare::acoustic::synthesize_rir;
using sonare::acoustic::uniform_material;

namespace {

ShoeboxRoom uniform_room(float length, float width, float height, float absorption) {
  ShoeboxRoom room;
  room.dims = {length, width, height};
  for (Material& w : room.walls) w = uniform_material(absorption, 0.0f);
  return room;
}

// Deterministic uniform noise in [-amp, amp] (no <random>: cross-platform
// reproducible LCG, like the rest of the acoustic module's seeded paths).
std::vector<float> add_noise(const Audio& rir, float amp, uint64_t seed) {
  std::vector<float> out(rir.size());
  uint64_t s = seed;
  for (size_t i = 0; i < rir.size(); ++i) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const float u = static_cast<float>(s >> 40) / static_cast<float>(1u << 24);  // [0,1)
    out[i] = rir[i] + (2.0f * u - 1.0f) * amp;
  }
  return out;
}

}  // namespace

TEST_CASE("estimate_room recovers a known shoebox within tolerance", "[acoustic][room_estimator]") {
  const int sr = 48000;
  const float kLen = 7.0f, kWid = 5.0f, kHgt = 3.0f, kAlpha = 0.15f;
  const ShoeboxRoom room = uniform_room(kLen, kWid, kHgt, kAlpha);
  const SourceListener pl{{1.5f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};

  const RirSynthResult synth = synthesize_rir(room, pl, sr);
  REQUIRE_FALSE(has_error(synth.diagnostics));
  REQUIRE(synth.rir.size() > 0);

  // Estimate with priors matching the known room (shape + absorption), the same
  // statistical model the synthesis used (Eyring). This is the analysis<->
  // synthesis round trip: a known RIR must invert back to its room.
  RoomEstimateConfig cfg;
  cfg.aspect_hint_lw = kLen / kWid;
  cfg.aspect_hint_lh = kLen / kHgt;
  cfg.reference_absorption = kAlpha;
  cfg.prefer_eyring = true;

  const RoomEstimate est = estimate_room(synth.rir, cfg);

  const float true_volume = kLen * kWid * kHgt;  // 105 m^3
  // Volume scales as RT60^3 at fixed shape, so it inherits the analyzer's RT60
  // error amplified cubically; the plan's acceptance bound is +/-20%.
  REQUIRE_THAT(est.volume, WithinRel(true_volume, 0.20f));
  REQUIRE(est.dims.length > 0.0f);
  REQUIRE(est.dims.width > 0.0f);
  REQUIRE(est.dims.height > 0.0f);

  // Recovered shape follows the hinted aspect exactly (scale solved from RT60).
  REQUIRE_THAT(est.dims.length / est.dims.width, WithinRel(kLen / kWid, 1e-3f));
  REQUIRE_THAT(est.dims.length / est.dims.height, WithinRel(kLen / kHgt, 1e-3f));

  // Headline acceptance: per-band RT60 recovered within +/-10% of the design
  // reverberation time. Allow a couple of band-edge bands to leak (the bandpass
  // overlap and finite tail length blur the extremes); the bulk must hold.
  const ReverbTime design = shoebox_reverb_time(room, ReverbModel::Eyring);
  REQUIRE(est.rt60_bands.size() == design.rt60_bands.size());
  int rt_ok = 0;
  for (size_t b = 0; b < est.rt60_bands.size(); ++b) {
    if (design.rt60_bands[b] > 0.0f &&
        std::abs(est.rt60_bands[b] - design.rt60_bands[b]) <= 0.10f * design.rt60_bands[b]) {
      ++rt_ok;
    }
  }
  REQUIRE(rt_ok >= 4);

  // Per-band absorption recovers the uniform wall absorption.
  REQUIRE(est.absorption_bands.size() >= 4);
  int near_alpha = 0;
  for (float a : est.absorption_bands) {
    if (std::abs(a - kAlpha) < 0.06f) ++near_alpha;
  }
  REQUIRE(near_alpha >= 4);

  REQUIRE(est.confidence > 0.0f);
  REQUIRE(std::isfinite(est.drr_db));
}

TEST_CASE("estimate_room round-trips through the Sabine model", "[acoustic][room_estimator]") {
  // Mirror of the headline test but on the Sabine branch (prefer_eyring=false):
  // a lightly-absorptive room synthesized and inverted under the same model.
  const int sr = 48000;
  const float kLen = 6.0f, kWid = 4.5f, kHgt = 3.0f, kAlpha = 0.1f;
  const ShoeboxRoom room = uniform_room(kLen, kWid, kHgt, kAlpha);
  const SourceListener pl{{1.0f, 1.0f, 1.2f}, {4.0f, 3.0f, 1.7f}};

  sonare::acoustic::RirSynthConfig sc;
  sc.late_model = ReverbModel::Sabine;
  const RirSynthResult synth = synthesize_rir(room, pl, sr, sc);
  REQUIRE_FALSE(has_error(synth.diagnostics));

  RoomEstimateConfig cfg;
  cfg.aspect_hint_lw = kLen / kWid;
  cfg.aspect_hint_lh = kLen / kHgt;
  cfg.reference_absorption = kAlpha;
  cfg.prefer_eyring = false;

  const RoomEstimate est = estimate_room(synth.rir, cfg);
  REQUIRE_THAT(est.volume, WithinRel(kLen * kWid * kHgt, 0.20f));

  const ReverbTime design = shoebox_reverb_time(room, ReverbModel::Sabine);
  REQUIRE(est.rt60_bands.size() == design.rt60_bands.size());
  int rt_ok = 0;
  for (size_t b = 0; b < est.rt60_bands.size(); ++b) {
    if (design.rt60_bands[b] > 0.0f &&
        std::abs(est.rt60_bands[b] - design.rt60_bands[b]) <= 0.10f * design.rt60_bands[b]) {
      ++rt_ok;
    }
  }
  REQUIRE(rt_ok >= 4);
}

TEST_CASE("estimate_room DRR falls as the listener moves away", "[acoustic][room_estimator]") {
  // The direct-to-reverberant ratio must drop with source-listener distance:
  // the direct window loses energy while the diffuse tail is unchanged.
  const int sr = 48000;
  const ShoeboxRoom room = uniform_room(8.0f, 6.0f, 3.5f, 0.15f);
  const Audio near = synthesize_rir(room, {{2.0f, 3.0f, 1.5f}, {3.0f, 3.0f, 1.5f}}, sr).rir;
  const Audio far = synthesize_rir(room, {{2.0f, 3.0f, 1.5f}, {7.0f, 5.0f, 1.5f}}, sr).rir;

  const float drr_near = estimate_room(near).drr_db;
  const float drr_far = estimate_room(far).drr_db;
  REQUIRE(std::isfinite(drr_near));
  REQUIRE(std::isfinite(drr_far));
  REQUIRE(drr_near > drr_far);
}

TEST_CASE("estimate_room confidence drops on a noisy recording", "[acoustic][room_estimator]") {
  // Silence early-returns (confidence 0) and never exercises the confidence
  // machinery; a noisy RIR does. Burying the decay in a noise floor must lower
  // the honesty score relative to the clean RIR.
  const int sr = 48000;
  const ShoeboxRoom room = uniform_room(7.0f, 5.0f, 3.0f, 0.15f);
  const SourceListener pl{{1.5f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};
  const Audio clean = synthesize_rir(room, pl, sr).rir;

  float peak = 0.0f;
  for (size_t i = 0; i < clean.size(); ++i) peak = std::max(peak, std::abs(clean[i]));
  REQUIRE(peak > 0.0f);

  std::vector<float> noisy_samples = add_noise(clean, peak * 0.05f, 0x9e3779b97f4a7c15ULL);
  const Audio noisy = Audio::from_vector(std::move(noisy_samples), sr);

  const float c_clean = estimate_room(clean).confidence;
  const float c_noisy = estimate_room(noisy).confidence;
  REQUIRE(c_clean > 0.0f);
  REQUIRE(c_noisy < c_clean);
  REQUIRE(c_noisy < 1.0f);
}

TEST_CASE("estimate_room is deterministic", "[acoustic][room_estimator]") {
  const int sr = 48000;
  const ShoeboxRoom room = uniform_room(6.0f, 4.0f, 2.8f, 0.2f);
  const SourceListener pl{{1.0f, 1.0f, 1.2f}, {4.0f, 3.0f, 1.7f}};
  const RirSynthResult synth = synthesize_rir(room, pl, sr);

  const RoomEstimate a = estimate_room(synth.rir);
  const RoomEstimate b = estimate_room(synth.rir);
  REQUIRE(a.volume == b.volume);
  REQUIRE(a.drr_db == b.drr_db);
  REQUIRE(a.confidence == b.confidence);
  REQUIRE(a.absorption_bands == b.absorption_bands);
  REQUIRE(a.rt60_bands == b.rt60_bands);
}

TEST_CASE("estimate_room reports zero confidence for silence", "[acoustic][room_estimator]") {
  std::vector<float> silence(48000, 0.0f);
  const Audio audio = Audio::from_vector(std::move(silence), 48000);
  const RoomEstimate est = estimate_room(audio);
  REQUIRE(est.confidence == 0.0f);
  REQUIRE(est.volume == 0.0f);
}

TEST_CASE("estimate_room volume follows the absorption-prior scaling law",
          "[acoustic][room_estimator]") {
  // The volume scale is anchored by the absorption prior (the inverse problem
  // is rank-deficient by one). At fixed RT60/shape the length scales with
  // -ln(1-a0) (Eyring), so the volume scales with its cube. Pin that exact law,
  // not mere monotonicity.
  const int sr = 48000;
  const ShoeboxRoom room = uniform_room(7.0f, 5.0f, 3.0f, 0.15f);
  const SourceListener pl{{1.5f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};
  const Audio rir = synthesize_rir(room, pl, sr).rir;

  RoomEstimateConfig low;
  low.reference_absorption = 0.1f;
  RoomEstimateConfig high;
  high.reference_absorption = 0.3f;

  const RoomEstimate a = estimate_room(rir, low);
  const RoomEstimate b = estimate_room(rir, high);
  REQUIRE(a.volume > 0.0f);

  const double ratio = std::pow(std::log(1.0 - 0.3) / std::log(1.0 - 0.1), 3.0);
  REQUIRE_THAT(static_cast<double>(b.volume / a.volume), WithinRel(ratio, 1e-3));
}
