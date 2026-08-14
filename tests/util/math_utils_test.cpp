/// @file math_utils_test.cpp
/// @brief Tests for math utility functions.

#include "util/math_utils.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "rt/polyphase_fir.h"
#include "util/exception.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("mean", "[math_utils]") {
  std::vector<float> data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  REQUIRE_THAT(mean(data.data(), data.size()), WithinAbs(3.0f, 1e-6f));

  std::vector<float> empty;
  REQUIRE_THAT(mean(empty.data(), empty.size()), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("variance", "[math_utils]") {
  std::vector<float> data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  REQUIRE_THAT(variance(data.data(), data.size()), WithinAbs(2.0f, 1e-6f));
}

TEST_CASE("stddev", "[math_utils]") {
  std::vector<float> data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  REQUIRE_THAT(stddev(data.data(), data.size()), WithinAbs(std::sqrt(2.0f), 1e-5f));
}

TEST_CASE("argmax", "[math_utils]") {
  std::vector<float> data = {1.0f, 5.0f, 3.0f, 2.0f};
  REQUIRE(argmax(data.data(), data.size()) == 1);

  std::vector<float> empty;
  REQUIRE(argmax(empty.data(), empty.size()) == 0);
}

TEST_CASE("cosine_similarity", "[math_utils]") {
  std::vector<float> a = {1.0f, 0.0f, 0.0f};
  std::vector<float> b = {1.0f, 0.0f, 0.0f};
  REQUIRE_THAT(cosine_similarity(a.data(), b.data(), a.size()), WithinAbs(1.0f, 1e-6f));

  std::vector<float> c = {0.0f, 1.0f, 0.0f};
  REQUIRE_THAT(cosine_similarity(a.data(), c.data(), a.size()), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("median", "[math_utils]") {
  std::vector<float> odd = {3.0f, 1.0f, 2.0f};
  REQUIRE_THAT(median(odd.data(), odd.size()), WithinAbs(2.0f, 1e-6f));

  std::vector<float> even = {4.0f, 1.0f, 3.0f, 2.0f};
  REQUIRE_THAT(median(even.data(), even.size()), WithinAbs(2.5f, 1e-6f));
}

TEST_CASE("percentile", "[math_utils]") {
  std::vector<float> data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  REQUIRE_THAT(percentile(data.data(), data.size(), 0.0f), WithinAbs(1.0f, 1e-6f));
  REQUIRE_THAT(percentile(data.data(), data.size(), 50.0f), WithinAbs(3.0f, 1e-6f));
  REQUIRE_THAT(percentile(data.data(), data.size(), 100.0f), WithinAbs(5.0f, 1e-6f));
}

TEST_CASE("percentile clamps out-of-range p to [0, 100]", "[math_utils][edge]") {
  std::vector<float> data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  // p < 0 must clamp to 0 (min element) and p > 100 to 100 (max element)
  // without reading out of bounds of the sorted buffer.
  REQUIRE_THAT(percentile(data.data(), data.size(), -5.0f), WithinAbs(1.0f, 1e-6f));
  REQUIRE_THAT(percentile(data.data(), data.size(), 150.0f), WithinAbs(5.0f, 1e-6f));
}

TEST_CASE("percentile_sorted interpolates between bracketing ranks", "[math_utils][percentile]") {
  // numpy.percentile's default 'linear' interpolation on the fractional rank
  // percentile * (size - 1).
  const std::vector<float> data = {10.0f, 20.0f, 30.0f, 40.0f, 50.0f};
  REQUIRE_THAT(percentile_sorted(data, 0.0), WithinAbs(10.0, 1e-12));
  REQUIRE_THAT(percentile_sorted(data, 1.0), WithinAbs(50.0, 1e-12));
  REQUIRE_THAT(percentile_sorted(data, 0.5), WithinAbs(30.0, 1e-12));
  // rank 0.10 * 4 = 0.4 -> 10 + 0.4 * (20 - 10)
  REQUIRE_THAT(percentile_sorted(data, 0.10), WithinAbs(14.0, 1e-12));
  // rank 0.95 * 4 = 3.8 -> 40 + 0.8 * (50 - 40)
  REQUIRE_THAT(percentile_sorted(data, 0.95), WithinAbs(48.0, 1e-12));
}

TEST_CASE("percentile_sorted handles degenerate distributions", "[math_utils][percentile][edge]") {
  REQUIRE_THAT(percentile_sorted({}, 0.5), WithinAbs(0.0, 1e-12));
  REQUIRE_THAT(percentile_sorted({7.0f}, 0.0), WithinAbs(7.0, 1e-12));
  REQUIRE_THAT(percentile_sorted({7.0f}, 1.0), WithinAbs(7.0, 1e-12));
  // Out-of-range fractions clamp instead of indexing past the ends.
  const std::vector<float> data = {1.0f, 2.0f, 3.0f};
  REQUIRE_THAT(percentile_sorted(data, -1.0), WithinAbs(1.0, 1e-12));
  REQUIRE_THAT(percentile_sorted(data, 2.0), WithinAbs(3.0, 1e-12));
}

TEST_CASE("percentile_sorted returns an exact rank without blending its neighbour",
          "[math_utils][percentile][edge]") {
  // These distributions carry dB values and a silent window is -inf. When the
  // fractional rank lands exactly on an element, that element must be returned
  // directly: blending it with an infinite neighbour at weight zero yields
  // inf * 0 == NaN, which is how a range metric silently becomes NaN.
  const float inf = std::numeric_limits<float>::infinity();
  const std::vector<float> low_silence = {-inf, -inf, -20.0f, -10.0f};
  // rank 0.0 * 3 = 0 -> exactly sorted[0], neighbour is also -inf.
  const double low = percentile_sorted(low_silence, 0.0);
  CAPTURE(low);
  REQUIRE(!std::isnan(low));
  REQUIRE(low == -static_cast<double>(inf));

  const std::vector<float> high_silence = {-10.0f, -20.0f, inf};
  // rank 1.0 * 2 = 2 -> exactly sorted[2].
  const double high = percentile_sorted(high_silence, 1.0);
  CAPTURE(high);
  REQUIRE(!std::isnan(high));
}

TEST_CASE("percentile agrees with the shared percentile_sorted kernel",
          "[math_utils][percentile]") {
  // percentile() is the [0, 100]-scaled, self-sorting entry point. It must not
  // carry its own copy of the interpolation: the two are pinned together here so
  // a divergent reimplementation fails rather than drifting silently.
  const std::vector<float> unsorted = {5.0f, -3.0f, 12.5f, 0.25f, 7.0f, -1.0f, 9.5f};
  std::vector<float> sorted = unsorted;
  std::sort(sorted.begin(), sorted.end());

  for (double fraction : {0.0, 0.05, 0.10, 0.25, 0.5, 0.75, 0.95, 1.0}) {
    const float via_percentile =
        percentile(unsorted.data(), unsorted.size(), static_cast<float>(fraction * 100.0));
    const float via_kernel = static_cast<float>(percentile_sorted(sorted, fraction));
    CAPTURE(fraction, via_percentile, via_kernel);
    REQUIRE_THAT(via_percentile, WithinAbs(via_kernel, 1e-6f));
  }
}

TEST_CASE("next_power_of_2", "[math_utils]") {
  SECTION("powers of 2") {
    REQUIRE(next_power_of_2(1) == 1);
    REQUIRE(next_power_of_2(2) == 2);
    REQUIRE(next_power_of_2(4) == 4);
    REQUIRE(next_power_of_2(1024) == 1024);
  }

  SECTION("non-powers of 2") {
    REQUIRE(next_power_of_2(3) == 4);
    REQUIRE(next_power_of_2(5) == 8);
    REQUIRE(next_power_of_2(7) == 8);
    REQUIRE(next_power_of_2(100) == 128);
    REQUIRE(next_power_of_2(1000) == 1024);
    REQUIRE(next_power_of_2(2000) == 2048);
  }

  SECTION("edge cases") {
    REQUIRE(next_power_of_2(0) == 1);
    REQUIRE(next_power_of_2(-1) == 1);
    REQUIRE(next_power_of_2(-100) == 1);
  }
}

TEST_CASE("power_to_db basic", "[math_utils]") {
  SECTION("ref=1.0 default") {
    std::vector<float> power = {1.0f, 0.1f, 0.01f};
    std::vector<float> db(3);
    power_to_db(power.data(), 3, 1.0f, 1e-10f, 80.0f, db.data());
    REQUIRE_THAT(db[0], WithinAbs(0.0f, 0.01f));
    REQUIRE_THAT(db[1], WithinAbs(-10.0f, 0.01f));
    REQUIRE_THAT(db[2], WithinAbs(-20.0f, 0.01f));
  }

  SECTION("ref=2.0") {
    std::vector<float> power = {2.0f};
    std::vector<float> db(1);
    power_to_db(power.data(), 1, 2.0f, 1e-10f, -1.0f, db.data());
    REQUIRE_THAT(db[0], WithinAbs(0.0f, 0.01f));
  }

  SECTION("top_db clipping") {
    std::vector<float> power = {1.0f, 1e-20f};
    std::vector<float> db(2);
    power_to_db(power.data(), 2, 1.0f, 1e-10f, 80.0f, db.data());
    REQUIRE(db[1] >= -80.0f);
  }

  SECTION("in-place conversion") {
    std::vector<float> data = {1.0f, 0.1f, 0.01f};
    power_to_db(data.data(), 3, 1.0f, 1e-10f, 80.0f, data.data());
    REQUIRE_THAT(data[0], WithinAbs(0.0f, 0.01f));
    REQUIRE_THAT(data[1], WithinAbs(-10.0f, 0.01f));
    REQUIRE_THAT(data[2], WithinAbs(-20.0f, 0.01f));
  }

  SECTION("empty input") {
    std::vector<float> db;
    power_to_db(nullptr, 0, 1.0f, 1e-10f, 80.0f, db.data());
    REQUIRE(db.empty());
  }

  SECTION("ref<=0 uses max(S) reference (librosa ref=np.max)") {
    // With ref <= 0 the reference must resolve to max(|S|), so the peak lands at
    // 0 dB. This is the unified behavior shared with core/db_convert; the old
    // divergent implementation incorrectly floored ref at amin instead.
    std::vector<float> power = {4.0f, 1.0f, 0.25f};
    std::vector<float> db(3);
    power_to_db(power.data(), 3, 0.0f, 1e-10f, -1.0f, db.data());
    REQUIRE_THAT(db[0], WithinAbs(0.0f, 0.01f));       // 10*log10(4/4)
    REQUIRE_THAT(db[1], WithinAbs(-6.0206f, 0.01f));   // 10*log10(1/4)
    REQUIRE_THAT(db[2], WithinAbs(-12.0412f, 0.01f));  // 10*log10(0.25/4)
  }
}

TEST_CASE("compute_autocorrelation", "[math_utils]") {
  SECTION("sine wave autocorrelation") {
    // Autocorrelation of a sine wave should be a cosine.
    // Use 16 cycles in 1024 samples => period = 64 samples.
    int n = 1024;
    int num_cycles = 16;
    std::vector<float> signal(n);
    for (int i = 0; i < n; ++i) {
      signal[i] =
          std::sin(2.0f * 3.14159f * static_cast<float>(num_cycles) * static_cast<float>(i) / n);
    }
    int max_lag = n / 2;
    std::vector<float> result(max_lag);
    compute_autocorrelation(signal.data(), n, max_lag, result.data());
    // Lag 0 should be ~1.0 (normalized autocorrelation)
    REQUIRE_THAT(result[0], WithinAbs(1.0f, 0.05f));
    // Autocorrelation at one period (lag = n/num_cycles = 64) should be high
    int period = n / num_cycles;
    REQUIRE(result[period] > 0.8f);
    // Autocorrelation at half-period should be negative (anti-correlated)
    REQUIRE(result[period / 2] < -0.5f);
  }

  SECTION("constant signal returns zeros") {
    std::vector<float> signal(128, 5.0f);
    std::vector<float> result(64);
    compute_autocorrelation(signal.data(), 128, 64, result.data());
    // Constant signal has zero variance, so autocorrelation should be all zeros
    for (int i = 0; i < 64; ++i) {
      REQUIRE_THAT(result[i], WithinAbs(0.0f, 1e-6f));
    }
  }

  SECTION("rejects null input or output for positive lengths") {
    const std::vector<float> signal = {1.0f, -2.0f, 3.0f, -4.0f};
    std::vector<float> result(4, 1.0f);

    try {
      compute_autocorrelation(nullptr, 4, 4, result.data());
      FAIL("Expected SonareException for null input");
    } catch (const SonareException& error) {
      REQUIRE(error.code() == ErrorCode::InvalidParameter);
    }

    try {
      compute_autocorrelation(signal.data(), 4, 4, nullptr);
      FAIL("Expected SonareException for null output");
    } catch (const SonareException& error) {
      REQUIRE(error.code() == ErrorCode::InvalidParameter);
    }
  }

  SECTION("rejects negative lengths before numeric conversion") {
    const std::vector<float> signal = {1.0f, -2.0f, 3.0f, -4.0f};
    std::vector<float> result(4, 1.0f);

    REQUIRE_THROWS_AS(compute_autocorrelation(signal.data(), -1, 4, result.data()),
                      SonareException);
    REQUIRE_THROWS_AS(compute_autocorrelation(signal.data(), 4, -1, result.data()),
                      SonareException);
  }

  SECTION("preserves empty input and zero-length output contracts") {
    std::vector<float> empty_input_result(3, 1.0f);
    compute_autocorrelation(nullptr, 0, 3, empty_input_result.data());
    for (float value : empty_input_result) {
      REQUIRE_THAT(value, WithinAbs(0.0f, 1e-6f));
    }

    const std::vector<float> signal = {1.0f, -2.0f, 3.0f};
    compute_autocorrelation(signal.data(), static_cast<int>(signal.size()), 0, nullptr);
    compute_autocorrelation(nullptr, 0, 0, nullptr);
  }
}

TEST_CASE("unnormalized_autocorrelation is the shared raw autocorrelation primitive",
          "[math_utils]") {
  SECTION("small input uses direct summation contract") {
    const std::vector<float> signal = {1.0f, -2.0f, 3.0f, 4.0f};
    const std::vector<float> result =
        unnormalized_autocorrelation(signal.data(), signal.size(), signal.size());

    REQUIRE(result.size() == signal.size());
    REQUIRE_THAT(result[0], WithinAbs(30.0f, 1e-6f));
    REQUIRE_THAT(result[1], WithinAbs(4.0f, 1e-6f));
    REQUIRE_THAT(result[2], WithinAbs(-5.0f, 1e-6f));
    REQUIRE_THAT(result[3], WithinAbs(4.0f, 1e-6f));
  }

  SECTION("FFT path preserves lag-zero energy and truncates output") {
    std::vector<float> signal(128, 0.0f);
    for (size_t i = 0; i < signal.size(); ++i) {
      signal[i] = std::sin(0.1f * static_cast<float>(i)) + 0.25f;
    }

    const std::vector<float> result =
        unnormalized_autocorrelation(signal.data(), signal.size(), 16);
    double energy = 0.0;
    for (float sample : signal) energy += static_cast<double>(sample) * sample;

    REQUIRE(result.size() == 16);
    REQUIRE_THAT(result[0], WithinRel(static_cast<float>(energy), 1e-4f));
  }
}
