/// @file dct_dimension_domain_test.cpp
/// @brief DCT dimension validation and scalar-formula controls.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <thread>
#include <vector>

#include "filters/dct.h"
#include "util/constants.h"
#include "util/exception.h"

#if defined(SONARE_WITH_MIXING)
#include "support/alloc_guard.h"
#endif

namespace {

#if defined(SONARE_WITH_MIXING)
constexpr std::size_t kLargeAllocationThreshold = 64 * 1024;
#endif

float scaled_float_tolerance(float expected) {
  return 16.0f * std::numeric_limits<float>::epsilon() * std::max(1.0f, std::abs(expected));
}

template <typename Operation>
bool throws_invalid_parameter(Operation&& operation) {
  bool rejected = false;
#if defined(SONARE_WITH_MIXING)
  sonare::test::AllocationFailureGuard allocation_guard(kLargeAllocationThreshold);
#endif
  try {
    operation();
  } catch (const sonare::SonareException& error) {
    rejected = error.code() == sonare::ErrorCode::InvalidParameter;
  } catch (...) {
  }
  return rejected;
}

float scalar_dct_matrix_value(int k, int n, int n_input) {
  const float scale = std::sqrt(2.0f / n_input);
  const float k_scale = (k == 0) ? scale / std::sqrt(2.0f) : scale;
  const float angle = sonare::constants::kPi * k * (2.0f * n + 1.0f) / (2.0f * n_input);
  return k_scale * std::cos(angle);
}

float scalar_dct_output(const std::vector<float>& input, int k) {
  const int n_input = static_cast<int>(input.size());
  float sum = 0.0f;
  for (int n = 0; n < n_input; ++n) {
    sum += input[static_cast<std::size_t>(n)] * scalar_dct_matrix_value(k, n, n_input);
  }
  return sum;
}

float scalar_idct_output(const std::vector<float>& input, int n_output, int n) {
  float sum = 0.0f;
  for (int k = 0; k < static_cast<int>(input.size()); ++k) {
    sum += input[static_cast<std::size_t>(k)] * scalar_dct_matrix_value(k, n, n_output);
  }
  return sum;
}

}  // namespace

TEST_CASE("DCT matrix and transform retain the scalar formula on rectangular shapes", "[dct]") {
  const std::vector<float> input{0.25f, -0.5f, 0.75f, 1.0f};
  const std::vector<float> matrix = sonare::create_dct_matrix(3, 4);
  REQUIRE(matrix.size() == 12);
  for (int k = 0; k < 3; ++k) {
    for (int n = 0; n < 4; ++n) {
      const float expected = scalar_dct_matrix_value(k, n, 4);
      const float actual = matrix[static_cast<std::size_t>(k) * 4 + n];
      CAPTURE(k, n);
      CHECK(std::abs(actual - expected) <= scaled_float_tolerance(expected));
    }
  }

  const std::vector<float> transformed = sonare::dct_ii(input, 3);
  REQUIRE(transformed.size() == 3);
  for (int k = 0; k < 3; ++k) {
    const float expected = scalar_dct_output(input, k);
    const float actual = transformed[static_cast<std::size_t>(k)];
    CAPTURE(k);
    CHECK(std::abs(actual - expected) <= scaled_float_tolerance(expected));
  }

  const std::vector<float> coefficients{0.5f, -0.25f, 0.75f};
  const std::vector<float> reconstructed = sonare::idct_ii(coefficients.data(), 3, 4);
  REQUIRE(reconstructed.size() == 4);
  for (int n = 0; n < 4; ++n) {
    const float expected = scalar_idct_output(coefficients, 4, n);
    const float actual = reconstructed[static_cast<std::size_t>(n)];
    CAPTURE(n);
    CHECK(std::abs(actual - expected) <= scaled_float_tolerance(expected));
  }
}

TEST_CASE("DCT non-positive output counts retain their input-sized default", "[dct]") {
  const std::vector<float> input{0.25f, -0.5f, 0.75f, 1.0f};
  const std::vector<float> vector_default = sonare::dct_ii(input);
  const std::vector<float> vector_explicit = sonare::dct_ii(input, static_cast<int>(input.size()));
  REQUIRE(vector_default == vector_explicit);

  const std::vector<float> pointer_default =
      sonare::dct_ii(input.data(), static_cast<int>(input.size()), -1);
  REQUIRE(pointer_default == vector_explicit);

  const std::vector<float> coefficients{0.5f, -0.25f, 0.75f};
  const std::vector<float> inverse_default = sonare::idct_ii(coefficients.data(), 3, 0);
  const std::vector<float> inverse_explicit = sonare::idct_ii(coefficients.data(), 3, 3);
  REQUIRE(inverse_default == inverse_explicit);
}

