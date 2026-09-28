#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "playback/renderer.h"
#include "support/alloc_guard.h"
#include "util/constants.h"

using namespace sonare::playback;

TEST_CASE("processing and input-layout switches do not allocate", "[playback][no-alloc]") {
  constexpr int kRate = 48000;
  constexpr int kBlock = 480;
  // Each run is longer than any front end's drain, so every switch in between
  // passes through a completed drain and a dormant reset.
  constexpr int kBlocksPerRun = 60;
  RendererConfig config;
  config.prepare.input_layout = InputLayout::Auto;
  PlaybackRenderer renderer(config, nullptr, kRate, kBlock);

  std::vector<std::vector<float>> in(8, std::vector<float>(kBlock, 0.0f));
  std::vector<std::vector<float>> out(2, std::vector<float>(kBlock, 0.0f));
  const float* in_ptrs[8];
  float* out_ptrs[2];
  for (int ch = 0; ch < 8; ++ch) in_ptrs[ch] = in[static_cast<size_t>(ch)].data();
  for (int ch = 0; ch < 2; ++ch) out_ptrs[ch] = out[static_cast<size_t>(ch)].data();

  const int sequence[] = {1, 2, 6, 8, 2};
  double energy = 0.0;
  size_t allocations = 0;
  int n = 0;
  {
    sonare::test::AllocationGuard guard;
    for (int channels : sequence) {
      for (int block = 0; block < kBlocksPerRun; ++block) {
        for (int ch = 0; ch < channels; ++ch) {
          for (int i = 0; i < kBlock; ++i) {
            in[static_cast<size_t>(ch)][static_cast<size_t>(i)] =
                0.25f * std::sin(sonare::constants::kTwoPi * 440.0f * static_cast<float>(n + i) /
                                 static_cast<float>(kRate));
          }
        }
        n += kBlock;
        renderer.set_head_orientation(static_cast<float>(block), 0.0f, 0.0f);
        renderer.process_planar(in_ptrs, channels, out_ptrs, 2, kBlock);
        for (int i = 0; i < kBlock; ++i) {
          energy +=
              static_cast<double>(out[0][static_cast<size_t>(i)]) * out[0][static_cast<size_t>(i)];
        }
      }
    }
    // A switch back before the previous drain has finished (a truncated drain).
    renderer.process_planar(in_ptrs, 6, out_ptrs, 2, kBlock);
    renderer.process_planar(in_ptrs, 2, out_ptrs, 2, kBlock);
    allocations = guard.count();
  }
  // Non-vacuity: the guarded path must actually have rendered sound.
  REQUIRE(energy > 0.0);
  CHECK(allocations == 0u);
}
