#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "core/audio_io.h"
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
  const auto result = bit_depth(make_audio({0.3f, -1.2f, 1.0f}), {8});

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
  const auto undithered = bit_depth(audio, {kTargetBits});

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

TEST_CASE("Noise-shaped dither does not leak clipped input into silence", "[mastering][final]") {
  constexpr std::size_t kFrames = 4096;
  constexpr float kLsb = 1.0f / 32768.0f;
  const float overload = GENERATE(2.0f, 1.01f);

  std::vector<float> input(kFrames, 0.0f);
  input.front() = overload;
  const auto result = dither(make_audio(input), {DitherType::NoiseShaped, 16, 1234});

  // The first sample must exercise the clamp before quantization. The rest of
  // the input is silence, so any large suffix is feedback from the clipped
  // sample rather than a legitimate signal.
  REQUIRE_THAT(result[0], WithinAbs(32767.0f / 32768.0f, 1.0e-7f));
  float suffix_peak = 0.0f;
  size_t off_grid = 0;
  size_t non_finite = 0;
  for (size_t i = 0; i < result.size(); ++i) {
    if (!std::isfinite(result[i])) ++non_finite;
    const float code = result[i] / kLsb;
    if (code != std::round(code)) ++off_grid;
    if (i > 0) suffix_peak = std::max(suffix_peak, std::abs(result[i]));
  }
  CAPTURE(overload, suffix_peak, suffix_peak / kLsb, non_finite, off_grid);
  REQUIRE(non_finite == 0);
  REQUIRE(off_grid == 0);
  // Shaped TPDF noise at 48 kHz has an RMS near 4 LSB and peaks near 16 over
  // these frames; a leak of even the 1.01 overload's excess is over 300 LSB.
  REQUIRE(suffix_peak <= 32.0f * kLsb);
}

TEST_CASE("Noise-shaped dither stays deterministic and shaped without clipping",
          "[mastering][final]") {
  constexpr size_t kFrames = 4096;
  std::vector<float> input(kFrames);
  for (size_t i = 0; i < input.size(); ++i) {
    const float t = static_cast<float>(i) / 48000.0f;
    input[i] = 0.4f * std::sin(sonare::constants::kTwoPi * 220.0f * t);
  }
  const auto audio = make_audio(input);
  const auto shaped = dither(audio, {DitherType::NoiseShaped, 16, 1234});
  const auto repeated = dither(audio, {DitherType::NoiseShaped, 16, 1234});
  const auto tpdf = dither(audio, {DitherType::Tpdf, 16, 1234});

  bool repeated_exactly = true;
  bool differs_from_tpdf = false;
  for (size_t i = 0; i < shaped.size(); ++i) {
    repeated_exactly = repeated_exactly && shaped[i] == repeated[i];
    differs_from_tpdf = differs_from_tpdf || shaped[i] != tpdf[i];
  }
  REQUIRE(repeated_exactly);
  REQUIRE(differs_from_tpdf);
  REQUIRE(std::all_of(shaped.data(), shaped.data() + shaped.size(),
                      [](float sample) { return std::isfinite(sample); }));
}

