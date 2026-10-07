/// @file peak_test.cpp
/// @brief Unit + librosa parity tests for util/peak_pick.

#include "util/peak.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <random>
#include <vector>

#include "support/far_event.h"
#include "util/exception.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;

TEST_CASE("peak_pick finds inserted peaks", "[util][peak]") {
  std::vector<float> x(100, 0.0f);
  x[10] = 1.0f;
  x[50] = 1.0f;
  x[90] = 1.0f;
  auto peaks = peak_pick(x, 3, 3, 5, 5, 0.1f, 5);
  REQUIRE(peaks.size() == 3);
  REQUIRE(peaks[0] == 10);
  REQUIRE(peaks[1] == 50);
  REQUIRE(peaks[2] == 90);
}

TEST_CASE("peak_pick respects wait window", "[util][peak]") {
  std::vector<float> x(20, 0.0f);
  x[5] = 1.0f;
  x[7] = 1.0f;  // within wait of x[5]
  auto peaks = peak_pick(x, 1, 1, 2, 2, 0.05f, 5);
  REQUIRE(peaks.size() == 1);
  REQUIRE(peaks[0] == 5);
}

TEST_CASE("peak_pick rejects negative params", "[util][peak][edge]") {
  std::vector<float> x(10, 0.0f);
  REQUIRE_THROWS_AS(peak_pick(x, -1, 1, 1, 1, 0.0f, 0), SonareException);
}

TEST_CASE("peak_pick matches librosa output", "[librosa][util][peak]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/peak_pick.json");
  const auto& d = json["data"];
  const auto& input_arr = d["input"].as_array();
  int pre_max = d["pre_max"].as_int();
  int post_max = d["post_max"].as_int();
  int pre_avg = d["pre_avg"].as_int();
  int post_avg = d["post_avg"].as_int();
  float delta = d["delta"].as_float();
  int wait = d["wait"].as_int();

  std::vector<float> input;
  input.reserve(input_arr.size());
  for (const auto& v : input_arr) input.push_back(v.as_float());

  auto got = peak_pick(input, pre_max, post_max, pre_avg, post_avg, delta, wait);
  const auto& expected_arr = d["expected_peaks"].as_array();

  REQUIRE(got.size() == expected_arr.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected_arr[i].as_int());
    REQUIRE(got[i] == expected_arr[i].as_int());
  }
}

TEST_CASE("peak_pick matches librosa on a plateau-heavy envelope", "[librosa][util][peak]") {
  // Plateaus and a decaying trailing slope with a small wait are exactly where an
  // inclusive-vs-exclusive local-max / moving-average window off-by-one emitted
  // spurious peaks. The C++ window must now match librosa's array slices exactly.
  auto json = JsonReader::parse_file("tests/librosa/reference/peak_pick_plateau.json");
  const auto& d = json["data"];
  const auto& input_arr = d["input"].as_array();
  std::vector<float> input;
  input.reserve(input_arr.size());
  for (const auto& v : input_arr) input.push_back(v.as_float());

  auto got = peak_pick(input, d["pre_max"].as_int(), d["post_max"].as_int(), d["pre_avg"].as_int(),
                       d["post_avg"].as_int(), d["delta"].as_float(), d["wait"].as_int());
  const auto& expected_arr = d["expected_peaks"].as_array();

  REQUIRE(got.size() == expected_arr.size());
  for (size_t i = 0; i < got.size(); ++i) {
    CAPTURE(i, got[i], expected_arr[i].as_int());
    REQUIRE(got[i] == expected_arr[i].as_int());
  }
}

namespace {

std::vector<float> brute_sliding_max(const std::vector<float>& x, std::size_t radius) {
  const std::size_t n = x.size();
  std::vector<float> y(n);
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t lo = i > radius ? i - radius : 0;
    const std::size_t hi = std::min(n - 1, i + radius);
    y[i] = *std::max_element(x.begin() + lo, x.begin() + hi + 1);
  }
  return y;
}

