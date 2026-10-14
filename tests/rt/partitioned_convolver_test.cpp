/// @file partitioned_convolver_test.cpp
/// @brief Correctness tests for the uniform-partitioned FFT convolver.

#include "rt/partitioned_convolver.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "util/exception.h"

using sonare::ErrorCode;
using sonare::SonareException;
using sonare::rt::PartitionedConvolver;
using sonare::rt::PartitionedConvolverConfig;

namespace {

// Direct time-domain linear convolution reference.
std::vector<float> naive_convolve(const std::vector<float>& x, const std::vector<float>& h) {
  if (x.empty() || h.empty()) return {};
  std::vector<float> y(x.size() + h.size() - 1, 0.0f);
  for (size_t n = 0; n < x.size(); ++n) {
    for (size_t k = 0; k < h.size(); ++k) {
      y[n + k] += x[n] * h[k];
    }
  }
  return y;
}

}  // namespace

TEST_CASE("PartitionedConvolver rejects an unrepresentable FFT size", "[rt][convolver]") {
  const int too_large = std::numeric_limits<int>::max() / 2 + 1;

  try {
    PartitionedConvolver conv(PartitionedConvolverConfig{too_large});
    FAIL("expected an invalid partition size");
  } catch (const SonareException& error) {
    REQUIRE(error.code() == ErrorCode::InvalidParameter);
    const std::string message = error.what();
    REQUIRE(message.find("partition_size") != std::string::npos);
    REQUIRE(message.find("INT_MAX / 2") != std::string::npos);
  }
}

TEST_CASE("PartitionedConvolver passes input through a unit-impulse IR", "[rt][convolver]") {
  constexpr int kN = 64;
  PartitionedConvolver conv(PartitionedConvolverConfig{kN});

  std::vector<float> ir(kN, 0.0f);
  ir[0] = 1.0f;
  conv.set_impulse_response(ir);

  std::mt19937 rng(123);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  for (int block = 0; block < 8; ++block) {
    std::vector<float> in(kN);
    for (int i = 0; i < kN; ++i) in[i] = dist(rng);
    std::vector<float> out(kN, 0.0f);
    conv.process_block(in.data(), out.data());
    for (int i = 0; i < kN; ++i) {
      CAPTURE(block, i, in[i], out[i]);
      REQUIRE(std::abs(out[i] - in[i]) < 1.0e-4f);
    }
  }
}

TEST_CASE("PartitionedConvolver matches naive convolution for a multi-partition IR",
          "[rt][convolver]") {
  constexpr int kN = 32;
  constexpr int kNumBlocks = 10;
  constexpr int kIrLen = kN * 3 + 11;  // spans 4 partitions, non-multiple length

  std::mt19937 rng(4567);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  std::vector<float> ir(kIrLen);
  for (auto& v : ir) v = dist(rng);

  std::vector<float> input(kN * kNumBlocks);
  for (auto& v : input) v = dist(rng);

  PartitionedConvolver conv(PartitionedConvolverConfig{kN});
  conv.set_impulse_response(ir);
  REQUIRE(conv.num_partitions() == 4);

  std::vector<float> output(input.size(), 0.0f);
  for (int block = 0; block < kNumBlocks; ++block) {
    conv.process_block(input.data() + block * kN, output.data() + block * kN);
  }

  const std::vector<float> reference = naive_convolve(input, ir);
  for (size_t i = 0; i < output.size(); ++i) {
    CAPTURE(i, output[i], reference[i]);
    REQUIRE(std::abs(output[i] - reference[i]) < 1.0e-4f);
  }
}

TEST_CASE("PartitionedConvolver produces silence with an empty IR", "[rt][convolver]") {
  constexpr int kN = 16;
  PartitionedConvolver conv(PartitionedConvolverConfig{kN});
  REQUIRE(conv.empty());

  std::vector<float> in(kN, 0.5f);
  std::vector<float> out(kN, 7.0f);
  conv.process_block(in.data(), out.data());
  for (int i = 0; i < kN; ++i) {
    REQUIRE(out[i] == 0.0f);
  }
}

TEST_CASE("PartitionedConvolver process_block is noexcept and null-safe", "[rt][convolver]") {
  static_assert(noexcept(std::declval<PartitionedConvolver&>().process_block(nullptr, nullptr)),
                "process_block must be noexcept for the audio thread");

  constexpr int kN = 16;
  PartitionedConvolver conv(PartitionedConvolverConfig{kN});
  std::vector<float> ir(kN, 0.0f);
  ir[0] = 1.0f;
  conv.set_impulse_response(ir);

  std::vector<float> out(kN, 9.0f);
  // Null input: output is zero-filled, no throw.
  conv.process_block(nullptr, out.data());
  for (int i = 0; i < kN; ++i) {
    REQUIRE(out[i] == 0.0f);
  }

  // Null output and both null: must simply return without throwing.
  std::vector<float> in(kN, 0.5f);
  conv.process_block(in.data(), nullptr);
  conv.process_block(nullptr, nullptr);
  SUCCEED();
}

TEST_CASE("PartitionedConvolver rejects a non-finite IR and keeps the loaded response",
          "[rt][convolver]") {
  constexpr int kN = 32;
  PartitionedConvolver conv(PartitionedConvolverConfig{kN});
  std::vector<float> ir(3 * kN, 0.0f);
  ir[0] = 1.0f;
  conv.set_impulse_response(ir);

  for (float bad :
       {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    // The bad sample sits in the last partition, after finite ones were already seen.
    std::vector<float> proposed(3 * kN, 0.25f);
    proposed.back() = bad;
    try {
      conv.set_impulse_response(proposed);
      FAIL("expected a non-finite IR to be rejected");
    } catch (const SonareException& error) {
      REQUIRE(error.code() == ErrorCode::InvalidParameter);
    }
  }

  REQUIRE(conv.num_partitions() == 3);
  std::vector<float> in(kN, 0.0f);
  in[3] = 1.0f;
  std::vector<float> out(kN, 0.0f);
  conv.process_block(in.data(), out.data());
  for (int i = 0; i < kN; ++i) REQUIRE(std::abs(out[i] - in[i]) < 1.0e-4f);
}
