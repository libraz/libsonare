/// @file sonare_c_metering_extra_test.cpp
/// @brief C API tests for the multi-channel / standards-compliant LUFS
///        extensions (sonare_lufs_interleaved, sonare_lufs_series_interleaved,
///        sonare_ebur128_loudness_range),
///        the extended true-peak oversample-factor validation (factor 16
///        accepted, non-power-of-two rejected), and sonare_metering_spectrum_frame's
///        windowed copy, windowed non-finite scan and reused FFT plan against the
///        whole-buffer metering::spectrum_frame oracle.

#include <sonare/sonare_c.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>
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

TEST_CASE("sonare_lufs_series_interleaved", "[c_api][metering][lufs]") {
  constexpr int kSampleRate = 48000;
  const std::vector<float> mono = generate_sine(440.0f, kSampleRate, 5.0f);

  SECTION("one channel reproduces the mono series entry points") {
    float* momentary = nullptr;
    size_t momentary_length = 0;
    float* short_term = nullptr;
    size_t short_term_length = 0;
    REQUIRE(sonare_lufs_series_interleaved(mono.data(), mono.size(), 1, kSampleRate, &momentary,
                                           &momentary_length, &short_term,
                                           &short_term_length) == SONARE_OK);

    float* expect_momentary = nullptr;
    size_t expect_momentary_length = 0;
    float* expect_short_term = nullptr;
    size_t expect_short_term_length = 0;
    REQUIRE(sonare_momentary_lufs(mono.data(), mono.size(), kSampleRate, &expect_momentary,
                                  &expect_momentary_length) == SONARE_OK);
    REQUIRE(sonare_short_term_lufs(mono.data(), mono.size(), kSampleRate, &expect_short_term,
                                   &expect_short_term_length) == SONARE_OK);

    // Element for element: one channel carries unit weight, so the summed path
    // lands on the same block energies the mono meters compute.
    REQUIRE(momentary_length == expect_momentary_length);
    REQUIRE(short_term_length == expect_short_term_length);
    REQUIRE(momentary_length > 0);
    REQUIRE(short_term_length > 0);
    for (size_t i = 0; i < momentary_length; ++i) {
      CHECK(momentary[i] == expect_momentary[i]);
    }
    for (size_t i = 0; i < short_term_length; ++i) {
      CHECK(short_term[i] == expect_short_term[i]);
    }

    sonare_free_floats(expect_short_term);
    sonare_free_floats(expect_momentary);
    sonare_free_floats(short_term);
    sonare_free_floats(momentary);
  }

  SECTION("stereo sums channel energies rather than averaging loudness in dB") {
    // Duplicating the channel doubles the summed energy, so the series moves by
    // 10*log10(2). An implementation that averaged per-channel loudness, or that
    // downmixed to (L+R)/2, would leave it where the mono reading is.
    const std::vector<float> stereo = interleave_stereo(mono, mono);

    float* mono_series = nullptr;
    size_t mono_length = 0;
    float* stereo_series = nullptr;
    size_t stereo_length = 0;
    REQUIRE(sonare_lufs_series_interleaved(mono.data(), mono.size(), 1, kSampleRate, nullptr,
                                           nullptr, &mono_series, &mono_length) == SONARE_OK);
    REQUIRE(sonare_lufs_series_interleaved(stereo.data(), mono.size(), 2, kSampleRate, nullptr,
                                           nullptr, &stereo_series, &stereo_length) == SONARE_OK);

    REQUIRE(stereo_length == mono_length);
    REQUIRE(mono_length > 0);
    for (size_t i = 0; i < mono_length; ++i) {
      CAPTURE(i, mono_series[i], stereo_series[i]);
      CHECK(stereo_series[i] - mono_series[i] == Catch::Approx(3.0103f).margin(1e-3f));
    }

    sonare_free_floats(stereo_series);
    sonare_free_floats(mono_series);
  }

  SECTION("each pointer and its length are required together") {
    float* series = nullptr;
    size_t length = 0;

    CHECK(sonare_lufs_series_interleaved(mono.data(), mono.size(), 1, kSampleRate, &series, nullptr,
                                         nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_lufs_series_interleaved(mono.data(), mono.size(), 1, kSampleRate, nullptr, &length,
                                         nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_lufs_series_interleaved(mono.data(), mono.size(), 1, kSampleRate, nullptr, nullptr,
                                         &series, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_lufs_series_interleaved(mono.data(), mono.size(), 1, kSampleRate, nullptr, nullptr,
                                         nullptr, &length) == SONARE_ERROR_INVALID_PARAMETER);

    // Both pairs omitted is a legal request for nothing, not an error.
    CHECK(sonare_lufs_series_interleaved(mono.data(), mono.size(), 1, kSampleRate, nullptr, nullptr,
                                         nullptr, nullptr) == SONARE_OK);
  }

  SECTION("a clip shorter than the short-term window yields an empty series") {
    const std::vector<float> clip = generate_sine(440.0f, kSampleRate, 1.0f);
    // Pre-set so an entry point that returned early without writing its outputs
    // would be caught rather than read as an empty series.
    float sentinel = 0.0f;
    float* momentary = &sentinel;
    size_t momentary_length = 99;
    float* short_term = &sentinel;
    size_t short_term_length = 99;
    REQUIRE(sonare_lufs_series_interleaved(clip.data(), clip.size(), 1, kSampleRate, &momentary,
                                           &momentary_length, &short_term,
                                           &short_term_length) == SONARE_OK);

    REQUIRE(momentary_length > 0);
    REQUIRE(short_term == nullptr);
    REQUIRE(short_term_length == 0);
    sonare_free_floats(momentary);
  }

  SECTION("rejects the same invalid input as sonare_lufs_interleaved") {
    float* series = nullptr;
    size_t length = 0;
    const auto call = [&](const float* samples, size_t frames, int channels, int sample_rate) {
      return sonare_lufs_series_interleaved(samples, frames, channels, sample_rate, &series,
                                            &length, nullptr, nullptr);
    };

    CHECK(call(nullptr, mono.size(), 1, kSampleRate) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(call(mono.data(), mono.size(), 0, kSampleRate) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(call(mono.data(), mono.size(), 1, 0) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(call(mono.data(), 0, 1, kSampleRate) == SONARE_ERROR_INVALID_PARAMETER);
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

// Runs one frame on a thread that has never run one, so its FFT plan is built for
// this call alone -- the reference a reused plan has to reproduce.
metering::SpectrumResult spectrum_frame_on_fresh_thread(const Audio& audio, size_t frame_offset,
                                                        const metering::SpectrumConfig& config) {
  metering::SpectrumResult result;
  std::thread worker([&] { result = metering::spectrum_frame(audio, frame_offset, config); });
  worker.join();
  return result;
}

void require_identical_spectra(const metering::SpectrumResult& actual,
                               const metering::SpectrumResult& expect) {
  REQUIRE(actual.magnitude.size() == expect.magnitude.size());
  REQUIRE(actual.n_fft == expect.n_fft);
  for (size_t i = 0; i < actual.magnitude.size(); ++i) {
    CAPTURE(i);
    CHECK(actual.frequencies[i] == expect.frequencies[i]);
    CHECK(actual.magnitude[i] == expect.magnitude[i]);
    CHECK(actual.power[i] == expect.power[i]);
    CHECK(actual.db[i] == expect.db[i]);
  }
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

TEST_CASE("sonare_metering_spectrum_frame's non-finite scan covers the analysis frame",
          "[c_api][metering][spectrum]") {
  // The frame is the only span read, so it is the only span whose finiteness is a
  // precondition. Which samples that covers moves with frame_offset, not with length.
  constexpr int kNFft = 2048;
  constexpr size_t kLength = 8192;
  constexpr size_t kPoisonIndex = 6000;

  std::vector<float> samples(kLength, 0.25f);
  SonareSpectrumResult result = {};

  SECTION("a non-finite sample inside the frame is refused") {
    samples[100] = std::numeric_limits<float>::quiet_NaN();
    const SonareError err = sonare_metering_spectrum_frame(
        samples.data(), samples.size(), kSpectrumFrameSampleRate, /*frame_offset=*/0, kNFft,
        /*apply_octave_smoothing=*/0, /*octave_fraction=*/0, /*db_ref=*/0.0f, /*db_amin=*/0.0f,
        &result);
    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("the same sample outside the frame does not refuse the call") {
    samples[kPoisonIndex] = std::numeric_limits<float>::quiet_NaN();
    const SonareError err = sonare_metering_spectrum_frame(
        samples.data(), samples.size(), kSpectrumFrameSampleRate, /*frame_offset=*/0, kNFft,
        /*apply_octave_smoothing=*/0, /*octave_fraction=*/0, /*db_ref=*/0.0f, /*db_amin=*/0.0f,
        &result);
    REQUIRE(err == SONARE_OK);
    REQUIRE(result.bin_count > 0);
    for (size_t i = 0; i < result.bin_count; ++i) {
      CHECK(std::isfinite(result.magnitude[i]));
      CHECK(std::isfinite(result.db[i]));
    }
    sonare_free_spectrum_result(&result);
  }

  SECTION("a frame moved onto that sample is refused") {
    samples[kPoisonIndex] = std::numeric_limits<float>::quiet_NaN();
    const SonareError err = sonare_metering_spectrum_frame(
        samples.data(), samples.size(), kSpectrumFrameSampleRate, /*frame_offset=*/4096, kNFft,
        /*apply_octave_smoothing=*/0, /*octave_fraction=*/0, /*db_ref=*/0.0f, /*db_amin=*/0.0f,
        &result);
    REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("a reused FFT plan gives the same single-frame spectrum as a fresh one",
          "[c_api][metering][spectrum]") {
  const std::vector<float> samples = make_spectrum_frame_fixture();
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), kSpectrumFrameSampleRate);

  // Smallest n_fft first so the sequence is safe to run against a deliberately
  // colliding cache key, and each size recurs so both a miss and a hit are covered.
  const std::vector<int> n_ffts = {256, 2048, 512, 2048, 256, 512, 2048};

  // The whole sequence runs on one thread that starts with no cached plan, so which
  // calls hit and which miss does not depend on what ran before this case.
  std::vector<metering::SpectrumResult> reused(n_ffts.size());
  std::thread worker([&] {
    for (size_t call = 0; call < n_ffts.size(); ++call) {
      metering::SpectrumConfig cfg;
      cfg.n_fft = n_ffts[call];
      reused[call] = metering::spectrum_frame(audio, call * 1024, cfg);
    }
  });
  worker.join();

  for (size_t call = 0; call < n_ffts.size(); ++call) {
    CAPTURE(call, n_ffts[call]);
    metering::SpectrumConfig cfg;
    cfg.n_fft = n_ffts[call];
    require_identical_spectra(reused[call],
                              spectrum_frame_on_fresh_thread(audio, call * 1024, cfg));
  }
}

TEST_CASE("sonare_metering_spectrum_frame's per-call cost does not track buffer length",
          "[.][perf][c_api][metering][spectrum]") {
  constexpr int kNFft = 2048;
  constexpr size_t kShortLength = 1u << 14;
  constexpr size_t kLongLength = 1u << 22;
  constexpr int kCalls = 200;

  const auto time_calls = [](size_t length) {
    std::vector<float> samples(length, 0.25f);
    SonareSpectrumResult result = {};
    // One untimed call so the plan and window caches are warm for both lengths.
    REQUIRE(sonare_metering_spectrum_frame(samples.data(), samples.size(), kSpectrumFrameSampleRate,
                                           0, kNFft, 0, 0, 0.0f, 0.0f, &result) == SONARE_OK);
    sonare_free_spectrum_result(&result);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kCalls; ++i) {
      REQUIRE(sonare_metering_spectrum_frame(samples.data(), samples.size(),
                                             kSpectrumFrameSampleRate, 0, kNFft, 0, 0, 0.0f, 0.0f,
                                             &result) == SONARE_OK);
      sonare_free_spectrum_result(&result);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double, std::micro>(elapsed).count() / kCalls;
  };

  const double short_us = time_calls(kShortLength);
  const double long_us = time_calls(kLongLength);
  WARN("us/call short=" << short_us << " long=" << long_us << " ratio=" << long_us / short_us);

  // The long buffer is 256x the short one, so a per-sample term anywhere in the
  // call shows up here; with every per-sample term bounded by n_fft it cannot.
  CHECK(long_us < short_us * 10.0);
}

TEST_CASE("the spectrum dB knobs refuse a non-finite request rather than defaulting it",
          "[c_api][metering][spectrum]") {
  const std::vector<float> samples = make_spectrum_frame_fixture();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  const auto averaged = [&samples](float db_ref, float db_amin, SonareSpectrumResult* out) {
    return sonare_metering_spectrum(samples.data(), samples.size(), kSpectrumFrameSampleRate, 2048,
                                    0, 0, db_ref, db_amin, out);
  };
  const auto single_frame = [&samples](float db_ref, float db_amin, SonareSpectrumResult* out) {
    return sonare_metering_spectrum_frame(samples.data(), samples.size(), kSpectrumFrameSampleRate,
                                          0, 2048, 0, 0, db_ref, db_amin, out);
  };

  SECTION("a NaN or an infinity is an invalid parameter on both entry points") {
    for (float bad : {nan, inf, -inf, -1.0f}) {
      CAPTURE(bad);
      SonareSpectrumResult result = {};
      CHECK(averaged(bad, 0.0f, &result) == SONARE_ERROR_INVALID_PARAMETER);
      CHECK(averaged(0.0f, bad, &result) == SONARE_ERROR_INVALID_PARAMETER);
      CHECK(single_frame(bad, 0.0f, &result) == SONARE_ERROR_INVALID_PARAMETER);
      CHECK(single_frame(0.0f, bad, &result) == SONARE_ERROR_INVALID_PARAMETER);
    }
  }

  // Without this the section above passes on an entry point that refuses every
  // request, and it passed before the fix for every value except NaN and +inf:
  // a NaN resolved to the default and an infinite db_amin returned a spectrum of
  // infinities, both with SONARE_OK.
  SECTION("the sentinel and a legal value are both still accepted, and differ") {
    SonareSpectrumResult defaulted = {};
    REQUIRE(single_frame(0.0f, 0.0f, &defaulted) == SONARE_OK);
    REQUIRE(defaulted.bin_count > 0);

    SonareSpectrumResult referenced = {};
    REQUIRE(single_frame(2.0f, 0.0f, &referenced) == SONARE_OK);
    REQUIRE(referenced.bin_count == defaulted.bin_count);

    bool differs = false;
    bool all_finite = true;
    for (size_t i = 0; i < defaulted.bin_count; ++i) {
      differs = differs || referenced.db[i] != defaulted.db[i];
      all_finite = all_finite && std::isfinite(defaulted.db[i]);
    }
    CHECK(differs);
    CHECK(all_finite);

    sonare_free_spectrum_result(&referenced);
    sonare_free_spectrum_result(&defaulted);
  }
}
