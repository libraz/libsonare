#include "playback/upmix.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "util/constants.h"

using sonare::ChannelLayout;
using namespace sonare::playback;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;
constexpr int kSeconds = 2;

}  // namespace

TEST_CASE("upmix latency is the STFT length", "[playback][upmix]") {
  CHECK(upmix_latency_frames(48000.0) == 1024);
  CHECK(upmix_latency_frames(44100.0) == 1024);
  CHECK(upmix_latency_frames(96000.0) == 2048);
}

TEST_CASE("upmix sends an L=R sine to the centre", "[playback][upmix]") {
  Upmixer upmix;
  upmix.prepare(kRate, kBlock, ChannelLayout::FivePointOne);
  upmix.set_params(UpmixParams{}, true);

  const int total = static_cast<int>(kRate) * kSeconds;
  std::vector<float> sine(static_cast<size_t>(total));
  for (int i = 0; i < total; ++i) {
    sine[static_cast<size_t>(i)] =
        0.25f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) /
                         static_cast<float>(kRate));
  }
  std::vector<std::vector<float>> out(6, std::vector<float>(kBlock));
  float* out_ptrs[6];
  for (int ch = 0; ch < 6; ++ch) out_ptrs[ch] = out[static_cast<size_t>(ch)].data();

  double e_l = 0.0, e_r = 0.0, e_c = 0.0;
  const int settle = static_cast<int>(kRate / 2);
  for (int start = 0; start + kBlock <= total; start += kBlock) {
    const float* x = sine.data() + start;
    upmix.process(x, x, out_ptrs, kBlock);
    if (start < settle) continue;
    for (int i = 0; i < kBlock; ++i) {
      e_l += static_cast<double>(out[0][static_cast<size_t>(i)]) * out[0][static_cast<size_t>(i)];
      e_r += static_cast<double>(out[1][static_cast<size_t>(i)]) * out[1][static_cast<size_t>(i)];
      e_c += static_cast<double>(out[2][static_cast<size_t>(i)]) * out[2][static_cast<size_t>(i)];
    }
  }
  const double front = e_l + e_r + e_c;
  REQUIRE(front > 0.0);
  CHECK(e_c / front >= 0.95);
}
