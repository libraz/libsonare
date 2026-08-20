/// @file sonare_c_features_extra_test.cpp
/// @brief Tests for the spectral-contrast / poly-features / zero-crossings /
///        tuning C API functions added on top of the core feature wrappers.

#include <sonare/sonare_c.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "support/audio_fixtures.h"
#include "util/constants.h"

namespace {

constexpr int kSampleRate = 22050;

// Returns a non-null sentinel so we can assert the wrapper clears outputs.
float* non_null_floats() { return reinterpret_cast<float*>(static_cast<std::uintptr_t>(0x1)); }
int* non_null_ints() { return reinterpret_cast<int*>(static_cast<std::uintptr_t>(0x1)); }

}  // namespace

TEST_CASE("sonare_note_segments validates and returns frame-accurate regions",
          "[c_api][features]") {
  const std::vector<float> f0 = {440.0f, 445.0f, 435.0f, 444.0f, 436.0f, 0.0f, 0.0f,
                                 660.0f, 666.0f, 654.0f, 665.0f, 655.0f, 0.0f};
  const std::vector<float> voiced = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f,
                                     1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f};
  SonareNoteSegmentsResult out{};
  SonareNoteSegmenterConfig config{};
  config.struct_version = 1;

  REQUIRE(sonare_note_segments(f0.data(), f0.size(), voiced.data(), voiced.size(), 100.0f, &config,
                               &out) == SONARE_OK);
  REQUIRE(out.count == 2);
  REQUIRE(out.segments != nullptr);
  REQUIRE(out.segments[0].frame_start == 0);
  REQUIRE(out.segments[0].frame_end == 5);
  REQUIRE(out.segments[0].start_seconds == Catch::Approx(0.0f));
  REQUIRE(out.segments[0].end_seconds == Catch::Approx(0.05f));
  REQUIRE(out.segments[1].frame_start == 7);
  REQUIRE(out.segments[1].frame_end == 12);
  REQUIRE(out.segments[1].start_seconds == Catch::Approx(0.07f));
  REQUIRE(out.segments[1].end_seconds == Catch::Approx(0.12f));
  sonare_free_note_segments(&out);
  REQUIRE(out.segments == nullptr);
  REQUIRE(out.count == 0);

  out.segments = reinterpret_cast<SonareNoteSegment*>(static_cast<std::uintptr_t>(0x1));
  out.count = 7;
  REQUIRE(sonare_note_segments(f0.data(), f0.size(), voiced.data(), voiced.size() - 1, 100.0f,
                               &config, &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out.segments == nullptr);
  REQUIRE(out.count == 0);

  const float non_finite = std::numeric_limits<float>::quiet_NaN();
  REQUIRE(sonare_note_segments(&non_finite, 1, voiced.data(), 1, 100.0f, &config, &out) ==
          SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare_spectral_contrast", "[c_api][features]") {
  auto samples = sonare::test::generate_sine_samples(
      440.0f, kSampleRate, static_cast<int>(static_cast<float>(kSampleRate) * 1.0f), 1.0f);

  SECTION("returns [(n_bands + 1) x n_frames] matrix") {
    const int n_bands = 6;
    float* out = nullptr;
    int rows = 0;
    int cols = 0;
    REQUIRE(sonare_spectral_contrast(samples.data(), samples.size(), kSampleRate, 2048, 512,
                                     n_bands, 200.0f, 0.02f, &out, &rows, &cols) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(rows == n_bands + 1);
    REQUIRE(cols > 0);
    const size_t total = static_cast<size_t>(rows) * static_cast<size_t>(cols);
    for (size_t i = 0; i < total; ++i) {
      REQUIRE(std::isfinite(out[i]));
    }
    sonare_free_floats(out);
  }

  SECTION("rejects null outputs without writing") {
    float* out = non_null_floats();
    int rows = 7;
    int cols = 7;
    REQUIRE(sonare_spectral_contrast(samples.data(), samples.size(), kSampleRate, 2048, 512, 6,
                                     200.0f, 0.02f, nullptr, &rows,
                                     &cols) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_spectral_contrast(samples.data(), samples.size(), kSampleRate, 2048, 512, 6,
                                     200.0f, 0.02f, &out, nullptr,
                                     &cols) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("invalid params clear outputs") {
    float* out = non_null_floats();
    int rows = 7;
    int cols = 7;
    // Null samples is an invalid parameter; outputs must be cleared.
    REQUIRE(sonare_spectral_contrast(nullptr, samples.size(), kSampleRate, 2048, 512, 6, 200.0f,
                                     0.02f, &out, &rows, &cols) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(rows == 0);
    REQUIRE(cols == 0);

    // n_bands <= 0 propagates as invalid parameter from the C++ layer.
    out = non_null_floats();
    rows = 7;
    cols = 7;
    REQUIRE(sonare_spectral_contrast(samples.data(), samples.size(), kSampleRate, 2048, 512, 0,
                                     200.0f, 0.02f, &out, &rows,
                                     &cols) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(rows == 0);
    REQUIRE(cols == 0);
  }
}

TEST_CASE("sonare_poly_features", "[c_api][features]") {
  auto samples = sonare::test::generate_sine_samples(
      440.0f, kSampleRate, static_cast<int>(static_cast<float>(kSampleRate) * 1.0f), 1.0f);

  SECTION("returns [(order + 1) x n_frames] matrix") {
    const int order = 2;
    float* out = nullptr;
    int rows = 0;
    int cols = 0;
    REQUIRE(sonare_poly_features(samples.data(), samples.size(), kSampleRate, 2048, 512, order,
                                 &out, &rows, &cols) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(rows == order + 1);
    REQUIRE(cols > 0);
    const size_t total = static_cast<size_t>(rows) * static_cast<size_t>(cols);
    for (size_t i = 0; i < total; ++i) {
      REQUIRE(std::isfinite(out[i]));
    }
    sonare_free_floats(out);
  }

  SECTION("rejects null outputs without writing") {
    float* out = non_null_floats();
    int rows = 7;
    int cols = 7;
    REQUIRE(sonare_poly_features(samples.data(), samples.size(), kSampleRate, 2048, 512, 1, nullptr,
                                 &rows, &cols) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_poly_features(samples.data(), samples.size(), kSampleRate, 2048, 512, 1, &out,
                                 &rows, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("invalid params clear outputs") {
    float* out = non_null_floats();
    int rows = 7;
    int cols = 7;
    REQUIRE(sonare_poly_features(nullptr, samples.size(), kSampleRate, 2048, 512, 1, &out, &rows,
                                 &cols) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(rows == 0);
    REQUIRE(cols == 0);
  }
}

TEST_CASE("sonare_zero_crossings", "[c_api][features]") {
  // Alternating-sign signal: a sign change occurs on every sample.
  std::vector<float> signal(16);
  for (size_t i = 0; i < signal.size(); ++i) {
    signal[i] = (i % 2 == 0) ? 1.0f : -1.0f;
  }

  SECTION("returns sorted crossing indices") {
    int* out = nullptr;
    size_t count = 0;
    REQUIRE(sonare_zero_crossings(signal.data(), signal.size(), sonare::constants::kEpsilon, 0, 1,
                                  1, &out, &count) == SONARE_OK);
    REQUIRE(count > 0);
    REQUIRE(out != nullptr);
    // pad=1 reports index 0; an alternating signal crosses on every later index.
    REQUIRE(out[0] == 0);
    for (size_t i = 1; i < count; ++i) {
      REQUIRE(out[i] > out[i - 1]);
    }
    sonare_free_ints(out);
  }

  SECTION("rejects null outputs without writing") {
    int* out = non_null_ints();
    size_t count = 7;
    REQUIRE(sonare_zero_crossings(signal.data(), signal.size(), sonare::constants::kEpsilon, 0, 1,
                                  1, nullptr, &count) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_zero_crossings(signal.data(), signal.size(), sonare::constants::kEpsilon, 0, 1,
                                  1, &out, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("invalid params clear outputs") {
    int* out = non_null_ints();
    size_t count = 7;
    // Negative threshold is invalid.
    REQUIRE(sonare_zero_crossings(signal.data(), signal.size(), -1.0f, 0, 1, 1, &out, &count) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(count == 0);

    // Null samples with non-zero length is invalid.
    out = non_null_ints();
    count = 7;
    REQUIRE(sonare_zero_crossings(nullptr, signal.size(), sonare::constants::kEpsilon, 0, 1, 1,
                                  &out, &count) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(count == 0);
  }
}

TEST_CASE("sonare_rms_energy follows empty-output C ABI contract", "[c_api][features]") {
  SECTION("empty input clears stale outputs") {
    float* out = non_null_floats();
    size_t count = 7;
    REQUIRE(sonare_rms_energy(nullptr, 0, kSampleRate, 2048, 512, &out, &count) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(count == 0);
  }

  SECTION("invalid params clear outputs") {
    auto samples = sonare::test::generate_sine_samples(
        440.0f, kSampleRate, static_cast<int>(static_cast<float>(kSampleRate) * 0.1f), 1.0f);
    float* out = non_null_floats();
    size_t count = 7;
    REQUIRE(sonare_rms_energy(nullptr, samples.size(), kSampleRate, 2048, 512, &out, &count) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(count == 0);
  }
}

TEST_CASE("sonare_pitch_tuning", "[c_api][features]") {
  SECTION("returns a tuning offset in (-0.5, 0.5]") {
    // Frequencies exactly on equal-tempered pitches -> tuning near 0.
    std::vector<float> freqs = {440.0f, 880.0f, 220.0f};
    float tuning = -1.0f;
    REQUIRE(sonare_pitch_tuning(freqs.data(), freqs.size(), 0.01f, 12, &tuning) == SONARE_OK);
    REQUIRE(std::isfinite(tuning));
    REQUIRE(tuning > -0.5f);
    REQUIRE(tuning <= 0.5f);
    REQUIRE(tuning == Catch::Approx(0.0f).margin(0.02f));
  }

  SECTION("rejects null out and invalid params") {
    std::vector<float> freqs = {440.0f};
    float tuning = 0.25f;
    REQUIRE(sonare_pitch_tuning(freqs.data(), freqs.size(), 0.01f, 12, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);

    tuning = 0.25f;
    REQUIRE(sonare_pitch_tuning(freqs.data(), freqs.size(), 0.0f, 12, &tuning) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(tuning == 0.0f);

    tuning = 0.25f;
    REQUIRE(sonare_pitch_tuning(freqs.data(), freqs.size(), 0.01f, 0, &tuning) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(tuning == 0.0f);
  }
}

TEST_CASE("sonare_estimate_tuning", "[c_api][features]") {
  auto samples = sonare::test::generate_sine_samples(
      440.0f, kSampleRate, static_cast<int>(static_cast<float>(kSampleRate) * 1.0f), 1.0f);

  SECTION("returns a finite tuning offset") {
    float tuning = -1.0f;
    REQUIRE(sonare_estimate_tuning(samples.data(), samples.size(), kSampleRate, 2048, 512, 0.01f,
                                   12, &tuning) == SONARE_OK);
    REQUIRE(std::isfinite(tuning));
    REQUIRE(tuning > -0.5f);
    REQUIRE(tuning <= 0.5f);
  }

  SECTION("rejects null out and invalid params") {
    REQUIRE(sonare_estimate_tuning(samples.data(), samples.size(), kSampleRate, 2048, 512, 0.01f,
                                   12, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

    float tuning = 0.25f;
    REQUIRE(sonare_estimate_tuning(samples.data(), samples.size(), kSampleRate, 2048, 512, 0.0f, 12,
                                   &tuning) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(tuning == 0.0f);

    tuning = 0.25f;
    REQUIRE(sonare_estimate_tuning(nullptr, samples.size(), kSampleRate, 2048, 512, 0.01f, 12,
                                   &tuning) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(tuning == 0.0f);
  }
}

// Like librosa.yin, YIN returns its global-minimum fallback for unvoiced
// frames. fill_na is retained for ABI compatibility and does not change YIN.
TEST_CASE("sonare_pitch_yin fill_na", "[c_api][features]") {
  std::vector<float> silence(static_cast<size_t>(kSampleRate), 0.0f);

  SECTION("fill_na = 0 returns finite fallback frequencies and unvoiced flags") {
    SonarePitchResult result{};
    REQUIRE(sonare_pitch_yin(silence.data(), silence.size(), kSampleRate, 2048, 512, 65.0f, 2093.0f,
                             0.3f, /*fill_na=*/0, &result) == SONARE_OK);
    REQUIRE(result.n_frames > 0);
    bool any_unvoiced = false;
    for (int i = 0; i < result.n_frames; ++i) {
      REQUIRE(std::isfinite(result.f0[i]));
      if (result.voiced_flag[i] == 0) any_unvoiced = true;
    }
    REQUIRE(any_unvoiced);
    sonare_free_pitch_result(&result);
  }

  SECTION("fill_na = 1 preserves the same finite fallback contract") {
    SonarePitchResult result{};
    REQUIRE(sonare_pitch_yin(silence.data(), silence.size(), kSampleRate, 2048, 512, 65.0f, 2093.0f,
                             0.3f, /*fill_na=*/1, &result) == SONARE_OK);
    REQUIRE(result.n_frames > 0);
    for (int i = 0; i < result.n_frames; ++i) {
      REQUIRE(std::isfinite(result.f0[i]));
    }
    sonare_free_pitch_result(&result);
  }
}

TEST_CASE("sonare_pitch_pyin fill_na", "[c_api][features]") {
  std::vector<float> silence(static_cast<size_t>(kSampleRate), 0.0f);

  SonarePitchResult nan_result{};
  REQUIRE(sonare_pitch_pyin(silence.data(), silence.size(), kSampleRate, 2048, 512, 65.0f, 2093.0f,
                            0.3f, /*fill_na=*/0, &nan_result) == SONARE_OK);
  REQUIRE(nan_result.n_frames > 0);
  bool any_nan = false;
  for (int i = 0; i < nan_result.n_frames; ++i) {
    if (std::isnan(nan_result.f0[i])) any_nan = true;
  }
  REQUIRE(any_nan);
  sonare_free_pitch_result(&nan_result);

  SonarePitchResult filled{};
  REQUIRE(sonare_pitch_pyin(silence.data(), silence.size(), kSampleRate, 2048, 512, 65.0f, 2093.0f,
                            0.3f, /*fill_na=*/1, &filled) == SONARE_OK);
  for (int i = 0; i < filled.n_frames; ++i) {
    REQUIRE(std::isfinite(filled.f0[i]));
  }
  sonare_free_pitch_result(&filled);
}

TEST_CASE("sonare_analyze_timbre exposes timbre_over_time", "[c_api][features]") {
  auto samples = sonare::test::generate_sine_samples(
      440.0f, kSampleRate, static_cast<int>(static_cast<float>(kSampleRate) * 2.0f), 1.0f);

  SonareTimbreResult result{};
  REQUIRE(sonare_analyze_timbre(samples.data(), samples.size(), kSampleRate, 2048, 512, 128, 13,
                                0.5f, &result) == SONARE_OK);

  // A 2s signal with a 0.5s window must yield at least one per-window snapshot,
  // and the pointer/count pair must be consistent.
  REQUIRE(result.timbre_over_time_count > 0);
  REQUIRE(result.timbre_over_time != nullptr);
  for (size_t i = 0; i < result.timbre_over_time_count; ++i) {
    const SonareTimbreFrame& frame = result.timbre_over_time[i];
    REQUIRE(std::isfinite(frame.brightness));
    REQUIRE(std::isfinite(frame.warmth));
    REQUIRE(std::isfinite(frame.density));
    REQUIRE(std::isfinite(frame.roughness));
    REQUIRE(std::isfinite(frame.complexity));
  }

  sonare_free_timbre_result(&result);

  // Double-free / free-after-clear must be safe (pointers nulled on free).
  sonare_free_timbre_result(&result);
  REQUIRE(result.timbre_over_time == nullptr);
  REQUIRE(result.timbre_over_time_count == 0);
}

TEST_CASE("sonare_trim_silence and sonare_split_silence reject an empty input",
          "[c_api][features][edge]") {
  // 0.1 s of silence framed by 0.1 s tones, so a real trim has to move both edges.
  const int quarter = kSampleRate / 10;
  std::vector<float> samples;
  const auto tone = sonare::test::generate_sine_samples(440.0f, kSampleRate, quarter, 1.0f);
  samples.insert(samples.end(), tone.begin(), tone.end());
  samples.insert(samples.end(), static_cast<size_t>(quarter), 0.0f);
  samples.insert(samples.end(), tone.begin(), tone.end());

  const std::vector<float> silence(static_cast<size_t>(kSampleRate), 0.0f);

  SECTION("trim: normal, all-silent and empty are three distinct outcomes") {
    float* out = nullptr;
    size_t count = 0;
    int start = -1;
    int end = -1;

    REQUIRE(sonare_trim_silence(samples.data(), samples.size(), 60.0f, 2048, 512, &out, &count,
                                &start, &end) == SONARE_OK);
    REQUIRE(count > 0);
    REQUIRE(out != nullptr);
    REQUIRE(end > start);
    sonare_free_floats(out);

    // All-silent still succeeds and reports the zero range.
    out = nullptr;
    count = 7;
    start = -1;
    end = -1;
    REQUIRE(sonare_trim_silence(silence.data(), silence.size(), 60.0f, 2048, 512, &out, &count,
                                &start, &end) == SONARE_OK);
    REQUIRE(out == nullptr);
    REQUIRE(count == 0);
    REQUIRE(start == 0);
    REQUIRE(end == 0);

    // Empty no longer shares that result: it is an error, with a message.
    REQUIRE(sonare_trim_silence(nullptr, 0, 60.0f, 2048, 512, &out, &count, &start, &end) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()).find("empty") != std::string::npos);
  }

  SECTION("split: normal, all-silent and empty are three distinct outcomes") {
    int* intervals = nullptr;
    size_t interval_count = 0;

    REQUIRE(sonare_split_silence(samples.data(), samples.size(), 60.0f, 2048, 512, &intervals,
                                 &interval_count) == SONARE_OK);
    REQUIRE(interval_count > 0);
    REQUIRE(intervals != nullptr);
    sonare_free_ints(intervals);

    intervals = nullptr;
    interval_count = 7;
    REQUIRE(sonare_split_silence(silence.data(), silence.size(), 60.0f, 2048, 512, &intervals,
                                 &interval_count) == SONARE_OK);
    REQUIRE(intervals == nullptr);
    REQUIRE(interval_count == 0);

    REQUIRE(sonare_split_silence(nullptr, 0, 60.0f, 2048, 512, &intervals, &interval_count) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(std::string(sonare_last_error_message()).find("empty") != std::string::npos);
  }
}

TEST_CASE("sonare_fix_frames rejects an empty frame list", "[c_api][features][edge]") {
  const std::vector<int> frames{2, 4};
  int* out = nullptr;
  size_t count = 0;

  REQUIRE(sonare_fix_frames(frames.data(), frames.size(), 0, 5, 1, &out, &count) == SONARE_OK);
  REQUIRE(count == 4);
  REQUIRE(out != nullptr);
  REQUIRE(out[0] == 0);
  REQUIRE(out[3] == 5);
  sonare_free_ints(out);

  // Previously this padded an empty list into the single element {x_min}.
  out = nullptr;
  count = 7;
  REQUIRE(sonare_fix_frames(nullptr, 0, 0, -1, 1, &out, &count) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(std::string(sonare_last_error_message()).find("empty") != std::string::npos);
}
