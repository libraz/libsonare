/// @file wavelet_filters_test.cpp
/// @brief Reference compatibility tests for filters/wavelet (lengths + kernels).

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "filters/wavelet.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;

namespace {

std::vector<float> as_floats(const JsonValue& arr) {
  std::vector<float> out;
  for (const auto& v : arr.as_array()) out.push_back(v.as_float());
  return out;
}

}  // namespace

TEST_CASE("wavelet_lengths matches librosa", "[librosa][wavelet]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/wavelet_filters.json");
  const auto& d = json["data"];
  const int sr = d["sr"].as_int();
  auto freqs = as_floats(d["freqs"]);

  auto got = wavelet_lengths(freqs, sr, /*window_param=*/1.0f);
  const auto& expected = d["lengths"].as_array();
  REQUIRE(got.size() == expected.size());
  for (size_t i = 0; i < got.size(); ++i) {
    // librosa returns raw float lengths. We round up to the next odd integer
    // (analogous to numpy.ceil + (1 if even)) so values should match within
    // 2 samples.
    const float ref = expected[i].as_float();
    CAPTURE(i, got[i], ref);
    REQUIRE(std::abs(got[i] - ref) <= 2.0f);
  }
}

TEST_CASE("wavelet (padded) has matching n_fft and total length", "[librosa][wavelet]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/wavelet_filters.json");
  const auto& d = json["data"];
  const int sr = d["sr"].as_int();
  auto freqs = as_floats(d["freqs"]);
  const int expected_n_fft = d["n_fft"].as_int();

  int n_fft_out = 0;
  auto kernels = wavelet(freqs, sr, /*window_param=*/1.0f, /*is_cqt=*/true,
                         /*pad_fft=*/true, /*Q=*/0.0f, &n_fft_out);
  REQUIRE(n_fft_out == expected_n_fft);
  REQUIRE(kernels.size() == static_cast<size_t>(freqs.size()) * static_cast<size_t>(n_fft_out));
  // Sanity: each kernel slot is centered, so the maximum |kernel| should sit
  // near the middle of its slot for typical Morlet wavelets.
  for (size_t i = 0; i < freqs.size(); ++i) {
    float max_abs = 0.0f;
    int max_idx = 0;
    for (int n = 0; n < n_fft_out; ++n) {
      const auto& v = kernels[i * n_fft_out + n];
      const float a = std::abs(v);
      if (a > max_abs) {
        max_abs = a;
        max_idx = n;
      }
    }
    CAPTURE(i, max_idx, n_fft_out);
    // Allow some asymmetry; centre is at (n_fft - L)/2 + (L-1)/2 ~= n_fft/2 - 1.
    REQUIRE(std::abs(max_idx - n_fft_out / 2) <= n_fft_out / 4);
  }
}
