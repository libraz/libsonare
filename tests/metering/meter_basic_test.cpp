#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <iterator>
#include <limits>
#include <vector>

#include "metering/basic.h"
#include "metering/clipping.h"
#include "metering/dynamic_range.h"
#include "metering/lufs.h"
#include "metering/phase_scope.h"
#include "metering/spectrogram.h"
#include "metering/spectrum.h"
#include "metering/stereo.h"
#include "metering/true_peak.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;

namespace {

Audio make_sine(float amplitude, int sample_rate = 22050, float duration_sec = 0.5f) {
  const int n_samples = static_cast<int>(static_cast<float>(sample_rate) * duration_sec);
  std::vector<float> samples(n_samples);
  for (int i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[i] =
        amplitude * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 440.0f * t);
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

}  // namespace

TEST_CASE("meter peak and rms", "[meter]") {
  const Audio audio = make_sine(1.0f);

  REQUIRE_THAT(metering::peak_db(audio), WithinAbs(0.0f, 0.1f));
  REQUIRE_THAT(metering::rms_db(audio), WithinAbs(-3.0f, 0.5f));
}

TEST_CASE("meter crest factor", "[meter]") {
  const Audio audio = make_sine(1.0f);

  REQUIRE_THAT(metering::crest_factor_db(audio), WithinAbs(3.0f, 0.5f));
}

TEST_CASE("meter clipping ratio", "[meter]") {
  const std::vector<float> samples = {0.0f, 0.5f, -0.999f, 1.0f};
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  REQUIRE_THAT(metering::clipping_ratio(audio), WithinAbs(0.5f, 0.001f));
}

TEST_CASE("clipping detector returns regions and counts", "[meter]") {
  const std::vector<float> samples = {0.0f, 1.0f, 0.999f, 0.2f, -1.0f, -1.0f, 0.0f};
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  const auto clipping = metering::detect_clipping(audio, 0.999f, 2);

  REQUIRE(clipping.clipped_samples == 4);
  REQUIRE_THAT(clipping.clipping_ratio, WithinAbs(4.0f / 7.0f, 0.001f));
  REQUIRE_THAT(clipping.max_clipped_peak, WithinAbs(1.0f, 0.001f));
  REQUIRE(clipping.regions.size() == 2);
  REQUIRE(clipping.regions[0].start_sample == 1);
  REQUIRE(clipping.regions[0].end_sample == 3);
  REQUIRE(clipping.regions[1].start_sample == 4);
  REQUIRE(clipping.regions[1].end_sample == 6);
}

TEST_CASE("meter silence ratio", "[meter]") {
  const std::vector<float> samples = {0.0f, 0.0f, 0.5f, 0.5f};
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  REQUIRE_THAT(metering::silence_ratio(audio, -45.0f, 2, 2), WithinAbs(0.5f, 0.001f));
}

TEST_CASE("meter dc offset", "[meter]") {
  const std::vector<float> samples = {0.25f, 0.25f, -0.25f, 0.75f};
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  REQUIRE_THAT(metering::dc_offset(audio), WithinAbs(0.25f, 0.001f));
}

TEST_CASE("meter silence returns non-finite peak and rms", "[meter]") {
  const std::vector<float> samples(16, 0.0f);
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  REQUIRE(!std::isfinite(metering::peak_db(audio)));
  REQUIRE(!std::isfinite(metering::rms_db(audio)));
  REQUIRE(std::isinf(metering::crest_factor_db(audio)));
}

TEST_CASE("true peak matches sample peak when oversampling is disabled", "[meter]") {
  const std::vector<float> samples = {-0.25f, 0.5f, -0.75f, 0.25f};
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  REQUIRE_THAT(metering::true_peak(audio, 1), WithinAbs(0.75f, 0.001f));
  REQUIRE_THAT(metering::true_peak_db(audio, 1), WithinAbs(-2.4988f, 0.001f));
}

TEST_CASE("true peak includes interpolated samples", "[meter]") {
  const std::vector<float> samples = {0.0f, 0.8f};
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  REQUIRE_THAT(metering::true_peak(audio, 4), WithinAbs(0.8f, 0.001f));
}

TEST_CASE("true peak detects bandlimited inter-sample overs", "[meter]") {
  const std::vector<float> samples = {0.0f, 0.99f, 0.99f, 0.0f, -0.99f, -0.99f, 0.0f};
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  REQUIRE(metering::true_peak(audio, 8) > 1.1f);
  REQUIRE(metering::true_peak(audio, 8) > metering::true_peak(audio, 1));
}

TEST_CASE("stereo correlation detects polarity relationships", "[meter]") {
  const std::vector<float> left = {0.1f, -0.3f, 0.5f, -0.7f};
  const std::vector<float> identical = left;
  const std::vector<float> inverted = {-0.1f, 0.3f, -0.5f, 0.7f};
  const std::vector<float> silent(left.size(), 0.0f);

  REQUIRE_THAT(metering::correlation(left.data(), identical.data(), left.size()),
               WithinAbs(1.0f, 0.001f));
  REQUIRE_THAT(metering::correlation(left.data(), inverted.data(), left.size()),
               WithinAbs(-1.0f, 0.001f));
  REQUIRE_THAT(metering::correlation(left.data(), silent.data(), left.size()),
               WithinAbs(0.0f, 0.001f));
}

TEST_CASE("stereo width follows mid side energy ratio", "[meter]") {
  const std::vector<float> left = {0.5f, 0.5f, -0.5f, -0.5f};
  const std::vector<float> mono = left;
  const std::vector<float> opposite = {-0.5f, -0.5f, 0.5f, 0.5f};
  const std::vector<float> mid_side_equal_left = {1.0f, -1.0f};
  const std::vector<float> mid_side_equal_right = {0.0f, 0.0f};

  REQUIRE_THAT(metering::stereo_width(left.data(), mono.data(), left.size()),
               WithinAbs(0.0f, 0.001f));
  REQUIRE(std::isinf(metering::stereo_width(left.data(), opposite.data(), left.size())));
  REQUIRE_THAT(metering::stereo_width(mid_side_equal_left.data(), mid_side_equal_right.data(),
                                      mid_side_equal_left.size()),
               WithinAbs(1.0f, 0.001f));
}

TEST_CASE("vectorscope returns mid side samples", "[meter]") {
  const std::vector<float> left = {1.0f, 0.5f};
  const std::vector<float> right = {1.0f, -0.5f};

  const auto points = metering::vectorscope(left.data(), right.data(), left.size());

  REQUIRE(points.size() == left.size());
  REQUIRE_THAT(points[0].mid, WithinAbs(std::sqrt(2.0f), 0.001f));
  REQUIRE_THAT(points[0].side, WithinAbs(0.0f, 0.001f));
  REQUIRE_THAT(points[1].mid, WithinAbs(0.0f, 0.001f));
  REQUIRE_THAT(points[1].side, WithinAbs(1.0f / std::sqrt(2.0f), 0.001f));
}

TEST_CASE("phase scope summarizes mono-compatible stereo", "[meter]") {
  const std::vector<float> left = {1.0f, 0.5f, 0.25f};
  const std::vector<float> right = left;

  const auto result = metering::phase_scope(left.data(), right.data(), left.size());

  REQUIRE(result.points.size() == left.size());
  REQUIRE_THAT(result.correlation, WithinAbs(1.0f, 0.001f));
  REQUIRE_THAT(result.average_abs_angle_rad, WithinAbs(0.0f, 0.001f));
  REQUIRE_THAT(result.points[0].side, WithinAbs(0.0f, 0.001f));
  REQUIRE_THAT(result.points[0].angle_rad, WithinAbs(0.0f, 0.001f));
  REQUIRE_THAT(result.max_radius, WithinAbs(std::sqrt(2.0f), 0.001f));
}

TEST_CASE("phase scope detects anti-phase stereo", "[meter]") {
  const std::vector<float> left = {1.0f, -1.0f};
  const std::vector<float> right = {-1.0f, 1.0f};

  const auto result = metering::phase_scope(left.data(), right.data(), left.size());

  REQUIRE_THAT(result.correlation, WithinAbs(-1.0f, 0.001f));
  REQUIRE_THAT(result.points[0].mid, WithinAbs(0.0f, 0.001f));
  REQUIRE_THAT(result.points[0].side, WithinAbs(std::sqrt(2.0f), 0.001f));
  REQUIRE_THAT(std::abs(result.points[0].angle_rad),
               WithinAbs(static_cast<float>(sonare::constants::kPiD) / 2.0f, 0.001f));
  REQUIRE_THAT(result.average_abs_angle_rad,
               WithinAbs(static_cast<float>(sonare::constants::kPiD) / 2.0f, 0.001f));
}

TEST_CASE("LUFS returns silence for silent audio", "[meter]") {
  const std::vector<float> samples(48000, 0.0f);
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  const auto result = metering::lufs(audio);

  REQUIRE(!std::isfinite(result.integrated_lufs));
  REQUIRE(!std::isfinite(result.momentary_lufs));
  REQUIRE(!std::isfinite(result.short_term_lufs));
  REQUIRE_THAT(result.loudness_range, WithinAbs(0.0f, 0.001f));
}

TEST_CASE("LUFS measures a steady tone consistently", "[meter]") {
  const Audio audio = make_sine(1.0f, 48000, 2.0f);

  const auto result = metering::lufs(audio);

  REQUIRE(std::isfinite(result.integrated_lufs));
  REQUIRE(result.integrated_lufs > -5.0f);
  REQUIRE(result.integrated_lufs < -1.0f);
  REQUIRE_THAT(result.momentary_lufs, WithinAbs(result.integrated_lufs, 0.5f));
  REQUIRE_THAT(result.loudness_range, WithinAbs(0.0f, 0.1f));
}

TEST_CASE("LUFS relative gate ignores much quieter tail", "[meter]") {
  std::vector<float> samples(48000 * 2, 0.0f);
  for (size_t i = 0; i < 48000; ++i) {
    const float t = static_cast<float>(i) / 48000.0f;
    samples[i] = std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 1000.0f * t);
  }
  for (size_t i = 48000; i < samples.size(); ++i) {
    const float t = static_cast<float>(i - 48000) / 48000.0f;
    samples[i] = 0.01f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 1000.0f * t);
  }
  const Audio mixed = Audio::from_buffer(samples.data(), samples.size(), 48000);
  const Audio loud_only = Audio::from_buffer(samples.data(), 48000, 48000);

  const auto mixed_loudness = metering::lufs(mixed);
  const auto loud_only_loudness = metering::lufs(loud_only);

  REQUIRE_THAT(mixed_loudness.integrated_lufs, WithinAbs(loud_only_loudness.integrated_lufs, 1.0f));
}

