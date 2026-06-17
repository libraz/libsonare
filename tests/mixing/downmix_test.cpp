/// @file downmix_test.cpp
/// @brief Analytic BS.775 downmix coefficient tests.

#include "mixing/downmix.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "util/exception.h"

using Catch::Matchers::WithinAbs;
using sonare::ChannelLayout;
using sonare::mixing::downmix;
using sonare::mixing::DownmixOptions;
using sonare::mixing::downmix_coeff::kMinus3dB;

TEST_CASE("downmix 5.1->stereo applies BS.775 coefficients", "[mixing][downmix]") {
  const float k = kMinus3dB;

  struct Case {
    std::array<float, 6> in;  // L R C LFE Ls Rs
    float lo;
    float ro;
  };
  const std::array<Case, 5> cases = {{
      {{1, 0, 0, 0, 0, 0}, 1.0f, 0.0f},  // L -> Lo only
      {{0, 1, 0, 0, 0, 0}, 0.0f, 1.0f},  // R -> Ro only
      {{0, 0, 1, 0, 0, 0}, k, k},        // C -> both at -3 dB
      {{0, 0, 0, 0, 1, 0}, k, 0.0f},     // Ls -> Lo at -3 dB
      {{0, 0, 0, 0, 0, 1}, 0.0f, k},     // Rs -> Ro at -3 dB
  }};

  for (const auto& c : cases) {
    std::array<float, 6> src = c.in;
    std::array<const float*, 6> in{};
    for (int i = 0; i < 6; ++i) in[i] = &src[static_cast<size_t>(i)];
    float lo = 0.0f;
    float ro = 0.0f;
    std::array<float*, 2> out = {&lo, &ro};
    downmix(ChannelLayout::FivePointOne, ChannelLayout::Stereo, in.data(), out.data(), 1);
    REQUIRE_THAT(lo, WithinAbs(c.lo, 1e-6f));
    REQUIRE_THAT(ro, WithinAbs(c.ro, 1e-6f));
  }
}

TEST_CASE("downmix drops LFE by default and folds it at -3 dB when requested",
          "[mixing][downmix]") {
  std::array<float, 6> src = {0, 0, 0, 1, 0, 0};  // LFE only
  std::array<const float*, 6> in{};
  for (int i = 0; i < 6; ++i) in[i] = &src[static_cast<size_t>(i)];
  float lo = 0.0f;
  float ro = 0.0f;
  std::array<float*, 2> out = {&lo, &ro};

  downmix(ChannelLayout::FivePointOne, ChannelLayout::Stereo, in.data(), out.data(), 1);
  REQUIRE_THAT(lo, WithinAbs(0.0f, 1e-6f));
  REQUIRE_THAT(ro, WithinAbs(0.0f, 1e-6f));

  DownmixOptions with_lfe;
  with_lfe.include_lfe = true;
  downmix(ChannelLayout::FivePointOne, ChannelLayout::Stereo, in.data(), out.data(), 1, with_lfe);
  REQUIRE_THAT(lo, WithinAbs(kMinus3dB, 1e-6f));
  REQUIRE_THAT(ro, WithinAbs(kMinus3dB, 1e-6f));
}

TEST_CASE("downmix 7.1->5.1 folds side+back into the surrounds", "[mixing][downmix]") {
  // 7.1 order: L R C LFE Lss Rss Ls Rs
  std::array<float, 8> src = {0.1f, 0.2f, 0.3f, 0.4f, 1.0f, 2.0f, 10.0f, 20.0f};
  std::array<const float*, 8> in{};
  for (int i = 0; i < 8; ++i) in[i] = &src[static_cast<size_t>(i)];
  std::array<float, 6> dst{};
  std::array<float*, 6> out{};
  for (int i = 0; i < 6; ++i) out[i] = &dst[static_cast<size_t>(i)];

  downmix(ChannelLayout::SevenPointOne, ChannelLayout::FivePointOne, in.data(), out.data(), 1);
  REQUIRE_THAT(dst[0], WithinAbs(0.1f, 1e-6f));   // L passthrough
  REQUIRE_THAT(dst[1], WithinAbs(0.2f, 1e-6f));   // R
  REQUIRE_THAT(dst[2], WithinAbs(0.3f, 1e-6f));   // C
  REQUIRE_THAT(dst[3], WithinAbs(0.4f, 1e-6f));   // LFE
  REQUIRE_THAT(dst[4], WithinAbs(11.0f, 1e-6f));  // Ls = Ls(10) + Lss(1)
  REQUIRE_THAT(dst[5], WithinAbs(22.0f, 1e-6f));  // Rs = Rs(20) + Rss(2)
}

