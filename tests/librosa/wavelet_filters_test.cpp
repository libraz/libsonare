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
    const float ref = expected[i].as_float();
    CAPTURE(i, got[i], ref);
    // Raw fractional lengths now agree with librosa to float precision.
    REQUIRE(std::abs(got[i] - ref) / std::max(ref, 1.0f) < 1e-5f);
  }
}

TEST_CASE("wavelet (padded) kernel values match librosa", "[librosa][wavelet]") {
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

  const auto& ref_real = d["filters_real"].as_array();
  const auto& ref_imag = d["filters_imag"].as_array();
  REQUIRE(ref_real.size() == freqs.size());
  REQUIRE(ref_imag.size() == freqs.size());

  double max_abs_diff = 0.0;
  double sum_sq_diff = 0.0;
  double sum_sq_ref = 0.0;
  for (size_t i = 0; i < freqs.size(); ++i) {
    const auto& row_real = ref_real[i].as_array();
    const auto& row_imag = ref_imag[i].as_array();
    REQUIRE(static_cast<int>(row_real.size()) == n_fft_out);
    for (int n = 0; n < n_fft_out; ++n) {
      const auto& v = kernels[i * n_fft_out + n];
      const double rr = row_real[n].as_float();
      const double ri = row_imag[n].as_float();
      const double dr = static_cast<double>(v.real()) - rr;
      const double di = static_cast<double>(v.imag()) - ri;
      const double d2 = dr * dr + di * di;
      max_abs_diff = std::max(max_abs_diff, std::sqrt(d2));
      sum_sq_diff += d2;
      sum_sq_ref += rr * rr + ri * ri;
    }
  }
  const double rel_frob = std::sqrt(sum_sq_diff / std::max(sum_sq_ref, 1e-12));
  CAPTURE(max_abs_diff, rel_frob);
  REQUIRE(max_abs_diff < 1e-4);
  REQUIRE(rel_frob < 1e-4);
}
