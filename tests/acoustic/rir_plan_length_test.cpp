#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "acoustic/image_source.h"
#include "acoustic/late_reverb.h"

using namespace sonare;
using namespace sonare::acoustic;

namespace {
ImageSource image_at(float distance) {
  ImageSource im;
  im.distance = distance;
  im.reflection.assign(1, 1.0f);
  return im;
}

int expected_length(float farthest_finite_distance, int sample_rate, const EarlyIrConfig& cfg) {
  const int half = (cfg.fdl < 1 ? 1 : cfg.fdl | 1) / 2;
  const float delay = farthest_finite_distance / kSoundSpeed * static_cast<float>(sample_rate);
  return static_cast<int>(std::ceil(static_cast<double>(delay))) + half + 2;
}
}  // namespace

TEST_CASE("early_ir_natural_length matches synthesize_early_ir for finite images",
          "[acoustic][rir_plan]") {
  const std::vector<ImageSource> images = {image_at(3.0f), image_at(12.5f), image_at(7.0f)};
  const EarlyIrConfig cfg;
  const int length = early_ir_natural_length(images, 48000, cfg);
  REQUIRE(length == expected_length(12.5f, 48000, cfg));
  REQUIRE(static_cast<int>(synthesize_early_ir(images, 48000, cfg).size()) == length);
}

TEST_CASE("early_ir_natural_length ignores images whose delay is not finite",
          "[acoustic][rir_plan]") {
  const float inf = std::numeric_limits<float>::infinity();
  // A distance near FLT_MAX is finite, but distance / c * sr overflows to infinity.
  const std::vector<ImageSource> images = {image_at(10.0f), image_at(3.0e38f), image_at(inf),
                                           image_at(std::numeric_limits<float>::quiet_NaN())};
  const EarlyIrConfig cfg;
  const int length = early_ir_natural_length(images, 48000, cfg);

  // The renderer skips those images, so the planned length is the one it produces.
  REQUIRE(static_cast<int>(synthesize_early_ir(images, 48000, cfg).size()) == length);
  REQUIRE(length == expected_length(10.0f, 48000, cfg));
  REQUIRE(length < kMaxAutoSamples);
}

TEST_CASE("early_ir_natural_length of an empty image set is one sample", "[acoustic][rir_plan]") {
  const EarlyIrConfig cfg;
  const int length = early_ir_natural_length({}, 48000, cfg);
  REQUIRE(static_cast<int>(synthesize_early_ir({}, 48000, cfg).size()) == length);
}
