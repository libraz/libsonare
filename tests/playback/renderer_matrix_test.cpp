#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <random>
#include <vector>

#include "playback/renderer.h"

using sonare::ChannelLayout;
using namespace sonare::playback;

namespace {

constexpr int kRate = 48000;
constexpr int kBlock = 512;

/// Seeded pink noise at -20 dBFS RMS (Kellet's refined filter).
std::vector<float> pink_noise(size_t frames, uint32_t seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> white(0.0f, 1.0f);
  float b[7] = {};
  std::vector<float> out(frames);
  double power = 0.0;
  for (float& s : out) {
    const float w = white(rng);
    b[0] = 0.99886f * b[0] + w * 0.0555179f;
    b[1] = 0.99332f * b[1] + w * 0.0750759f;
    b[2] = 0.96900f * b[2] + w * 0.1538520f;
    b[3] = 0.86650f * b[3] + w * 0.3104856f;
    b[4] = 0.55000f * b[4] + w * 0.5329522f;
    b[5] = -0.7616f * b[5] - w * 0.0168980f;
    s = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
    b[6] = w * 0.115926f;
    power += static_cast<double>(s) * s;
  }
  const double rms = std::sqrt(power / static_cast<double>(frames));
  const float gain = static_cast<float>(0.1 / rms);
  for (float& s : out) s *= gain;
  return out;
}

}  // namespace

TEST_CASE("mono to 5.1 speakers feeds the centre plane only", "[playback][matrix]") {
  RendererConfig config;
  config.prepare.input_layout = InputLayout::Mono;
  config.prepare.target_kind = TargetKind::Speakers;
  config.prepare.target_layout = ChannelLayout::FivePointOne;
  PlaybackRenderer renderer(config, nullptr, kRate, kBlock);
  REQUIRE(renderer.output_channel_count() == 6);

  const size_t total = static_cast<size_t>(kRate) * 2;
  const std::vector<float> input = pink_noise(total, 5u);
  std::vector<std::vector<float>> out(6, std::vector<float>(kBlock));
  float* out_ptrs[6];
  for (int ch = 0; ch < 6; ++ch) out_ptrs[ch] = out[static_cast<size_t>(ch)].data();
  double energy[6] = {};
  bool finite = true;
  for (size_t start = 0; start + kBlock <= total; start += kBlock) {
    const float* in_ptrs[1] = {input.data() + start};
    REQUIRE(renderer.process_planar(in_ptrs, 1, out_ptrs, 6, kBlock));
    for (int ch = 0; ch < 6; ++ch) {
      for (float s : out[static_cast<size_t>(ch)]) {
        finite = finite && std::isfinite(s);
        energy[ch] += static_cast<double>(s) * s;
      }
    }
  }
  CHECK(finite);
  const double c_rms_db = 10.0 * std::log10(energy[2] / static_cast<double>(total) + 1e-30);
  CHECK(c_rms_db > -60.0);
  for (int ch : {0, 1, 3, 4, 5}) CHECK(energy[ch] == 0.0);
}
