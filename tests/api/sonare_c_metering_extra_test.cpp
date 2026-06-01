/// @file sonare_c_metering_extra_test.cpp
/// @brief C API tests for the multi-channel / standards-compliant LUFS
///        extensions (sonare_lufs_interleaved, sonare_ebur128_loudness_range)
///        and the extended true-peak oversample-factor validation
///        (factor 16 accepted, non-power-of-two rejected).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "sonare_c.h"
#include "util/constants.h"

namespace {

// Generate a mono sine wave buffer.
std::vector<float> generate_sine(float freq, int sample_rate, float duration) {
  size_t n_samples = static_cast<size_t>(sample_rate * duration);
  std::vector<float> samples(n_samples);
  for (size_t i = 0; i < n_samples; ++i) {
    samples[i] =
        std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * freq * i / sample_rate);
  }
  return samples;
}

// Interleave two equal-length mono channels into a stereo buffer.
std::vector<float> interleave_stereo(const std::vector<float>& left,
                                     const std::vector<float>& right) {
  std::vector<float> out(left.size() * 2);
  for (size_t i = 0; i < left.size(); ++i) {
    out[2 * i] = left[i];
    out[2 * i + 1] = right[i];
  }
  return out;
}

}  // namespace

TEST_CASE("sonare_lufs_interleaved", "[c_api]") {
  SECTION("mono interleaved matches sonare_lufs") {
    auto samples = generate_sine(440.0f, 48000, 3.0f);

    SonareLufsResult mono_result = {};
    SonareLufsResult inter_result = {};
    REQUIRE(sonare_lufs(samples.data(), samples.size(), 48000, &mono_result) == SONARE_OK);
    REQUIRE(sonare_lufs_interleaved(samples.data(), samples.size(), 1, 48000, &inter_result) ==
            SONARE_OK);

    REQUIRE(inter_result.integrated_lufs ==
            Catch::Approx(mono_result.integrated_lufs).margin(1e-3f));
    REQUIRE(inter_result.short_term_lufs ==
            Catch::Approx(mono_result.short_term_lufs).margin(1e-3f));
    REQUIRE(inter_result.loudness_range == Catch::Approx(mono_result.loudness_range).margin(1e-3f));
  }

  SECTION("stereo interleaved buffer yields finite loudness") {
    auto left = generate_sine(440.0f, 48000, 3.0f);
    auto right = generate_sine(660.0f, 48000, 3.0f);
    auto stereo = interleave_stereo(left, right);

    SonareLufsResult result = {};
    REQUIRE(sonare_lufs_interleaved(stereo.data(), left.size(), 2, 48000, &result) == SONARE_OK);

    REQUIRE(std::isfinite(result.integrated_lufs));
    REQUIRE(std::isfinite(result.momentary_lufs));
    REQUIRE(std::isfinite(result.short_term_lufs));
    REQUIRE(std::isfinite(result.loudness_range));
    REQUIRE(result.loudness_range >= 0.0f);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_sine(440.0f, 48000, 1.0f);
    SonareLufsResult result = {};

    REQUIRE(sonare_lufs_interleaved(samples.data(), samples.size(), 1, 48000, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_lufs_interleaved(nullptr, samples.size(), 1, 48000, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_lufs_interleaved(samples.data(), samples.size(), 0, 48000, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_lufs_interleaved(samples.data(), samples.size(), 2, 0, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_ebur128_loudness_range", "[c_api]") {
  SECTION("returns a finite, non-negative loudness range") {
    auto samples = generate_sine(440.0f, 48000, 6.0f);
    float lra = -1.0f;
    REQUIRE(sonare_ebur128_loudness_range(samples.data(), samples.size(), 48000, &lra) ==
            SONARE_OK);
    REQUIRE(std::isfinite(lra));
    REQUIRE(lra >= 0.0f);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_sine(440.0f, 48000, 1.0f);
    float lra = 0.0f;

    REQUIRE(sonare_ebur128_loudness_range(samples.data(), samples.size(), 48000, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_ebur128_loudness_range(nullptr, samples.size(), 48000, &lra) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_metering_true_peak_db oversample factor validation", "[c_api]") {
  auto samples = generate_sine(440.0f, 48000, 1.0f);

  SECTION("accepts factor 16") {
    float tp_db = 0.0f;
    REQUIRE(sonare_metering_true_peak_db(samples.data(), samples.size(), 48000, 16, &tp_db) ==
            SONARE_OK);
    REQUIRE(std::isfinite(tp_db));
  }

  SECTION("accepts the other supported power-of-two factors") {
    for (int factor : {1, 2, 4, 8}) {
      float tp_db = 0.0f;
      REQUIRE(sonare_metering_true_peak_db(samples.data(), samples.size(), 48000, factor, &tp_db) ==
              SONARE_OK);
      REQUIRE(std::isfinite(tp_db));
    }
  }

  SECTION("rejects non-power-of-two factor 3") {
    float tp_db = 0.0f;
    REQUIRE(sonare_metering_true_peak_db(samples.data(), samples.size(), 48000, 3, &tp_db) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("rejects factor above the supported maximum") {
    float tp_db = 0.0f;
    REQUIRE(sonare_metering_true_peak_db(samples.data(), samples.size(), 48000, 32, &tp_db) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}