TEST_CASE("DCT rejects invalid dimensions across its public entry paths", "[dct]") {
#if defined(SONARE_WITH_MIXING)
  std::vector<float> allocation_probe_result;
  bool allocation_was_blocked = false;
  {
    sonare::test::AllocationFailureGuard guard(kLargeAllocationThreshold);
    try {
      allocation_probe_result = sonare::create_dct_matrix(256, 256);
    } catch (const std::bad_alloc&) {
      allocation_was_blocked = true;
    }
  }
  REQUIRE(allocation_was_blocked);
  REQUIRE(allocation_probe_result.empty());
#endif

  REQUIRE(throws_invalid_parameter([] { (void)sonare::create_dct_matrix(0, 1); }));
  REQUIRE(throws_invalid_parameter([] { (void)sonare::create_dct_matrix(1, -1); }));
  REQUIRE(throws_invalid_parameter([] {
    (void)sonare::create_dct_matrix(std::numeric_limits<int>::max(),
                                    std::numeric_limits<int>::max());
  }));
  REQUIRE(throws_invalid_parameter(
      [] { (void)sonare::create_dct_matrix(std::numeric_limits<int>::max(), 2); }));

  const float sample = 0.25f;
  REQUIRE(throws_invalid_parameter(
      [&] { (void)sonare::dct_ii(&sample, std::numeric_limits<int>::max(), 2); }));
  REQUIRE(throws_invalid_parameter(
      [&] { (void)sonare::dct_ii(&sample, 2, std::numeric_limits<int>::max()); }));
  const std::vector<float> short_input{0.25f, -0.5f};
  REQUIRE(throws_invalid_parameter(
      [&] { (void)sonare::dct_ii(short_input, std::numeric_limits<int>::max()); }));

  REQUIRE(throws_invalid_parameter(
      [&] { (void)sonare::idct_ii(&sample, std::numeric_limits<int>::max(), 2); }));
  REQUIRE(throws_invalid_parameter(
      [&] { (void)sonare::idct_ii(&sample, 2, std::numeric_limits<int>::max()); }));
  float output = 0.0f;
  REQUIRE(throws_invalid_parameter(
      [&] { sonare::idct_ii_into(&sample, 2, &output, std::numeric_limits<int>::max()); }));

  // Use a fresh thread so the DCT cache starts empty, then fill all eight slots.
  bool invalid_cache_request_rejected = false;
#if defined(SONARE_WITH_MIXING)
  // The allocation counter confirms the invalid request did not flush the cache.
  bool cache_entry_survived = false;
  std::size_t allocations_for_cached_hit = 0;

  std::size_t control_allocations = 0;
  std::vector<float> count_probe_result;
  {
    sonare::test::AllocationGuard allocation_guard;
    count_probe_result = sonare::create_dct_matrix(2, 3);
    control_allocations = allocation_guard.count();
  }
  REQUIRE(control_allocations > 0);
  REQUIRE(count_probe_result.size() == 6);
#endif
  std::thread cache_probe([&] {
    constexpr int kAnchorOutput = 2;
    constexpr int kAnchorInput = 3;
    (void)sonare::get_dct_matrix_cached(kAnchorOutput, kAnchorInput);
    for (int i = 1; i < 8; ++i) {
      (void)sonare::get_dct_matrix_cached(kAnchorOutput + i, kAnchorInput + i);
    }

    try {
      (void)sonare::get_dct_matrix_cached(std::numeric_limits<int>::max(),
                                          std::numeric_limits<int>::max());
    } catch (const sonare::SonareException& error) {
      invalid_cache_request_rejected = error.code() == sonare::ErrorCode::InvalidParameter;
    } catch (...) {
    }

#if defined(SONARE_WITH_MIXING)
    {
      sonare::test::AllocationGuard allocation_guard;
      (void)sonare::get_dct_matrix_cached(kAnchorOutput, kAnchorInput);
      allocations_for_cached_hit = allocation_guard.count();
    }
    cache_entry_survived = allocations_for_cached_hit == 0;
#endif
  });
  cache_probe.join();
  REQUIRE(invalid_cache_request_rejected);
#if defined(SONARE_WITH_MIXING)
  REQUIRE(cache_entry_survived);
#endif
}
