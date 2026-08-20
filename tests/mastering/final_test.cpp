#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "mastering/final/bit_depth.h"
#include "mastering/final/dither.h"
#include "mastering/final/output_chain.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;
using namespace sonare::mastering::final;

namespace {

Audio make_audio(const std::vector<float>& samples) {
  return Audio::from_buffer(samples.data(), samples.size(), 48000);
}

}  // namespace

TEST_CASE("BitDepth quantizes samples to target grid", "[mastering][final]") {
  const auto result = bit_depth(make_audio({0.3f, -1.2f, 1.0f}), {8, true});

  REQUIRE(result.size() == 3);
  REQUIRE_THAT(result[0], WithinAbs(38.0f / 128.0f, 0.0001f));
  REQUIRE_THAT(result[1], WithinAbs(-1.0f, 0.0001f));
  REQUIRE_THAT(result[2], WithinAbs(127.0f / 128.0f, 0.0001f));
}

TEST_CASE("Dither adds deterministic low-level noise", "[mastering][final]") {
  // Long enough for the noise to be certain: dither is added at LSB scale and
  // then quantized, so on silence most individual samples round back to zero and
  // only a long run reliably shows the +-1 LSB codes.
  const std::vector<float> silence(256, 0.0f);
  const auto input = make_audio(silence);
  const auto a = dither(input, {DitherType::Tpdf, 16, 1234});
  const auto b = dither(input, {DitherType::Tpdf, 16, 1234});

  REQUIRE(a.size() == input.size());
  size_t noisy = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    REQUIRE_THAT(a[i], WithinAbs(b[i], 0.0f));
    if (a[i] != 0.0f) ++noisy;
    // Dither noise stays at LSB scale: never more than one code from silence.
    REQUIRE(std::abs(a[i]) <= 1.0f / 32768.0f);
  }
  CAPTURE(noisy);
  REQUIRE(noisy > 0);
}

TEST_CASE("Every dither mode lands on the same target-bit grid", "[mastering][final]") {
  constexpr int kTargetBits = 16;
  constexpr float kScale = 32768.0f;  // 2^(kTargetBits - 1)

  std::vector<float> input(512);
  for (size_t i = 0; i < input.size(); ++i) {
    const float t = static_cast<float>(i) / 48000.0f;
    input[i] = 0.6f * std::sin(sonare::constants::kTwoPi * 220.0f * t);
  }
  const auto audio = make_audio(input);
  const auto undithered = bit_depth(audio, {kTargetBits, true});

  for (const DitherType type : {DitherType::Rpdf, DitherType::Tpdf, DitherType::NoiseShaped}) {
    CAPTURE(static_cast<int>(type));
    const auto result = dither(audio, {type, kTargetBits, 1234});
    REQUIRE(result.size() == input.size());

    // Every sample must be an exact integer multiple of the target LSB: a
    // target_bits of 16 has to yield a 16-bit signal whichever mode produced it.
    size_t off_grid = 0;
    for (size_t i = 0; i < result.size(); ++i) {
      const float code = result[i] * kScale;
      if (code != std::round(code)) ++off_grid;
    }
    CAPTURE(off_grid);
    CHECK(off_grid == 0);

    // Non-vacuity: quantizing must not have replaced the dither. At least one
    // sample has to differ from the undithered quantization of the same input.
    size_t moved = 0;
    for (size_t i = 0; i < result.size(); ++i) {
      if (result[i] != undithered[i]) ++moved;
    }
    CHECK(moved > 0);
  }
}

TEST_CASE("OutputChain applies dither then quantization", "[mastering][final]") {
  const auto result = output_chain(make_audio({0.1f, -0.1f}), {12, DitherType::None, true});

  REQUIRE(result.size() == 2);
  REQUIRE_THAT(result[0] * 2048.0f, WithinAbs(205.0f, 0.001f));
  REQUIRE_THAT(result[1] * 2048.0f, WithinAbs(-205.0f, 0.001f));
}

TEST_CASE("Final dither and bit depth sanitize non-finite samples", "[mastering][final]") {
  const auto input =
      make_audio({std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                  -std::numeric_limits<float>::infinity(), 0.25f});

  const auto quantized = bit_depth(input, {16, true});
  for (size_t i = 0; i < quantized.size(); ++i) REQUIRE(std::isfinite(quantized[i]));

  for (const DitherType type :
       {DitherType::None, DitherType::Rpdf, DitherType::Tpdf, DitherType::NoiseShaped}) {
    const auto dithered = dither(input, {type, 16, 1234});
    for (size_t i = 0; i < dithered.size(); ++i) REQUIRE(std::isfinite(dithered[i]));
  }
}

TEST_CASE("Final helpers validate inputs", "[mastering][final]") {
  const Audio empty;
  REQUIRE_THROWS(bit_depth(empty));
  REQUIRE_THROWS(dither(empty));
  REQUIRE_THROWS(output_chain(empty));
  REQUIRE_THROWS(bit_depth(make_audio({0.0f}), {1, true}));
  REQUIRE_THROWS(dither(make_audio({0.0f}), {DitherType::Tpdf, 40, 0}));
}