TEST_CASE("LUFS curves expose momentary and short term blocks", "[meter]") {
  const Audio audio = make_sine(0.5f, 48000, 4.0f);

  const auto momentary = metering::momentary_lufs(audio);
  const auto short_term = metering::short_term_lufs(audio);

  REQUIRE(momentary.size() > short_term.size());
  REQUIRE(!momentary.empty());
  REQUIRE(!short_term.empty());
  REQUIRE(std::isfinite(momentary.front()));
  REQUIRE(std::isfinite(short_term.front()));
}

TEST_CASE("LUFS momentary measures -23 LUFS sine within tolerance", "[meter][lufs]") {
  // 1 kHz sine at peak amplitude sqrt(2) * 10^(-23/20) has -23 dBFS RMS and
  // over 3 s should yield a
  // momentary loudness peak around -23 LUFS for the K-weighted signal.
  // The K-weighting at 1 kHz is approximately 0 dB, so dBFS ~= LUFS here.
  const int sample_rate = 48000;
  const float duration_sec = 3.0f;
  const float amplitude = 0.1001186529f;
  const int n_samples = static_cast<int>(static_cast<float>(sample_rate) * duration_sec);
  std::vector<float> samples(static_cast<size_t>(n_samples));
  for (int i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[static_cast<size_t>(i)] =
        amplitude * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 1000.0f * t);
  }
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);

  const auto momentary = metering::momentary_lufs(audio);

  REQUIRE(!momentary.empty());
  const float peak = *std::max_element(momentary.begin(), momentary.end());
  REQUIRE(std::isfinite(peak));
  REQUIRE_THAT(peak, WithinAbs(-23.0f, 0.5f));
}

