#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <random>
#include <vector>

#include "playback/night_mode_drc.h"

using sonare::ChannelLayout;
using namespace sonare::playback;

TEST_CASE("night-mode static curve matches its analytic values", "[playback][night]") {
  CHECK(std::abs(night_mode_curve_db(15.0f, 1.0f) - 6.0f) < 0.05f);
  CHECK(std::abs(night_mode_curve_db(-25.0f, 1.0f) - (-18.5f)) < 0.05f);
  CHECK(std::abs(night_mode_curve_db(-50.0f, 1.0f) - (-50.0f)) < 0.05f);
}

TEST_CASE("night mode at amount 0 leaves the level unchanged", "[playback][night]") {
  constexpr double kRate = 48000.0;
  constexpr int kBlock = 480;
  NightModeDrc drc;
  drc.prepare(kRate, kBlock, ChannelLayout::Stereo);
  drc.set_target_lufs(-24.0f);
  drc.set_amount(0.0f);

  std::mt19937 rng(7u);
  std::normal_distribution<float> dist(0.0f, 0.1f);
  double e_in = 0.0, e_out = 0.0;
  std::vector<float> l(kBlock), r(kBlock);
  float* planes[2] = {l.data(), r.data()};
  for (int block = 0; block < 200; ++block) {
    for (int i = 0; i < kBlock; ++i) {
      l[static_cast<size_t>(i)] = dist(rng);
      r[static_cast<size_t>(i)] = dist(rng);
      if (block >= 10) {
        e_in += static_cast<double>(l[static_cast<size_t>(i)]) * l[static_cast<size_t>(i)] +
                static_cast<double>(r[static_cast<size_t>(i)]) * r[static_cast<size_t>(i)];
      }
    }
    drc.process(planes, kBlock);
    if (block < 10) continue;
    for (int i = 0; i < kBlock; ++i) {
      e_out += static_cast<double>(l[static_cast<size_t>(i)]) * l[static_cast<size_t>(i)] +
               static_cast<double>(r[static_cast<size_t>(i)]) * r[static_cast<size_t>(i)];
    }
  }
  REQUIRE(e_out > 0.0);
  CHECK(std::abs(10.0 * std::log10(e_out / e_in)) <= 0.2);
}