// Repeated argmax over the candidates still eligible; ties go to the earlier frame.
std::vector<int> reference_select(const std::vector<int>& candidates, const std::vector<float>& v,
                                  int min_distance) {
  std::vector<int> remaining = candidates;
  std::vector<int> accepted;
  while (!remaining.empty()) {
    auto best = remaining.begin();
    for (auto it = remaining.begin(); it != remaining.end(); ++it) {
      if (v[*it] > v[*best] || (v[*it] == v[*best] && *it < *best)) best = it;
    }
    const int pick = *best;
    accepted.push_back(pick);
    std::vector<int> next;
    for (int c : remaining) {
      if (std::abs(c - pick) >= min_distance) next.push_back(c);
    }
    remaining = next;
  }
  std::sort(accepted.begin(), accepted.end());
  return accepted;
}

}  // namespace

TEST_CASE("sliding_max matches a brute-force window max", "[util][peak]") {
  std::mt19937 rng(12345);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
  for (std::size_t n : {1u, 2u, 7u, 64u, 200u}) {
    std::vector<float> x(n);
    for (auto& v : x) v = dist(rng);
    for (std::size_t radius :
         {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{10}, n - 1, n, n + 5}) {
      CAPTURE(n, radius);
      REQUIRE(sliding_max(x.data(), n, radius) == brute_sliding_max(x, radius));
    }
  }
  REQUIRE(sliding_max(nullptr, 0, 3).empty());

  const std::vector<float> x = {1.0f, 5.0f, 2.0f};
  const auto all = sliding_max(x.data(), x.size(), 3);
  for (float v : all) REQUIRE(v == 5.0f);
}

TEST_CASE("select_peaks_min_distance keeps the strongest peaks at least min_distance apart",
          "[util][peak]") {
  const int D = 10;
  std::vector<float> v(100, 0.0f);

  SECTION("a pair exactly D apart keeps both") {
    v[20] = 1.0f;
    v[20 + D] = 2.0f;
    REQUIRE(select_peaks_min_distance({20, 20 + D}, v.data(), D) == std::vector<int>{20, 30});
  }

  SECTION("a pair D-1 apart keeps the stronger") {
    v[20] = 1.0f;
    v[20 + D - 1] = 2.0f;
    REQUIRE(select_peaks_min_distance({20, 29}, v.data(), D) == std::vector<int>{29});
    v[20] = 3.0f;
    REQUIRE(select_peaks_min_distance({20, 29}, v.data(), D) == std::vector<int>{20});
  }

  SECTION("equal values keep the earlier frame") {
    v[20] = 1.0f;
    v[25] = 1.0f;
    REQUIRE(select_peaks_min_distance({25, 20}, v.data(), D) == std::vector<int>{20});
  }

  SECTION("increasing and decreasing 3-chains spaced 0.7 D mirror each other") {
    const int gap = 7;
    v[10] = 1.0f;
    v[10 + gap] = 2.0f;
    v[10 + 2 * gap] = 3.0f;
    REQUIRE(select_peaks_min_distance({10, 17, 24}, v.data(), D) == std::vector<int>{10, 24});
    v[10] = 3.0f;
    v[17] = 2.0f;
    v[24] = 1.0f;
    REQUIRE(select_peaks_min_distance({10, 17, 24}, v.data(), D) == std::vector<int>{10, 24});
  }

  SECTION("min_distance <= 1 returns every candidate ascending") {
    REQUIRE(select_peaks_min_distance({5, 3, 4}, v.data(), 1) == std::vector<int>{3, 4, 5});
    REQUIRE(select_peaks_min_distance({5, 3, 4}, v.data(), 0) == std::vector<int>{3, 4, 5});
    REQUIRE(select_peaks_min_distance({}, v.data(), D).empty());
  }

  SECTION("agrees with a repeated-argmax reference on random inputs") {
    std::mt19937 rng(2024);
    for (int trial = 0; trial < 200; ++trial) {
      const int n = 20 + static_cast<int>(rng() % 60);
      const int dist = 2 + static_cast<int>(rng() % 12);
      std::vector<float> vals(n);
      // Coarse quantization produces frequent ties.
      for (auto& x : vals) x = static_cast<float>(rng() % 8);
      std::vector<int> cand;
      for (int i = 0; i < n; ++i) {
        if (rng() % 3 == 0) cand.push_back(i);
      }
      CAPTURE(trial, n, dist);
      REQUIRE(select_peaks_min_distance(cand, vals.data(), dist) ==
              reference_select(cand, vals, dist));
    }
  }
}

