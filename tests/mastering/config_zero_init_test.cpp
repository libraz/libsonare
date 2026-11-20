/// @file config_zero_init_test.cpp
/// @brief SonareMasteringConfig: sonare_mastering_config_init writes the loudness defaults, a
///        zero-initialised struct is refused by struct_size, and a zero true_peak_oversample or
///        release_ms is refused by name rather than read as a default.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <vector>

#include "support/audio_fixtures.h"
#include "util/constants.h"

#ifdef SONARE_WITH_MASTERING
using Catch::Matchers::ContainsSubstring;

// NOTE: sonare_mastering_process runs the full maximizer (LUFS measurement plus
// an oversampling true-peak limiter). It is kept short (0.25 s mono) so this
// stays a default-tier test rather than a slow one.
TEST_CASE("mastering config init supplies the loudness defaults", "[c_api][mastering]") {
  const int sample_rate = 22050;
  auto samples = sonare::test::generate_sine_samples(
      440.0f, sample_rate, static_cast<int>(static_cast<float>(sample_rate) * 0.25f), 0.2f);

  SonareMasteringConfig config;
  sonare_mastering_config_init(&config);
  REQUIRE(config.struct_size == sizeof(SonareMasteringConfig));
  REQUIRE(config.target_lufs == -14.0f);
  REQUIRE(config.ceiling_dbtp == -1.0f);
  REQUIRE(config.true_peak_oversample == 4);
  REQUIRE(config.release_ms == 50.0f);
  REQUIRE(config.apply_gain_at_input_rate == 0);

  config.target_lufs = -18.0f;
  SonareMasteringResult result{};
  REQUIRE(sonare_mastering_process(samples.data(), samples.size(), sample_rate, &config, &result) ==
          SONARE_OK);
  REQUIRE(result.samples != nullptr);
  REQUIRE(result.length == samples.size());
  REQUIRE(std::isfinite(result.output_lufs));
  sonare_free_mastering_result(&result);
}

TEST_CASE("mastering config refuses an unset struct_size and a zero oversample or release",
          "[c_api][mastering]") {
  const int sample_rate = 22050;
  auto samples = sonare::test::generate_sine_samples(
      440.0f, sample_rate, static_cast<int>(static_cast<float>(sample_rate) * 0.25f), 0.2f);

  SonareMasteringResult result{};
  SonareMasteringConfig zeroed{};
  REQUIRE(sonare_mastering_process(samples.data(), samples.size(), sample_rate, &zeroed, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE_THAT(sonare_last_error_message(),
               ContainsSubstring("config.struct_size must be set by sonare_mastering_config_init"));

  SonareMasteringConfig config;
  sonare_mastering_config_init(&config);
  config.true_peak_oversample = 0;
  REQUIRE(sonare_mastering_process(samples.data(), samples.size(), sample_rate, &config, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE_THAT(sonare_last_error_message(), ContainsSubstring("true_peak_oversample"));

  sonare_mastering_config_init(&config);
  config.release_ms = 0.0f;
  REQUIRE(sonare_mastering_process(samples.data(), samples.size(), sample_rate, &config, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE_THAT(sonare_last_error_message(), ContainsSubstring("release_ms"));
}
#endif  // SONARE_WITH_MASTERING
