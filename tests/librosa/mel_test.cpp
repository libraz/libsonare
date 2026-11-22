/// @file mel_test.cpp
/// @brief Reference compatibility tests for Mel filterbank.
/// @details Reference values from: tests/librosa/reference/mel_filterbank.json and
///          tests/librosa/reference/mel_frequencies.json

#include "filters/mel.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinRel;

TEST_CASE("mel filterbank reference compatibility", "[mel][reference]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/mel_filterbank.json");
  const auto& data = json["data"].as_array();

  for (const auto& item : data) {
    int sr = item["sr"].as_int();
    int n_fft = item["n_fft"].as_int();
    int n_mels = item["n_mels"].as_int();
    bool htk = item["htk"].as_bool();
    float expected_sum = item["sum"].as_float();
    float expected_max = item["max"].as_float();
    // A row without "norm" is librosa's default Slaney area normalization; null is none.
    const bool normalized = !item.contains("norm") || !item["norm"].is_null();
    const float fmax = item.contains("fmax") ? item["fmax"].as_float() : 0.0f;

    std::string section_name =
        "sr=" + std::to_string(sr) + " n_fft=" + std::to_string(n_fft) +
        " n_mels=" + std::to_string(n_mels) + " htk=" + (htk ? "true" : "false") +
        " norm=" + (normalized ? "slaney" : "none") + " fmax=" + std::to_string(fmax);

    SECTION(section_name) {
      MelFilterConfig config;
      config.n_mels = n_mels;
      config.htk = htk;
      config.fmax = fmax;
      config.norm = normalized ? MelNorm::Slaney : MelNorm::None;

      std::vector<float> fb = create_mel_filterbank(sr, n_fft, config);

      // Verify dimensions
      int n_bins = n_fft / 2 + 1;
      REQUIRE(fb.size() == static_cast<size_t>(n_mels * n_bins));

      // Compare sum
      float sum = 0.0f;
      for (float v : fb) {
        sum += v;
      }
      REQUIRE_THAT(sum, WithinRel(expected_sum, 1e-3f));

      // Compare max
      float max_val = 0.0f;
      for (float v : fb) {
        max_val = std::max(max_val, v);
      }
      REQUIRE_THAT(max_val, WithinRel(expected_max, 1e-3f));

      // Verify row sums if available
      if (item.contains("row_sums")) {
        const auto& row_sums = item["row_sums"].as_array();
        for (int m = 0; m < n_mels; ++m) {
          float row_sum = 0.0f;
          for (int k = 0; k < n_bins; ++k) {
            row_sum += fb[m * n_bins + k];
          }
          float expected_row_sum = row_sums[m].as_float();
          REQUIRE_THAT(row_sum, WithinRel(expected_row_sum, 1e-3f));
        }
      }

      // The first rows' lowest bins, value by value, where the reference carries them.
      if (item.contains("first_rows")) {
        const auto& rows = item["first_rows"].as_array();
        for (size_t m = 0; m < rows.size(); ++m) {
          const auto& row = rows[m].as_array();
          for (size_t k = 0; k < row.size(); ++k) {
            INFO("row " << m << " bin " << k);
            const float expected = row[k].as_float();
            const float actual = fb[m * static_cast<size_t>(n_bins) + k];
            if (expected == 0.0f) {
              REQUIRE(std::abs(actual) <= 1e-7f);
            } else {
              REQUIRE_THAT(actual, WithinRel(expected, 1e-3f));
            }
          }
        }
      }
    }
  }
}

TEST_CASE("mel frequency points reference compatibility", "[mel][reference]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/mel_frequencies.json");
  for (const auto& item : json["data"].as_array()) {
    const int n_points = item["n_mels"].as_int();
    const float fmin = item["fmin"].as_float();
    const float fmax = item["fmax"].as_float();
    const bool htk = item["htk"].as_bool();
    INFO("n_points=" << n_points << " fmin=" << fmin << " fmax=" << fmax << " htk=" << htk);
    const auto& expected = item["values"].as_array();
    const std::vector<float> points = mel_frequency_points(n_points, fmin, fmax, htk);
    REQUIRE(points.size() == expected.size());
    for (size_t i = 0; i < points.size(); ++i) {
      INFO("point " << i);
      const float want = expected[i].as_float();
      if (want == 0.0f) {
        REQUIRE(std::abs(points[i]) <= 1e-3f);
      } else {
        REQUIRE_THAT(points[i], WithinRel(want, 1e-4f));
      }
    }
  }
}
