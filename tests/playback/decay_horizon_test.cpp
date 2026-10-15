#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "playback/layout_convert.h"
#include "playback/upmix.h"
#include "rt/tail_budget.h"

using sonare::ChannelLayout;
using namespace sonare::playback;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

/// Largest magnitude in @p planes from frame @p begin on.
double peak_from(const std::vector<std::vector<float>>& planes, int begin) {
  double peak = 0.0;
  for (const auto& plane : planes) {
    for (size_t i = static_cast<size_t>(begin); i < plane.size(); ++i) {
      peak = std::max(peak, std::abs(static_cast<double>(plane[i])));
    }
  }
  return peak;
}

}  // namespace

TEST_CASE("upmix decay_frames ends where a unit impulse's output stays below the tail floor",
          "[playback][upmix]") {
  Upmixer upmix;
  upmix.prepare(kRate, kBlock, ChannelLayout::FivePointOne);
  UpmixParams params;
  params.lfe_from_upmix = true;
  upmix.set_params(params, true);
  const int horizon = 2 * upmix.latency_samples() + upmix.decay_frames();
  const int total = horizon + static_cast<int>(kRate / 2);

  std::vector<float> left(static_cast<size_t>(total), 0.0f);
  std::vector<float> right(static_cast<size_t>(total), 0.0f);
  left[0] = 1.0f;
  right[0] = -0.5f;
  std::vector<std::vector<float>> out(6, std::vector<float>(static_cast<size_t>(total), 0.0f));
  std::array<float*, 6> ptrs{};
  for (int start = 0; start < total; start += kBlock) {
    const int count = std::min(kBlock, total - start);
    for (size_t ch = 0; ch < out.size(); ++ch) ptrs[ch] = out[ch].data() + start;
    upmix.process(left.data() + start, right.data() + start, ptrs.data(), count);
  }

  CHECK(peak_from(out, horizon) <= sonare::rt::kTailFloor);
}

TEST_CASE("layout fold-down decay_frames ends where a unit impulse stays below the tail floor",
          "[playback][layout]") {
  OutputBus bus;
  bus.kind = TargetKind::Speakers;
  bus.speaker_layout = ChannelLayout::Stereo;
  LayoutConverter converter;
  converter.prepare(kRate, kBlock, ChannelLayout::FivePointOne, bus);
  converter.set_lfe_mix_db(0.0f, true);
  const int horizon = converter.decay_frames();
  REQUIRE(horizon > 0);
  const int total = horizon + static_cast<int>(kRate / 2);

  std::vector<std::vector<float>> in(6, std::vector<float>(static_cast<size_t>(total), 0.0f));
  in[3][0] = 1.0f;  // LFE plane
  std::vector<std::vector<float>> out(2, std::vector<float>(static_cast<size_t>(total), 0.0f));
  std::array<const float*, 6> in_ptrs{};
  std::array<float*, 2> out_ptrs{};
  for (int start = 0; start < total; start += kBlock) {
    const int count = std::min(kBlock, total - start);
    for (size_t ch = 0; ch < in.size(); ++ch) in_ptrs[ch] = in[ch].data() + start;
    for (size_t ch = 0; ch < out.size(); ++ch) out_ptrs[ch] = out[ch].data() + start;
    converter.process(in_ptrs.data(), out_ptrs.data(), count);
  }

  CHECK(peak_from(out, horizon) <= sonare::rt::kTailFloor);
}