TEST_CASE("downmix 7.1->stereo combines side and back surrounds", "[mixing][downmix]") {
  const float k = kMinus3dB;
  // Lss and Ls both feed Lo at -3 dB.
  std::array<float, 8> src = {0, 0, 0, 0, 1, 0, 1, 0};  // Lss=1, Ls=1
  std::array<const float*, 8> in{};
  for (int i = 0; i < 8; ++i) in[i] = &src[static_cast<size_t>(i)];
  float lo = 0.0f;
  float ro = 0.0f;
  std::array<float*, 2> out = {&lo, &ro};

  downmix(ChannelLayout::SevenPointOne, ChannelLayout::Stereo, in.data(), out.data(), 1);
  REQUIRE_THAT(lo, WithinAbs(2.0f * k, 1e-6f));
  REQUIRE_THAT(ro, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("downmix stereo->mono averages the pair", "[mixing][downmix]") {
  std::array<float, 2> a = {1.0f, 1.0f};
  std::array<float, 2> b = {1.0f, -1.0f};
  std::array<const float*, 2> ina = {&a[0], &a[1]};
  std::array<const float*, 2> inb = {&b[0], &b[1]};
  float m = 0.0f;
  std::array<float*, 1> out = {&m};

  downmix(ChannelLayout::Stereo, ChannelLayout::Mono, ina.data(), out.data(), 1);
  REQUIRE_THAT(m, WithinAbs(1.0f, 1e-6f));
  downmix(ChannelLayout::Stereo, ChannelLayout::Mono, inb.data(), out.data(), 1);
  REQUIRE_THAT(m, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("downmix 5.1->mono folds to stereo then averages", "[mixing][downmix]") {
  const float k = kMinus3dB;
  // C only -> Lo=Ro=k -> mono = k.
  std::array<float, 6> src = {0, 0, 1, 0, 0, 0};
  std::array<const float*, 6> in{};
  for (int i = 0; i < 6; ++i) in[i] = &src[static_cast<size_t>(i)];
  float m = 0.0f;
  std::array<float*, 1> out = {&m};

  downmix(ChannelLayout::FivePointOne, ChannelLayout::Mono, in.data(), out.data(), 1);
  REQUIRE_THAT(m, WithinAbs(k, 1e-6f));
}

TEST_CASE("downmix normalize keeps the worst-case sum at unity", "[mixing][downmix]") {
  // All-ones 5.1: Lo = 1 + k + k; normalized divides by (1 + 2k) -> 1.0.
  std::array<float, 6> src = {1, 1, 1, 1, 1, 1};
  std::array<const float*, 6> in{};
  for (int i = 0; i < 6; ++i) in[i] = &src[static_cast<size_t>(i)];
  float lo = 0.0f;
  float ro = 0.0f;
  std::array<float*, 2> out = {&lo, &ro};

  DownmixOptions norm;
  norm.normalize = true;
  downmix(ChannelLayout::FivePointOne, ChannelLayout::Stereo, in.data(), out.data(), 1, norm);
  REQUIRE_THAT(lo, WithinAbs(1.0f, 1e-6f));
  REQUIRE_THAT(ro, WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("downmix identity copies planes and rejects upmix", "[mixing][downmix]") {
  std::array<float, 2> src = {0.3f, -0.7f};
  std::array<float, 2> dst{};
  std::array<const float*, 2> in = {&src[0], &src[1]};
  std::array<float*, 2> out = {&dst[0], &dst[1]};
  downmix(ChannelLayout::Stereo, ChannelLayout::Stereo, in.data(), out.data(), 1);
  REQUIRE_THAT(dst[0], WithinAbs(0.3f, 1e-6f));
  REQUIRE_THAT(dst[1], WithinAbs(-0.7f, 1e-6f));

  // Upmix (stereo -> 5.1) is rejected.
  std::array<float, 6> wide{};
  std::array<float*, 6> wide_out{};
  for (int i = 0; i < 6; ++i) wide_out[i] = &wide[static_cast<size_t>(i)];
  REQUIRE_THROWS_AS(
      downmix(ChannelLayout::Stereo, ChannelLayout::FivePointOne, in.data(), wide_out.data(), 1),
      sonare::SonareException);
}
