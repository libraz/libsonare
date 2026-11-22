/// @file crest_factor_silence_test.cpp
/// @brief The mono and interleaved crest factors agree on silence and on very quiet input.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "metering/basic.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using sonare::Audio;
namespace metering = sonare::metering;

namespace {

constexpr int kSampleRate = 48000;

Audio from_samples(std::vector<float> samples) {
  return Audio::from_vector(std::move(samples), kSampleRate);
}

}  // namespace

TEST_CASE("crest factor of all-zero input is not measured in both forms", "[meter]") {
  const std::vector<float> zeros(10000, 0.0f);

  CHECK(std::isnan(metering::crest_factor_db(from_samples(zeros))));
  CHECK(std::isnan(metering::crest_factor_db_interleaved(zeros.data(), zeros.size(), 1)));
  CHECK(std::isnan(metering::crest_factor_db_interleaved(nullptr, 0, 1)));
}

TEST_CASE("crest factor of a very quiet spike is measured in both forms", "[meter]") {
  // One sample in 10000 puts the RMS at peak / 100: 40 dB, however low the spike sits.
  std::vector<float> samples(10000, 0.0f);
  samples[5000] = 5.0e-9f;

  const float interleaved =
      metering::crest_factor_db_interleaved(samples.data(), samples.size(), 1);
  REQUIRE_THAT(interleaved, WithinAbs(40.0f, 1e-3f));
  REQUIRE_THAT(metering::crest_factor_db(from_samples(samples)), WithinAbs(interleaved, 1e-3f));
}

TEST_CASE("crest factor of a sine is 3 dB in both forms", "[meter]") {
  std::vector<float> samples(kSampleRate);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    samples[i] = 0.5f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 440.0f * t);
  }

  const float expected = 3.0103f;
  REQUIRE_THAT(metering::crest_factor_db(from_samples(samples)), WithinAbs(expected, 0.05f));
  REQUIRE_THAT(metering::crest_factor_db_interleaved(samples.data(), samples.size(), 1),
               WithinAbs(expected, 0.05f));
}
