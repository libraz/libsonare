/// @file decompose_test.cpp
/// @brief Unit tests for effects/decompose (NMF / nn_filter).

#include "effects/decompose.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "util/constants.h"

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

namespace {

/// @brief Deterministic strictly-positive [n_features x n_frames] matrix, row-major.
/// @details Strictly positive so the multiplicative updates never latch a factor to zero,
///          and structured per frame so the cosine neighbours of a frame are a distinct set
///          rather than a tie that any ordering satisfies.
std::vector<float> traversal_fixture(int n_features, int n_frames, uint32_t seed) {
  std::vector<float> S(static_cast<std::size_t>(n_features) * n_frames, 0.0f);
  uint32_t state = seed;
  for (int f = 0; f < n_features; ++f) {
    for (int t = 0; t < n_frames; ++t) {
      state = state * 1664525u + 1013904223u;
      const float unit = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
      const float envelope = 0.25f + 0.75f * static_cast<float>((f + 2 * t) % 7) / 6.0f;
      S[static_cast<std::size_t>(f) * n_frames + t] = 0.05f + unit * envelope;
    }
  }
  return S;
}

void require_bit_equal(const std::vector<float>& got, const std::vector<float>& want) {
  REQUIRE(got.size() == want.size());
  for (std::size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], want[i]);
    REQUIRE(std::isfinite(got[i]));
    REQUIRE(got[i] == want[i]);
  }
}

/// @brief Distance in representable floats between two non-negative finite values.
/// @details IEEE-754 orders non-negative floats monotonically as int32, so the bit patterns
///          subtract directly. The caller checks finiteness and sign first.
long long ulp_distance(float a, float b) {
  std::int32_t ia = 0;
  std::int32_t ib = 0;
  std::memcpy(&ia, &a, sizeof(ia));
  std::memcpy(&ib, &b, sizeof(ib));
  return std::llabs(static_cast<long long>(ia) - static_cast<long long>(ib));
}

/// @brief How two results disagree: how many cells, and the largest distance among them.
struct Disagreement {
  std::size_t total = 0;
  std::size_t count = 0;
  long long worst_ulps = 0;
  std::size_t worst_index = 0;
};

Disagreement disagreement_of(const std::vector<float>& got, const std::vector<float>& want) {
  Disagreement d;
  d.total = got.size();
  for (std::size_t i = 0; i < got.size(); ++i) {
    if (got[i] == want[i]) continue;
    ++d.count;
    const long long u = ulp_distance(got[i], want[i]);
    if (u > d.worst_ulps) {
      d.worst_ulps = u;
      d.worst_index = i;
    }
  }
  return d;
}

/// @brief Bounds how far two results may disagree in ulps, placing no bound on how many
///        cells do.
/// @details For an array where exactness is not available and which carries no ordering
///          claim. Scans every cell rather than stopping at the first, so a failure reports
///          the count alongside the bound that it broke.
void require_ulp_bounded(const std::vector<float>& got, const std::vector<float>& want,
                         long long max_ulps) {
  REQUIRE(got.size() == want.size());
  bool representable = true;
  for (float v : got) representable = representable && std::isfinite(v) && v >= 0.0f;
  for (float v : want) representable = representable && std::isfinite(v) && v >= 0.0f;
  REQUIRE(representable);  // precondition for the ulp arithmetic below

  const Disagreement d = disagreement_of(got, want);
  const double worst_got = d.count > 0 ? static_cast<double>(got[d.worst_index]) : 0.0;
  const double worst_want = d.count > 0 ? static_cast<double>(want[d.worst_index]) : 0.0;
  CAPTURE(d.total, d.count, d.worst_ulps, max_ulps, d.worst_index, worst_got, worst_want);
  REQUIRE(d.worst_ulps <= max_ulps);
}

/// @brief Bounds how many cells of two results may disagree, placing no bound on how far.
/// @details The count is what separates per-TU codegen noise from a wrong traversal order in an
///          array that carries no exactness claim; the ulp distance does not separate them.
void require_cell_count_bounded(const std::vector<float>& got, const std::vector<float>& want,
                                std::size_t max_cells) {
  REQUIRE(got.size() == want.size());
  const Disagreement d = disagreement_of(got, want);
  CAPTURE(d.total, d.count, max_cells, d.worst_ulps);
  REQUIRE(d.count <= max_cells);
}