TEST_CASE("far_event cases have the layout the design states", "[util][peak]") {
  const float window_sec = 5.0f;
  const int sr = 22050;
  const auto row = GENERATE(
      from_range(std::begin(sonare::test::kFarEventRows), std::end(sonare::test::kFarEventRows)));
  const auto c = sonare::test::make_far_event_case(row, window_sec, sr);

  const double target_sec = 2.0 * (window_sec + 2.0);
  const double insert_sec = row.distance_factor * window_sec + 4.0;
  CAPTURE(static_cast<int>(row.dominant), static_cast<int>(row.side), row.distance_factor,
          static_cast<int>(row.weak));
  REQUIRE(std::abs(static_cast<double>(c.base.size()) - target_sec * sr) <= 1.0);
  REQUIRE(std::abs(static_cast<double>(c.inserted.size()) - (target_sec + insert_sec) * sr) <= 1.0);
  REQUIRE(c.target_length_sec == Catch::Approx(target_sec));
  REQUIRE(c.weak_change_sec == Catch::Approx(target_sec / 2.0));
  if (row.side == sonare::test::Side::Before) {
    REQUIRE(c.target_offset_sec == Catch::Approx(insert_sec));
  } else {
    REQUIRE(c.target_offset_sec == 0.0f);
  }

  // The target region is identical in both signals, and every part has RMS 0.1.
  const std::size_t off = static_cast<std::size_t>(std::lround(c.target_offset_sec * sr));
  REQUIRE(std::equal(c.base.begin(), c.base.end(), c.inserted.begin() + off));
  double ss = 0.0;
  for (float s : c.base) ss += static_cast<double>(s) * s;
  REQUIRE(std::sqrt(ss / c.base.size()) == Catch::Approx(0.1).epsilon(0.02));
}

TEST_CASE("max_excluding_top is not moved by one dominant outlier", "[util][peak]") {
  std::vector<float> body = {0.4f, 0.5f, 0.6f, 0.55f, 0.45f, 0.7f};
  const float without = max_excluding_top(body, kReferenceIgnoredTopEvents);

  body.push_back(100.0f);
  CHECK(max_excluding_top(body, 1) == Catch::Approx(0.7f));
  // Two ignored: the outlier and the body's own maximum are both set aside.
  CHECK(max_excluding_top(body, kReferenceIgnoredTopEvents) == Catch::Approx(0.6f));
  CHECK(without == Catch::Approx(0.55f));
  CHECK(max_excluding_top(body, 0) == Catch::Approx(100.0f));
}

TEST_CASE("max_excluding_top falls back to the plain maximum on short input", "[util][peak]") {
  CHECK(max_excluding_top({3.0f, 9.0f}, 2) == Catch::Approx(9.0f));
  CHECK(max_excluding_top({3.0f, 9.0f, 5.0f}, 3) == Catch::Approx(9.0f));
  CHECK(max_excluding_top({7.0f}, 2) == Catch::Approx(7.0f));
}

TEST_CASE("max_excluding_top of an empty population is zero", "[util][peak]") {
  CHECK(max_excluding_top({}, 0) == 0.0f);
  CHECK(max_excluding_top({}, kReferenceIgnoredTopEvents) == 0.0f);
}

