/// @file dct_test.cpp
/// @brief Tests for DCT implementation.

#include "filters/dct.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <numeric>

using namespace sonare;
using Catch::Matchers::WithinAbs;

TEST_CASE("create_dct_matrix dimensions", "[dct]") {
  auto matrix = create_dct_matrix(13, 40);
  REQUIRE(matrix.size() == 13 * 40);
}

TEST_CASE("create_dct_matrix orthonormality", "[dct]") {
  int n = 8;
  auto matrix = create_dct_matrix(n, n);

  // Check orthonormality: D * D^T should be identity
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      float dot = 0.0f;
      for (int k = 0; k < n; ++k) {
        dot += matrix[i * n + k] * matrix[j * n + k];
      }
      float expected = (i == j) ? 1.0f : 0.0f;
      REQUIRE_THAT(dot, WithinAbs(expected, 1e-5f));
    }
  }
}

TEST_CASE("dct_ii DC input", "[dct]") {
  // DC input (all ones) should only have DC component
  std::vector<float> input(8, 1.0f);
  auto output = dct_ii(input);

  REQUIRE(output.size() == input.size());

  // DC component should be sqrt(N) for ortho-normalized DCT
  // With orthonormal scaling: DC = sqrt(2/N) / sqrt(2) * sum = sqrt(1/N) * N = sqrt(N)
  float expected_dc = std::sqrt(static_cast<float>(input.size()));
  REQUIRE_THAT(output[0], WithinAbs(expected_dc, 1e-4f));

  // All other components should be ~0
  for (size_t i = 1; i < output.size(); ++i) {
    REQUIRE_THAT(output[i], WithinAbs(0.0f, 1e-5f));
  }
}

TEST_CASE("dct_ii alternating input", "[dct]") {
  // Alternating +1, -1 should have energy in high frequencies
  std::vector<float> input = {1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f};
  auto output = dct_ii(input);

  // DC should be 0
  REQUIRE_THAT(output[0], WithinAbs(0.0f, 1e-5f));

  // Last coefficient (highest frequency) should have most energy
  float max_coef = 0.0f;
  int max_idx = 0;
  for (size_t i = 0; i < output.size(); ++i) {
    if (std::abs(output[i]) > max_coef) {
      max_coef = std::abs(output[i]);
      max_idx = static_cast<int>(i);
    }
  }
  REQUIRE(max_idx == static_cast<int>(output.size() - 1));
}

TEST_CASE("dct_ii/idct_ii roundtrip", "[dct]") {
  std::vector<float> input = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};

  auto dct = dct_ii(input);
  auto recovered = idct_ii(dct.data(), static_cast<int>(dct.size()));

  REQUIRE(recovered.size() == input.size());

  for (size_t i = 0; i < input.size(); ++i) {
    REQUIRE_THAT(recovered[i], WithinAbs(input[i], 1e-4f));
  }
}

TEST_CASE("dct_ii reduced output", "[dct]") {
  std::vector<float> input(40, 0.0f);
  // Create a simple signal
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = std::sin(3.14159f * 2.0f * i / input.size());
  }

  // Get only first 13 coefficients (like MFCC)
  auto output = dct_ii(input, 13);

  REQUIRE(output.size() == 13);
}

TEST_CASE("dct_ii energy preservation", "[dct]") {
  std::vector<float> input = {1.0f, 2.0f, 3.0f, 4.0f};

  auto output = dct_ii(input);

  // Parseval's theorem: sum of squares should be preserved
  float input_energy = 0.0f;
  for (float x : input) {
    input_energy += x * x;
  }

  float output_energy = 0.0f;
  for (float x : output) {
    output_energy += x * x;
  }

  REQUIRE_THAT(output_energy, WithinAbs(input_energy, 1e-4f));
}
