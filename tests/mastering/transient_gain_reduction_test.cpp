/// @file transient_gain_reduction_test.cpp
/// @brief Gain reduction reported by the transient shaper and the vocal rider.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/dynamics/vocal_rider.h"
#include "util/constants.h"

namespace {

using sonare::mastering::api::MasteringChain;
using sonare::mastering::api::MasteringChainConfig;
using sonare::mastering::dynamics::TransientShaper;
using sonare::mastering::dynamics::TransientShaperConfig;
using sonare::mastering::dynamics::VocalRider;
using sonare::mastering::dynamics::VocalRiderConfig;

constexpr int kRate = 44100;

// Decaying 200 Hz bursts every 0.1 s: sharp onsets followed by a tail.
std::vector<float> drum_signal(std::size_t length) {
  std::vector<float> x(length);
  const std::size_t period = kRate / 10;
  for (std::size_t i = 0; i < length; ++i) {
    const float t = static_cast<float>(i % period) / static_cast<float>(kRate);
    x[i] = 0.9f * std::exp(-t * 40.0f) *
           std::sin(sonare::constants::kTwoPi * 200.0f * static_cast<float>(i) / kRate);
  }
  return x;
}

float run_shaper(TransientShaper& s, std::vector<float>& x) {
  float* ch[1] = {x.data()};
  s.process(ch, 1, static_cast<int>(x.size()));
  return s.last_gain_reduction_db();
}

float run_rider(VocalRider& r, std::vector<float>& x) {
  float* ch[1] = {x.data()};
  r.process(ch, 1, static_cast<int>(x.size()));
  return r.last_gain_reduction_db();
}

TransientShaper make_shaper(float attack_db, float sustain_db) {
  TransientShaperConfig cfg;
  cfg.attack_gain_db = attack_db;
  cfg.sustain_gain_db = sustain_db;
  TransientShaper s(cfg);
  s.prepare(kRate, 8192);
  return s;
}

VocalRider make_rider(float target_db, bool linked) {
  VocalRiderConfig cfg;
  cfg.target_db = target_db;
  cfg.linked_detection = linked;
  cfg.attack_ms = 1.0f;
  cfg.release_ms = 10.0f;
  cfg.gain_smoothing_ms = 5.0f;
  VocalRider r(cfg);
  r.prepare(kRate, 8192);
  return r;
}

}  // namespace

TEST_CASE("TransientShaper reports gain reduction", "[mastering][dynamics][transient_gr]") {
  auto cut = make_shaper(-8.0f, 0.0f);
  auto x = drum_signal(8192);
  CHECK(run_shaper(cut, x) < 0.0f);
  CHECK(cut.minimum_gain_reduction_db() <= cut.last_gain_reduction_db());

  // A boosting attack with a cutting sustain still reduces during the tail.
  auto mixed = make_shaper(6.0f, -3.0f);
  x = drum_signal(8192);
  CHECK(run_shaper(mixed, x) < 0.0f);

  // Boost only: no sample is ever reduced.
  auto boost = make_shaper(6.0f, 0.0f);
  x = drum_signal(8192);
  CHECK(run_shaper(boost, x) == 0.0f);
  CHECK(boost.minimum_gain_reduction_db() == 0.0f);

  cut.reset();
  CHECK(cut.last_gain_reduction_db() == 0.0f);
  CHECK(cut.minimum_gain_reduction_db() == 0.0f);
}

TEST_CASE("VocalRider reports gain reduction", "[mastering][dynamics][transient_gr]") {
  for (const bool linked : {true, false}) {
    CAPTURE(linked);
    // Signal peaks near -1 dBFS; a -30 dB target forces a cut.
    auto cut = make_rider(-30.0f, linked);
    auto x = drum_signal(8192);
    CHECK(run_rider(cut, x) < 0.0f);
    CHECK(cut.minimum_gain_reduction_db() <= cut.last_gain_reduction_db());

    // A 0 dB target sits above the signal level: only boosts.
    auto boost = make_rider(0.0f, linked);
    x = drum_signal(8192);
    CHECK(run_rider(boost, x) == 0.0f);
    CHECK(boost.minimum_gain_reduction_db() == 0.0f);

    cut.reset();
    CHECK(cut.last_gain_reduction_db() == 0.0f);
    CHECK(cut.minimum_gain_reduction_db() == 0.0f);
  }
}

TEST_CASE("Mastering chain reports the transient shaper gain reduction",
          "[mastering][dynamics][transient_gr]") {
  MasteringChainConfig config;
  config.dynamics.transient_shaper.enabled = true;
  config.dynamics.transient_shaper.config.attack_gain_db = -8.0f;
  config.dynamics.transient_shaper.config.sustain_gain_db = 0.0f;
  const auto x = drum_signal(kRate);

  const auto result = MasteringChain(config).process_mono(x.data(), x.size(), kRate);
  bool found = false;
  for (const auto& entry : result.stage_gain_reductions) {
    if (entry.stage == "dynamics.transientShaper") {
      found = true;
      CHECK(entry.gain_reduction_db < 0.0f);
    }
  }
  CHECK(found);
}
