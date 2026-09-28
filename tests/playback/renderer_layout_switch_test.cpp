#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "playback/renderer.h"
#include "util/constants.h"

using sonare::ChannelLayout;
using namespace sonare::playback;

TEST_CASE("stereo to 5.1 switch leaves a seamless L plane", "[playback][layout-switch]") {
  constexpr int kRate = 48000;
  constexpr int kBlock = 480;
  constexpr int kBlocks = 400;
  constexpr int kSwitchBlock = 200;
  RendererConfig config;
  config.prepare.input_layout = InputLayout::Auto;
  config.prepare.target_kind = TargetKind::Speakers;
  config.prepare.target_layout = ChannelLayout::FivePointOne;
  config.realtime.upmix_enabled = false;
  config.realtime.night_amount = 0.0f;
  config.realtime.limiter_enabled = false;
  config.realtime.dialogue_level_db = 0.0f;
  PlaybackRenderer renderer(config, nullptr, kRate, kBlock);
  const int latency = renderer.latency_samples();
  REQUIRE(latency == 1312);

  const auto sine = [&](int n) {
    return 0.25f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(n) /
                            static_cast<float>(kRate));
  };
  std::vector<std::vector<float>> in(8, std::vector<float>(kBlock, 0.0f));
  std::vector<std::vector<float>> out(6, std::vector<float>(kBlock, 0.0f));
  const float* in_ptrs[8];
  float* out_ptrs[6];
  for (int ch = 0; ch < 8; ++ch) in_ptrs[ch] = in[static_cast<size_t>(ch)].data();
  for (int ch = 0; ch < 6; ++ch) out_ptrs[ch] = out[static_cast<size_t>(ch)].data();

  float max_error = 0.0f;
  for (int block = 0; block < kBlocks; ++block) {
    const int channels = block < kSwitchBlock ? 2 : 6;
    for (int i = 0; i < kBlock; ++i) in[0][static_cast<size_t>(i)] = sine(block * kBlock + i);
    REQUIRE(renderer.process_planar(in_ptrs, channels, out_ptrs, 6, kBlock));
    for (int i = 0; i < kBlock; ++i) {
      const int n = block * kBlock + i - latency;
      const float expected = n >= 0 ? sine(n) : 0.0f;
      max_error = std::max(max_error, std::abs(out[0][static_cast<size_t>(i)] - expected));
    }
  }
  CHECK(renderer.input_channel_count() == 6);
  CHECK(renderer.diagnostics().layout_switches == 1u);
  CHECK(max_error <= 1e-5f);
}
