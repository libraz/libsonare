/// @file mu_quantization_domain_test.cpp
/// @brief Domain and compatibility tests for quantized mu-law compression.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <climits>
#include <cmath>
#include <iterator>
#include <limits>
#include <new>
#include <random>
#include <vector>

#include "core/audio_ops.h"
#include "util/exception.h"

#if defined(SONARE_WITH_MIXING)
#include "support/alloc_guard.h"
#endif

namespace {

std::vector<float> legacy_materialized_mu_quantize(const std::vector<float>& input, int mu) {
  const float mu_f = static_cast<float>(mu);
  const float log1p_mu = std::log1p(mu_f);

  std::vector<float> compressed(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    const float value = input[i];
    const float sign = (value > 0.0f) ? 1.0f : (value < 0.0f) ? -1.0f : 0.0f;
    compressed[i] = sign * std::log1p(mu_f * std::abs(value)) / log1p_mu;
  }

  const int n_edges = 1 + mu;
  std::vector<float> edges(static_cast<std::size_t>(n_edges));
  for (int k = 0; k < n_edges; ++k) {
    edges[static_cast<std::size_t>(k)] =
        -1.0f + 2.0f * static_cast<float>(k) / static_cast<float>(mu);
  }

  const int offset = (mu + 1) / 2;
  for (float& value : compressed) {
    const auto edge = std::lower_bound(edges.begin(), edges.end(), value);
    const auto index = static_cast<int>(std::distance(edges.begin(), edge));
    value = static_cast<float>(index - offset);
  }
  return compressed;
}

std::vector<float> make_ordinary_domain_inputs(int mu) {
  std::vector<float> input;
  input.reserve(2200 + static_cast<std::size_t>(mu + 1) * 3);

  // A dense sweep covers the complete legal domain, including both endpoints.
  for (int i = 0; i <= 2000; ++i) {
    input.push_back(-1.0f + 2.0f * static_cast<float>(i) / 2000.0f);
  }

  // Exercise each materialized threshold and its nearest representable input
  // neighbors after applying the inverse mu-law transform.
  const float mu_f = static_cast<float>(mu);
  for (int k = 0; k <= mu; ++k) {
    const float edge = -1.0f + 2.0f * static_cast<float>(k) / mu_f;
    const float sign = (edge > 0.0f) ? 1.0f : (edge < 0.0f) ? -1.0f : 0.0f;
    const float x = sign * (std::pow(1.0f + mu_f, std::abs(edge)) - 1.0f) / mu_f;
    input.push_back(x);

    const float below = std::nextafter(x, -std::numeric_limits<float>::infinity());
    const float above = std::nextafter(x, std::numeric_limits<float>::infinity());
    if (below >= -1.0f) input.push_back(below);
    if (above <= 1.0f) input.push_back(above);
  }

  input.push_back(-1.0f);
  input.push_back(1.0f);
  input.push_back(-0.0f);
  input.push_back(0.0f);
  input.push_back(std::numeric_limits<float>::quiet_NaN());
  return input;
}

std::vector<float> make_edge_boundary_inputs(int mu, std::size_t count) {
  const float mu_f = static_cast<float>(mu);
  const int k = mu / 2;
  const float edge = -1.0f + 2.0f * static_cast<float>(k) / mu_f;
  const float sign = (edge > 0.0f) ? 1.0f : (edge < 0.0f) ? -1.0f : 0.0f;
  const float x = sign * (std::pow(1.0f + mu_f, std::abs(edge)) - 1.0f) / mu_f;

  std::vector<float> input{
      -1.0f,
      1.0f,
      std::nextafter(x, -std::numeric_limits<float>::infinity()),
      x,
      std::nextafter(x, std::numeric_limits<float>::infinity()),
      -0.0f,
      std::numeric_limits<float>::quiet_NaN(),
  };
  while (input.size() < count) input.push_back(0.0f);
  input.resize(count);
  return input;
}

#if defined(SONARE_WITH_MIXING)
constexpr std::size_t kLargeAllocationThreshold = 64 * 1024;
#endif

}  // namespace

TEST_CASE("mu quantization matches the materialized legacy edge oracle",
          "[mu_quantization][audio_ops]") {
  for (const int mu : {1, 2, 3, 4, 255}) {
    const std::vector<float> input = make_ordinary_domain_inputs(mu);
    const std::vector<float> expected = legacy_materialized_mu_quantize(input, mu);
    const std::vector<float> actual = sonare::mu_compress(input, mu, /*quantize=*/true);

    CAPTURE(mu);
    REQUIRE(actual == expected);
  }
}

