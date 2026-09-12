/// @file sonare_c_metering_extra_test.cpp
/// @brief C API tests for the multi-channel / standards-compliant LUFS
///        extensions (sonare_lufs_interleaved, sonare_ebur128_loudness_range),
///        the extended true-peak oversample-factor validation (factor 16
///        accepted, non-power-of-two rejected), and sonare_metering_spectrum_frame's
///        windowed-copy path against the whole-buffer metering::spectrum_frame oracle.

#include <sonare/sonare_c.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "core/audio.h"
#include "metering/spectrum.h"
#include "util/constants.h"

using namespace sonare;

namespace {

// Generate a mono sine wave buffer.
std::vector<float> generate_sine(float freq, int sample_rate, float duration) {
  size_t n_samples = static_cast<size_t>(sample_rate * duration);
  std::vector<float> samples(n_samples);
  for (size_t i = 0; i < n_samples; ++i) {
    samples[i] =
        std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * freq * i / sample_rate);
  }
  return samples;
}

// Interleave two equal-length mono channels into a stereo buffer.
std::vector<float> interleave_stereo(const std::vector<float>& left,
                                     const std::vector<float>& right) {
  std::vector<float> out(left.size() * 2);
  for (size_t i = 0; i < left.size(); ++i) {
    out[2 * i] = left[i];
    out[2 * i + 1] = right[i];
  }
  return out;
}

}  // namespace

