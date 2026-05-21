#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <iterator>
#include <limits>
#include <vector>

#include "analysis/meter/basic.h"
#include "analysis/meter/clipping.h"
#include "analysis/meter/dynamic_range.h"
#include "analysis/meter/lufs.h"
#include "analysis/meter/phase_scope.h"
#include "analysis/meter/spectrogram.h"
#include "analysis/meter/spectrum.h"
#include "analysis/meter/stereo.h"
#include "analysis/meter/true_peak.h"

using Catch::Matchers::WithinAbs;
using namespace sonare;

namespace {

Audio make_sine(float amplitude, int sample_rate = 22050, float duration_sec = 0.5f) {
  const int n_samples = static_cast<int>(static_cast<float>(sample_rate) * duration_sec);
  std::vector<float> samples(n_samples);
  for (int i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[i] = amplitude * std::sin(2.0f * static_cast<float>(M_PI) * 440.0f * t);
  }
  return Audio::from_vector(std::move(samples), sample_rate);
}

}  // namespace

TEST_CASE("meter peak and rms", "[meter]") {
  const Audio audio = make_sine(1.0f);

  REQUIRE_THAT(analysis::meter::peak_db(audio), WithinAbs(0.0f, 0.1f));
  REQUIRE_THAT(analysis::meter::rms_db(audio), WithinAbs(-3.0f, 0.5f));
}

TEST_CASE("meter crest factor", "[meter]") {
  const Audio audio = make_sine(1.0f);

  REQUIRE_THAT(analysis::meter::crest_factor_db(audio), WithinAbs(3.0f, 0.5f));
}

TEST_CASE("meter clipping ratio", "[meter]") {
  const std::vector<float> samples = {0.0f, 0.5f, -0.999f, 1.0f};
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  REQUIRE_THAT(analysis::meter::clipping_ratio(audio), WithinAbs(0.5f, 0.001f));
}

TEST_CASE("clipping detector returns regions and counts", "[meter]") {
  const std::vector<float> samples = {0.0f, 1.0f, 0.999f, 0.2f, -1.0f, -1.0f, 0.0f};
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  const auto clipping = analysis::meter::detect_clipping(audio, 0.999f, 2);

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

  REQUIRE_THAT(analysis::meter::silence_ratio(audio, -45.0f, 2, 2), WithinAbs(0.5f, 0.001f));
}

TEST_CASE("meter dc offset", "[meter]") {
  const std::vector<float> samples = {0.25f, 0.25f, -0.25f, 0.75f};
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  REQUIRE_THAT(analysis::meter::dc_offset(audio), WithinAbs(0.25f, 0.001f));
}

TEST_CASE("meter silence returns non-finite peak and rms", "[meter]") {
  const std::vector<float> samples(16, 0.0f);
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  REQUIRE(!std::isfinite(analysis::meter::peak_db(audio)));
  REQUIRE(!std::isfinite(analysis::meter::rms_db(audio)));
  REQUIRE(std::isinf(analysis::meter::crest_factor_db(audio)));
}

TEST_CASE("true peak matches sample peak when oversampling is disabled", "[meter]") {
  const std::vector<float> samples = {-0.25f, 0.5f, -0.75f, 0.25f};
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  REQUIRE_THAT(analysis::meter::true_peak(audio, 1), WithinAbs(0.75f, 0.001f));
  REQUIRE_THAT(analysis::meter::true_peak_db(audio, 1), WithinAbs(-2.4988f, 0.001f));
}

TEST_CASE("true peak includes interpolated samples", "[meter]") {
  const std::vector<float> samples = {0.0f, 0.8f};
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  REQUIRE_THAT(analysis::meter::true_peak(audio, 4), WithinAbs(0.8f, 0.001f));
}

TEST_CASE("true peak detects bandlimited inter-sample overs", "[meter]") {
  const std::vector<float> samples = {0.0f, 0.99f, 0.99f, 0.0f, -0.99f, -0.99f, 0.0f};
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  REQUIRE(analysis::meter::true_peak(audio, 8) > 1.1f);
  REQUIRE(analysis::meter::true_peak(audio, 8) > analysis::meter::true_peak(audio, 1));
}

