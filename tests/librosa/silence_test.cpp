/// @file silence_test.cpp
/// @brief Reference compatibility tests for librosa-style trim / split.

#include "effects/silence.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "util/constants.h"
#include "util/exception.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::constants;
using namespace sonare::test;

namespace {

std::vector<float> tone(int sr, float duration, float freq, float amp) {
  size_t n = static_cast<size_t>(duration * sr);
  std::vector<float> y(n);
  for (size_t i = 0; i < n; ++i) {
    y[i] = amp * std::sin(constants::kTwoPi * freq * static_cast<float>(i) / sr);
  }
  return y;
}

std::vector<float> build_signal(int sr) {
  std::vector<float> y;
  // 0.2s silence + 0.3s 440Hz + 0.2s silence + 0.3s 660Hz + 0.2s silence
  auto add_silence = [&](float d) {
    size_t n = static_cast<size_t>(d * sr);
    y.insert(y.end(), n, 0.0f);
  };
  auto add_tone = [&](float d, float f) {
    auto t = tone(sr, d, f, 0.5f);
    y.insert(y.end(), t.begin(), t.end());
  };
  add_silence(0.2f);
  add_tone(0.3f, 440.0f);
  add_silence(0.2f);
  add_tone(0.3f, 660.0f);
  add_silence(0.2f);
  return y;
}

}  // namespace

TEST_CASE("trim narrows silent edges (librosa parity within 1024 samples)", "[librosa][silence]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/silence.json");
  const auto& d = json["data"];
  int sr = d["sr"].as_int();
  float top_db = d["top_db"].as_float();
  int frame_length = d["frame_length"].as_int();
  int hop = d["hop_length"].as_int();
  int expected_start = d["trim_start_sample"].as_int();
  int expected_end = d["trim_end_sample"].as_int();

  auto y = build_signal(sr);
  auto r = trim(y, top_db, frame_length, hop);
  CAPTURE(r.start_sample, r.end_sample, expected_start, expected_end);
  // Allow ~1 hop tolerance (RMS frame center differences with librosa).
  REQUIRE(std::abs(r.start_sample - expected_start) <= hop + 1);
  REQUIRE(std::abs(r.end_sample - expected_end) <= hop + 1);
  REQUIRE(static_cast<int>(r.audio.size()) == r.end_sample - r.start_sample);
}

TEST_CASE("split returns non-silent intervals (librosa parity)", "[librosa][silence]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/silence.json");
  const auto& d = json["data"];
  int sr = d["sr"].as_int();
  float top_db = d["top_db"].as_float();
  int frame_length = d["frame_length"].as_int();
  int hop = d["hop_length"].as_int();
  const auto& expected = d["split_intervals"].as_array();

  auto y = build_signal(sr);
  auto intervals = split(y, top_db, frame_length, hop);
  // librosa may merge two close tones into one interval depending on
  // top_db, so just sanity-check counts and rough alignment.
  REQUIRE(intervals.size() >= 1);
  REQUIRE(intervals.size() <= expected.size() + 1);

  // First interval start should match within one hop.
  int first_expected_start = expected[0][0].as_int();
  CAPTURE(intervals.front().first, first_expected_start);
  REQUIRE(std::abs(intervals.front().first - first_expected_start) <= hop + 1);
}

TEST_CASE("trim on all-silent input returns empty audio", "[silence][edge]") {
  std::vector<float> y(22050, 0.0f);
  auto r = trim(y);
  REQUIRE(r.audio.empty());
  REQUIRE(r.start_sample == 0);
  REQUIRE(r.end_sample == 0);
}

TEST_CASE("trim separates an empty input from an all-silent one", "[silence][edge]") {
  const int sr = 22050;

  SECTION("a normal signal still trims") {
    const auto y = build_signal(sr);
    const auto r = trim(y, 60.0f, 2048, 512);
    REQUIRE(r.end_sample > r.start_sample);
    REQUIRE_FALSE(r.audio.empty());
  }

  SECTION("all-silent succeeds with the empty-range result") {
    const std::vector<float> silent(sr, 0.0f);
    const auto r = trim(silent, 60.0f, 2048, 512);
    REQUIRE(r.audio.empty());
    REQUIRE(r.start_sample == 0);
    REQUIRE(r.end_sample == 0);
  }

  SECTION("empty is rejected, so the two cases are distinguishable") {
    const std::vector<float> empty;
    try {
      static_cast<void>(trim(empty, 60.0f, 2048, 512));
      FAIL("Expected trim to reject an empty input");
    } catch (const SonareException& error) {
      REQUIRE(error.code() == ErrorCode::InvalidParameter);
    }
    REQUIRE_THROWS_AS(trim(nullptr, 0, 60.0f, 2048, 512), SonareException);
  }
}

TEST_CASE("split separates an empty input from an all-silent one", "[silence][edge]") {
  const int sr = 22050;

  SECTION("a normal signal still yields intervals") {
    const auto y = build_signal(sr);
    REQUIRE_FALSE(split(y, 60.0f, 2048, 512).empty());
  }

  SECTION("all-silent succeeds with zero intervals") {
    const std::vector<float> silent(sr, 0.0f);
    REQUIRE(split(silent, 60.0f, 2048, 512).empty());
  }

  SECTION("empty is rejected, so the two cases are distinguishable") {
    const std::vector<float> empty;
    try {
      static_cast<void>(split(empty, 60.0f, 2048, 512));
      FAIL("Expected split to reject an empty input");
    } catch (const SonareException& error) {
      REQUIRE(error.code() == ErrorCode::InvalidParameter);
    }
    REQUIRE_THROWS_AS(split(nullptr, 0, 60.0f, 2048, 512), SonareException);
  }
}