namespace {

/// @brief Reference by enumeration: every frame's window events, sorted and walked from the top.
std::vector<float> brute_without_lone_peaks(const std::vector<float>& x, std::size_t radius,
                                            const std::vector<int>& events, float ratio) {
  const auto n = static_cast<long long>(x.size());
  const auto r = static_cast<long long>(radius);
  std::vector<float> y(x.size());
  for (long long i = 0; i < n; ++i) {
    std::vector<float> heights;
    float window_max = x[static_cast<std::size_t>(i)];
    for (long long j = std::max(0LL, i - r); j <= std::min(n - 1, i + r); ++j) {
      window_max = std::max(window_max, x[static_cast<std::size_t>(j)]);
      if (std::find(events.begin(), events.end(), static_cast<int>(j)) != events.end()) {
        heights.push_back(x[static_cast<std::size_t>(j)]);
      }
    }
    if (heights.empty()) {
      y[static_cast<std::size_t>(i)] = window_max;
      continue;
    }
    std::sort(heights.begin(), heights.end(), std::greater<float>());
    std::size_t k = 0;
    while (k + 1 < heights.size() && heights[k + 1] < ratio * heights[k]) ++k;
    y[static_cast<std::size_t>(i)] = heights[k];
  }
  return y;
}

/// @brief Reference level at the middle sample of three events placed apart within one window.
float lone_peak_reference(const std::vector<float>& heights, float ratio) {
  std::vector<float> x(9, 0.0f);
  const std::vector<int> events = {1, 4, 7};
  for (std::size_t e = 0; e < heights.size(); ++e)
    x[static_cast<std::size_t>(events[e])] = heights[e];
  std::vector<int> used(events.begin(),
                        events.begin() + static_cast<std::ptrdiff_t>(heights.size()));
  return sliding_max_without_lone_peaks(x.data(), x.size(), x.size(), used, ratio)[4];
}

}  // namespace

TEST_CASE("sliding_max_without_lone_peaks sets aside an event nothing else comes near",
          "[util][peak]") {
  // 0.5 reaches 0.3 of 1.0, so the strongest event keeps the level.
  CHECK(lone_peak_reference({1.0f, 0.5f, 0.1f}, 0.3f) == 1.0f);
  // 0.2 does not, and 0.15 reaches 0.3 of 0.2, so the walk stops at 0.2.
  CHECK(lone_peak_reference({1.0f, 0.2f, 0.15f}, 0.3f) == 0.2f);
  // Each event is lone against the one above it, so the walk reaches the weakest.
  CHECK(lone_peak_reference({1.0f, 0.2f, 0.05f}, 0.3f) == 0.05f);
  CHECK(lone_peak_reference({0.7f}, 0.3f) == 0.7f);
  CHECK(lone_peak_reference({1.0f, 0.2f, 0.05f}, 0.0f) == 1.0f);
}

TEST_CASE("sliding_max_without_lone_peaks falls back to the window maximum away from events",
          "[util][peak]") {
  const std::vector<float> x = {0.2f, 0.9f, 0.1f, 0.0f, 0.0f, 0.0f, 0.3f, 0.1f};
  const std::vector<int> events = {1};
  const auto y = sliding_max_without_lone_peaks(x.data(), x.size(), 2, events, 0.3f);
  CHECK(y == std::vector<float>{0.9f, 0.9f, 0.9f, 0.9f, 0.3f, 0.3f, 0.3f, 0.3f});
  // A zero radius makes every sample its own reference.
  CHECK(sliding_max_without_lone_peaks(x.data(), x.size(), 0, events, 0.3f) == x);
  CHECK(sliding_max_without_lone_peaks(nullptr, 0, 3, {}, 0.3f).empty());
}

TEST_CASE("sliding_max_without_lone_peaks matches an enumerated reference", "[util][peak]") {
  std::mt19937 rng(4242);
  // Heights spread over two decades so lone events of every depth occur.
  std::uniform_real_distribution<float> log_height(-2.0f, 0.0f);
  for (int trial = 0; trial < 300; ++trial) {
    const std::size_t n = 1 + rng() % 40;
    std::vector<float> x(n);
    for (auto& v : x) v = std::pow(10.0f, log_height(rng));
    std::vector<int> events;
    for (std::size_t i = 0; i < n; ++i) {
      if (rng() % 4 == 0) events.push_back(static_cast<int>(i));
    }
    const std::size_t radius = rng() % (n + 3);
    const float ratio = static_cast<float>(rng() % 6) * 0.1f;
    CAPTURE(trial, n, radius, ratio);
    REQUIRE(sliding_max_without_lone_peaks(x.data(), n, radius, events, ratio) ==
            brute_without_lone_peaks(x, radius, events, ratio));
  }
}
