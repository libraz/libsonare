#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "mastering/api/chain.h"
#include "mastering/api/presets.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/gate.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/mid_side_eq.h"
#include "mastering/eq/parametric.h"
#include "mastering/stereo/mid_side.h"
#include "metering/lufs.h"
#include "metering/true_peak.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;

namespace {
using sonare::test::generate_sine_samples;
using sonare::test::process;
using sonare::test::process_stereo;
using sonare::test::rms;

std::vector<float> deterministic_signal(int samples) {
  std::vector<float> out(static_cast<size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    const float slow = 0.17f * std::sin(static_cast<float>(sonare::constants::kTwoPiD) * i / 97.0f);
    const float fast = 0.05f * std::sin(static_cast<float>(sonare::constants::kTwoPiD) * i / 13.0f);
    out[static_cast<size_t>(i)] = slow + fast;
  }
  return out;
}

void require_close(const std::vector<float>& actual, const std::vector<float>& expected,
                   float tolerance) {
  REQUIRE(actual.size() == expected.size());
  for (size_t i = 0; i < actual.size(); ++i) {
    INFO("sample index: " << i);
    REQUIRE_THAT(actual[i], WithinAbs(expected[i], tolerance));
  }
}

}  // namespace

TEST_CASE("Property: compressor ratio 1 preserves mono audio", "[mastering][property]") {
  using sonare::mastering::dynamics::Compressor;
  using sonare::mastering::dynamics::CompressorConfig;
  using sonare::mastering::dynamics::DetectorMode;

  CompressorConfig config;
  config.threshold_db = -60.0f;
  config.ratio = 1.0f;
  config.attack_ms = 0.0f;
  config.release_ms = 0.0f;
  config.makeup_gain_db = 0.0f;
  config.auto_makeup = false;
  config.detector = DetectorMode::Peak;

  Compressor compressor(config);
  compressor.prepare(48000.0, 512);

  auto audio = deterministic_signal(2048);
  const auto original = audio;
  process(compressor, audio);

  require_close(audio, original, 1.0e-7f);
}

TEST_CASE("Property: compressor ratio 1 preserves stereo audio", "[mastering][property]") {
  using sonare::mastering::dynamics::Compressor;
  using sonare::mastering::dynamics::CompressorConfig;
  using sonare::mastering::dynamics::DetectorMode;

  CompressorConfig config;
  config.threshold_db = -60.0f;
  config.ratio = 1.0f;
  config.attack_ms = 0.0f;
  config.release_ms = 0.0f;
  config.makeup_gain_db = 0.0f;
  config.auto_makeup = false;
  config.detector = DetectorMode::Peak;

  Compressor compressor(config);
  compressor.prepare(48000.0, 512);

  auto left = deterministic_signal(2048);
  auto right = generate_sine_samples(880.0f, 48000, 2048, 0.11f);
  const auto original_left = left;
  const auto original_right = right;
  process_stereo(compressor, left, right);

  require_close(left, original_left, 1.0e-7f);
  require_close(right, original_right, 1.0e-7f);
}

TEST_CASE("Property: disabled parametric EQ preserves audio", "[mastering][property]") {
  sonare::mastering::eq::ParametricEq eq;
  eq.prepare(48000.0, 512);

  auto audio = deterministic_signal(4096);
  const auto original = audio;
  process(eq, audio);

  require_close(audio, original, 1.0e-6f);
}

TEST_CASE("Property: zero-gain parametric EQ preserves audio", "[mastering][property]") {
  using sonare::mastering::eq::EqBandType;
  using sonare::mastering::eq::ParametricEq;

  ParametricEq eq;
  eq.prepare(48000.0, 512);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 0.70710678f, true});

  auto audio = deterministic_signal(4096);
  const auto original = audio;
  process(eq, audio);

  require_close(audio, original, 1.0e-5f);
}

TEST_CASE("Property: open gate preserves audio", "[mastering][property]") {
  using sonare::mastering::dynamics::Gate;
  using sonare::mastering::dynamics::GateConfig;

  GateConfig config;
  config.threshold_db = -120.0f;
  config.close_threshold_db = -120.0f;
  config.attack_ms = 0.0f;
  config.release_ms = 0.0f;
  config.range_db = -80.0f;

  Gate gate(config);
  gate.prepare(48000.0, 512);

  auto audio = generate_sine_samples(440.0f, 48000, 2048, 0.25f);
  const auto original = audio;
  process(gate, audio);

  require_close(audio, original, 1.0e-7f);
}