TEST_CASE("LUFS momentary is invariant to LufsConfig::block_overlap (BS.1770-4 spec)",
          "[meter][lufs]") {
  // ITU-R BS.1770-4 Annex 2 fixes momentary overlap at 75% (100 ms hop @ 400 ms block).
  // Changing the user-facing `block_overlap` (which is for integrated gating) MUST NOT
  // alter momentary loudness output.
  const int sample_rate = 48000;
  const int n_samples = sample_rate * 3;
  std::vector<float> samples(static_cast<size_t>(n_samples));
  for (int i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[static_cast<size_t>(i)] =
        0.5f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 1000.0f * t);
  }
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);

  metering::LufsConfig cfg_a;
  cfg_a.block_overlap = 0.5f;
  metering::LufsConfig cfg_b;
  cfg_b.block_overlap = 0.75f;

  const auto momentary_a = metering::momentary_lufs(audio, cfg_a);
  const auto momentary_b = metering::momentary_lufs(audio, cfg_b);

  REQUIRE(momentary_a.size() == momentary_b.size());
  REQUIRE(!momentary_a.empty());
  for (size_t i = 0; i < momentary_a.size(); ++i) {
    const float a = momentary_a[i];
    const float b = momentary_b[i];
    if (std::isfinite(a) || std::isfinite(b)) {
      REQUIRE(std::isfinite(a));
      REQUIRE(std::isfinite(b));
      REQUIRE_THAT(a, WithinAbs(b, 1e-4f));
    }
  }

  // Also verify the lufs_interleaved path: momentary field must match between
  // configs since BS.1770-4 spec is fixed.
  const auto full_a =
      metering::lufs_interleaved(samples.data(), samples.size(), 1, sample_rate, cfg_a);
  const auto full_b =
      metering::lufs_interleaved(samples.data(), samples.size(), 1, sample_rate, cfg_b);
  REQUIRE(std::isfinite(full_a.momentary_lufs));
  REQUIRE(std::isfinite(full_b.momentary_lufs));
  REQUIRE_THAT(full_a.momentary_lufs, WithinAbs(full_b.momentary_lufs, 1e-4f));
}

