/// @file mel_test.cpp
/// @brief librosa compatibility tests for Mel filterbank.
/// @details Reference values from: tests/librosa/reference/mel_filterbank.json

#include "filters/mel.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;
using Catch::Matchers::WithinRel;

TEST_CASE("mel filterbank librosa compatibility", "[mel][librosa]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/mel_filterbank.json");
  const auto& data = json["data"].as_array();

  for (const auto& item : data) {
    int sr = item["sr"].as_int();
    int n_fft = item["n_fft"].as_int();
    int n_mels = item["n_mels"].as_int();
    bool htk = item["htk"].as_bool();
    float expected_sum = item["sum"].as_float();
    float expected_max = item["max"].as_float();

    std::string section_name = "sr=" + std::to_string(sr) + " n_fft=" + std::to_string(n_fft) +
                               " n_mels=" + std::to_string(n_mels) +
                               " htk=" + (htk ? "true" : "false");

    SECTION(section_name) {
      MelFilterConfig config;
      config.n_mels = n_mels;
      config.htk = htk;
      config.norm = MelNorm::Slaney;

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
    }
  }
}