/// @brief Relative Frobenius error of the factorisation, ||S - W H|| / ||S||.
/// @details Accumulated in double so the comparison between iteration counts is not itself
///          measuring float rounding.
double reconstruction_error(const float* S, const DecomposeResult& r, int n_features, int n_frames,
                            int n_components) {
  double err = 0.0;
  double ref = 0.0;
  for (int f = 0; f < n_features; ++f) {
    for (int t = 0; t < n_frames; ++t) {
      double model = 0.0;
      for (int c = 0; c < n_components; ++c) {
        model += static_cast<double>(r.W[f * n_components + c]) *
                 static_cast<double>(r.H[c * n_frames + t]);
      }
      const double target = static_cast<double>(S[f * n_frames + t]);
      err += (target - model) * (target - model);
      ref += target * target;
    }
  }
  return ref > 0.0 ? std::sqrt(err / ref) : 0.0;
}

void require_finite_non_negative(const DecomposeResult& r) {
  for (float v : r.W) {
    CAPTURE(v);
    REQUIRE(std::isfinite(v));
    REQUIRE(v >= 0.0f);
  }
  for (float v : r.H) {
    CAPTURE(v);
    REQUIRE(std::isfinite(v));
    REQUIRE(v >= 0.0f);
  }
}

/// @brief decompose()'s MU loop from an explicit (W, H), in its original traversals.
/// @details Two shapes the library replaced live here: `rebuild()` derives each WH cell with a
///          reduction over components, and the H update's inner loop over features strides
///          num_feat and den_feat. The seed is a parameter because comparing a second update
///          step requires starting it from the library's own first-step output; see the test
///          cases for why the iteration count is what decides the comparison.
DecomposeResult oracle_mu_steps(const float* S, DecomposeResult out, int n_features, int n_frames,
                                int n_components, int n_iter, float beta) {
  constexpr float kEps = constants::kAmpEpsilon;

  const float exp_num = beta - 2.0f;
  const float exp_den = beta - 1.0f;
  const std::size_t cells = static_cast<std::size_t>(n_features) * n_frames;
  std::vector<float> WH(cells, 0.0f);
  std::vector<float> num_feat(cells, 0.0f);
  std::vector<float> den_feat(cells, 0.0f);

  auto rebuild = [&]() {
    for (int f = 0; f < n_features; ++f) {
      for (int t = 0; t < n_frames; ++t) {
        float s = 0.0f;
        for (int c = 0; c < n_components; ++c) {
          s += out.W[f * n_components + c] * out.H[c * n_frames + t];
        }
        WH[static_cast<std::size_t>(f) * n_frames + t] = s;
      }
    }
    for (std::size_t i = 0; i < cells; ++i) {
      const float wh = WH[i] + kEps;
      const float pow_num = (exp_num == 0.0f) ? 1.0f : std::pow(wh, exp_num);
      const float pow_den = (exp_den == 0.0f) ? 1.0f : std::pow(wh, exp_den);
      num_feat[i] = S[i] * pow_num;
      den_feat[i] = pow_den;
    }
  };

  for (int it = 0; it < n_iter; ++it) {
    rebuild();
    for (int c = 0; c < n_components; ++c) {
      for (int t = 0; t < n_frames; ++t) {
        float num = 0.0f;
        float den = 0.0f;
        for (int f = 0; f < n_features; ++f) {
          const float w = out.W[f * n_components + c];
          num += w * num_feat[f * n_frames + t];
          den += w * den_feat[f * n_frames + t];
        }
        out.H[c * n_frames + t] *= num / (den + kEps);
      }
    }
    rebuild();
    for (int f = 0; f < n_features; ++f) {
      for (int c = 0; c < n_components; ++c) {
        float num = 0.0f;
        float den = 0.0f;
        for (int t = 0; t < n_frames; ++t) {
          const float h = out.H[c * n_frames + t];
          num += num_feat[f * n_frames + t] * h;
          den += den_feat[f * n_frames + t] * h;
        }
        out.W[f * n_components + c] *= num / (den + kEps);
      }
    }
  }
  return out;
}

