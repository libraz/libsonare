#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

#include "mastering/final/bit_depth.h"
#include "mastering/final/dither.h"
#include "mastering/final/output_chain.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;
using namespace sonare::mastering::final;

namespace {

Audio make_audio(const std::vector<float>& samples) {
  return Audio::from_buffer(samples.data(), samples.size(), 48000);
}

}  // namespace

TEST_CASE("BitDepth quantizes samples to target grid", "[mastering][final]") {
  const auto result = bit_depth(make_audio({0.3f, -1.2f}), {8, true});

  REQUIRE(result.size() == 2);
  REQUIRE_THAT(result[0], WithinAbs(38.0f / 128.0f, 0.0001f));
  REQUIRE_THAT(result[1], WithinAbs(-1.0f, 0.0001f));
}

TEST_CASE("Dither adds deterministic low-level noise", "[mastering][final]") {
  const auto input = make_audio({0.0f, 0.0f, 0.0f});
  const auto a = dither(input, {DitherType::Tpdf, 16, 1234});
  const auto b = dither(input, {DitherType::Tpdf, 16, 1234});

  REQUIRE(a.size() == input.size());
  REQUIRE_THAT(a[0], WithinAbs(b[0], 0.0f));
  const bool any_noise = a[0] != 0.0f || a[1] != 0.0f || a[2] != 0.0f;
  REQUIRE(any_noise);
}

TEST_CASE("OutputChain applies dither then quantization", "[mastering][final]") {
  const auto result = output_chain(make_audio({0.1f, -0.1f}), {12, DitherType::None, true});

  REQUIRE(result.size() == 2);
  REQUIRE_THAT(result[0] * 2048.0f, WithinAbs(205.0f, 0.001f));
  REQUIRE_THAT(result[1] * 2048.0f, WithinAbs(-205.0f, 0.001f));
}

TEST_CASE("Final helpers validate inputs", "[mastering][final]") {
  const Audio empty;
  REQUIRE_THROWS(bit_depth(empty));
  REQUIRE_THROWS(dither(empty));
  REQUIRE_THROWS(output_chain(empty));
  REQUIRE_THROWS(bit_depth(make_audio({0.0f}), {1, true}));
  REQUIRE_THROWS(dither(make_audio({0.0f}), {DitherType::Tpdf, 40, 0}));
}
