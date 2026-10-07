/// @file resample_test.cpp
/// @brief Tests for audio resampling.

#include "core/resample.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <numeric>
#include <vector>

#include "support/audio_fixtures.h"
#include "util/constants.h"
#include "util/exception.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
using sonare::constants::kTwoPi;
using sonare::test::generate_sine;
using sonare::test::rms;

double sum(const std::vector<float>& samples) {
  return std::accumulate(samples.begin(), samples.end(), 0.0);
}

double l1_norm(const std::vector<float>& samples) {
  double total = 0.0;
  for (const float sample : samples) total += std::abs(static_cast<double>(sample));
  return total;
}
}  // namespace

TEST_CASE("resample same rate returns copy", "[resample]") {
  constexpr int sr = 22050;
  std::vector<float> samples = generate_sine(1000, 440.0f, sr);

  std::vector<float> result = resample(samples.data(), samples.size(), sr, sr);

  REQUIRE(result.size() == samples.size());
  for (size_t i = 0; i < samples.size(); ++i) {
    REQUIRE(result[i] == samples[i]);
  }
}

TEST_CASE("resample rejects null samples for non-empty input", "[resample]") {
  constexpr size_t size = 1;

  SECTION("same sample rate") {
    try {
      (void)resample(nullptr, size, 22050, 22050);
      FAIL("Expected SonareException");
    } catch (const SonareException& error) {
      REQUIRE(error.code() == ErrorCode::InvalidParameter);
    }
  }

  SECTION("sample-rate conversion") {
    try {
      (void)resample(nullptr, size, 22050, 44100);
      FAIL("Expected SonareException");
    } catch (const SonareException& error) {
      REQUIRE(error.code() == ErrorCode::InvalidParameter);
    }
  }
}

TEST_CASE("resample accepts null samples for empty input", "[resample]") {
  REQUIRE(resample(nullptr, 0, 22050, 44100).empty());
}

TEST_CASE("resample 44100 to 22050 (2x downsample)", "[resample]") {
  constexpr int src_sr = 44100;
  constexpr int dst_sr = 22050;
  constexpr float freq = 440.0f;
  constexpr int src_samples = src_sr;  // 1 second

  std::vector<float> samples = generate_sine(src_samples, freq, src_sr);
  std::vector<float> result = resample(samples.data(), samples.size(), src_sr, dst_sr);

  // Output should be approximately half the length
  size_t expected_size = static_cast<size_t>(src_samples * dst_sr / src_sr);
  REQUIRE_THAT(static_cast<float>(result.size()),
               WithinRel(static_cast<float>(expected_size), 0.02f));

  // RMS should be similar (within 10% for a sine wave)
  float src_rms = rms(samples.data(), samples.size());
  float dst_rms = rms(result.data(), result.size());
  REQUIRE_THAT(dst_rms, WithinRel(src_rms, 0.1f));
}

TEST_CASE("resample 22050 to 44100 (2x upsample)", "[resample]") {
  constexpr int src_sr = 22050;
  constexpr int dst_sr = 44100;
  constexpr float freq = 440.0f;
  constexpr int src_samples = src_sr;  // 1 second

  std::vector<float> samples = generate_sine(src_samples, freq, src_sr);
  std::vector<float> result = resample(samples.data(), samples.size(), src_sr, dst_sr);

  // Output should be approximately double the length
  size_t expected_size = static_cast<size_t>(src_samples * dst_sr / src_sr);
  REQUIRE_THAT(static_cast<float>(result.size()),
               WithinRel(static_cast<float>(expected_size), 0.02f));

  // RMS should be similar
  float src_rms = rms(samples.data(), samples.size());
  float dst_rms = rms(result.data(), result.size());
  REQUIRE_THAT(dst_rms, WithinRel(src_rms, 0.1f));
}

TEST_CASE("resample Audio object", "[resample]") {
  constexpr int src_sr = 44100;
  constexpr int dst_sr = 22050;

  std::vector<float> samples = generate_sine(src_sr, 440.0f, src_sr);  // 1 second
  Audio audio = Audio::from_vector(std::move(samples), src_sr);

  Audio resampled = resample(audio, dst_sr);

  REQUIRE(resampled.sample_rate() == dst_sr);
  // Duration should be preserved
  REQUIRE_THAT(resampled.duration(), WithinRel(audio.duration(), 0.02f));
}

TEST_CASE("resample Audio same rate shares the input buffer", "[resample]") {
  constexpr int sr = 22050;
  std::vector<float> samples = generate_sine(1000, 440.0f, sr);
  const Audio audio = Audio::from_vector(std::move(samples), sr);

  const Audio resampled = resample(audio, sr);

  REQUIRE(resampled.size() == audio.size());
  REQUIRE(resampled.sample_rate() == sr);
  // The buffer is immutable, so the already-at-rate path hands back a view
  // instead of duplicating the samples.
  REQUIRE(resampled.data() == audio.data());
  // Positive control: a rate that does need work must not share the buffer.
  const Audio converted = resample(audio, sr * 2);
  REQUIRE(converted.data() != audio.data());
}

TEST_CASE("resample empty audio", "[resample]") {
  Audio empty;
  Audio result = resample(empty, 22050);
  REQUIRE(result.empty());
  REQUIRE(result.sample_rate() == 22050);
}

