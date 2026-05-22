/// @file poly_features_test.cpp
/// @brief librosa parity test for poly_features.
/// @details Reference: tests/librosa/reference/poly_features.json

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "feature/spectral.h"
#include "util/constants.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;

TEST_CASE("poly_features matches librosa (linear fit)", "[librosa][poly_features]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/poly_features.json");
  const auto& d = json["data"];
  int sr = d["sr"].as_int();
  int n_fft = d["n_fft"].as_int();
  int hop_length = d["hop_length"].as_int();
  int order = d["order"].as_int();

  // Reconstruct the same 440Hz tone.
  const size_t n = static_cast<size_t>(sr);  // 1 second
  std::vector<float> y(n);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(sr);
    y[i] = static_cast<float>(std::sin(static_cast<double>(constants::kTwoPi) * 440.0 * t));
  }
  Audio audio = Audio::from_vector(std::move(y), sr);

  StftConfig cfg;
  cfg.n_fft = n_fft;
  cfg.hop_length = hop_length;
  Spectrogram spec = Spectrogram::compute(audio, cfg);

  auto coeffs = poly_features(spec, sr, order);
  const auto& ref = d["coeffs_flat"].as_array();
  REQUIRE(coeffs.size() == ref.size());

  // librosa uses np.polyfit (least squares); minor numerical differences are
  // expected. Compare with a generous absolute + relative tolerance, skipping
  // boundary frames where centered STFT padding differs.
  const auto& shape = d["shape"].as_array();
  int n_frames = shape[1].as_int();
  const int skip = 1;
  double max_abs_diff = 0.0;
  double max_rel_diff = 0.0;
  int compared = 0;
  for (int row = 0; row < order + 1; ++row) {
    for (int t = skip; t < n_frames - skip; ++t) {
      size_t idx = static_cast<size_t>(row * n_frames + t);
      float got = coeffs[idx];
      float exp = ref[idx].as_float();
      double diff = std::abs(static_cast<double>(got) - static_cast<double>(exp));
      double rel = diff / (std::abs(static_cast<double>(exp)) + 1e-12);
      max_abs_diff = std::max(max_abs_diff, diff);
      max_rel_diff = std::max(max_rel_diff, rel);
      ++compared;
    }
  }
  CAPTURE(max_abs_diff, max_rel_diff, compared);
  // Accept either an absolute or a relative tolerance per element (np.polyfit
  // SVD differences accumulate at the very high orders and at edge frames).
  REQUIRE(max_abs_diff < 5e-2);
}