/// @brief The same loop seeded from the library's own initialisation.
/// @details decompose(n_iter=0) returns the seed, so the oracle inherits the NNDSVD singular
///          vectors, which are not reproducible here.
DecomposeResult oracle_decompose(const float* S, int n_features, int n_frames, int n_components,
                                 int n_iter, float beta, const std::string& init) {
  return oracle_mu_steps(
      S, decompose(S, n_features, n_frames, n_components, /*n_iter=*/0, "mu", beta, init),
      n_features, n_frames, n_components, n_iter, beta);
}

/// @brief nn_filter() with the column norms and the cosine similarities in their original
///        frame-major traversals, both of which stride a column of S.
/// @details The similarities only reach the output through the neighbour ranking, so this also
///          catches a reordering that moves a similarity by one bit across a ranking tie.
std::vector<float> oracle_nn_filter(const float* S, int n_features, int n_frames,
                                    const std::string& aggregate, int k, int width) {
  if (k <= 0) k = std::min(5, n_frames);

  std::vector<float> norms(n_frames, 0.0f);
  for (int t = 0; t < n_frames; ++t) {
    float s = 0.0f;
    for (int f = 0; f < n_features; ++f) {
      const float v = S[f * n_frames + t];
      s += v * v;
    }
    norms[t] = std::sqrt(s);
  }

  std::vector<std::vector<int>> selectors_for(n_frames);
  std::vector<std::pair<float, int>> sims;
  const int n_neighbors = std::min(n_frames - 1, k + 2 * width);
  for (int i = 0; i < n_frames; ++i) {
    sims.clear();
    for (int j = 0; j < n_frames; ++j) {
      if (j == i) continue;
      float dot = 0.0f;
      for (int f = 0; f < n_features; ++f) {
        dot += S[f * n_frames + i] * S[f * n_frames + j];
      }
      const float sim = (norms[i] > 0.0f && norms[j] > 0.0f) ? dot / (norms[i] * norms[j]) : 0.0f;
      sims.push_back({sim, j});
    }
    if (sims.empty()) continue;
    const int nn = std::min(n_neighbors, static_cast<int>(sims.size()));
    std::partial_sort(sims.begin(), sims.begin() + nn, sims.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<int> kept;
    for (int q = 0; q < nn; ++q) {
      const int j = sims[q].second;
      if (std::abs(i - j) < width) continue;
      kept.push_back(j);
    }
    std::sort(kept.begin(), kept.end());
    const int kk = std::min<int>(k, static_cast<int>(kept.size()));
    for (int q = 0; q < kk; ++q) selectors_for[i].push_back(kept[q]);
  }

  std::vector<float> out(static_cast<std::size_t>(n_features) * n_frames, 0.0f);
  for (int t = 0; t < n_frames; ++t) {
    const std::vector<int>& selectors = selectors_for[t];
    if (selectors.empty()) {
      for (int f = 0; f < n_features; ++f) out[f * n_frames + t] = S[f * n_frames + t];
      continue;
    }
    if (aggregate == "median") {
      for (int f = 0; f < n_features; ++f) {
        std::vector<float> vals(selectors.size());
        for (std::size_t q = 0; q < selectors.size(); ++q) vals[q] = S[f * n_frames + selectors[q]];
        const auto mid = vals.begin() + vals.size() / 2;
        std::nth_element(vals.begin(), mid, vals.end());
        float median = *mid;
        if ((vals.size() % 2) == 0) {
          const float lower = *std::max_element(vals.begin(), mid);
          median = 0.5f * (lower + median);
        }
        out[f * n_frames + t] = median;
      }
    } else if (aggregate == "min") {
      for (int f = 0; f < n_features; ++f) {
        float m = std::numeric_limits<float>::infinity();
        for (int i : selectors) m = std::min(m, S[f * n_frames + i]);
        out[f * n_frames + t] = m;
      }
    } else if (aggregate == "max") {
      for (int f = 0; f < n_features; ++f) {
        float m = -std::numeric_limits<float>::infinity();
        for (int i : selectors) m = std::max(m, S[f * n_frames + i]);
        out[f * n_frames + t] = m;
      }
    } else {
      const float inv_count = 1.0f / static_cast<float>(selectors.size());
      for (int f = 0; f < n_features; ++f) {
        float s = 0.0f;
        for (int i : selectors) s += S[f * n_frames + i];
        out[f * n_frames + t] = s * inv_count;
      }
    }
  }
  return out;
}

}  // namespace