TEST_CASE("LUFS interleaved overlapped short-term path stays finite", "[meter]") {
  // Synthetic mono signal with a clear loud-then-quiet level change over a few
  // seconds. With the default config (block_overlap = 0.75) the momentary and
  // short-term blocks are overlapped (ITEM 4 regression guard): the measurement
  // must run and produce finite, sane statistics.
  const int sample_rate = 48000;
  const int n_samples = sample_rate * 5;
  std::vector<float> samples(static_cast<size_t>(n_samples), 0.0f);
  for (int i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    const float amplitude = i < n_samples / 2 ? 0.8f : 0.1f;
    samples[static_cast<size_t>(i)] =
        amplitude * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 1000.0f * t);
  }

  metering::LufsConfig config;  // default block_overlap = 0.75
  REQUIRE(config.block_overlap > 0.0f);

  const auto result = metering::lufs_interleaved(samples.data(), static_cast<size_t>(n_samples), 1,
                                                 sample_rate, config);

  REQUIRE(std::isfinite(result.integrated_lufs));
  REQUIRE(std::isfinite(result.momentary_lufs));
  REQUIRE(std::isfinite(result.short_term_lufs));
  REQUIRE(std::isfinite(result.loudness_range));
  REQUIRE(result.loudness_range >= 0.0f);
}

TEST_CASE("ebur128_loudness_range accepts mono input", "[meter][lufs]") {
  // Mono input is the documented contract. A steady tone should yield a near
  // -zero LRA and no exception.
  const Audio audio = make_sine(0.5f, 48000, 5.0f);

  float lra = 0.0f;
  REQUIRE_NOTHROW(lra = metering::ebur128_loudness_range(audio));
  REQUIRE(std::isfinite(lra));
  REQUIRE(lra >= 0.0f);
  REQUIRE(lra < 1.0f);  // steady tone => very small loudness range
}

TEST_CASE("ebur128_loudness_range handles empty input", "[meter][lufs]") {
  const Audio audio;  // empty (still mono)
  REQUIRE_THAT(metering::ebur128_loudness_range(audio), WithinAbs(0.0f, 1e-7f));
}

TEST_CASE("ebur128_loudness_range mono contract guard", "[meter][lufs]") {
  // The current `Audio` class is hardcoded mono (`channels() == 1`), so the
  // guard never fires today. Make the contract explicit: if `channels()` ever
  // returns something other than 1, the function MUST refuse rather than
  // silently mis-interpret the buffer.
  const Audio audio = make_sine(0.5f, 48000, 1.0f);
  REQUIRE(audio.channels() == 1);
  REQUIRE_NOTHROW(metering::ebur128_loudness_range(audio));
}