TEST_CASE("resample preserves DC offset", "[resample]") {
  constexpr int src_sr = 44100;
  constexpr int dst_sr = 22050;
  constexpr float dc = 0.5f;

  // DC signal with small sine
  std::vector<float> samples(src_sr);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = dc + 0.1f * std::sin(kTwoPi * 100.0f * i / src_sr);
  }

  std::vector<float> result = resample(samples.data(), samples.size(), src_sr, dst_sr);

  // Compute mean of result (should be close to DC)
  float mean = std::accumulate(result.begin(), result.end(), 0.0f) / result.size();
  REQUIRE_THAT(mean, WithinRel(dc, 0.05f));
}

TEST_CASE("resample impulse response preserves DC gain across rates", "[resample]") {
  for (const int impulse_tap : {0, 128}) {
    std::vector<float> source(256, 0.0f);
    source[static_cast<size_t>(impulse_tap)] = 1.0f;

    for (const int source_rate : {24000, 44100, 96000}) {
      INFO("impulse tap " << impulse_tap << " source rate " << source_rate);
      const std::vector<float> result =
          resample_impulse_response(source.data(), source.size(), source_rate, 48000);
      const double output_sum = sum(result);
      INFO("output sum " << output_sum);
      CHECK(std::abs(output_sum - 1.0) < 1e-3);
    }
  }
}

TEST_CASE("resample impulse response rejects a measured DC gain far from the rate ratio",
          "[resample]") {
  // Content above the target Nyquist leaves a source sum of ordinary size and a
  // target sum that is only the resampler's residue (measured gain about 15).
  constexpr int kSourceRate = 96000;
  constexpr int kTargetRate = 48000;
  std::vector<float> source(256);
  for (size_t i = 0; i < source.size(); ++i) {
    source[i] = std::sin(kTwoPi * 0.6f * static_cast<float>(i) + 0.3f);
  }
  const std::vector<float> generic =
      resample(source.data(), source.size(), kSourceRate, kTargetRate);
  const std::vector<float> result =
      resample_impulse_response(source.data(), source.size(), kSourceRate, kTargetRate);
  REQUIRE(result.size() == generic.size());

  const float ratio = static_cast<float>(kSourceRate) / static_cast<float>(kTargetRate);
  for (size_t index = 0; index < result.size(); ++index) {
    CHECK(std::abs(result[index] - ratio * generic[index]) < 1e-6f);
  }
}

TEST_CASE("resample impulse response uses area fallback for unstable DC sums", "[resample]") {
  std::vector<float> all_zero(256, 0.0f);
  const std::vector<float> zero_result =
      resample_impulse_response(all_zero.data(), all_zero.size(), 24000, 48000);
  REQUIRE(zero_result.size() == 512);
  CHECK(l1_norm(zero_result) == 0.0);

  std::vector<float> cancelling(256, 0.0f);
  cancelling[128] = 1.0f;
  cancelling[129] = -1.0f;
  const std::vector<float> generic_result =
      resample(cancelling.data(), cancelling.size(), 24000, 48000);
  const std::vector<float> cancelling_result =
      resample_impulse_response(cancelling.data(), cancelling.size(), 24000, 48000);
  REQUIRE(cancelling_result.size() == generic_result.size());
  INFO("cancelling output sum " << sum(cancelling_result));
  INFO("cancelling output L1 " << l1_norm(cancelling_result));
  CHECK(std::abs(sum(cancelling_result) - 0.5 * sum(generic_result)) < 1e-6);
  for (size_t index = 0; index < cancelling_result.size(); ++index) {
    const float sample = cancelling_result[index];
    CHECK(std::isfinite(sample));
    CHECK(std::abs(sample - 0.5f * generic_result[index]) < 1e-6f);
  }
}

TEST_CASE("resample impulse response preserves negative polarity", "[resample]") {
  std::vector<float> source(256, 0.0f);
  source[128] = -1.0f;
  const std::vector<float> result =
      resample_impulse_response(source.data(), source.size(), 24000, 48000);
  INFO("negative output sum " << sum(result));
  CHECK(std::abs(sum(result) + 1.0) < 1e-3);
}

TEST_CASE("resample impulse response keeps same-rate coefficients bit-exact", "[resample]") {
  const std::vector<float> source = {0.25f, -0.5f, 1.0f, -2.0f};
  const std::vector<float> result =
      resample_impulse_response(source.data(), source.size(), 48000, 48000);
  REQUIRE(result.size() == source.size());
  CHECK(result == source);
}

TEST_CASE("resample keeps at least one sample for non-empty input shorter than a target sample",
          "[resample]") {
  const std::vector<float> one(1, 0.5f);
  CHECK(resample(one.data(), one.size(), 384000, 8000).size() == 1);

  std::vector<float> source(16, 0.0f);
  source[0] = 1.0f;
  const std::vector<float> response =
      resample_impulse_response(source.data(), source.size(), 384000, 8000);
  REQUIRE(response.size() == 1);
  CHECK(std::abs(sum(response) - 1.0) < 1e-3);

  // Lengths that already round to at least one sample keep round-to-nearest.
  const std::vector<float> buffer(1001, 0.0f);
  CHECK(resample(buffer.data(), buffer.size(), 22050, 16000).size() == 726);
  CHECK(resample(buffer.data(), 48, 384000, 8000).size() == 1);
  CHECK(resample(buffer.data(), 96, 384000, 8000).size() == 2);
}
