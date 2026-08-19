/// @file decompose_test.cpp
/// @brief Unit tests for effects/decompose (NMF / nn_filter).

#include "effects/decompose.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

using namespace sonare;

TEST_CASE("decompose returns non-negative W and H of expected shape", "[util][decompose]") {
  // Construct a small non-negative spectrogram.
  std::vector<float> S{
      1.0f, 2.0f, 1.0f, 2.0f, 4.0f, 2.0f, 0.5f, 1.0f, 0.5f,
  };
  auto r = decompose(S.data(), 3, 3, /*n_components=*/2, /*n_iter=*/50);
  REQUIRE(r.W.size() == 3 * 2);
  REQUIRE(r.H.size() == 2 * 3);
  for (float v : r.W) REQUIRE(v >= 0.0f);
  for (float v : r.H) REQUIRE(v >= 0.0f);
}

TEST_CASE("decompose nndsvd init produces valid non-negative factorization", "[util][decompose]") {
  std::vector<float> S{
      1.0f, 2.0f, 1.0f, 2.0f, 4.0f, 2.0f, 0.5f, 1.0f, 0.5f,
  };
  auto r = decompose(S.data(), 3, 3, /*n_components=*/2, /*n_iter=*/50, "mu", 2.0f, "nndsvd");
  REQUIRE(r.W.size() == 3 * 2);
  REQUIRE(r.H.size() == 2 * 3);
  for (float v : r.W) REQUIRE(v >= 0.0f);
  for (float v : r.H) REQUIRE(v >= 0.0f);
}

TEST_CASE("decompose rejects an unknown init strategy", "[util][decompose]") {
  std::vector<float> S{1.0f, 2.0f, 3.0f, 4.0f};
  REQUIRE_THROWS(decompose(S.data(), 2, 2, /*n_components=*/1, /*n_iter=*/10, "mu", 2.0f, "bogus"));
}

namespace {

/// Two tones an octave and a half apart, gated so they never overlap in time.
/// NMF has an easy job separating them, which keeps the assertions about the
/// mask being genuinely selective meaningful.
std::vector<float> two_gated_tones(int sample_rate, std::size_t n) {
  std::vector<float> x(n, 0.0f);
  const double two_pi = 6.283185307179586;
  for (std::size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / sample_rate;
    const bool first_half = i < n / 2;
    const double hz = first_half ? 220.0 : 880.0;
    x[i] = static_cast<float>(0.5 * std::sin(two_pi * hz * t));
  }
  return x;
}

}  // namespace

TEST_CASE("decompose_stems components sum back to the input", "[util][decompose]") {
  constexpr int kSampleRate = 22050;
  const std::vector<float> x = two_gated_tones(kSampleRate, 8192);
  DecomposeStemsConfig config;
  config.n_components = 2;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.n_iter = 60;
  auto r = decompose_stems(x.data(), x.size(), kSampleRate, config);
  REQUIRE(r.components.size() == 2);
  for (const std::vector<float>& component : r.components) {
    REQUIRE(component.size() == x.size());
  }

  // The masks partition the complex spectrogram and the inverse STFT is linear,
  // so the components reconstruct the signal. Compare over the interior, where
  // the analysis window overlap is complete.
  const std::size_t start = static_cast<std::size_t>(config.n_fft);
  const std::size_t end = x.size() - static_cast<std::size_t>(config.n_fft);
  double err = 0.0;
  double ref = 0.0;
  for (std::size_t i = start; i < end; ++i) {
    double sum = 0.0;
    for (const std::vector<float>& component : r.components) sum += component[i];
    err += (sum - x[i]) * (sum - x[i]);
    ref += static_cast<double>(x[i]) * x[i];
  }
  REQUIRE(ref > 0.0);
  REQUIRE(std::sqrt(err / ref) < 0.05);
}

TEST_CASE("decompose_stems components carry phase and separate the tones", "[util][decompose]") {
  constexpr int kSampleRate = 22050;
  const std::vector<float> x = two_gated_tones(kSampleRate, 8192);
  DecomposeStemsConfig config;
  config.n_components = 2;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.n_iter = 60;
  auto r = decompose_stems(x.data(), x.size(), kSampleRate, config);
  REQUIRE(r.components.size() == 2);
  REQUIRE(r.W.size() == static_cast<std::size_t>(config.n_fft / 2 + 1) * 2);
  REQUIRE(r.H.size() % 2 == 0);

  // Every component must carry audible signal; an all-zero stem would mean the
  // mask collapsed onto one component.
  auto energy = [](const std::vector<float>& v, std::size_t from, std::size_t to) {
    double e = 0.0;
    for (std::size_t i = from; i < to; ++i) e += static_cast<double>(v[i]) * v[i];
    return e;
  };
  const std::size_t half = x.size() / 2;
  for (const std::vector<float>& component : r.components) {
    REQUIRE(energy(component, 0, x.size()) > 0.0);
  }

  // The two tones are disjoint in time, so each component should concentrate in
  // one half. Whichever component owns the first half must not also own the
  // second, or no separation happened at all.
  const double a_first = energy(r.components[0], 0, half);
  const double a_second = energy(r.components[0], half, x.size());
  const double b_first = energy(r.components[1], 0, half);
  const double b_second = energy(r.components[1], half, x.size());
  const bool a_leads = a_first > a_second;
  REQUIRE(a_leads != (b_first > b_second));
  const double leader_ratio = a_leads ? a_first / (a_second + 1e-12) : b_first / (b_second + 1e-12);
  REQUIRE(leader_ratio > 4.0);
}

TEST_CASE("decompose_stems rejects an invalid configuration", "[util][decompose]") {
  const std::vector<float> x(2048, 0.1f);
  DecomposeStemsConfig config;
  config.mask_power = 0.5f;
  REQUIRE_THROWS(decompose_stems(x.data(), x.size(), 22050, config));
  config = DecomposeStemsConfig();
  config.n_components = 0;
  REQUIRE_THROWS(decompose_stems(x.data(), x.size(), 22050, config));
  REQUIRE_THROWS(decompose_stems(x.data(), 0, 22050, DecomposeStemsConfig()));
}

TEST_CASE("nn_filter rejects a negative width", "[util][decompose]") {
  std::vector<float> S{
      1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f,
  };
  REQUIRE_THROWS(nn_filter(S.data(), 2, 4, "mean", 2, /*width=*/-1));
}

TEST_CASE("nn_filter preserves shape and stays non-negative", "[util][decompose]") {
  std::vector<float> S{
      1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f,
  };
  auto out = nn_filter(S.data(), 2, 4, "mean", 2, 1);
  REQUIRE(out.size() == 8);
  for (float v : out) REQUIRE(v >= 0.0f);
}

TEST_CASE("nn_filter median aggregator", "[util][decompose]") {
  std::vector<float> S{
      1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f,
  };
  auto out = nn_filter(S.data(), 2, 4, "median", 2, 1);
  REQUIRE(out.size() == 8);
}

TEST_CASE("nn_filter median averages the two central values for an even count",
          "[util][decompose]") {
  // 1 feature, 5 frames, all parallel so cosine similarity is uniform; with
  // width=1 and k=2, column 2 selects the two lowest-index neighbours {0, 1}.
  // numpy.median of two values is their mean (15), not the upper one (20).
  std::vector<float> S{10.0f, 20.0f, 1.0f, 1.0f, 1.0f};
  auto out = nn_filter(S.data(), /*n_features=*/1, /*n_frames=*/5, "median", /*k=*/2, /*width=*/1);
  REQUIRE(out.size() == 5);
  REQUIRE(out[2] == Catch::Approx(15.0f));
}
