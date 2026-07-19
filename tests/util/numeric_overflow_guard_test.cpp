/// @file numeric_overflow_guard_test.cpp
/// @brief Numeric-overflow hardening regression tests for core utilities.
/// @details Covers guards against signed-shift overflow in next_power_of_2(int)
///          (would spin forever) and undefined float->size_t casts in
///          Audio::slice for non-finite / out-of-range time bounds. The
///          flattened-index promotions in pcen.cpp / harmonic.cpp are guarded by
///          static reasoning (size_t accumulation) rather than a unit test, as
///          triggering int overflow there requires multi-gigabyte spectrograms.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "core/audio.h"
#include "util/math_utils.h"
#include "util/numeric_validation.h"
#include "util/resource_limits.h"

using namespace sonare;

TEST_CASE("checked projected counts reject non-finite and oversized results",
          "[numeric][overflow]") {
  std::size_t out = 99;
  REQUIRE(numeric::checked_projected_count(std::size_t{100}, 0.5f, std::size_t{200}, &out));
  REQUIRE(out == 200);

  out = 99;
  REQUIRE_FALSE(numeric::checked_projected_count(
      std::size_t{100}, std::numeric_limits<float>::quiet_NaN(), std::size_t{200}, &out));
  REQUIRE(out == 99);
  REQUIRE_FALSE(numeric::checked_projected_count(
      std::size_t{100}, std::numeric_limits<float>::infinity(), std::size_t{200}, &out));
  REQUIRE_FALSE(numeric::checked_projected_count(std::size_t{100}, 0.49f, std::size_t{200}, &out));
  REQUIRE_FALSE(numeric::checked_projected_count(
      std::size_t{100}, std::numeric_limits<float>::min(), std::size_t{200}, &out));

  REQUIRE(numeric::checked_size_product(std::size_t{10}, std::size_t{20}, std::size_t{200}, &out));
  REQUIRE(out == 200);
  REQUIRE_FALSE(
      numeric::checked_size_product(std::size_t{10}, std::size_t{21}, std::size_t{200}, &out));
}

TEST_CASE("checked and saturating addition handle both signed boundaries", "[numeric][overflow]") {
  int64_t out = 0;
  REQUIRE(numeric::checked_add<int64_t>(40, 2, &out));
  REQUIRE(out == 42);

  out = 7;
  REQUIRE_FALSE(numeric::checked_add(std::numeric_limits<int64_t>::max(), int64_t{1}, &out));
  REQUIRE(out == 7);
  REQUIRE_FALSE(numeric::checked_add(std::numeric_limits<int64_t>::lowest(), int64_t{-1}, &out));
  REQUIRE(out == 7);

  REQUIRE(numeric::saturating_add(std::numeric_limits<int64_t>::max(), int64_t{1}) ==
          std::numeric_limits<int64_t>::max());
  REQUIRE(numeric::saturating_add(std::numeric_limits<int64_t>::lowest(), int64_t{-1}) ==
          std::numeric_limits<int64_t>::lowest());
}

TEST_CASE("checked ceil ratios enforce iteration limits at the exact boundary",
          "[numeric][overflow]") {
  size_t iterations = 99;
  REQUIRE(numeric::checked_ceil_ratio(4.0, 1.0, size_t{4}, &iterations));
  REQUIRE(iterations == 4);

  constexpr size_t kMidiExportIterationLimit = 1'000'000;
  REQUIRE(numeric::checked_ceil_ratio(1'000'000.0, 1.0, kMidiExportIterationLimit, &iterations));
  REQUIRE(iterations == kMidiExportIterationLimit);
  REQUIRE_FALSE(
      numeric::checked_ceil_ratio(1'000'001.0, 1.0, kMidiExportIterationLimit, &iterations));

  iterations = 99;
  REQUIRE_FALSE(numeric::checked_ceil_ratio(4.0, 0.99, size_t{4}, &iterations));
  REQUIRE(iterations == 99);
  REQUIRE_FALSE(
      numeric::checked_ceil_ratio(4.0, std::numeric_limits<double>::min(), size_t{4}, &iterations));
  REQUIRE_FALSE(numeric::checked_ceil_ratio(4.0, 0.0, size_t{4}, &iterations));
}