TEST_CASE("Interleaved dither validates channel shape and preserves mono behavior",
          "[mastering][final]") {
  const auto input = make_audio({0.1f, -0.2f, 0.3f, -0.4f});
  REQUIRE_THROWS(dither_interleaved(input, 0));
  REQUIRE_THROWS(dither_interleaved(input, 3));

  sonare::QuantizeReport mono_report{99, 99};
  sonare::QuantizeReport interleaved_report{99, 99};
  const auto mono = dither(input, {DitherType::None, 16, 1234}, &mono_report);
  const auto interleaved =
      dither_interleaved(input, 1, {DitherType::None, 16, 1234}, &interleaved_report);
  REQUIRE(mono_report.non_finite_samples == 0);
  REQUIRE(interleaved_report.non_finite_samples == 0);
  REQUIRE(mono_report.clipped_samples == 0);
  REQUIRE(interleaved_report.clipped_samples == 0);
  REQUIRE(mono.size() == interleaved.size());
  for (size_t i = 0; i < mono.size(); ++i) {
    REQUIRE(interleaved[i] == mono[i]);
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
    CHECK_THAT(smallest_step(bit_depth(audio, {bits})), WithinAbs(nominal, nominal * 1e-3f));
  }
  // Past that the step stops shrinking, and every higher setting produces the
  // same grid rather than the nominal one.
  const float ceiling_step = smallest_step(bit_depth(audio, {25}));
  for (const int bits : {26, 28, 32}) {
    CAPTURE(bits);
    const float step = smallest_step(bit_depth(audio, {bits}));
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

TEST_CASE("Dither output stays below full scale at every target width", "[mastering][final]") {
  std::vector<float> hot;
  for (int index = 0; index < 64; ++index) {
    hot.push_back(2.0f);
    hot.push_back(1.0f);
    hot.push_back(std::nextafter(1.0f, 0.0f));
  }
  const auto audio = make_audio(hot);
  for (const DitherType type : {DitherType::Rpdf, DitherType::Tpdf, DitherType::NoiseShaped}) {
    for (const int bits : {16, 24, 25, 26, 28, 32}) {
      CAPTURE(static_cast<int>(type), bits);
      const auto out = dither(audio, {type, bits, 1234});
      const float peak = *std::max_element(out.data(), out.data() + out.size());
      REQUIRE(peak < 1.0f);
      if (bits <= 24) {
        // The top code of an exactly representable grid is unchanged.
        REQUIRE(peak == 1.0f - std::pow(2.0f, -static_cast<float>(bits - 1)));
      }
    }
  }
}

TEST_CASE("OutputChain applies dither then quantization", "[mastering][final]") {
  const auto result = output_chain(make_audio({0.1f, -0.1f}), {12, DitherType::None});

  REQUIRE(result.size() == 2);
  REQUIRE_THAT(result[0] * 2048.0f, WithinAbs(205.0f, 0.001f));
  REQUIRE_THAT(result[1] * 2048.0f, WithinAbs(-205.0f, 0.001f));
}

TEST_CASE("Final quantization preserves the positive PCM ceiling at every width",
          "[mastering][final]") {
  const int bits = GENERATE(16, 24, 25, 26, 28, 32);
  const auto input = make_audio({2.0f, 1.0f, std::nextafter(1.0f, 0.0f), -1.0f, -2.0f});
  const float ceiling = std::min(1.0f - std::ldexp(1.0f, 1 - bits), std::nextafter(1.0f, 0.0f));
  sonare::QuantizeReport report;
  const auto encoded = bit_depth(input, {bits}, &report);
  const auto delivered = output_chain(input, {bits, DitherType::None});
  // Only 2.0 and -2.0 are past full scale; 1.0 and -1.0 are the grid's ends.
  CHECK(report.clipped_samples == 2);
  for (size_t i = 0; i < 3; ++i) {
    CHECK(encoded[i] == ceiling);
    CHECK(delivered[i] == ceiling);
  }
  CHECK(encoded[3] == -1.0f);
  CHECK(encoded[4] == -1.0f);
}

namespace {

// Every assertion below is on a value or on a count. Finiteness is satisfied by
// full scale as readily as by silence, so it cannot tell a substitution from a
// sample the caller delivered at the ceiling.
constexpr int kEncodeBits = 16;
constexpr float kEncodeLsb = 1.0f / 32768.0f;

}  // namespace

TEST_CASE("A non-finite sample reaching bit_depth leaves silence", "[mastering][final]") {
  // The three arrivals take different branches, so each one is its own run.
  const float arrival =
      GENERATE(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
               -std::numeric_limits<float>::infinity());
  sonare::QuantizeReport report{99, 99};
  const auto result = bit_depth(make_audio({arrival}), {kEncodeBits}, &report);
  REQUIRE(result[0] == 0.0f);
  REQUIRE(report.non_finite_samples == 1);
  REQUIRE(report.clipped_samples == 0);
}

TEST_CASE("A non-finite sample reaching any dither mode leaves silence", "[mastering][final]") {
  const float arrival =
      GENERATE(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
               -std::numeric_limits<float>::infinity());
  // One mode per run, and one sample per run so the shaper's error history is
  // still zero when the substituted sample is quantized.
  const DitherType type =
      GENERATE(DitherType::None, DitherType::Rpdf, DitherType::Tpdf, DitherType::NoiseShaped);

  sonare::QuantizeReport report{99, 99};
  const auto result = dither(make_audio({arrival}), {type, kEncodeBits, 1234}, &report);
  REQUIRE(report.non_finite_samples == 1);
  REQUIRE(report.clipped_samples == 0);
  if (type == DitherType::None) {
    // None does not quantize, so the replacement arrives bit-identical.
    REQUIRE(result[0] == 0.0f);
  } else {
    // The added noise is at LSB scale, so silence survives quantization as at
    // most one code either way.
    REQUIRE(std::abs(result[0]) <= kEncodeLsb);
  }
}

TEST_CASE("A finite sample at the ceiling reaches a final encode stage untouched",
          "[mastering][final]") {
  // The negative control for the case above: the substitution must fire on the
  // non-finite value and on nothing else, including the values it used to
  // produce. A fix that replaced everything would pass that case and fail here.
  constexpr int kBits = 16;

  sonare::QuantizeReport report{99, 99};
  const auto quantized = bit_depth(make_audio({1.0f, -1.0f, 0.999f}), {kBits}, &report);
  REQUIRE(report.non_finite_samples == 0);
  REQUIRE(report.clipped_samples == 0);
  // 1.0f lands on the top code because the grid has no code for it, which is the
  // stage's own transfer function rather than a replacement.
  REQUIRE_THAT(quantized[0], WithinAbs(32767.0f / 32768.0f, 1e-6f));
  REQUIRE_THAT(quantized[1], WithinAbs(-1.0f, 1e-6f));
  REQUIRE_THAT(quantized[2], WithinAbs(32735.0f / 32768.0f, 1e-6f));

  // None does not quantize, so full scale must arrive bit-identical.
  report = {99, 99};
  const auto passed =
      dither(make_audio({1.0f, -1.0f, 0.999f}), {DitherType::None, kBits, 1234}, &report);
  REQUIRE(report.non_finite_samples == 0);
  REQUIRE(report.clipped_samples == 0);
  REQUIRE(passed[0] == 1.0f);
  REQUIRE(passed[1] == -1.0f);
  REQUIRE(passed[2] == 0.999f);
}

TEST_CASE("Final helpers validate inputs", "[mastering][final]") {
  const Audio empty;
  REQUIRE_THROWS(bit_depth(empty));
  REQUIRE_THROWS(dither(empty));
  REQUIRE_THROWS(output_chain(empty));
  REQUIRE_THROWS(bit_depth(make_audio({0.0f}), {1}));
  REQUIRE_THROWS(dither(make_audio({0.0f}), {DitherType::Tpdf, 40, 0}));
}

TEST_CASE("output_chain equals running the dither and bit-depth stages in turn",
          "[mastering][final][output-chain-single-pass]") {
  const auto type =
      GENERATE(DitherType::None, DitherType::Rpdf, DitherType::Tpdf, DitherType::NoiseShaped);
  const int bits = GENERATE(2, 8, 16, 24, 26, 32);
  std::vector<float> input;
  for (int i = 0; i < 4000; ++i) {
    input.push_back(1.3f * std::sin(0.037f * static_cast<float>(i)) + 0.0004f * (i % 7));
  }
  input[10] = std::numeric_limits<float>::quiet_NaN();
  input[11] = std::numeric_limits<float>::infinity();
  input[12] = -std::numeric_limits<float>::infinity();
  const Audio audio = make_audio(input);

  sonare::QuantizeReport dither_report;
  sonare::QuantizeReport bit_report;
  const Audio staged = bit_depth(dither(audio, {type, bits}, &dither_report), {bits}, &bit_report);
  sonare::QuantizeReport chain_report;
  const Audio chained = output_chain(audio, {bits, type}, &chain_report);

  REQUIRE(chain_report.non_finite_samples ==
          dither_report.non_finite_samples + bit_report.non_finite_samples);
  REQUIRE(chain_report.clipped_samples ==
          dither_report.clipped_samples + bit_report.clipped_samples);
  // The 1.3 sine passes full scale on many samples, so the count is exercised.
  REQUIRE(chain_report.clipped_samples > 0);
  REQUIRE(chained.size() == staged.size());
  for (size_t i = 0; i < staged.size(); ++i) {
    INFO("sample " << i);
    REQUIRE(std::memcmp(&chained.data()[i], &staged.data()[i], sizeof(float)) == 0);
  }
}

// A sample the final stage put on the b-bit grid is a PCM code already, so the
// same-width WAV writer must store exactly that code.
TEST_CASE("A finalized PCM code survives a same-width WAV save", "[mastering][final]") {
  const int bits = GENERATE(16, 24);
  const double scale = std::ldexp(1.0, bits - 1);
  const std::vector<double> levels = {-1.0, -0.75, -0.25, 0.0, 0.25, 0.75, 1.0 - 1.0 / scale};
  std::vector<float> input;
  for (double level : levels) input.push_back(static_cast<float>(level));
  const auto delivered = output_chain(make_audio(input), {bits, DitherType::None});

  const std::string path = "test_final_grid_" + std::to_string(bits) + ".wav";
  save_wav(path, delivered.data(), delivered.size(), 48000, bits);
  std::ifstream in(path, std::ios::binary);
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
  in.close();
  std::remove(path.c_str());

  const size_t width = static_cast<size_t>(bits / 8);
  const std::string tag = "data";
  const auto chunk = std::search(bytes.begin(), bytes.end(), tag.begin(), tag.end());
  REQUIRE(chunk != bytes.end());
  const size_t data_off = static_cast<size_t>(chunk - bytes.begin()) + 8;
  for (size_t i = 0; i < levels.size(); ++i) {
    uint32_t raw = 0;
    for (size_t k = 0; k < width; ++k)
      raw |= static_cast<uint32_t>(bytes[data_off + i * width + k]) << (8 * k);
    if (raw & (1u << (bits - 1))) raw |= ~((1u << bits) - 1u);
    const auto code = static_cast<int32_t>(raw);
    CAPTURE(bits, i, levels[i]);
    CHECK(code == static_cast<int32_t>(std::lround(static_cast<double>(delivered[i]) * scale)));
    CHECK(code == static_cast<int32_t>(std::lround(levels[i] * scale)));
  }
}

TEST_CASE("A quantizing stage counts only the samples past full scale it held",
          "[mastering][final]") {
  // 1.0 lands one step under +1.0 and a sample under full scale may reach the top code through
  // the noise; neither is counted. Only the values past +-1 are.
  const int bits = GENERATE(8, 16, 24);
  const float top = 1.0f - std::ldexp(1.0f, 1 - bits);
  const std::vector<float> values = {3.0f,   1.0f,  top,   0.999f,  0.5f,
                                     -0.25f, -1.0f, -7.5f, 1.0e30f, 1.0000001f};
  const auto input = make_audio(values);
  const auto kPast = static_cast<std::size_t>(
      std::count_if(values.begin(), values.end(), [](float v) { return std::abs(v) > 1.0f; }));
  REQUIRE(kPast == 4);

  sonare::QuantizeReport bit_report;
  const auto encoded = bit_depth(input, {bits}, &bit_report);
  CHECK(bit_report.clipped_samples == kPast);
  for (size_t i = 0; i < input.size(); ++i) {
    CHECK(encoded[i] >= -1.0f);
    CHECK(encoded[i] < 1.0f);
  }
  const DitherType type = GENERATE(DitherType::Rpdf, DitherType::Tpdf, DitherType::NoiseShaped);
  sonare::QuantizeReport dither_report;
  (void)dither(input, {type, bits, 1234}, &dither_report);
  CHECK(dither_report.clipped_samples == kPast);

  // A full-scale sine at exactly 1.0 peak, dithered: the noise reaches the top code, no count.
  std::vector<float> sine(4096);
  for (size_t i = 0; i < sine.size(); ++i) {
    sine[i] = std::sin(sonare::constants::kTwoPi * static_cast<float>(i) / 64.0f);
  }
  sine[16] = 1.0f;
  sonare::QuantizeReport sine_report;
  (void)dither(make_audio(sine), {type, bits, 1234}, &sine_report);
  CHECK(sine_report.clipped_samples == 0);
  sonare::QuantizeReport exact_report;
  (void)bit_depth(make_audio({1.0f, -1.0f, 1.0f}), {bits}, &exact_report);
  CHECK(exact_report.clipped_samples == 0);
}