TEST_CASE("decompose matches a frame-major oracle on H at one update step", "[util][decompose]") {
  // H is compared exactly and W is not, and that asymmetry is the point: the H update is the
  // traversal this case guards, and reversing its feature loop moves 48 of 87 H cells. The
  // oracle is a reimplementation in another translation unit and FMA contraction is decided per
  // TU, so nothing makes W exact -- but W's disagreement is bounded on both axes, because the
  // count separates codegen from a wrong order where no ulp distance can. One iteration,
  // because both shapes round alike on the seed and part on its image: divergence starts at
  // step 2.
  constexpr long long kMaxUlpsW = 16;     // codegen reaches 3 ulp; a wrong row stride, 1.7e6
  constexpr std::size_t kMaxCellsW = 16;  // codegen moves 9 of 39 cells; a reversed order, 26

  const int n_features = 13;
  const int n_frames = 29;
  const int n_components = 3;
  const std::vector<float> S = traversal_fixture(n_features, n_frames, 0x5eedu);

  // beta 2 leaves both WH exponents at their identity shortcut; beta 1 (KL) takes the
  // std::pow path on both, so the numerator the H update consumes is a different matrix.
  for (float beta : {2.0f, 1.0f}) {
    for (const std::string& init : {std::string("nndsvd"), std::string("random")}) {
      CAPTURE(beta, init);
      const DecomposeResult got =
          decompose(S.data(), n_features, n_frames, n_components, 1, "mu", beta, init);
      const DecomposeResult want =
          oracle_decompose(S.data(), n_features, n_frames, n_components, 1, beta, init);
      require_bit_equal(got.H, want.H);
      require_ulp_bounded(got.W, want.W, kMaxUlpsW);
      require_cell_count_bounded(got.W, want.W, kMaxCellsW);
    }
  }
}

TEST_CASE("decompose matches the oracle on a second update step", "[util][decompose]") {
  // The first step cannot see a WH buffer that is accumulated into rather than overwritten:
  // WH is constructed zero-filled, so the first product is right whether or not the buffer is
  // reset, and H is updated from that first product alone. The defect appears at the second
  // step and nowhere earlier. Measured: an accumulating buffer leaves H exact at one
  // iteration and moves all 87 cells at two.
  //
  // Two steps are compared without inheriting the first step's drift by seeding the oracle
  // from the library's own n_iter=1 result, which is exactly the state the library's second
  // iteration begins from. What is left is one update step from a shared seed, which is the
  // comparison the case above shows comes back exact; H is exact here for all four
  // beta/init combinations. W is bounded on both axes for the reason given there.
  constexpr long long kMaxUlpsW = 16;     // codegen reaches 2 ulp here; a wrong row stride, 1.7e6
  constexpr std::size_t kMaxCellsW = 16;  // codegen moves 7 of 39 cells; a reversed order, 26

  const int n_features = 13;
  const int n_frames = 29;
  const int n_components = 3;
  const std::vector<float> S = traversal_fixture(n_features, n_frames, 0x5eedu);

  for (float beta : {2.0f, 1.0f}) {
    for (const std::string& init : {std::string("nndsvd"), std::string("random")}) {
      CAPTURE(beta, init);
      const DecomposeResult seed =
          decompose(S.data(), n_features, n_frames, n_components, 1, "mu", beta, init);
      const DecomposeResult got =
          decompose(S.data(), n_features, n_frames, n_components, 2, "mu", beta, init);
      const DecomposeResult want =
          oracle_mu_steps(S.data(), seed, n_features, n_frames, n_components, 1, beta);
      require_bit_equal(got.H, want.H);
      require_ulp_bounded(got.W, want.W, kMaxUlpsW);
      require_cell_count_bounded(got.W, want.W, kMaxCellsW);
    }
  }
}