TEST_CASE("stereo correlation detects polarity relationships", "[meter]") {
  const std::vector<float> left = {0.1f, -0.3f, 0.5f, -0.7f};
  const std::vector<float> identical = left;
  const std::vector<float> inverted = {-0.1f, 0.3f, -0.5f, 0.7f};
  const std::vector<float> silent(left.size(), 0.0f);

  REQUIRE_THAT(analysis::meter::correlation(left.data(), identical.data(), left.size()),
               WithinAbs(1.0f, 0.001f));
  REQUIRE_THAT(analysis::meter::correlation(left.data(), inverted.data(), left.size()),
               WithinAbs(-1.0f, 0.001f));
  REQUIRE_THAT(analysis::meter::correlation(left.data(), silent.data(), left.size()),
               WithinAbs(0.0f, 0.001f));
}

TEST_CASE("stereo width follows mid side energy ratio", "[meter]") {
  const std::vector<float> left = {0.5f, 0.5f, -0.5f, -0.5f};
  const std::vector<float> mono = left;
  const std::vector<float> opposite = {-0.5f, -0.5f, 0.5f, 0.5f};
  const std::vector<float> mid_side_equal_left = {1.0f, -1.0f};
  const std::vector<float> mid_side_equal_right = {0.0f, 0.0f};

  REQUIRE_THAT(analysis::meter::stereo_width(left.data(), mono.data(), left.size()),
               WithinAbs(0.0f, 0.001f));
  REQUIRE(std::isinf(analysis::meter::stereo_width(left.data(), opposite.data(), left.size())));
  REQUIRE_THAT(
      analysis::meter::stereo_width(mid_side_equal_left.data(), mid_side_equal_right.data(),
                                    mid_side_equal_left.size()),
      WithinAbs(1.0f, 0.001f));
}

TEST_CASE("vectorscope returns mid side samples", "[meter]") {
  const std::vector<float> left = {1.0f, 0.5f};
  const std::vector<float> right = {1.0f, -0.5f};

  const auto points = analysis::meter::vectorscope(left.data(), right.data(), left.size());

  REQUIRE(points.size() == left.size());
  REQUIRE_THAT(points[0].mid, WithinAbs(std::sqrt(2.0f), 0.001f));
  REQUIRE_THAT(points[0].side, WithinAbs(0.0f, 0.001f));
  REQUIRE_THAT(points[1].mid, WithinAbs(0.0f, 0.001f));
  REQUIRE_THAT(points[1].side, WithinAbs(1.0f / std::sqrt(2.0f), 0.001f));
}

TEST_CASE("phase scope summarizes mono-compatible stereo", "[meter]") {
  const std::vector<float> left = {1.0f, 0.5f, 0.25f};
  const std::vector<float> right = left;

  const auto result = analysis::meter::phase_scope(left.data(), right.data(), left.size());

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

  const auto result = analysis::meter::phase_scope(left.data(), right.data(), left.size());

  REQUIRE_THAT(result.correlation, WithinAbs(-1.0f, 0.001f));
  REQUIRE_THAT(result.points[0].mid, WithinAbs(0.0f, 0.001f));
  REQUIRE_THAT(result.points[0].side, WithinAbs(std::sqrt(2.0f), 0.001f));
  REQUIRE_THAT(std::abs(result.points[0].angle_rad),
               WithinAbs(static_cast<float>(M_PI) / 2.0f, 0.001f));
  REQUIRE_THAT(result.average_abs_angle_rad, WithinAbs(static_cast<float>(M_PI) / 2.0f, 0.001f));
}

TEST_CASE("LUFS returns silence for silent audio", "[meter]") {
  const std::vector<float> samples(48000, 0.0f);
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  const auto result = analysis::meter::lufs(audio);

  REQUIRE(!std::isfinite(result.integrated_lufs));
  REQUIRE(!std::isfinite(result.momentary_lufs));
  REQUIRE(!std::isfinite(result.short_term_lufs));
  REQUIRE_THAT(result.loudness_range, WithinAbs(0.0f, 0.001f));
}