TEST_CASE("mu quantization preserves ordinary unquantized compression",
          "[mu_quantization][audio_ops]") {
  const std::vector<float> input{-1.0f, -0.5f, -0.0f, 0.0f, 0.125f, 0.5f, 1.0f};
  for (const int mu : {1, 2, 255}) {
    const float mu_f = static_cast<float>(mu);
    const float denominator = std::log1p(mu_f);
    const std::vector<float> actual = sonare::mu_compress(input, mu, /*quantize=*/false);

    REQUIRE(actual.size() == input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
      const float value = input[i];
      const float sign = (value > 0.0f) ? 1.0f : (value < 0.0f) ? -1.0f : 0.0f;
      const float expected = sign * std::log1p(mu_f * std::abs(value)) / denominator;
      const float tolerance =
          16.0f * std::numeric_limits<float>::epsilon() * std::max(1.0f, std::abs(expected));
      CAPTURE(mu, i);
      CHECK(std::abs(actual[i] - expected) <= tolerance);
    }
  }
}

TEST_CASE("mu quantization matches at the materialization crossover",
          "[mu_quantization][audio_ops]") {
#if defined(SONARE_WITH_MIXING)
  const std::vector<float> allocation_control_input{0.25f, -0.5f, 0.75f};
  std::vector<float> allocation_control_result;
  std::size_t control_allocations = 0;
  {
    sonare::test::AllocationGuard allocation_guard;
    allocation_control_result =
        sonare::mu_compress(allocation_control_input, 255, /*quantize=*/false);
    control_allocations = allocation_guard.count();
  }
  REQUIRE(control_allocations > 0);
  REQUIRE(allocation_control_result.size() == allocation_control_input.size());
#endif

  // mu=7 has eight edges; mu=8 has nine. The n=edge_count-1 call must use
  // implicit search, while n=edge_count takes the exact materialized path.
  for (const int mu : {7, 8}) {
    const std::size_t edge_count = static_cast<std::size_t>(mu) + 1;
    for (const std::size_t input_count : {edge_count - 1, edge_count}) {
      const std::vector<float> input = make_edge_boundary_inputs(mu, input_count);
      const std::vector<float> expected = legacy_materialized_mu_quantize(input, mu);
      std::vector<float> actual;
#if defined(SONARE_WITH_MIXING)
      std::size_t allocations = 0;
      {
        sonare::test::AllocationGuard allocation_guard;
        actual = sonare::mu_compress(input, mu, /*quantize=*/true);
        allocations = allocation_guard.count();
      }
      const std::size_t expected_allocations = (input_count < edge_count) ? 1 : 2;
      CAPTURE(mu, input_count, allocations);
      CHECK(allocations == expected_allocations);
#else
      actual = sonare::mu_compress(input, mu, /*quantize=*/true);
#endif
      CAPTURE(mu, input_count);
      REQUIRE(actual == expected);
    }
  }
}

TEST_CASE("mu quantization stays implicit above the edge table cap",
          "[mu_quantization][audio_ops]") {
  constexpr int kMu = 65536;
  constexpr std::size_t kEdgeCount = static_cast<std::size_t>(kMu) + 1;
  const std::vector<float> input(kEdgeCount, 0.25f);
  const std::vector<float> expected = legacy_materialized_mu_quantize(input, kMu);
  std::vector<float> actual;

#if defined(SONARE_WITH_MIXING)
  std::size_t allocations = 0;
  {
    sonare::test::AllocationGuard allocation_guard;
    actual = sonare::mu_compress(input, kMu, /*quantize=*/true);
    allocations = allocation_guard.count();
  }
  // Only the result vector should allocate; an edge table would add a second
  // allocation of the same order as the input.
  CAPTURE(allocations);
  REQUIRE(allocations == 1);
#else
  actual = sonare::mu_compress(input, kMu, /*quantize=*/true);
#endif

  REQUIRE(actual == expected);
}