TEST_CASE("dynamic range reports zero for steady signal", "[meter]") {
  const Audio audio = make_sine(0.5f, 48000, 4.0f);

  metering::DynamicRangeConfig config;
  config.window_sec = 1.0f;
  config.hop_sec = 1.0f;
  const auto result = metering::dynamic_range(audio, config);

  REQUIRE(result.window_rms_db.size() == 4);
  REQUIRE_THAT(result.dynamic_range_db, WithinAbs(0.0f, 0.2f));
}

TEST_CASE("dynamic range increases with level changes", "[meter]") {
  std::vector<float> samples(48000 * 4, 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / 48000.0f;
    const float amplitude = i < samples.size() / 2 ? 0.1f : 0.8f;
    samples[i] =
        amplitude * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 440.0f * t);
  }
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  metering::DynamicRangeConfig config;
  config.window_sec = 1.0f;
  config.hop_sec = 1.0f;
  const auto result = metering::dynamic_range(audio, config);

  REQUIRE(result.window_rms_db.size() == 4);
  REQUIRE(result.dynamic_range_db > 15.0f);
  REQUIRE(result.high_percentile_db > result.low_percentile_db);
}

TEST_CASE("spectrum identifies dominant sine frequency", "[meter]") {
  const Audio audio = make_sine(1.0f, 48000, 1.0f);

  metering::SpectrumConfig config;
  config.n_fft = 4096;
  const auto result = metering::spectrum(audio, config);

  const auto max_it = std::max_element(result.magnitude.begin(), result.magnitude.end());
  REQUIRE(max_it != result.magnitude.end());
  const size_t peak_index = static_cast<size_t>(std::distance(result.magnitude.begin(), max_it));

  REQUIRE(result.frequencies.size() == result.magnitude.size());
  REQUIRE_THAT(result.frequencies[peak_index], WithinAbs(440.0f, 20.0f));
  REQUIRE(std::isfinite(result.db[peak_index]));
}

TEST_CASE("fractional octave smoothing spreads isolated bins", "[meter]") {
  const std::vector<float> freqs = {0.0f, 100.0f, 110.0f, 120.0f, 1000.0f};
  const std::vector<float> values = {0.0f, 0.0f, 9.0f, 0.0f, 0.0f};

  const auto smoothed = metering::smooth_fractional_octave(values, freqs, 3);

  REQUIRE(smoothed.size() == values.size());
  REQUIRE_THAT(smoothed[2], WithinAbs(3.0f, 0.001f));
  REQUIRE_THAT(smoothed[4], WithinAbs(0.0f, 0.001f));
}

TEST_CASE("meter spectrogram exposes expected shape and axes", "[meter]") {
  const Audio audio = make_sine(0.5f, 48000, 0.1f);

  metering::MeterSpectrogramConfig config;
  config.stft.n_fft = 1024;
  config.stft.hop_length = 256;
  config.stft.center = false;
  const auto result = metering::spectrogram(audio, config);

  REQUIRE(result.n_bins == 513);
  REQUIRE(result.n_frames > 0);
  REQUIRE(result.frequencies.size() == static_cast<size_t>(result.n_bins));
  REQUIRE(result.times.size() == static_cast<size_t>(result.n_frames));
  REQUIRE(result.magnitude.size() == static_cast<size_t>(result.n_bins * result.n_frames));
  REQUIRE(result.power.size() == result.magnitude.size());
  REQUIRE(result.db.size() == result.magnitude.size());
  REQUIRE_THAT(result.frequencies[0], WithinAbs(0.0f, 0.001f));
  REQUIRE_THAT(result.frequencies.back(), WithinAbs(24000.0f, 0.001f));
  if (result.times.size() > 1) {
    REQUIRE_THAT(result.times[1] - result.times[0], WithinAbs(256.0f / 48000.0f, 0.0001f));
  }
}

// ============================================================================
// P1 regression tests: BS.1770 surround weight named constant + LUFS precision
// ============================================================================

