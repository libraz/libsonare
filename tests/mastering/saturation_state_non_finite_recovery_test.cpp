/// @file saturation_state_non_finite_recovery_test.cpp
/// @brief One non-finite sample must not outlive the block that carried it, at
///        the two saturation owners whose recursive cells nothing returns to
///        rest: the bit crusher's noise-shaping error history and the
///        transformer's magnetic hysteresis.
///
/// Neither owner is reached by a guard elsewhere. The bit crusher's history is
/// written only on the noise-shaped dither branch, which is not the default but
/// is a published chain parameter, and the transformer holds the same hysteresis
/// state the tape machine already returns to rest.
///
/// The claim is read against the INPUT rather than against a clean run, because
/// neither cell converges back to a clean run's trajectory: both sit inside a
/// feedback loop whose state after a discard is rest, not wherever the clean run
/// had reached. What a caller can hold either owner to is that the output tracks
/// the input again, to within what the owner does to a clean stream.
///
/// The bound is therefore taken FROM the clean run instead of written down, so
/// it cannot be set loose enough to pass a degraded stream by accident.
///
/// Unguarded, both owners fail on finiteness rather than on that bound, and at
/// the bit crusher that is not obvious: the quantizer's own clamp holds the
/// output finite, so the first sample looks survivable. What carries the defect
/// past it is the shaping filter, whose coefficients alternate in sign — once a
/// second tap holds an infinity, the sum is inf - inf and every later sample is
/// NaN. The deviation bound is what would catch a substituting owner, and it is
/// asserted here for the case where an owner acquires that shape later.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include "mastering/final/dither.h"
#include "mastering/saturation/bitcrusher.h"
#include "mastering/saturation/transformer.h"
#include "util/constants.h"

namespace {

using sonare::constants::kPiD;

constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 512;
constexpr int kPoisonBlock = 1;
constexpr int kPoisonIndex = 100;
// Neither owner oversamples and neither holds a delay line, so the rule running
// at the end of a block leaves nothing for the next one to read. The first block
// past the carrying block is where recovery is read.
constexpr int kRecoveryBlock = kPoisonBlock + 1;
constexpr int kBlockCount = 60;
// How far past the clean run's own worst deviation a recovered stream may sit.
// The recovered run re-enters its loop from rest while the control is
// mid-trajectory, so the two are not required to coincide -- only to stay on the
// same scale. Both owners fail on finiteness before this is read, so the margin
// decides neither case today.
constexpr double kDeviationMargin = 4.0;

/// Fixture well inside full scale, so the bit crusher's only effect on it is
/// quantization and the transformer's is its own curve -- not a clip.
std::vector<float> stream_block(int block_index) {
  std::vector<float> out(static_cast<size_t>(kBlockSize));
  for (int i = 0; i < kBlockSize; ++i) {
    const double t = static_cast<double>(block_index * kBlockSize + i) / kSampleRate;
    out[static_cast<size_t>(i)] = static_cast<float>(0.35 * std::sin(2.0 * kPiD * 110.0 * t) +
                                                     0.20 * std::sin(2.0 * kPiD * 1970.0 * t));
  }
  return out;
}

using Blocks = std::vector<std::vector<float>>;
using BlockProcessor = std::function<void(std::vector<float>&)>;

std::array<float, 3> poison_values() {
  return {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()};
}

Blocks run_stream(const BlockProcessor& process, float poison_value, bool poison) {
  Blocks outputs;
  outputs.reserve(static_cast<size_t>(kBlockCount));
  for (int k = 0; k < kBlockCount; ++k) {
    std::vector<float> block = stream_block(k);
    if (poison && k == kPoisonBlock) {
      block[static_cast<size_t>(kPoisonIndex)] = poison_value;
    }
    process(block);
    outputs.push_back(std::move(block));
  }
  return outputs;
}

/// Largest distance between the output and the clean source over the blocks from
/// @p from onward, or infinity when any distance is non-finite.
/// @note The non-finite return is load-bearing. std::max returns its first
///       argument when the second is non-finite, so folding a NaN distance into
///       a running maximum skips it and a wholly degraded run reads as a small
///       deviation -- passing the bound on the worst possible result.
double deviation_from_input(const Blocks& out, int from) {
  double worst = 0.0;
  for (int k = from; k < kBlockCount; ++k) {
    const std::vector<float> source = stream_block(k);
    for (int i = 0; i < kBlockSize; ++i) {
      const double distance =
          std::abs(static_cast<double>(out[static_cast<size_t>(k)][static_cast<size_t>(i)]) -
                   static_cast<double>(source[static_cast<size_t>(i)]));
      if (!std::isfinite(distance)) {
        return std::numeric_limits<double>::infinity();
      }
      worst = std::max(worst, distance);
    }
  }
  return worst;
}

bool block_has_non_finite(const std::vector<float>& block) {
  return std::any_of(block.begin(), block.end(), [](float v) { return !std::isfinite(v); });
}

/// Drives one owner twice -- clean and poisoned -- and asserts the invariant.
void check_owner(const std::function<BlockProcessor()>& make, float poison_value) {
  const Blocks control = run_stream(make(), 0.0f, false);

  // Non-vacuity, read before any recovery result. The first says the owner is
  // not a passthrough; the second says it is still doing something where the
  // recovery bound is read, which is what a stream that goes quiet late would
  // otherwise pass on.
  const double control_deviation = deviation_from_input(control, 0);
  const double control_deviation_late = deviation_from_input(control, kRecoveryBlock);
  INFO("control deviation " << control_deviation << ", from the recovery block "
                            << control_deviation_late);
  REQUIRE(control_deviation > 0.0);
  REQUIRE(control_deviation_late > 0.0);
  REQUIRE_FALSE(std::any_of(control.begin(), control.end(), block_has_non_finite));

  const Blocks poisoned = run_stream(make(), poison_value, true);

  // The carrying block may come out stained; no later block may.
  int last_non_finite = -1;
  for (int k = 0; k < kBlockCount; ++k) {
    if (block_has_non_finite(poisoned[static_cast<size_t>(k)])) {
      last_non_finite = k;
    }
  }
  INFO("last non-finite block " << last_non_finite);
  REQUIRE(last_non_finite <= kPoisonBlock);

  // Finiteness alone is not recovery: an owner whose feedback path substitutes
  // an in-domain value for a non-finite one stays finite and stops tracking its
  // input. The bound is the clean run's own worst deviation.
  const double recovered = deviation_from_input(poisoned, kRecoveryBlock);
  INFO("recovered deviation " << recovered << " against bound "
                              << control_deviation * kDeviationMargin);
  REQUIRE(recovered <= control_deviation * kDeviationMargin);
}

}  // namespace

