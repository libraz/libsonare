#include <algorithm>
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

// target_bits accepts up to 32, but the grid is realized in float samples, so
// it stops getting finer once the step drops below binary32's own spacing. The
// headers state that ceiling; this pins the number they state, and pins that it
// is the sample TYPE that sets it - the same computation carried out in double
// and stored back into a float lands on exactly the same grid, so promoting the
// arithmetic would not buy the finer grid either.
TEST_CASE("bit_depth grids finer than float32 collapse onto the achievable one",
          "[mastering][final]") {
  // A dense ramp around half scale, where binary32's spacing is 2^-24 and the
  // finest achievable grid is therefore 25 bits.
  constexpr int kCount = 40000;
  std::vector<float> ramp(kCount);
  for (int index = 0; index < kCount; ++index) {
    ramp[static_cast<std::size_t>(index)] = 0.5f + static_cast<float>(index) * 1.0e-9f;
  }
  const auto audio = make_audio(ramp);

  const auto smallest_step = [](const Audio& out) {
    std::vector<float> values(out.data(), out.data() + out.size());
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    float smallest = 1.0f;
    for (std::size_t index = 1; index < values.size(); ++index) {
      smallest = std::min(smallest, values[index] - values[index - 1]);
    }
    return smallest;
  };

  // Everything a delivery format uses is exact.
  for (const int bits : {16, 20, 24, 25}) {
    CAPTURE(bits);
    const float nominal = std::pow(2.0f, -static_cast<float>(bits - 1));
    CHECK_THAT(smallest_step(bit_depth(audio, {bits, true})), WithinAbs(nominal, nominal * 1e-3f));
  }
  // Past that the step stops shrinking, and every higher setting produces the
  // same grid rather than the nominal one.
  const float ceiling_step = smallest_step(bit_depth(audio, {25, true}));
  for (const int bits : {26, 28, 32}) {
    CAPTURE(bits);
    const float step = smallest_step(bit_depth(audio, {bits, true}));
    CHECK(step == ceiling_step);
    CHECK(step > std::pow(2.0f, -static_cast<float>(bits - 1)));
  }

  // The ceiling is the storage type, not the intermediate precision: quantizing
  // in double and storing the result as float reproduces the same step.
  for (const int bits : {26, 32}) {
    CAPTURE(bits);
    const double scale = std::pow(2.0, bits - 1);
    std::vector<float> in_double(ramp.size());
    for (std::size_t index = 0; index < ramp.size(); ++index) {
      in_double[index] =
          static_cast<float>(std::round(static_cast<double>(ramp[index]) * scale) / scale);
    }
    CHECK(smallest_step(make_audio(in_double)) == ceiling_step);
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