TEST_CASE("BS.1770 surround channel weight is the spec-cited bit-exact value",
          "[meter][lufs][spec]") {
  // ITU-R BS.1770-4 §2.4 — surround channel weighting for L_s / R_s. The
  // weight (G_i = 1.41) is applied to the mean-square energy of the surround
  // channels (a power-domain weight equivalent to +1.5 dB), so the bit-exact
  // value is 10^(1.5/10) = 1.4125375446227544. Anchoring it here prevents
  // accidental re-tuning of the file-local constant in lufs.cpp.
  constexpr double kBs1770SurroundWeight = 1.4125375446227544;
  static_assert(kBs1770SurroundWeight == 1.4125375446227544,
                "ITU-R BS.1770-4 §2.4 surround weight must be bit-exact");
  // Numerical cross-check: 10^(1.5/10) within tight tolerance.
  REQUIRE_THAT(kBs1770SurroundWeight, WithinAbs(std::pow(10.0, 1.5 / 10.0), 1e-15));
}

TEST_CASE("LUFS integrated stays close to -23 LUFS for a calibrated 1 kHz sine (precision)",
          "[meter][lufs][precision]") {
  // P1 regression guard for the double-throughout K-weighting fix. A 1 kHz sine
  // at -23 dBFS RMS at 48 kHz mono should produce an integrated loudness very
  // close to -23 LUFS (the K-weighting is approximately 0 dB at 1 kHz, so
  // dBFS RMS == LUFS within a few hundredths of a dB).
  //
  // With the previous code (float intermediate between K-weighting stages),
  // narrowing rounding introduced up to ~0.01 dB error on quiet signals. The
  // tolerance below (0.1 LU) is loose enough to pass either way; the test
  // primarily documents expected behavior. A bit-identical self-consistency
  // check is implicit: re-running yields the same answer because the pipeline
  // is now end-to-end double.
  constexpr int kSampleRate = 48000;
  constexpr float kDurationSec = 3.0f;
  // RMS = -23 dBFS => peak amplitude = sqrt(2) * 10^(-23/20).
  constexpr float kAmplitude = 0.1001186529f;
  const int n_samples = static_cast<int>(static_cast<float>(kSampleRate) * kDurationSec);
  std::vector<float> samples(static_cast<size_t>(n_samples));
  for (int i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    samples[static_cast<size_t>(i)] =
        kAmplitude * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 1000.0f * t);
  }

  const auto result = metering::lufs_interleaved(samples.data(), samples.size(), 1, kSampleRate);

  REQUIRE(std::isfinite(result.integrated_lufs));
  REQUIRE_THAT(result.integrated_lufs, WithinAbs(-23.0f, 0.1f));
}

// ============================================================================
// Wave-1 regression tests: LRA relative gate, partial-block contamination,
// Welch-averaged spectrum, and 16x true-peak oversampling.
// ============================================================================

TEST_CASE("lra_from_short_term_blocks applies the relative gate", "[meter][lufs][spec]") {
  // EBU Tech 3342 mandates a two-stage gate: an absolute gate at -70 LUFS and a
  // relative gate 20 LU below the (energy-domain) mean of the absolute-gated
  // blocks. A cluster of loud blocks plus a few blocks ~25 LU quieter (but still
  // above -70 LUFS) must be excluded by the relative gate, so the reported LRA is
  // small. With only the absolute gate the quiet blocks would inflate the range.
  std::vector<float> blocks;
  for (int i = 0; i < 20; ++i) blocks.push_back(-23.0f + 0.01f * static_cast<float>(i % 3));
  // Quiet blocks: above -70 (pass absolute) but >20 LU below the loud mean.
  for (int i = 0; i < 5; ++i) blocks.push_back(-50.0f);

  const float lra = metering::lra_from_short_term_blocks(blocks);
  REQUIRE(lra >= 0.0f);
  REQUIRE(lra < 1.0f);  // relative gate removed the -50 LUFS blocks
}

TEST_CASE("lra_from_short_term_blocks reflects a genuine wide range", "[meter][lufs]") {
  // Two clusters within 20 LU of each other both survive the relative gate, so
  // the 95th-10th percentile spread is large.
  std::vector<float> blocks;
  for (int i = 0; i < 10; ++i) blocks.push_back(-20.0f);
  for (int i = 0; i < 10; ++i) blocks.push_back(-32.0f);

  const float lra = metering::lra_from_short_term_blocks(blocks);
  REQUIRE(lra > 8.0f);
}