TEST_CASE("LUFS measures a steady tone consistently", "[meter]") {
  const Audio audio = make_sine(1.0f, 48000, 2.0f);

  const auto result = analysis::meter::lufs(audio);

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
    samples[i] = std::sin(2.0f * static_cast<float>(M_PI) * 1000.0f * t);
  }
  for (size_t i = 48000; i < samples.size(); ++i) {
    const float t = static_cast<float>(i - 48000) / 48000.0f;
    samples[i] = 0.01f * std::sin(2.0f * static_cast<float>(M_PI) * 1000.0f * t);
  }
  const Audio mixed = Audio::from_buffer(samples.data(), samples.size(), 48000);
  const Audio loud_only = Audio::from_buffer(samples.data(), 48000, 48000);

  const auto mixed_loudness = analysis::meter::lufs(mixed);
  const auto loud_only_loudness = analysis::meter::lufs(loud_only);

  REQUIRE_THAT(mixed_loudness.integrated_lufs, WithinAbs(loud_only_loudness.integrated_lufs, 1.0f));
}

TEST_CASE("LUFS curves expose momentary and short term blocks", "[meter]") {
  const Audio audio = make_sine(0.5f, 48000, 4.0f);

  const auto momentary = analysis::meter::momentary_lufs(audio);
  const auto short_term = analysis::meter::short_term_lufs(audio);

  REQUIRE(momentary.size() > short_term.size());
  REQUIRE(!momentary.empty());
  REQUIRE(!short_term.empty());
  REQUIRE(std::isfinite(momentary.front()));
  REQUIRE(std::isfinite(short_term.front()));
}

TEST_CASE("dynamic range reports zero for steady signal", "[meter]") {
  const Audio audio = make_sine(0.5f, 48000, 4.0f);

  analysis::meter::DynamicRangeConfig config;
  config.window_sec = 1.0f;
  config.hop_sec = 1.0f;
  const auto result = analysis::meter::dynamic_range(audio, config);

  REQUIRE(result.window_rms_db.size() == 4);
  REQUIRE_THAT(result.dynamic_range_db, WithinAbs(0.0f, 0.2f));
}

TEST_CASE("dynamic range increases with level changes", "[meter]") {
  std::vector<float> samples(48000 * 4, 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / 48000.0f;
    const float amplitude = i < samples.size() / 2 ? 0.1f : 0.8f;
    samples[i] = amplitude * std::sin(2.0f * static_cast<float>(M_PI) * 440.0f * t);
  }
  const Audio audio = Audio::from_buffer(samples.data(), samples.size(), 48000);

  analysis::meter::DynamicRangeConfig config;
  config.window_sec = 1.0f;
  config.hop_sec = 1.0f;
  const auto result = analysis::meter::dynamic_range(audio, config);

  REQUIRE(result.window_rms_db.size() == 4);
  REQUIRE(result.dynamic_range_db > 15.0f);
  REQUIRE(result.high_percentile_db > result.low_percentile_db);
}

TEST_CASE("spectrum identifies dominant sine frequency", "[meter]") {
  const Audio audio = make_sine(1.0f, 48000, 1.0f);

  analysis::meter::SpectrumConfig config;
  config.n_fft = 4096;
  const auto result = analysis::meter::spectrum(audio, config);

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

  const auto smoothed = analysis::meter::smooth_fractional_octave(values, freqs, 3);

  REQUIRE(smoothed.size() == values.size());
  REQUIRE_THAT(smoothed[2], WithinAbs(3.0f, 0.001f));
  REQUIRE_THAT(smoothed[4], WithinAbs(0.0f, 0.001f));
}

TEST_CASE("meter spectrogram exposes expected shape and axes", "[meter]") {
  const Audio audio = make_sine(0.5f, 48000, 0.1f);

  analysis::meter::MeterSpectrogramConfig config;
  config.stft.n_fft = 1024;
  config.stft.hop_length = 256;
  config.stft.center = false;
  const auto result = analysis::meter::spectrogram(audio, config);

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
