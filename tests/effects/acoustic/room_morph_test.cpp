/// @file room_morph_test.cpp
/// @brief Lifecycle regression for RoomMorph's bypassed source suppressor.

#include "effects/acoustic/room_morph.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "acoustic/material.h"
#include "acoustic/room_model.h"
#include "util/constants.h"

namespace {

using sonare::acoustic::Material;
using sonare::acoustic::ShoeboxRoom;
using sonare::acoustic::SourceListener;
using sonare::acoustic::uniform_material;
using sonare::effects::acoustic::RoomMorphConfig;
using sonare::effects::acoustic::RoomMorphProcessor;

constexpr int kPrepareBlock = 127;
constexpr int kGapSeconds = 1;
constexpr int kReentry = 9600;

ShoeboxRoom room() {
  ShoeboxRoom value;
  value.dims = {4.0f, 3.0f, 2.5f};
  for (Material& wall : value.walls) wall = uniform_material(0.5f, 0.0f);
  return value;
}

RoomMorphConfig config() {
  RoomMorphConfig value;
  value.target = room();
  value.placement = SourceListener{{1.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 1.0f}};
  value.wet = 0.0f;
  value.source_tail_suppression = 1.0f;
  value.max_seconds = 0.1f;
  value.ism_order = 3;
  value.seed = 1u;
  return value;
}

void process_blocks(RoomMorphProcessor& processor, float* samples, int count) {
  int offset = 0;
  for (const int requested : {1, 7, 31, 127}) {
    if (offset >= count) break;
    const int size = std::min(requested, count - offset);
    float* block = samples + offset;
    processor.process(&block, 1, size);
    offset += size;
  }
  while (offset < count) {
    const int size = std::min(kPrepareBlock, count - offset);
    float* block = samples + offset;
    processor.process(&block, 1, size);
    offset += size;
  }
}

void tone(float* samples, int count, int plateau_samples, int sample_rate, float amplitude,
          float tau_seconds) {
  const double plateau_seconds = static_cast<double>(plateau_samples) / sample_rate;
  for (int i = 0; i < count; ++i) {
    const double time = static_cast<double>(i) / static_cast<double>(sample_rate);
    const float envelope =
        i < plateau_samples ? 1.0f
                            : static_cast<float>(std::exp(-(time - plateau_seconds) / tau_seconds));
    samples[i] = amplitude * envelope *
                 static_cast<float>(std::cos(sonare::constants::kTwoPiD * 1000.0 * time));
  }
}

std::vector<float> run(double sample_rate, bool bypass_during_gap) {
  const int rate = static_cast<int>(sample_rate);
  RoomMorphProcessor processor(config(), sonare::acoustic::ReceiverLayout::Mono);
  processor.prepare(sample_rate, kPrepareBlock);
  const int warm_tone = rate;
  const int warm_tail = rate / 5;
  std::vector<float> warm(static_cast<std::size_t>(warm_tone + warm_tail), 0.0f);
  tone(warm.data(), static_cast<int>(warm.size()), warm_tone, rate, 0.5f, 0.03f);
  process_blocks(processor, warm.data(), static_cast<int>(warm.size()));
  if (bypass_during_gap) REQUIRE(processor.set_parameter(1, 0.0f));
  std::vector<float> gap(static_cast<std::size_t>(rate * kGapSeconds), 0.0f);
  process_blocks(processor, gap.data(), static_cast<int>(gap.size()));
  if (bypass_during_gap) REQUIRE(processor.set_parameter(1, 1.0f));
  std::vector<float> reentry(static_cast<std::size_t>(kReentry), 0.0f);
  tone(reentry.data(), static_cast<int>(reentry.size()), static_cast<int>(reentry.size()), rate,
       0.1f, 1000.0f);
  process_blocks(processor, reentry.data(), static_cast<int>(reentry.size()));
  return reentry;
}

float peak(const std::vector<float>& samples, int first) {
  float out = 0.0f;
  for (std::size_t i = static_cast<std::size_t>(first); i < samples.size(); ++i) {
    out = std::max(out, std::abs(samples[i]));
  }
  return out;
}

float difference(const std::vector<float>& lhs, const std::vector<float>& rhs, int first) {
  REQUIRE(lhs.size() == rhs.size());
  float out = 0.0f;
  for (std::size_t i = static_cast<std::size_t>(first); i < lhs.size(); ++i) {
    out = std::max(out, std::abs(lhs[i] - rhs[i]));
  }
  return out;
}

}  // namespace

TEST_CASE("room morph suppressor re-entry starts at unity after bypass", "[boundary]") {
  for (const double sample_rate : {48000.0, 96000.0}) {
    const std::vector<float> subject = run(sample_rate, true);
    const std::vector<float> continuous = run(sample_rate, false);
    CAPTURE(sample_rate);
    REQUIRE(peak(continuous, 256) > 0.05f);
    REQUIRE(difference(subject, continuous, 256) <= 2.0e-5f);
  }
}
