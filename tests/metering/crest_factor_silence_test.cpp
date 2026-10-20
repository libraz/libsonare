/// @file crest_factor_silence_test.cpp
/// @brief The mono crest factor agrees with the interleaved one wherever the level is unmeasurable.

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

TEST_CASE("crest factor of all-zero input is 0 dB in both forms", "[meter]") {
  const std::vector<float> zeros(10000, 0.0f);

  REQUIRE_THAT(metering::crest_factor_db(from_samples(zeros)), WithinAbs(0.0f, 1e-6f));
  REQUIRE_THAT(metering::crest_factor_db_interleaved(zeros.data(), zeros.size(), 1),
               WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("crest factor of a peak above the floor over an RMS below it is the neutral 0 dB",
          "[meter]") {
  // The peak clears kEpsilon but the RMS (peak / 100) does not, so only the
  // RMS reading is the silence floor. Subtracting a floor from a measured peak
  // is not a crest factor.
  std::vector<float> samples(10000, 0.0f);
  samples[5000] = 5.0e-9f;

  const float interleaved =
      metering::crest_factor_db_interleaved(samples.data(), samples.size(), 1);
  REQUIRE_THAT(interleaved, WithinAbs(0.0f, 1e-6f));
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
