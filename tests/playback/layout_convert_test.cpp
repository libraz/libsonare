#include "playback/layout_convert.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <random>
#include <vector>

#include "mixing/downmix.h"
#include "util/exception.h"

using sonare::ChannelLayout;
using sonare::SpeakerRole;
using namespace sonare::playback;

namespace {

constexpr double kRate = 48000.0;
constexpr int kFrames = 4096;

std::vector<std::vector<float>> noise_planes(int channels, int silent_plane, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> dist(-0.1f, 0.1f);
  std::vector<std::vector<float>> planes(static_cast<size_t>(channels),
                                         std::vector<float>(kFrames, 0.0f));
  for (int ch = 0; ch < channels; ++ch) {
    if (ch == silent_plane) continue;
    for (float& s : planes[static_cast<size_t>(ch)]) s = dist(rng);
  }
  return planes;
}

template <typename Planes>
std::vector<const float*> const_ptrs(const Planes& planes) {
  std::vector<const float*> out;
  for (const auto& p : planes) out.push_back(p.data());
  return out;
}

template <typename Planes>
std::vector<float*> ptrs(Planes& planes) {
  std::vector<float*> out;
  for (auto& p : planes) out.push_back(p.data());
  return out;
}

double energy(const std::vector<float>& plane) {
  double e = 0.0;
  for (float s : plane) e += static_cast<double>(s) * s;
  return e;
}

}  // namespace

TEST_CASE("5.1 to stereo speakers matches mixing::downmix with LFE silent", "[playback][layout]") {
  auto in = noise_planes(6, 3, 11u);
  const auto in_ptrs = const_ptrs(in);

  std::vector<std::vector<float>> ref(2, std::vector<float>(kFrames, 0.0f));
  auto ref_ptrs = ptrs(ref);
  sonare::mixing::DownmixOptions options;
  options.include_lfe = false;
  sonare::mixing::downmix(ChannelLayout::FivePointOne, ChannelLayout::Stereo, in_ptrs.data(),
                          ref_ptrs.data(), kFrames, options);
  REQUIRE(energy(ref[0]) > 0.0);

  OutputBus bus;
  bus.kind = TargetKind::Speakers;
  bus.speaker_layout = ChannelLayout::Stereo;
  LayoutConverter converter;
  converter.prepare(kRate, kFrames, ChannelLayout::FivePointOne, bus);
  std::vector<std::vector<float>> out(2, std::vector<float>(kFrames, 0.0f));
  auto out_ptrs = ptrs(out);
  converter.process(in_ptrs.data(), out_ptrs.data(), kFrames);

  for (int ch = 0; ch < 2; ++ch) {
    float max_diff = 0.0f;
    for (int i = 0; i < kFrames; ++i) {
      max_diff = std::max(max_diff, std::abs(out[static_cast<size_t>(ch)][static_cast<size_t>(i)] -
                                             ref[static_cast<size_t>(ch)][static_cast<size_t>(i)]));
    }
    CHECK(max_diff == 0.0f);
  }
}

TEST_CASE("mono to 5.1 speakers feeds only the centre plane", "[playback][layout]") {
  auto in = noise_planes(1, -1, 12u);
  const auto in_ptrs = const_ptrs(in);
  OutputBus bus;
  bus.kind = TargetKind::Speakers;
  bus.speaker_layout = ChannelLayout::FivePointOne;
  LayoutConverter converter;
  converter.prepare(kRate, kFrames, ChannelLayout::Mono, bus);
  std::vector<std::vector<float>> out(6, std::vector<float>(kFrames, 0.0f));
  auto out_ptrs = ptrs(out);
  converter.process(in_ptrs.data(), out_ptrs.data(), kFrames);

  REQUIRE(energy(out[2]) > 0.0);
  for (int ch : {0, 1, 3, 4, 5}) CHECK(energy(out[static_cast<size_t>(ch)]) == 0.0);
}

TEST_CASE("channel map validation rejects a duplicated role", "[playback][layout]") {
  const std::array<SpeakerRole, 6> map = {SpeakerRole::C,  SpeakerRole::L,  SpeakerRole::R,
                                          SpeakerRole::Ls, SpeakerRole::Ls, SpeakerRole::LFE};
  REQUIRE_THROWS_AS(validate_channel_map(ChannelLayout::FivePointOne, map.data(), 6),
                    sonare::SonareException);
}