TEST_CASE("the bit crusher's noise-shaping history is returned to rest",
          "[mastering][saturation]") {
  using sonare::mastering::saturation::BitCrusher;
  using sonare::mastering::saturation::BitCrusherConfig;

  // Noise shaping is the only branch that writes the error history. The depth is
  // chosen high so the clean run's own deviation stays near the quantization
  // step, which is what makes the deviation bound worth reading at all.
  BitCrusherConfig config;
  config.bit_depth = 16;
  config.downsample_factor = 1;
  config.mix = 1.0f;
  config.dither_type = sonare::mastering::final::DitherType::NoiseShaped;

  const auto make = [config]() {
    auto processor = std::make_shared<BitCrusher>(config);
    processor->prepare(kSampleRate, kBlockSize);
    return BlockProcessor([processor](std::vector<float>& block) {
      float* channels[] = {block.data()};
      processor->process(channels, 1, static_cast<int>(block.size()));
    });
  };

  // A section per poison value, so an assertion that stops the case cannot hide
  // the other two. The three reach the history by different routes -- a NaN
  // arrives as one, an infinity becomes one inside the shaping sum -- and only
  // running all three says so.
  for (const float poison_value : poison_values()) {
    DYNAMIC_SECTION("poison " << poison_value) { check_owner(make, poison_value); }
  }
}

TEST_CASE("the transformer's magnetic hysteresis is returned to rest", "[mastering][saturation]") {
  using sonare::mastering::saturation::Transformer;
  using sonare::mastering::saturation::TransformerConfig;

  // Enough drive that the core is on the curved part of its loop, so the clean
  // run's deviation is the hysteresis rather than a near-linear pass.
  TransformerConfig config;
  config.drive_db = 12.0f;
  config.asymmetry = 0.3f;
  config.mix = 1.0f;

  const auto make = [config]() {
    auto processor = std::make_shared<Transformer>(config);
    processor->prepare(kSampleRate, kBlockSize);
    return BlockProcessor([processor](std::vector<float>& block) {
      float* channels[] = {block.data()};
      processor->process(channels, 1, static_cast<int>(block.size()));
    });
  };

  for (const float poison_value : poison_values()) {
    DYNAMIC_SECTION("poison " << poison_value) { check_owner(make, poison_value); }
  }
}