TEST_CASE("silence thresholds are finite and strictly positive", "[silence][validation]") {
  const std::vector<float> signal{0.0f, 1.0f, 0.0f};
  const float negative_zero = -0.0f;
  const float negative_neighbor = std::nextafter(0.0f, -1.0f);
  const float positive_neighbor = std::nextafter(0.0f, 1.0f);
  const std::vector<float> invalid = {
      std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(),
      0.0f,
      negative_zero,
      negative_neighbor,
  };

  for (const float top_db : invalid) {
    CAPTURE(top_db);
    REQUIRE_THROWS_AS(trim(signal, top_db, 3, 1), SonareException);
    REQUIRE_THROWS_AS(split(signal, top_db, 3, 1), SonareException);
  }

  // The positive subnormal and FLT_MAX are both valid domain values, even
  // though the detector's threshold arithmetic may classify this tiny signal
  // differently at the two extremes.
  REQUIRE_NOTHROW(trim(signal, positive_neighbor, 3, 1));
  REQUIRE_NOTHROW(split(signal, positive_neighbor, 3, 1));
  REQUIRE_NOTHROW(trim(signal, std::numeric_limits<float>::max(), 3, 1));
  REQUIRE_NOTHROW(split(signal, std::numeric_limits<float>::max(), 3, 1));
}

TEST_CASE("silence validation preserves input and framing precedence", "[silence][validation]") {
  const float sample = 1.0f;
  const float nan = std::numeric_limits<float>::quiet_NaN();

  SECTION("empty input wins over every other parameter") {
    try {
      static_cast<void>(trim(nullptr, 0, nan, 0, 0));
      FAIL("Expected trim to reject an empty input first");
    } catch (const SonareException& error) {
      REQUIRE(std::string(error.what()).find("empty") != std::string::npos);
    }
  }

  SECTION("null input wins over framing and threshold checks") {
    try {
      static_cast<void>(trim(nullptr, 1, nan, 0, 0));
      FAIL("Expected trim to reject a null input first");
    } catch (const SonareException& error) {
      REQUIRE(std::string(error.what()).find("null input") != std::string::npos);
    }
  }

  SECTION("framing wins over threshold checks") {
    try {
      static_cast<void>(trim(&sample, 1, nan, 0, 0));
      FAIL("Expected trim to reject framing first");
    } catch (const SonareException& error) {
      REQUIRE(std::string(error.what()).find("frame_length") != std::string::npos);
    }
  }
}

TEST_CASE("silence rejects lengths that do not fit int sample indices", "[silence][validation]") {
  const float sample = 1.0f;
  const std::size_t too_long = static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1;

  REQUIRE_THROWS_AS(trim(&sample, too_long, 60.0f, 2048, 512), SonareException);
  REQUIRE_THROWS_AS(split(&sample, too_long, 60.0f, 2048, 512), SonareException);

  const float* signals[] = {&sample};
  const std::size_t lengths[] = {too_long};
  REQUIRE_THROWS_AS(split_common_with_report(signals, lengths, 1, 60.0f, 2048, 512),
                    SonareException);
}

namespace {

std::vector<std::pair<int, int>> reference_split(const std::vector<float>& x, float top_db,
                                                 int frame_length, int hop) {
  const int n = static_cast<int>(x.size());
  const int half = frame_length / 2;
  const int n_frames = n / hop + 1;
  std::vector<float> rms(static_cast<size_t>(n_frames));
  float peak = 0.0f;
  for (int f = 0; f < n_frames; ++f) {
    const int start = f * hop - half;
    double sum = 0.0;
    for (int i = std::max(0, start); i < std::min(n, start + frame_length); ++i) {
      sum += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
    }
    rms[static_cast<size_t>(f)] = static_cast<float>(std::sqrt(sum / frame_length));
    peak = std::max(peak, rms[static_cast<size_t>(f)]);
  }
  const float thr = peak * std::pow(10.0f, -top_db / 20.0f);
  std::vector<std::pair<int, int>> out;
  int run = -1;
  for (int f = 0; f <= n_frames; ++f) {
    const bool on = f < n_frames && rms[static_cast<size_t>(f)] > thr;
    if (on && run < 0) {
      run = f;
    } else if (!on && run >= 0) {
      const int s = run * hop;
      const int e = f == n_frames ? n : std::min(n, f * hop);
      if (e > s) out.emplace_back(s, e);
      run = -1;
    }
  }
  return out;
}

}  // namespace

TEST_CASE("split matches a direct per-frame RMS reference across window shapes",
          "[librosa][silence][frame-rms]") {
  uint32_t state = 0x1357bdf1u;
  const auto next = [&]() {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) / static_cast<float>(1u << 24) - 0.5f;
  };
  std::vector<float> y(30000, 0.0f);
  // Bursts of widely different level so a drifting running sum would show at the edges.
  for (size_t i = 3000; i < 7000; ++i) y[i] = 0.9f * next();
  for (size_t i = 12000; i < 13500; ++i) y[i] = 0.002f * next();
  for (size_t i = 20000; i < 26000; ++i) y[i] = 0.3f * next();
  for (int frame_length : {255, 256, 1024, 2047, 2048}) {
    for (int hop : {64, 512, 3000}) {
      INFO("frame_length " << frame_length << " hop " << hop);
      for (float top_db : {30.0f, 60.0f}) {
        REQUIRE(split(y, top_db, frame_length, hop) ==
                reference_split(y, top_db, frame_length, hop));
      }
    }
  }
}
