/// @file non_finite_sample_test.cpp
/// @brief The destination rule for a sample that cannot be represented.
///
/// Every assertion here is on a VALUE or on a COUNT. A finiteness assertion
/// would pass whether or not the substitution ran, which is what lets this
/// decision live in twelve places at once.

#include "util/non_finite_sample.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using sonare::resolve_non_finite;
using sonare::resolve_non_finite_run;
using sonare::SampleDestination;
using sonare::substitutes_non_finite;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

/// The four classes that cannot hold a non-finite value.
constexpr SampleDestination kSubstituting[] = {
    SampleDestination::kRecursiveState,
    SampleDestination::kIrreversibleOutput,
    SampleDestination::kOrderedContainer,
    SampleDestination::kBoundedResult,
};

}  // namespace

TEST_CASE("A substituting destination leaves silence, whichever non-finite arrived", "[util]") {
  for (const SampleDestination destination : kSubstituting) {
    REQUIRE(substitutes_non_finite(destination));
    for (const float arrival : {kNaN, kInf, -kInf}) {
      float sample = arrival;
      REQUIRE(resolve_non_finite(destination, sample));
      // Exactly zero, not merely finite and not in the neighbourhood: full
      // scale, a ceiling and unity are all values a stage produces on its own.
      REQUIRE(sample == 0.0f);
      REQUIRE(!std::signbit(sample));
    }
  }
}

TEST_CASE("kCallerReturn propagates the value it was given", "[util]") {
  REQUIRE(!substitutes_non_finite(SampleDestination::kCallerReturn));

  float nan_sample = kNaN;
  REQUIRE(resolve_non_finite(SampleDestination::kCallerReturn, nan_sample));
  REQUIRE(std::isnan(nan_sample));

  float inf_sample = kInf;
  REQUIRE(resolve_non_finite(SampleDestination::kCallerReturn, inf_sample));
  REQUIRE(inf_sample == kInf);

  float neg_inf_sample = -kInf;
  REQUIRE(resolve_non_finite(SampleDestination::kCallerReturn, neg_inf_sample));
  REQUIRE(neg_inf_sample == -kInf);
}

TEST_CASE("A finite sample is reported false and left bit-identical", "[util]") {
  // The negative control for every case above: the rule fires on the non-finite
  // value and on nothing else, including the values nearest to zero and to the
  // bounds a clamp would have used.
  const float finite_values[] = {0.0f,
                                 -0.0f,
                                 1.0f,
                                 -1.0f,
                                 0.5f,
                                 -std::numeric_limits<float>::denorm_min(),
                                 std::numeric_limits<float>::max(),
                                 std::numeric_limits<float>::lowest()};
  for (const SampleDestination destination :
       {SampleDestination::kRecursiveState, SampleDestination::kIrreversibleOutput,
        SampleDestination::kOrderedContainer, SampleDestination::kBoundedResult,
        SampleDestination::kCallerReturn}) {
    for (const float value : finite_values) {
      float sample = value;
      REQUIRE(!resolve_non_finite(destination, sample));
      REQUIRE(std::memcmp(&sample, &value, sizeof(float)) == 0);
    }
  }
}

TEST_CASE("The run form returns how many samples were non-finite", "[util]") {
  std::vector<float> samples = {0.25f, kNaN, -0.5f, kInf, 0.75f, -kInf, 1.0f, kNaN};
  REQUIRE(resolve_non_finite_run(SampleDestination::kIrreversibleOutput, samples.data(),
                                 samples.size()) == 4);
  const std::vector<float> expected = {0.25f, 0.0f, -0.5f, 0.0f, 0.75f, 0.0f, 1.0f, 0.0f};
  REQUIRE(samples == expected);

  // A second pass over the same run counts nothing, so the count is of arrivals
  // rather than of visits.
  REQUIRE(resolve_non_finite_run(SampleDestination::kIrreversibleOutput, samples.data(),
                                 samples.size()) == 0);
  REQUIRE(samples == expected);
}

TEST_CASE("The run form over kCallerReturn counts without substituting", "[util]") {
  std::vector<float> samples = {kNaN, 0.25f, kInf};
  REQUIRE(resolve_non_finite_run(SampleDestination::kCallerReturn, samples.data(),
                                 samples.size()) == 2);
  REQUIRE(std::isnan(samples[0]));
  REQUIRE(samples[1] == 0.25f);
  REQUIRE(samples[2] == kInf);
}

TEST_CASE("A double run carries the same rule", "[util]") {
  // The spectral descriptors accumulate in double; the rule must not be a float
  // one that a double site spells for itself.
  std::vector<double> values = {1.5, std::numeric_limits<double>::quiet_NaN(),
                                -std::numeric_limits<double>::infinity()};
  REQUIRE(resolve_non_finite_run(SampleDestination::kBoundedResult, values.data(), values.size()) ==
          2);
  REQUIRE(values[0] == 1.5);
  REQUIRE(values[1] == 0.0);
  REQUIRE(values[2] == 0.0);
}