TEST_CASE("offline import resource helpers enforce exact custom-budget boundaries",
          "[numeric][resource_limit]") {
  const resource::SpectrumResourceLimits spectrum_limits{16u, 9u, 800u};
  REQUIRE(resource::spectrum_shape_fits(15, spectrum_limits));
  REQUIRE(resource::spectrum_shape_fits(16, spectrum_limits));
  REQUIRE_FALSE(resource::spectrum_shape_fits(17, spectrum_limits));
  auto tight_spectrum_limits = spectrum_limits;
  tight_spectrum_limits.max_peak_bytes = 799u;
  REQUIRE_FALSE(resource::spectrum_shape_fits(16, tight_spectrum_limits));

  const resource::AcousticBandResourceLimits acoustic_limits{6u, 24u};
  REQUIRE(resource::acoustic_band_counts_fit(5, 23, acoustic_limits));
  REQUIRE(resource::acoustic_band_counts_fit(6, 24, acoustic_limits));
  REQUIRE_FALSE(resource::acoustic_band_counts_fit(7, 24, acoustic_limits));
  REQUIRE_FALSE(resource::acoustic_band_counts_fit(6, 25, acoustic_limits));

  const resource::ProjectImportResourceLimits project_limits{10u, 20u, 30u, 40u, 50u};
  REQUIRE(resource::project_document_fits(9, 19, 29, 39, 49, project_limits));
  REQUIRE(resource::project_document_fits(10, 20, 30, 40, 50, project_limits));
  REQUIRE_FALSE(resource::project_document_fits(11, 20, 30, 40, 50, project_limits));
  REQUIRE_FALSE(resource::project_document_fits(10, 21, 30, 40, 50, project_limits));
  REQUIRE_FALSE(resource::project_document_fits(10, 20, 31, 40, 50, project_limits));
  REQUIRE_FALSE(resource::project_document_fits(10, 20, 30, 41, 50, project_limits));
  REQUIRE_FALSE(resource::project_document_fits(10, 20, 30, 40, 51, project_limits));

  const resource::MidiImportResourceLimits midi_limits{10u, 20u, 30u, 40u, 50u};
  REQUIRE(resource::midi_import_shape_fits(9, 19, 29, 39, 49, midi_limits));
  REQUIRE(resource::midi_import_shape_fits(10, 20, 30, 40, 50, midi_limits));
  REQUIRE_FALSE(resource::midi_import_shape_fits(11, 20, 30, 40, 50, midi_limits));
  REQUIRE_FALSE(resource::midi_import_shape_fits(10, 21, 30, 40, 50, midi_limits));
  REQUIRE_FALSE(resource::midi_import_shape_fits(10, 20, 31, 40, 50, midi_limits));
  REQUIRE_FALSE(resource::midi_import_shape_fits(10, 20, 30, 41, 50, midi_limits));
  REQUIRE_FALSE(resource::midi_import_shape_fits(10, 20, 30, 40, 51, midi_limits));
}

TEST_CASE("next_power_of_2(int) saturates for huge inputs without hanging",
          "[math_utils][overflow]") {
  // In-range behavior is preserved.
  REQUIRE(next_power_of_2(1) == 1);
  REQUIRE(next_power_of_2(3) == 4);
  REQUIRE(next_power_of_2(1024) == 1024);
  REQUIRE(next_power_of_2(1025) == 2048);
  REQUIRE(next_power_of_2(0) == 1);
  REQUIRE(next_power_of_2(-5) == 1);

  // An input just above the largest representable power of two must terminate
  // (previously the doubling overflowed to negative and looped forever) and
  // saturate at the cap rather than returning a negative value.
  constexpr int kCap = 1 << 30;
  const int result = next_power_of_2((1 << 30) + 1);
  REQUIRE(result == kCap);
  REQUIRE(result > 0);

  // The maximum valid int must not hang either.
  const int at_max = next_power_of_2(std::numeric_limits<int>::max());
  REQUIRE(at_max == kCap);
  REQUIRE(at_max > 0);
}

TEST_CASE("Audio::slice clamps out-of-range and non-finite time bounds", "[audio][overflow]") {
  std::vector<float> samples(1000, 0.25f);
  Audio audio = Audio::from_buffer(samples.data(), samples.size(), 1000);

  SECTION("valid slice") {
    Audio s = audio.slice(0.1f, 0.5f);
    REQUIRE(s.size() == 400);
  }

  SECTION("start beyond end of audio yields empty slice, no UB") {
    Audio s = audio.slice(100.0f, 200.0f);
    REQUIRE(s.empty());
  }

  SECTION("end_time far past buffer clamps to length") {
    Audio s = audio.slice(0.0f, 1e30f);
    REQUIRE(s.size() == audio.size());
  }

  SECTION("non-finite start_time yields a well-defined slice, no UB") {
    const float inf = std::numeric_limits<float>::infinity();
    // A non-finite start is treated as 0 rather than an undefined
    // float->size_t cast; the resulting slice is well-defined and bounded.
    Audio s = audio.slice(inf, 0.5f);
    REQUIRE(s.size() <= audio.size());
    REQUIRE(s.size() == 500);
  }

  SECTION("non-finite end_time is rejected/clamped, no UB") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Audio s = audio.slice(0.0f, nan);
    // NaN end clamps to 0, so start >= end yields an empty slice.
    REQUIRE(s.empty());
  }

  SECTION("negative start_time clamps to 0") {
    Audio s = audio.slice(-5.0f, 0.5f);
    REQUIRE(s.size() == 500);
  }
}