TEST_CASE("decompose matches the oracle on single-row and single-column inputs",
          "[util][decompose]") {
  // Extent coverage, not order coverage: at these shapes the feature loop is one iteration
  // long or its accumulator holds one frame, so neither a reversed feature order nor a wrong
  // row stride moves a single value (0 of 2 and 0 of 9 for both). What is left is that the
  // index arithmetic still addresses the right cells where one extent collapses.
  // W is ulp-bounded rather than exact for the reason given in the case above; it happens to
  // be exact at these extents today, which is not something to depend on.
  constexpr long long kMaxUlpsW = 16;

  SECTION("one frame") {
    const int n_features = 7;
    const std::vector<float> S = traversal_fixture(n_features, 1, 0xc0ffeeu);
    const DecomposeResult got =
        decompose(S.data(), n_features, 1, /*n_components=*/2, 1, "mu", 2.0f, "random");
    const DecomposeResult want =
        oracle_decompose(S.data(), n_features, 1, /*n_components=*/2, 1, 2.0f, "random");
    require_bit_equal(got.H, want.H);
    require_ulp_bounded(got.W, want.W, kMaxUlpsW);
  }

  SECTION("one feature") {
    const int n_frames = 9;
    const std::vector<float> S = traversal_fixture(1, n_frames, 0xb0bau);
    const DecomposeResult got =
        decompose(S.data(), 1, n_frames, /*n_components=*/1, 1, "mu", 2.0f, "random");
    const DecomposeResult want =
        oracle_decompose(S.data(), 1, n_frames, /*n_components=*/1, 1, 2.0f, "random");
    require_bit_equal(got.H, want.H);
    require_ulp_bounded(got.W, want.W, kMaxUlpsW);
  }
}

TEST_CASE("decompose stays finite and descends over the full iteration count",
          "[util][decompose]") {
  // What 200 iterations can carry is not a value comparison. The per-TU codegen spread grows
  // with the iteration count and overtakes what a traversal error produces -- at 200 it is
  // 1.11e-05 against 9.01e-06 -- so any tolerance wide enough to be reliably green admits the
  // drift this file exists to exclude, and the crossover is near 50 rather than at the end.
  // The two cases at the top of this file already cover shape and non-negativity at 50
  // iterations on a 3x3; new here are depth, finiteness -- which `v >= 0` does not give,
  // since +Inf passes it -- and the descent itself, which nothing else asserts.
  const int n_features = 13;
  const int n_frames = 29;
  const int n_components = 3;
  const std::vector<float> S = traversal_fixture(n_features, n_frames, 0x5eedu);

  for (const std::string& init : {std::string("nndsvd"), std::string("random")}) {
    CAPTURE(init);
    // beta 2 is the Frobenius case, so the multiplicative updates descend exactly the error
    // measured here. Checkpoints rather than a threshold: monotonicity is the property the
    // updates guarantee, and it needs no fixture-dependent constant to state.
    double previous = std::numeric_limits<double>::infinity();
    for (int n_iter : {1, 10, 50, 200}) {
      CAPTURE(n_iter);
      const DecomposeResult r =
          decompose(S.data(), n_features, n_frames, n_components, n_iter, "mu", 2.0f, init);
      REQUIRE(r.W.size() == static_cast<std::size_t>(n_features) * n_components);
      REQUIRE(r.H.size() == static_cast<std::size_t>(n_components) * n_frames);
      require_finite_non_negative(r);

      const double error = reconstruction_error(S.data(), r, n_features, n_frames, n_components);
      CAPTURE(error, previous);
      REQUIRE(error <= previous);
      previous = error;
    }
    // Strictly below the first step, so the assertion above cannot be satisfied by standing
    // still for 199 iterations.
    const DecomposeResult one =
        decompose(S.data(), n_features, n_frames, n_components, 1, "mu", 2.0f, init);
    REQUIRE(previous < reconstruction_error(S.data(), one, n_features, n_frames, n_components));
  }

  // beta 1 minimises the KL divergence, not the error measured above, so only the invariants
  // carry over to it.
  require_finite_non_negative(
      decompose(S.data(), n_features, n_frames, n_components, 200, "mu", 1.0f, "nndsvd"));
}