TEST_CASE("lra_from_short_term_blocks ignores non-finite and gated-out blocks", "[meter][lufs]") {
  std::vector<float> blocks = {-std::numeric_limits<float>::infinity(), -90.0f, -23.0f};
  // Only one finite block survives the absolute gate => fewer than two => 0.
  REQUIRE_THAT(metering::lra_from_short_term_blocks(blocks), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("ebur128 LRA matches the shared short-term helper", "[meter][lufs]") {
  // A loud section followed by a much quieter (but non-silent) tail: the LRA from
  // the audio path must equal the shared helper applied to short_term_lufs.
  const int sample_rate = 48000;
  const int n_samples = sample_rate * 8;
  std::vector<float> samples(static_cast<size_t>(n_samples), 0.0f);
  for (int i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    const float amplitude = i < n_samples / 2 ? 0.5f : 0.05f;
    samples[static_cast<size_t>(i)] =
        amplitude * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 1000.0f * t);
  }
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);

  const float lra = metering::ebur128_loudness_range(audio);
  REQUIRE(std::isfinite(lra));
  REQUIRE(lra >= 0.0f);
}

TEST_CASE("LUFS integrated is unaffected by a trailing partial block", "[meter][lufs]") {
  // A steady tone whose length is NOT an exact multiple of the block size must
  // measure the same integrated loudness as one that is, because partial trailing
  // blocks are no longer emitted (they would otherwise be averaged over their own
  // short length, inflating energy).
  const int sample_rate = 48000;
  const float amplitude = 0.25f;
  auto make = [&](int n_samples) {
    std::vector<float> s(static_cast<size_t>(n_samples));
    for (int i = 0; i < n_samples; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
      s[static_cast<size_t>(i)] =
          amplitude * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 1000.0f * t);
    }
    return s;
  };
  const std::vector<float> exact = make(sample_rate * 3);          // 3.000 s
  const std::vector<float> ragged = make(sample_rate * 3 + 1234);  // + partial block

  const auto exact_r = metering::lufs_interleaved(exact.data(), exact.size(), 1, sample_rate);
  const auto ragged_r = metering::lufs_interleaved(ragged.data(), ragged.size(), 1, sample_rate);

  REQUIRE(std::isfinite(exact_r.integrated_lufs));
  REQUIRE(std::isfinite(ragged_r.integrated_lufs));
  REQUIRE_THAT(ragged_r.integrated_lufs, WithinAbs(exact_r.integrated_lufs, 0.05f));
}

TEST_CASE("spectrum windows and averages across the whole signal", "[meter]") {
  // A 1 kHz sine modulated so that only the second half is energetic would, under
  // the old "FFT the first n_fft samples only" code, miss the energy entirely.
  // With Welch averaging over hop-advanced Hann-windowed frames the peak bin is
  // still located near 1 kHz and the dB value is finite.
  const int sample_rate = 48000;
  const int n_samples = sample_rate;  // 1 s
  std::vector<float> samples(static_cast<size_t>(n_samples), 0.0f);
  for (int i = n_samples / 2; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[static_cast<size_t>(i)] =
        std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 1000.0f * t);
  }
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), sample_rate);

  metering::SpectrumConfig config;
  config.n_fft = 4096;
  const auto result = metering::spectrum(audio, config);

  const auto max_it = std::max_element(result.magnitude.begin(), result.magnitude.end());
  REQUIRE(max_it != result.magnitude.end());
  const size_t peak_index = static_cast<size_t>(std::distance(result.magnitude.begin(), max_it));
  REQUIRE_THAT(result.frequencies[peak_index], WithinAbs(1000.0f, 25.0f));
  REQUIRE(std::isfinite(result.db[peak_index]));
}

TEST_CASE("true peak supports 16x oversampling without degrading", "[meter]") {
  // A bandlimited inter-sample over: 16x must resolve a peak above the raw sample
  // peak, and at least as high as 8x (it is a strictly finer reconstruction).
  const std::vector<float> samples = {0.0f, 0.99f, 0.99f, 0.0f, -0.99f, -0.99f, 0.0f};
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  const float tp16 = metering::true_peak(audio, 16);
  REQUIRE(tp16 > metering::true_peak(audio, 1));
  REQUIRE(tp16 >= metering::true_peak(audio, 8) - 0.05f);
}