TEST_CASE("Property: mid-side sample roundtrip preserves stereo", "[mastering][property]") {
  using sonare::mastering::stereo::decode_sample;
  using sonare::mastering::stereo::encode_sample;

  const auto encoded = encode_sample(0.25f, -0.125f);
  const auto decoded = decode_sample(encoded.mid, encoded.side);

  REQUIRE_THAT(decoded.mid, WithinAbs(0.25f, 1.0e-7f));
  REQUIRE_THAT(decoded.side, WithinAbs(-0.125f, 1.0e-7f));
}

TEST_CASE("Property: mid-side buffer roundtrip preserves stereo", "[mastering][property]") {
  using sonare::mastering::stereo::decode_buffer;
  using sonare::mastering::stereo::encode_buffer;

  auto left = deterministic_signal(2048);
  auto right = generate_sine_samples(660.0f, 48000, 2048, 0.13f);
  const auto original_left = left;
  const auto original_right = right;
  std::vector<float> mid(left.size());
  std::vector<float> side(left.size());

  encode_buffer(left.data(), right.data(), mid.data(), side.data(), left.size());
  decode_buffer(mid.data(), side.data(), left.data(), right.data(), left.size());

  require_close(left, original_left, 1.0e-7f);
  require_close(right, original_right, 1.0e-7f);
}

TEST_CASE("Property: empty mid-side EQ preserves stereo", "[mastering][property]") {
  sonare::mastering::eq::MidSideEq eq;
  eq.prepare(48000.0, 512);

  auto left = deterministic_signal(2048);
  auto right = generate_sine_samples(880.0f, 48000, 2048, 0.09f);
  const auto original_left = left;
  const auto original_right = right;
  process_stereo(eq, left, right);

  require_close(left, original_left, 1.0e-6f);
  require_close(right, original_right, 1.0e-6f);
}

TEST_CASE("Property: zero-gain mid-side EQ preserves stereo", "[mastering][property]") {
  using sonare::mastering::eq::EqBandType;
  using sonare::mastering::eq::MidSideEq;

  MidSideEq eq;
  eq.prepare(48000.0, 512);
  eq.set_mid_band(0, {EqBandType::Peak, 1000.0f, 0.0f, 0.70710678f, true});
  eq.set_side_band(0, {EqBandType::Peak, 3000.0f, 0.0f, 0.70710678f, true});

  auto left = deterministic_signal(4096);
  auto right = generate_sine_samples(660.0f, 48000, 4096, 0.12f);
  const auto original_left = left;
  const auto original_right = right;
  process_stereo(eq, left, right);

  require_close(left, original_left, 2.0e-5f);
  require_close(right, original_right, 2.0e-5f);
}

TEST_CASE("Property: EqualizerProcessor stereo placement preserves side energy symmetry",
          "[mastering][property]") {
  using namespace sonare::mastering::eq;
  EqualizerProcessor eq({2});
  eq.prepare(48000.0, 4096);
  EqBand side_band{EqBandType::Peak, 2000.0f, 6.0f, 1.0f, true};
  side_band.placement = StereoPlacement::Side;
  eq.set_band(0, side_band);

  auto left = generate_sine_samples(2000.0f, 48000, 4096, 0.2f);
  auto right = left;
  const auto original_left = left;
  const auto original_right = right;
  process_stereo(eq, left, right);

  require_close(left, original_left, 1.0e-6f);
  require_close(right, original_right, 1.0e-6f);
}

TEST_CASE("Property: EqualizerProcessor auto-gain trends boosted RMS toward unity",
          "[mastering][property]") {
  using namespace sonare::mastering::eq;
  EqualizerProcessor eq({1});
  eq.prepare(48000.0, 48000);
  eq.set_auto_gain_enabled(true);
  eq.set_band(0, {EqBandType::Peak, 1000.0f, 9.0f, 1.0f, true});

  auto audio = generate_sine_samples(1000.0f, 48000, 48000, 0.2f);
  const float before = rms(audio);
  process(eq, audio);
  const float after = rms(audio);

  REQUIRE(after < before * 1.35f);
  REQUIRE(eq.last_auto_gain_db() < -6.0f);
}

TEST_CASE("Property: empty mastering chain preserves mono silence", "[mastering][property]") {
  sonare::mastering::api::MasteringChain chain({});
  std::vector<float> silence(2048, 0.0f);

  const auto result = chain.process_mono(silence.data(), silence.size(), 48000);

  REQUIRE(result.samples.size() == silence.size());
  require_close(result.samples, silence, 0.0f);
  REQUIRE(result.stages.empty());
}