TEST_CASE("nn_filter matches a frame-major oracle for every aggregator", "[util][decompose]") {
  // What this reaches: a wrong element, a dropped feature, and a scratch buffer carried
  // between frames without being reset or resized. What it cannot reach: a reordered
  // summation inside the norms or the dot products, because neither leaves the function --
  // they are consumed by the neighbour ranking, and a last-bit move does not flip it. That
  // order is held by construction instead (each accumulator still takes its terms in feature
  // order), and a fixture tuned to sit on a ranking tie would only make the test flaky.
  const int n_features = 11;
  const int n_frames = 37;
  const std::vector<float> S = traversal_fixture(n_features, n_frames, 0xdecafu);

  for (const std::string& aggregate :
       {std::string("mean"), std::string("median"), std::string("min"), std::string("max")}) {
    for (int width : {0, 2}) {
      CAPTURE(aggregate, width);
      const std::vector<float> got =
          nn_filter(S.data(), n_features, n_frames, aggregate, /*k=*/4, width);
      const std::vector<float> want =
          oracle_nn_filter(S.data(), n_features, n_frames, aggregate, /*k=*/4, width);
      require_bit_equal(got, want);
    }
  }

  // Few enough frames that the exclusion band leaves some of them fewer than k survivors, so
  // the gathered count changes from frame to frame and crosses the median's odd/even split.
  // A buffer reused across frames that only ever grows reads a stale tail here and nowhere
  // in the case above, where every frame selects exactly k.
  const std::vector<float> narrow = traversal_fixture(6, 6, 0xfeedu);
  for (const std::string& aggregate :
       {std::string("mean"), std::string("median"), std::string("min"), std::string("max")}) {
    CAPTURE(aggregate);
    require_bit_equal(nn_filter(narrow.data(), 6, 6, aggregate, /*k=*/4, /*width=*/2),
                      oracle_nn_filter(narrow.data(), 6, 6, aggregate, /*k=*/4, /*width=*/2));
  }

  // More features than frames. Everywhere else here the feature extent is the smaller of the
  // two, so a loop bound taken from the frame extent runs past the end rather than stopping
  // short of the last rows, and only this shape turns that into a plain wrong answer.
  const std::vector<float> tall = traversal_fixture(37, 11, 0xc0a1u);
  for (const std::string& aggregate :
       {std::string("mean"), std::string("median"), std::string("min"), std::string("max")}) {
    CAPTURE(aggregate);
    require_bit_equal(nn_filter(tall.data(), 37, 11, aggregate, /*k=*/4, /*width=*/2),
                      oracle_nn_filter(tall.data(), 37, 11, aggregate, /*k=*/4, /*width=*/2));
  }
}

TEST_CASE("nn_filter matches the oracle on single-row and single-column inputs",
          "[util][decompose]") {
  SECTION("one frame") {
    // No neighbour survives, so every output column is a copy -- the branch that reads S and
    // writes out with the same stride. All four aggregators run it because the copy is shared
    // between them rather than repeated per aggregator.
    const int n_features = 5;
    const std::vector<float> S = traversal_fixture(n_features, 1, 0x1234u);
    for (const std::string& aggregate :
         {std::string("mean"), std::string("median"), std::string("min"), std::string("max")}) {
      CAPTURE(aggregate);
      require_bit_equal(nn_filter(S.data(), n_features, 1, aggregate, /*k=*/3, /*width=*/1),
                        oracle_nn_filter(S.data(), n_features, 1, aggregate, /*k=*/3, /*width=*/1));
    }
  }

  SECTION("one feature") {
    // Every column is a scalar, so the cosine similarities collapse to 1 and the neighbour set
    // is decided entirely by the column-index tie-break.
    const int n_frames = 13;
    const std::vector<float> S = traversal_fixture(1, n_frames, 0x4321u);
    for (const std::string& aggregate : {std::string("mean"), std::string("max")}) {
      CAPTURE(aggregate);
      require_bit_equal(nn_filter(S.data(), 1, n_frames, aggregate, /*k=*/3, /*width=*/2),
                        oracle_nn_filter(S.data(), 1, n_frames, aggregate, /*k=*/3, /*width=*/2));
    }
  }
}
