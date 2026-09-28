#include "playback/speaker_stage.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "util/constants.h"

using sonare::ChannelLayout;
using sonare::SpeakerRole;
using namespace sonare::playback;

TEST_CASE("speaker trim applies its gain", "[playback][speakers]") {
  constexpr double kRate = 48000.0;
  constexpr int kFrames = 4800;
  SpeakerStage stage;
  std::array<SpeakerPrepare, kSpeakerRoleCount> speakers{};
  stage.prepare(kRate, kFrames, ChannelLayout::Stereo, speakers, BassManagementConfig{});
  std::array<float, kSpeakerRoleCount> trims{};
  trims[static_cast<size_t>(SpeakerRole::L)] = -6.0f;
  stage.set_levels(trims, 10.0f, 0.0f);

  std::vector<float> l(kFrames), r(kFrames);
  for (int i = 0; i < kFrames; ++i) {
    const float s = 0.5f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) /
                                    static_cast<float>(kRate));
    l[static_cast<size_t>(i)] = s;
    r[static_cast<size_t>(i)] = s;
  }
  float* planes[2] = {l.data(), r.data()};
  stage.process(planes, kFrames);

  double e_l = 0.0, e_r = 0.0;
  for (int i = 0; i < kFrames; ++i) {
    e_l += static_cast<double>(l[static_cast<size_t>(i)]) * l[static_cast<size_t>(i)];
    e_r += static_cast<double>(r[static_cast<size_t>(i)]) * r[static_cast<size_t>(i)];
  }
  REQUIRE(e_r > 0.0);
  CHECK(std::abs(10.0 * std::log10(e_l / e_r) - (-6.0)) <= 0.01);
}
