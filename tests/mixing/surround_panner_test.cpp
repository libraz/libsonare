/// @file surround_panner_test.cpp
/// @brief Constant-power surround panner gain + scatter tests.

#include "mixing/surround_panner.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "util/exception.h"

using Catch::Matchers::WithinAbs;
using sonare::ChannelLayout;
using sonare::mixing::compute_surround_pan_gains;
using sonare::mixing::SurroundPanGains;
using sonare::mixing::SurroundPannerProcessor;
using sonare::mixing::SurroundPanParams;

namespace {

// Sum of squares of every non-LFE plane (LFE is plane 3 for 5.1/7.1).
float non_lfe_power(const SurroundPanGains& g, int lfe) {
  float power = 0.0f;
  for (int p = 0; p < g.count; ++p) {
    if (p != lfe) power += g.gain[p] * g.gain[p];
  }
  return power;
}

}  // namespace

TEST_CASE("surround panner places point sources at exact speaker azimuths", "[mixing][surround]") {
  // 5.1 plane order: L(0) R(1) C(2) LFE(3) Ls(4) Rs(5).
  struct Case {
    float azimuth;
    int plane;
  };
  const std::array<Case, 5> cases = {{
      {0.0f, 2},     // front-center -> C
      {-30.0f, 0},   // front-left -> L
      {30.0f, 1},    // front-right -> R
      {-110.0f, 4},  // surround-left -> Ls
      {110.0f, 5},   // surround-right -> Rs
  }};

  for (const auto& c : cases) {
    SurroundPanParams p;
    p.azimuth = c.azimuth;
    const SurroundPanGains g = compute_surround_pan_gains(p, ChannelLayout::FivePointOne);
    REQUIRE(g.count == 6);
    CHECK_THAT(g.gain[c.plane], WithinAbs(1.0f, 1e-5f));
    for (int plane = 0; plane < 6; ++plane) {
      if (plane != c.plane) CHECK_THAT(g.gain[plane], WithinAbs(0.0f, 1e-5f));
    }
  }
}

TEST_CASE("surround panner is constant-power between adjacent speakers", "[mixing][surround]") {
  SurroundPanParams p;
  p.azimuth = 15.0f;  // halfway between C (0) and R (30)
  const SurroundPanGains g = compute_surround_pan_gains(p, ChannelLayout::FivePointOne);
  const float k = std::cos(0.25f * 3.14159265358979323846f);  // cos(pi/4) = sin(pi/4)
  CHECK_THAT(g.gain[2], WithinAbs(k, 1e-5f));                 // C
  CHECK_THAT(g.gain[1], WithinAbs(k, 1e-5f));                 // R
  CHECK_THAT(non_lfe_power(g, 3), WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("surround panner keeps unit power across the ring", "[mixing][surround]") {
  for (float a : {-180.0f, -150.0f, -90.0f, -30.0f, 0.0f, 45.0f, 90.0f, 150.0f, 180.0f}) {
    SurroundPanParams p;
    p.azimuth = a;
    const SurroundPanGains g = compute_surround_pan_gains(p, ChannelLayout::FivePointOne);
    CHECK_THAT(non_lfe_power(g, 3), WithinAbs(1.0f, 1e-5f));
  }
}

TEST_CASE("surround panner divergence spreads across the front, stays unit power",
          "[mixing][surround]") {
  SurroundPanParams p;
  p.azimuth = 0.0f;
  p.divergence = 1.0f;
  const SurroundPanGains g = compute_surround_pan_gains(p, ChannelLayout::FivePointOne);
  const float equal = 1.0f / std::sqrt(3.0f);
  CHECK_THAT(g.gain[0], WithinAbs(equal, 1e-5f));  // L
  CHECK_THAT(g.gain[1], WithinAbs(equal, 1e-5f));  // R
  CHECK_THAT(g.gain[2], WithinAbs(equal, 1e-5f));  // C
  CHECK_THAT(g.gain[4], WithinAbs(0.0f, 1e-5f));   // Ls
  CHECK_THAT(g.gain[5], WithinAbs(0.0f, 1e-5f));   // Rs
  CHECK_THAT(non_lfe_power(g, 3), WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("surround panner LFE is a raw scalar outside normalization", "[mixing][surround]") {
  SurroundPanParams p;
  p.azimuth = 0.0f;
  p.lfe = 0.5f;
  const SurroundPanGains g = compute_surround_pan_gains(p, ChannelLayout::FivePointOne);
  CHECK_THAT(g.gain[3], WithinAbs(0.5f, 1e-6f));            // LFE plane carries lfe verbatim
  CHECK_THAT(non_lfe_power(g, 3), WithinAbs(1.0f, 1e-5f));  // unchanged by LFE
}

TEST_CASE("surround panner handles the 7.1 ring", "[mixing][surround]") {
  // 7.1 plane order: L(0) R(1) C(2) LFE(3) Lss(4) Rss(5) Ls(6) Rs(7).
  SurroundPanParams p;
  p.azimuth = 90.0f;  // exactly at Rss
  const SurroundPanGains g = compute_surround_pan_gains(p, ChannelLayout::SevenPointOne);
  REQUIRE(g.count == 8);
  CHECK_THAT(g.gain[5], WithinAbs(1.0f, 1e-5f));  // Rss
  CHECK_THAT(non_lfe_power(g, 3), WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("surround panner rejects non-surround layouts", "[mixing][surround]") {
  SurroundPanParams p;
  CHECK_THROWS_AS(compute_surround_pan_gains(p, ChannelLayout::Stereo), sonare::SonareException);
  CHECK_THROWS_AS(compute_surround_pan_gains(p, ChannelLayout::Mono), sonare::SonareException);
}

TEST_CASE("surround panner scatters a mono lane additively", "[mixing][surround]") {
  SurroundPanParams p;
  p.azimuth = 0.0f;  // centre -> C plane (2)
  SurroundPannerProcessor panner(ChannelLayout::FivePointOne, p);
  panner.prepare(48000.0, 64);
  panner.reset();  // smoothers start at target so steady state is immediate

  const int n = 64;
  std::vector<float> mono(n, 1.0f);
  const float* mono_ptr = mono.data();
  std::array<std::vector<float>, 6> planes;
  std::array<float*, 6> out{};
  for (int c = 0; c < 6; ++c) {
    planes[c].assign(n, 0.0f);
    out[c] = planes[c].data();
  }

  panner.process_add(&mono_ptr, 1, out.data(), 6, n);

  CHECK_THAT(planes[2][0], WithinAbs(1.0f, 1e-4f));  // centre
  CHECK_THAT(planes[2][n - 1], WithinAbs(1.0f, 1e-4f));
  CHECK_THAT(planes[0][0], WithinAbs(0.0f, 1e-4f));  // L silent
  CHECK_THAT(planes[5][0], WithinAbs(0.0f, 1e-4f));  // Rs silent
}
