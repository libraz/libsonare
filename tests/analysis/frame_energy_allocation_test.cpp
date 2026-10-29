/// @file frame_energy_allocation_test.cpp
/// @brief Frame-energy boundary, scalar-reference, and allocation tests.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "analysis/acoustic/internal.h"
#include "util/constants.h"
#if defined(SONARE_WITH_MIXING)
#include "support/alloc_guard.h"
#endif

namespace {

constexpr int kSampleRate = 8000;
constexpr int kFrameSize = 240;
constexpr int kHopSize = 80;

struct ScalarFrameEnergy {
  std::vector<float> times;
  std::vector<float> energy;
  std::vector<float> db;
};

ScalarFrameEnergy scalar_frame_energy(const std::vector<float>& samples, int sample_rate) {
  ScalarFrameEnergy expected;
  for (std::size_t start = 0; start + kFrameSize <= samples.size(); start += kHopSize) {
    double sum = 0.0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kFrameSize); ++i) {
      const double sample = static_cast<double>(samples[start + i]);
      sum += sample * sample;
    }
    expected.energy.push_back(static_cast<float>(sum / kFrameSize));
    expected.times.push_back((static_cast<float>(start) + 0.5f * kFrameSize) /
                             static_cast<float>(sample_rate));
  }

  if (!expected.energy.empty()) {
    double reference = static_cast<double>(sonare::acoustic_detail::kEnergyEpsilon);
    for (float value : expected.energy) {
      reference = std::max(reference, static_cast<double>(value));
    }
    for (float value : expected.energy) {
      const double power = std::max(static_cast<double>(value),
                                    static_cast<double>(sonare::acoustic_detail::kEnergyEpsilon));
      expected.db.push_back(static_cast<float>(10.0 * std::log10(power / reference)));
    }
  }
  return expected;
}

double scale_tolerance(float expected) {
  return std::max(1e-7, std::abs(static_cast<double>(expected)) * 2e-6);
}

double db_tolerance(float expected) {
  // Bound float ratio rounding near unity through the dB derivative, 10 / ln(10).
  const double ratio_rounding_budget = (10.0 / sonare::constants::kLn10D) *
                                       static_cast<double>(std::numeric_limits<float>::epsilon());
  return std::max(scale_tolerance(expected), ratio_rounding_budget);
}

void check_matches_scalar_oracle(const sonare::acoustic_detail::FrameEnergy& actual,
                                 const std::vector<float>& samples, int sample_rate) {
  const ScalarFrameEnergy expected = scalar_frame_energy(samples, sample_rate);
  REQUIRE(actual.times.size() == expected.times.size());
  REQUIRE(actual.energy.size() == expected.energy.size());
  REQUIRE(actual.db.size() == expected.db.size());
  for (std::size_t i = 0; i < expected.energy.size(); ++i) {
    CHECK_THAT(actual.times[i],
               Catch::Matchers::WithinAbs(expected.times[i], scale_tolerance(expected.times[i])));
    CHECK_THAT(actual.energy[i],
               Catch::Matchers::WithinAbs(expected.energy[i], scale_tolerance(expected.energy[i])));
    CHECK_THAT(actual.db[i],
               Catch::Matchers::WithinAbs(expected.db[i], db_tolerance(expected.db[i])));
  }
}

std::vector<float> make_samples(std::size_t size) {
  std::vector<float> samples(size);
  for (std::size_t i = 0; i < size; ++i) {
    const float position = static_cast<float>(i);
    samples[i] = 0.35f * std::sin(0.017f * position) + 0.2f * std::cos(0.043f * position) - 0.01f;
  }
  return samples;
}

}  // namespace

TEST_CASE("Frame energy preserves framing boundaries and scalar results",
          "[analysis][acoustic_frame_energy]") {
  using sonare::acoustic_detail::compute_frame_energy;

  const auto empty = compute_frame_energy(nullptr, 0, kSampleRate);
  CHECK(empty.times.empty());
  CHECK(empty.energy.empty());
  CHECK(empty.db.empty());

  const auto short_samples = make_samples(kFrameSize - 1);
  const auto short_result =
      compute_frame_energy(short_samples.data(), short_samples.size(), kSampleRate);
  CHECK(short_result.times.empty());
  CHECK(short_result.energy.empty());
  CHECK(short_result.db.empty());

  const auto exact_frame_samples = make_samples(kFrameSize);
  const auto exact_frame =
      compute_frame_energy(exact_frame_samples.data(), exact_frame_samples.size(), kSampleRate);
  CHECK(exact_frame.energy.size() == 1);
  check_matches_scalar_oracle(exact_frame, exact_frame_samples, kSampleRate);

  const auto partial_hop_samples = make_samples(kFrameSize + kHopSize - 1);
  const auto partial_hop =
      compute_frame_energy(partial_hop_samples.data(), partial_hop_samples.size(), kSampleRate);
  CHECK(partial_hop.energy.size() == 1);
  check_matches_scalar_oracle(partial_hop, partial_hop_samples, kSampleRate);

  const auto exact_hop_samples = make_samples(kFrameSize + kHopSize);
  const auto exact_hop =
      compute_frame_energy(exact_hop_samples.data(), exact_hop_samples.size(), kSampleRate);
  CHECK(exact_hop.energy.size() == 2);
  check_matches_scalar_oracle(exact_hop, exact_hop_samples, kSampleRate);

  constexpr std::size_t kSampleCount = 4096;
  const auto samples = make_samples(kSampleCount);
  const auto result = compute_frame_energy(samples.data(), samples.size(), kSampleRate);
  CHECK(result.energy.size() == 49);
  check_matches_scalar_oracle(result, samples, kSampleRate);
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("Frame energy uses one allocation per output vector",
          "[analysis][acoustic_frame_energy]") {
  using sonare::acoustic_detail::compute_frame_energy;
  using sonare::test::AllocationGuard;

  constexpr std::size_t kSampleCount = 4096;
  constexpr std::size_t kExpectedFrameCount = 49;
  constexpr std::size_t kExpectedAllocations = 3;
  const auto samples = make_samples(kSampleCount);

  const auto warmup = compute_frame_energy(samples.data(), samples.size(), kSampleRate);
  REQUIRE(warmup.energy.size() == kExpectedFrameCount);

  std::vector<double> positive_control_result;
  std::size_t positive_control_allocations = 0;
  {
    AllocationGuard guard;
    positive_control_result =
        sonare::acoustic_detail::squared_energy(samples.data(), samples.size());
    positive_control_allocations = guard.count();
  }
  REQUIRE(positive_control_result.size() == samples.size());
  REQUIRE(positive_control_result.back() > 0.0);
  CHECK(positive_control_allocations >= 1);

  sonare::acoustic_detail::FrameEnergy observed;
  std::size_t frame_energy_allocations = 0;
  {
    AllocationGuard guard;
    observed = compute_frame_energy(samples.data(), samples.size(), kSampleRate);
    frame_energy_allocations = guard.count();
  }
  REQUIRE(observed.energy.size() == kExpectedFrameCount);
  REQUIRE(frame_energy_allocations == kExpectedAllocations);
}
#endif