TEST_CASE("sonare_lufs_interleaved", "[c_api]") {
  SECTION("mono interleaved matches sonare_lufs") {
    auto samples = generate_sine(440.0f, 48000, 3.0f);

    SonareLufsResult mono_result = {};
    SonareLufsResult inter_result = {};
    REQUIRE(sonare_lufs(samples.data(), samples.size(), 48000, &mono_result) == SONARE_OK);
    REQUIRE(sonare_lufs_interleaved(samples.data(), samples.size(), 1, 48000, &inter_result) ==
            SONARE_OK);

    REQUIRE(inter_result.integrated_lufs ==
            Catch::Approx(mono_result.integrated_lufs).margin(1e-3f));
    REQUIRE(inter_result.short_term_lufs ==
            Catch::Approx(mono_result.short_term_lufs).margin(1e-3f));
    REQUIRE(inter_result.loudness_range == Catch::Approx(mono_result.loudness_range).margin(1e-3f));
  }

  SECTION("stereo interleaved buffer yields finite loudness") {
    auto left = generate_sine(440.0f, 48000, 3.0f);
    auto right = generate_sine(660.0f, 48000, 3.0f);
    auto stereo = interleave_stereo(left, right);

    SonareLufsResult result = {};
    REQUIRE(sonare_lufs_interleaved(stereo.data(), left.size(), 2, 48000, &result) == SONARE_OK);

    REQUIRE(std::isfinite(result.integrated_lufs));
    REQUIRE(std::isfinite(result.momentary_lufs));
    REQUIRE(std::isfinite(result.short_term_lufs));
    REQUIRE(std::isfinite(result.loudness_range));
    REQUIRE(result.loudness_range >= 0.0f);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_sine(440.0f, 48000, 1.0f);
    SonareLufsResult result = {};

    REQUIRE(sonare_lufs_interleaved(samples.data(), samples.size(), 1, 48000, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_lufs_interleaved(nullptr, samples.size(), 1, 48000, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_lufs_interleaved(samples.data(), samples.size(), 0, 48000, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_lufs_interleaved(samples.data(), samples.size(), 2, 0, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_ebur128_loudness_range", "[c_api]") {
  SECTION("returns a finite, non-negative loudness range") {
    auto samples = generate_sine(440.0f, 48000, 6.0f);
    float lra = -1.0f;
    REQUIRE(sonare_ebur128_loudness_range(samples.data(), samples.size(), 48000, &lra) ==
            SONARE_OK);
    REQUIRE(std::isfinite(lra));
    REQUIRE(lra >= 0.0f);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_sine(440.0f, 48000, 1.0f);
    float lra = 0.0f;

    REQUIRE(sonare_ebur128_loudness_range(samples.data(), samples.size(), 48000, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_ebur128_loudness_range(nullptr, samples.size(), 48000, &lra) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_metering_true_peak_db oversample factor validation", "[c_api]") {
  auto samples = generate_sine(440.0f, 48000, 1.0f);

  SECTION("accepts factor 16") {
    float tp_db = 0.0f;
    REQUIRE(sonare_metering_true_peak_db(samples.data(), samples.size(), 48000, 16, &tp_db) ==
            SONARE_OK);
    REQUIRE(std::isfinite(tp_db));
  }

  SECTION("accepts the other supported power-of-two factors") {
    for (int factor : {1, 2, 4, 8}) {
      float tp_db = 0.0f;
      REQUIRE(sonare_metering_true_peak_db(samples.data(), samples.size(), 48000, factor, &tp_db) ==
              SONARE_OK);
      REQUIRE(std::isfinite(tp_db));
    }
  }

  SECTION("rejects non-power-of-two factor 3") {
    float tp_db = 0.0f;
    REQUIRE(sonare_metering_true_peak_db(samples.data(), samples.size(), 48000, 3, &tp_db) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("rejects factor above the supported maximum") {
    float tp_db = 0.0f;
    REQUIRE(sonare_metering_true_peak_db(samples.data(), samples.size(), 48000, 32, &tp_db) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

namespace {

// One 5-second, fixed-seed-PRNG mono buffer shared by every frame/offset case below.
constexpr size_t kSpectrumFrameLength = 220500;  // 5 s at 44100 Hz
constexpr int kSpectrumFrameSampleRate = 44100;

std::vector<float> make_spectrum_frame_fixture() {
  std::vector<float> samples(kSpectrumFrameLength);
  uint32_t state = 22695477u;
  for (size_t i = 0; i < kSpectrumFrameLength; ++i) {
    state = state * 1664525u + 1013904223u;
    const float unit = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    samples[i] = 2.0f * unit - 1.0f;
  }
  return samples;
}

// A SpectrumConfig axis; 0 in octave_fraction/db_ref/db_amin means "library default"
// (mirrors sonare_metering_spectrum_frame's own 0-is-default parameter convention).
struct SpectrumFrameConfigCase {
  int n_fft;
  int apply_octave_smoothing;
  int octave_fraction;
  float db_ref;
  float db_amin;
};

metering::SpectrumConfig to_cpp_config(const SpectrumFrameConfigCase& c) {
  metering::SpectrumConfig cfg;
  cfg.n_fft = c.n_fft;
  cfg.apply_octave_smoothing = c.apply_octave_smoothing != 0;
  if (c.octave_fraction > 0) cfg.octave_fraction = c.octave_fraction;
  if (c.db_ref > 0.0f) cfg.db_ref = c.db_ref;
  if (c.db_amin > 0.0f) cfg.db_amin = c.db_amin;
  return cfg;
}

}  // namespace

TEST_CASE("sonare_metering_spectrum_frame's windowed copy matches the whole-buffer oracle",
          "[c_api][metering][spectrum]") {
  const std::vector<float> samples = make_spectrum_frame_fixture();
  const Audio full_audio =
      Audio::from_buffer(samples.data(), samples.size(), kSpectrumFrameSampleRate);

  const std::vector<SpectrumFrameConfigCase> configs = {
      {2048, 0, 0, 0.0f, 0.0f},  // library defaults
      {2048, 1, 3, 0.0f, 0.0f},  // 1/3-octave smoothing
      {2048, 0, 0, 0.0f, 1.0f},  // non-default db_amin -- the empty-Audio-shortcut trap
      {512, 0, 0, 0.0f, 0.0f},   // a second n_fft
  };

  for (const SpectrumFrameConfigCase& config_case : configs) {
    const size_t n_fft = static_cast<size_t>(config_case.n_fft);
    // kSpectrumFrameLength - 1 is a legitimate boundary and stays in the axis, but it
    // cannot catch an offset mix-up: at that position the window is a single sample,
    // and the periodic Hann window's first coefficient is exactly 0.0 (0.5*(1-cos(0))),
    // so that one sample is zeroed regardless of which offset it was read from. -2
    // covers the same tail boundary with a 2-sample window, whose second coefficient
    // is nonzero.
    const std::vector<size_t> offsets = {
        0,
        1,
        n_fft / 2,
        kSpectrumFrameLength / 2,
        kSpectrumFrameLength - n_fft,
        kSpectrumFrameLength - n_fft / 2,
        kSpectrumFrameLength - 2,
        kSpectrumFrameLength - 1,
        kSpectrumFrameLength,
        kSpectrumFrameLength + 1,
        SIZE_MAX,
    };

    const metering::SpectrumConfig cpp_cfg = to_cpp_config(config_case);

    for (size_t frame_offset : offsets) {
      CAPTURE(config_case.n_fft, config_case.apply_octave_smoothing, config_case.octave_fraction,
              config_case.db_ref, config_case.db_amin, frame_offset);

      const metering::SpectrumResult expect =
          metering::spectrum_frame(full_audio, frame_offset, cpp_cfg);

      SonareSpectrumResult actual = {};
      const SonareError err = sonare_metering_spectrum_frame(
          samples.data(), samples.size(), kSpectrumFrameSampleRate, frame_offset, config_case.n_fft,
          config_case.apply_octave_smoothing, config_case.octave_fraction, config_case.db_ref,
          config_case.db_amin, &actual);

      REQUIRE(err == SONARE_OK);
      REQUIRE(actual.bin_count == expect.magnitude.size());

      for (size_t i = 0; i < actual.bin_count; ++i) {
        CHECK(actual.frequencies[i] == expect.frequencies[i]);
        CHECK(actual.magnitude[i] == expect.magnitude[i]);
        CHECK(actual.power[i] == expect.power[i]);
        CHECK(actual.db[i] == expect.db[i]);
      }

      sonare_free_spectrum_result(&actual);
    }
  }
}

TEST_CASE("sonare_metering_spectrum_frame still scans the whole buffer for non-finite samples",
          "[c_api][metering][spectrum]") {
  // The windowed copy only touches [0, n_fft), but a NaN planted well outside that
  // window (near the end of a buffer shorter than n_fft) must still fail -- the
  // full-length finiteness scan is the retained contract, not an incidental effect
  // of copying the whole buffer.
  std::vector<float> samples(512, 0.0f);
  samples[samples.size() - 10] = std::numeric_limits<float>::quiet_NaN();

  SonareSpectrumResult result = {};
  const SonareError err = sonare_metering_spectrum_frame(
      samples.data(), samples.size(), 44100, /*frame_offset=*/0, /*n_fft=*/2048,
      /*apply_octave_smoothing=*/0, /*octave_fraction=*/0, /*db_ref=*/0.0f, /*db_amin=*/0.0f,
      &result);

  REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
}