TEST_CASE("high-mu implicit quantization matches selected materialized edges",
          "[mu_quantization][audio_ops]") {
  constexpr int kMu = 65535;
  constexpr float kMuFloat = static_cast<float>(kMu);
  std::vector<float> input;
  input.reserve(1400);

  for (int i = 0; i <= 1000; ++i) {
    input.push_back(-1.0f + 2.0f * static_cast<float>(i) / 1000.0f);
  }

  std::mt19937 random(0x5a17u);
  std::uniform_real_distribution<float> sample(-1.0f, 1.0f);
  for (int i = 0; i < 256; ++i) input.push_back(sample(random));

  for (const int k : {0, 1, kMu / 4, kMu / 2, 3 * kMu / 4, kMu - 1, kMu}) {
    const float edge = -1.0f + 2.0f * static_cast<float>(k) / kMuFloat;
    const float sign = (edge > 0.0f) ? 1.0f : (edge < 0.0f) ? -1.0f : 0.0f;
    const float x = sign * (std::pow(1.0f + kMuFloat, std::abs(edge)) - 1.0f) / kMuFloat;
    input.push_back(x);
    const float below = std::nextafter(x, -std::numeric_limits<float>::infinity());
    const float above = std::nextafter(x, std::numeric_limits<float>::infinity());
    if (below >= -1.0f) input.push_back(below);
    if (above <= 1.0f) input.push_back(above);
  }
  input.push_back(-1.0f);
  input.push_back(1.0f);
  input.push_back(-0.0f);
  input.push_back(0.0f);
  input.push_back(std::numeric_limits<float>::quiet_NaN());
  REQUIRE(input.size() < static_cast<std::size_t>(kMu) + 1);

  const std::vector<float> expected = legacy_materialized_mu_quantize(input, kMu);
  const std::vector<float> actual = sonare::mu_compress(input, kMu, /*quantize=*/true);
  REQUIRE(actual == expected);
}

TEST_CASE("high-mu quantization is finite, deterministic, and bounded",
          "[mu_quantization][audio_ops]") {
  const std::vector<float> input{-1.0f, -0.5f, -0.0f, 0.0f, 0.25f, 0.5f, 1.0f};

#if defined(SONARE_WITH_MIXING)
  // Probe the allocation counter through a core API result consumed after the
  // guard, so the result allocation cannot be optimized away.
  std::vector<float> allocation_count_probe;
  std::size_t control_allocations = 0;
  {
    sonare::test::AllocationGuard guard;
    allocation_count_probe = sonare::mu_compress(input, 255, /*quantize=*/false);
    control_allocations = guard.count();
  }
  REQUIRE(control_allocations > 0);
  REQUIRE(allocation_count_probe.size() == input.size());

  // The input is allocated before arming the hook. Its non-quantized result is
  // larger than the threshold, so the core API must attempt a blocked output
  // allocation. Observe the escaped result after the hook is disarmed.
  const std::vector<float> failure_probe_input(kLargeAllocationThreshold / sizeof(float) + 1,
                                               0.25f);
  std::vector<float> failure_probe_result;
  bool allocation_was_blocked = false;
  {
    sonare::test::AllocationFailureGuard guard(kLargeAllocationThreshold);
    try {
      failure_probe_result = sonare::mu_compress(failure_probe_input, 255, /*quantize=*/false);
    } catch (const std::bad_alloc&) {
      allocation_was_blocked = true;
    }
  }
  REQUIRE(allocation_was_blocked);
  REQUIRE(failure_probe_result.empty());
#endif

  for (const int mu : {65535, INT_MAX}) {
    std::vector<float> first;
    std::vector<float> second;
    bool empty_result = false;
    bool completed = false;

#if defined(SONARE_WITH_MIXING)
    // Short input must not materialize the large edge table. This also bounds
    // the legacy INT_MAX overflow case if the tests are run before the fix.
    {
      sonare::test::AllocationFailureGuard guard(kLargeAllocationThreshold);
      try {
        first = sonare::mu_compress(input, mu, /*quantize=*/true);
        second = sonare::mu_compress(input, mu, /*quantize=*/true);
        empty_result = sonare::mu_compress(std::vector<float>{}, mu, /*quantize=*/true).empty();
        completed = true;
      } catch (...) {
        completed = false;
      }
    }
#else
    first = sonare::mu_compress(input, mu, /*quantize=*/true);
    second = sonare::mu_compress(input, mu, /*quantize=*/true);
    empty_result = sonare::mu_compress(std::vector<float>{}, mu, /*quantize=*/true).empty();
    completed = true;
#endif

    CAPTURE(mu);
    REQUIRE(completed);
    REQUIRE(empty_result);
    REQUIRE(first == second);
    REQUIRE(first.size() == input.size());
    REQUIRE(
        std::all_of(first.begin(), first.end(), [](float value) { return std::isfinite(value); }));
  }
}