TEST_CASE("Property: empty mastering chain preserves mono signal", "[mastering][property]") {
  sonare::mastering::api::MasteringChain chain({});
  auto audio = deterministic_signal(4096);

  const auto result = chain.process_mono(audio.data(), audio.size(), 48000);

  require_close(result.samples, audio, 0.0f);
  REQUIRE(result.stages.empty());
}

TEST_CASE("Property: chain compressor ratio 1 preserves mono signal", "[mastering][property]") {
  sonare::mastering::api::MasteringChainConfig config;
  config.dynamics.compressor.enabled = true;
  config.dynamics.compressor.config.threshold_db = -60.0f;
  config.dynamics.compressor.config.ratio = 1.0f;
  config.dynamics.compressor.config.attack_ms = 0.0f;
  config.dynamics.compressor.config.release_ms = 0.0f;
  config.dynamics.compressor.config.makeup_gain_db = 0.0f;
  config.dynamics.compressor.config.auto_makeup = false;
  config.dynamics.compressor.config.detector = sonare::mastering::dynamics::DetectorMode::Peak;
  sonare::mastering::api::MasteringChain chain(config);
  auto audio = deterministic_signal(4096);

  const auto result = chain.process_mono(audio.data(), audio.size(), 48000);

  require_close(result.samples, audio, 1.0e-7f);
  REQUIRE(result.stages.size() == 1);
  REQUIRE(result.stages[0] == "dynamics.compressor");
}

TEST_CASE("Property: LUFS measurement is deterministic", "[mastering][property]") {
  const auto samples = generate_sine_samples(997.0f, 48000, 48000, 0.25f);
  const auto audio = sonare::Audio::from_buffer(samples.data(), samples.size(), 48000);

  const auto first = sonare::metering::lufs(audio);
  const auto second = sonare::metering::lufs(audio);

  REQUIRE_THAT(second.integrated_lufs, WithinAbs(first.integrated_lufs, 1.0e-6f));
  REQUIRE_THAT(second.momentary_lufs, WithinAbs(first.momentary_lufs, 1.0e-6f));
  REQUIRE_THAT(second.short_term_lufs, WithinAbs(first.short_term_lufs, 1.0e-6f));
  REQUIRE_THAT(second.loudness_range, WithinAbs(first.loudness_range, 1.0e-6f));
}

TEST_CASE("Property: true peak measurement is deterministic", "[mastering][property]") {
  const auto samples = deterministic_signal(4096);
  const auto audio = sonare::Audio::from_buffer(samples.data(), samples.size(), 48000);

  const float first = sonare::metering::true_peak_db(audio, 4);
  const float second = sonare::metering::true_peak_db(audio, 4);

  REQUIRE_THAT(second, WithinAbs(first, 1.0e-6f));
}

TEST_CASE("Property: built-in preset processing is deterministic", "[mastering][property]") {
  const auto audio = generate_sine_samples(440.0f, 48000, 48000, 0.2f);

  const auto first = sonare::mastering::api::master_audio_mono(
      sonare::mastering::api::Preset::Streaming, audio.data(), audio.size(), 48000);
  const auto second = sonare::mastering::api::master_audio_mono(
      sonare::mastering::api::Preset::Streaming, audio.data(), audio.size(), 48000);

  require_close(second.samples, first.samples, 1.0e-6f);
  REQUIRE_THAT(second.input_lufs, WithinAbs(first.input_lufs, 1.0e-6f));
  REQUIRE_THAT(second.output_lufs, WithinAbs(first.output_lufs, 1.0e-6f));
  REQUIRE_THAT(rms(second.samples), WithinAbs(rms(first.samples), 1.0e-6f));
  REQUIRE(second.stages == first.stages);
}

TEST_CASE("Property: empty mastering chain preserves stereo silence", "[mastering][property]") {
  sonare::mastering::api::MasteringChain chain({});
  std::vector<float> left(2048, 0.0f);
  std::vector<float> right(2048, 0.0f);

  const auto result = chain.process_stereo(left.data(), right.data(), left.size(), 48000);

  REQUIRE(result.left.size() == left.size());
  REQUIRE(result.right.size() == right.size());
  require_close(result.left, left, 0.0f);
  require_close(result.right, right, 0.0f);
  REQUIRE(result.stages.empty());
}
