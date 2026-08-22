#include "mastering/api/chain.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "core/audio.h"
#include "mastering/api/audio_utils.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/common/loudness_measure.h"
#include "mastering/match/reference_loudness.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/json.h"

using Catch::Matchers::WithinAbs;

namespace sonare::mastering::api {

namespace {

float max_abs_difference(const std::vector<float>& left, const std::vector<float>& right) {
  if (left.size() != right.size()) return std::numeric_limits<float>::infinity();
  float maximum = 0.0f;
  for (size_t i = 0; i < left.size(); ++i) {
    maximum = std::max(maximum, std::abs(left[i] - right[i]));
  }
  return maximum;
}

}  // namespace

TEST_CASE("MasteringChain passes through with empty config (mono)", "[mastering][chain]") {
  std::vector<float> samples(44100, 0.1f);
  MasteringChainConfig config;
  MasteringChain chain(config);
  auto result = chain.process_mono(samples.data(), samples.size(), 44100);
  REQUIRE(result.samples.size() == samples.size());
  REQUIRE(result.sample_rate == 44100);
  REQUIRE(result.stages.empty());
}

TEST_CASE("MasteringChain aggregates a compact before and after report", "[mastering][chain]") {
  constexpr int sample_rate = 44100;
  std::vector<float> samples(static_cast<size_t>(sample_rate) * 4);
  for (size_t index = 0; index < samples.size(); ++index) {
    samples[index] = 0.2f * std::sin(static_cast<float>(index) * 440.0f *
                                     sonare::constants::kTwoPi / sample_rate);
  }
  MasteringChainConfig config;
  config.dynamics.compressor.enabled = true;
  config.dynamics.compressor.config.threshold_db = -30.0f;
  config.loudness.enabled = true;
  config.loudness.target_lufs = -14.0f;

  const auto result =
      MasteringChain(config).process_mono(samples.data(), samples.size(), sample_rate);
  REQUIRE(result.report.before.integrated_lufs == result.input_lufs);
  REQUIRE(result.report.after.integrated_lufs == result.output_lufs);
  REQUIRE(result.report.after.true_peak_dbtp == result.output_true_peak_dbtp);
  REQUIRE(result.report.after.loudness_range == result.output_lra);
  REQUIRE(std::isfinite(result.report.before.max_momentary_lufs));
  REQUIRE(std::isfinite(result.report.after.max_short_term_lufs));
  REQUIRE(result.report.band_energy_delta_db.size() == kMasteringReportBandCount);
  REQUIRE(result.report.max_gain_reduction_db <= 0.0f);
  REQUIRE(result.report.loudness_target_limited == result.loudness_target_limited);
}

TEST_CASE("MasteringChain validates offline input at the core (all surfaces inherit)",
          "[mastering][chain]") {
  // Centralized validation in process_mono/process_stereo so every binding
  // (C ABI, Node, WASM, Python) rejects degenerate input identically instead of
  // silently producing empty/garbage results.
  MasteringChain chain(MasteringChainConfig{});
  std::vector<float> ok(2048, 0.1f);

  SECTION("empty input") {
    REQUIRE_THROWS_AS(chain.process_mono(nullptr, 0, 44100), SonareException);
  }
  SECTION("out-of-range sample rate") {
    REQUIRE_THROWS_AS(chain.process_mono(ok.data(), ok.size(), 100), SonareException);
    REQUIRE_THROWS_AS(chain.process_mono(ok.data(), ok.size(), 10000000), SonareException);
  }
  SECTION("non-finite sample") {
    std::vector<float> bad = ok;
    bad[10] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_AS(chain.process_mono(bad.data(), bad.size(), 44100), SonareException);
  }
  SECTION("stereo validates both channels") {
    std::vector<float> bad = ok;
    bad[5] = std::numeric_limits<float>::infinity();
    REQUIRE_THROWS_AS(chain.process_stereo(ok.data(), bad.data(), ok.size(), 44100),
                      SonareException);
  }
}

TEST_CASE("MasteringChain reports enabled stage names in result", "[mastering][chain]") {
  std::vector<float> samples(44100, 0.1f);
  MasteringChainConfig config;
  config.eq.tilt.enabled = true;
  config.eq.tilt.tilt_db = 1.0f;
  MasteringChain chain(config);
  auto result = chain.process_mono(samples.data(), samples.size(), 44100);
  REQUIRE_FALSE(result.stages.empty());
  REQUIRE(result.stages.front() == "eq.tilt");
}

TEST_CASE("MasteringChain regular processing skips the cancellation callback",
          "[mastering][chain]") {
  std::vector<float> samples(44100, 0.1f);
  MasteringChainConfig config;
  config.eq.tilt.enabled = true;
  MasteringChain chain(config);
  int cancel_queries = 0;
  chain.set_cancel_callback([&] {
    ++cancel_queries;
    return true;
  });

  const auto result = chain.process_mono(samples.data(), samples.size(), 44100);

  REQUIRE(result.samples.size() == samples.size());
  REQUIRE(cancel_queries == 0);
}

TEST_CASE("MasteringChain stereo LRA uses channel summing, not a phase-cancelling mono downmix",
          "[mastering][chain]") {
  // Anti-phase stereo (R = -L) with a real quiet->loud loudness range. A
  // 0.5*(L+R) mono downmix collapses to silence and would report ~0 LRA; the
  // channel-summed measurement (matching output_lufs) must preserve the range.
  const int sr = 44100;
  const std::size_t half = static_cast<std::size_t>(sr) * 4;  // 4 s quiet + 4 s loud
  std::vector<float> left(half * 2);
  std::vector<float> right(half * 2);
  const double w = sonare::constants::kTwoPiD * 220.0 / static_cast<double>(sr);
  for (std::size_t i = 0; i < left.size(); ++i) {
    const float amp = i < half ? 0.10f : 0.30f;  // ~9.5 dB range, both above the gate
    const float s = amp * static_cast<float>(std::sin(w * static_cast<double>(i)));
    left[i] = s;
    right[i] = -s;  // anti-phase: mono downmix cancels to zero
  }

  MasteringChain chain(MasteringChainConfig{});
  auto result = chain.process_stereo(left.data(), right.data(), left.size(), sr);
  REQUIRE(result.output_lra > 1.0f);
}

// The progress callback re-enters caller code mid-render, and every binding
// lends the chain a pointer it does not own across that boundary (a Node
// TypedArray, a Python buffer, a WASM heap view). That is only safe because
// process_mono_impl / process_stereo_impl copy the input into their own vector
// before the first callback fires, so the callback observes a snapshot. Mutating
// the caller's buffer from inside the callback is what makes the difference
// observable: if the chain ever borrows instead of copying, the mutated run
// diverges and this fails. Without it a binding could only close the hazard by
// duplicating a whole recording a second time.
TEST_CASE("MasteringChain copies its input before the first progress callback",
          "[mastering][chain]") {
  constexpr int kSampleRate = 22050;
  constexpr std::size_t kLength = kSampleRate / 4;

  MasteringChainConfig config;
  config.eq.tilt.enabled = true;
  config.eq.tilt.tilt_db = 1.5f;
  config.dynamics.compressor.enabled = true;
  config.dynamics.compressor.config.threshold_db = -24.0f;

  auto tone = [](std::size_t length) {
    std::vector<float> samples(length);
    for (std::size_t i = 0; i < length; ++i) {
      samples[i] = 0.5f * std::sin(constants::kTwoPi * 440.0 * static_cast<double>(i) /
                                   static_cast<double>(kSampleRate));
    }
    return samples;
  };

  MasteringChain reference_chain(config);
  int reference_calls = 0;
  reference_chain.set_progress_callback([&](float, const char*) { ++reference_calls; });
  std::vector<float> untouched = tone(kLength);
  const auto reference =
      reference_chain.process_mono(untouched.data(), untouched.size(), kSampleRate);
  REQUIRE(reference_calls > 0);

  MasteringChain mutated_chain(config);
  std::vector<float> mutated = tone(kLength);
  mutated_chain.set_progress_callback([&](float, const char*) {
    // Overwrite the caller's buffer with a signal the chain would render very
    // differently, on every callback.
    std::fill(mutated.begin(), mutated.end(), -1.0f);
  });
  const auto observed = mutated_chain.process_mono(mutated.data(), mutated.size(), kSampleRate);

  REQUIRE(observed.samples.size() == reference.samples.size());
  CHECK(max_abs_difference(observed.samples, reference.samples) == 0.0f);
  CHECK(observed.input_lufs == reference.input_lufs);

  // Non-vacuity: the chain renders the overwritten signal to something else
  // entirely, so a borrowed input could not have gone unnoticed above.
  MasteringChain overwritten_chain(config);
  std::vector<float> overwritten(kLength, -1.0f);
  const auto overwritten_result =
      overwritten_chain.process_mono(overwritten.data(), overwritten.size(), kSampleRate);
  CHECK(max_abs_difference(overwritten_result.samples, reference.samples) > 0.0f);

  // The same contract on the stereo path.
  MasteringChain stereo_reference(config);
  std::vector<float> stereo_left = tone(kLength);
  std::vector<float> stereo_right = tone(kLength);
  const auto stereo_expected = stereo_reference.process_stereo(
      stereo_left.data(), stereo_right.data(), stereo_left.size(), kSampleRate);

  MasteringChain stereo_chain(config);
  std::vector<float> victim_left = tone(kLength);
  std::vector<float> victim_right = tone(kLength);
  stereo_chain.set_progress_callback([&](float, const char*) {
    std::fill(victim_left.begin(), victim_left.end(), -1.0f);
    std::fill(victim_right.begin(), victim_right.end(), -1.0f);
  });
  const auto stereo_observed = stereo_chain.process_stereo(victim_left.data(), victim_right.data(),
                                                           victim_left.size(), kSampleRate);

  CHECK(max_abs_difference(stereo_observed.left, stereo_expected.left) == 0.0f);
  CHECK(max_abs_difference(stereo_observed.right, stereo_expected.right) == 0.0f);
}

// validate_mastering_chain_config() promises to reject anything a later stage
// would throw on, before any stage runs. eq.tilt's pivot was missing from it,
// and eq.tilt sits behind six repair stages: an album-length render with denoise
// enabled used to spend minutes on STFT work and then die on an EQ error that
// never named the tilt stage.
TEST_CASE("MasteringChain rejects an invalid eq.tilt pivot at construction", "[mastering][chain]") {
  for (const float pivot : {0.0f, -100.0f, std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity()}) {
    CAPTURE(pivot);
    MasteringChainConfig config;
    config.eq.tilt.enabled = true;
    config.eq.tilt.tilt_db = 1.0f;
    config.eq.tilt.pivot_hz = pivot;
    CHECK_THROWS_AS(MasteringChain(config), sonare::SonareException);
  }
  // A disabled tilt carries no constraint.
  MasteringChainConfig disabled;
  disabled.eq.tilt.pivot_hz = 0.0f;
  CHECK_NOTHROW(MasteringChain(disabled));
}

TEST_CASE("MasteringChain rejects a tilt pivot above Nyquist before the first stage",
          "[mastering][chain]") {
  constexpr int kSampleRate = 22050;  // Nyquist 11025
  MasteringChainConfig config;
  config.repair.declick.enabled = true;  // the expensive stage that must not run
  config.eq.tilt.enabled = true;
  config.eq.tilt.tilt_db = 1.5f;
  config.eq.tilt.pivot_hz = 18000.0f;
  MasteringChain chain(config);

  // The progress callback fires once per completed stage, so "never called" is
  // the observable proof that nothing ran before the rejection.
  int stages_run = 0;
  chain.set_progress_callback([&](float, const char*) { ++stages_run; });

  std::vector<float> samples(kSampleRate, 0.1f);
  CHECK_THROWS_AS(chain.process_mono(samples.data(), samples.size(), kSampleRate),
                  sonare::SonareException);
  CHECK(stages_run == 0);
  CHECK_THROWS_AS(chain.process_stereo(samples.data(), samples.data(), samples.size(), kSampleRate),
                  sonare::SonareException);
  CHECK(stages_run == 0);

  // The same pivot is legal at a rate whose Nyquist is above it.
  CHECK_NOTHROW(chain.process_mono(samples.data(), samples.size(), 48000));

  // A zero tilt leaves both shelves disabled, and a disabled band never has its
  // coefficients designed - so the rate check must not reject what the stage
  // would have run happily.
  MasteringChainConfig flat = config;
  flat.eq.tilt.tilt_db = 0.0f;
  MasteringChain flat_chain(flat);
  CHECK_NOTHROW(flat_chain.process_mono(samples.data(), samples.size(), kSampleRate));
}

TEST_CASE("MasteringChain rejects unsupported true-peak oversampling before processing",
          "[mastering][chain]") {
  MasteringChainConfig config;
  config.loudness.enabled = false;
  config.loudness.true_peak_oversample = 3;  // unsupported
  REQUIRE_THROWS_AS(MasteringChain(config), SonareException);

  Param params[] = {{"loudness.truePeakOversample", 3.0}};
  REQUIRE_THROWS_AS(parse_chain_config_params(params, 1), SonareException);
}

TEST_CASE("parse_chain_config_params builds nested config from flat params", "[mastering][chain]") {
  Param params[] = {
      {"dynamics.compressor.thresholdDb", -24.0},
      {"dynamics.compressor.ratio", 2.0},
      {"loudness.targetLufs", -14.0},
  };
  auto config = parse_chain_config_params(params, 3);
  REQUIRE(config.dynamics.compressor.enabled);
  REQUIRE_THAT(config.dynamics.compressor.config.threshold_db, WithinAbs(-24.0f, 1e-6f));
  REQUIRE_THAT(config.dynamics.compressor.config.ratio, WithinAbs(2.0f, 1e-6f));
  REQUIRE(config.loudness.enabled);
  REQUIRE_THAT(config.loudness.target_lufs, WithinAbs(-14.0f, 1e-6f));
}

TEST_CASE("parse_chain_config_params rejects unknown keys", "[mastering][chain]") {
  Param params[] = {{"nonexistent.key", 0.0}};
  REQUIRE_THROWS_AS(parse_chain_config_params(params, 1), sonare::SonareException);
}

TEST_CASE("parse_chain_config_params honors explicit enabled=false", "[mastering][chain]") {
  Param params[] = {
      {"dynamics.compressor.thresholdDb", -24.0},
      {"dynamics.compressor.enabled", 0.0},
  };
  auto config = parse_chain_config_params(params, 2);
  REQUIRE_FALSE(config.dynamics.compressor.enabled);
}

TEST_CASE("multiband override rejects an out-of-range band index", "[mastering][chain]") {
  // On a config shrunk below the indexed band, a per-band override used to be
  // silently dropped while still reporting success; it must now throw.
  MasteringChainConfig cfg;
  cfg.dynamics.multiband_comp.config.bands.resize(2);  // no band index 2
  Param high[] = {{"dynamics.multibandComp.highRatio", 4.0}};
  REQUIRE_THROWS_AS(apply_chain_config_overrides(cfg, high, 1), sonare::SonareException);
  // An in-range band still applies without throwing.
  Param low[] = {{"dynamics.multibandComp.lowRatio", 3.0}};
  REQUIRE_NOTHROW(apply_chain_config_overrides(cfg, low, 1));
  REQUIRE_THAT(cfg.dynamics.multiband_comp.config.bands[0].ratio, WithinAbs(3.0f, 1e-6f));
}

TEST_CASE("color-stage override does not silently disable a preset stage", "[mastering][chain]") {
  // A preset with tape enabled, then an override of a tape param without an
  // explicit `enabled`, must leave tape enabled (it previously recomputed
  // enabled from any_key_seen && meaningful and could turn it off).
  MasteringChainConfig cfg;
  cfg.saturation.tape.enabled = true;
  Param override_params[] = {{"saturation.tape.driveDb", 1.0}};
  apply_chain_config_overrides(cfg, override_params, 1);
  REQUIRE(cfg.saturation.tape.enabled);

  // An explicit enabled=false still wins.
  Param disable[] = {{"saturation.tape.enabled", 0.0}};
  apply_chain_config_overrides(cfg, disable, 1);
  REQUIRE_FALSE(cfg.saturation.tape.enabled);
}

TEST_CASE("MasteringChain processes stereo audio with stereo stage", "[mastering][chain]") {
  std::vector<float> left(22050, 0.1f);
  std::vector<float> right(22050, -0.1f);
  MasteringChainConfig config;
  config.stereo.imager.enabled = true;
  config.stereo.imager.config.width = 1.2f;
  MasteringChain chain(config);
  auto result = chain.process_stereo(left.data(), right.data(), left.size(), 44100);
  REQUIRE(result.left.size() == left.size());
  REQUIRE(result.right.size() == right.size());
}

TEST_CASE("Stereo chain LUFS uses BS1770 channel summing", "[mastering][chain][loudness]") {
  constexpr int sample_rate = 48000;
  std::vector<float> left(static_cast<size_t>(sample_rate));
  std::vector<float> right(left.size());
  std::vector<float> interleaved(left.size() * 2);
  for (size_t i = 0; i < left.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    left[i] = 0.1f * std::sin(2.0f * 3.14159265358979323846f * 440.0f * t);
    right[i] = -left[i];
    interleaved[2 * i] = left[i];
    interleaved[2 * i + 1] = right[i];
  }

  const float expected_lufs =
      common::measure_lufs_interleaved(interleaved.data(), left.size(), 2, sample_rate);
  REQUIRE(std::isfinite(expected_lufs));

  MasteringChain chain(MasteringChainConfig{});
  const auto chain_result =
      chain.process_stereo(left.data(), right.data(), left.size(), sample_rate);
  REQUIRE_THAT(chain_result.input_lufs, WithinAbs(expected_lufs, 1.0e-5f));
  REQUIRE_THAT(chain_result.output_lufs, WithinAbs(expected_lufs, 1.0e-5f));

  const auto named_result = apply_named_processor_stereo(
      "stereo.stereoBalance", left.data(), right.data(), left.size(), sample_rate, {});
  REQUIRE_THAT(named_result.input_lufs, WithinAbs(expected_lufs, 1.0e-5f));
}

TEST_CASE("MasteringChain reports when peak headroom limits the LUFS target",
          "[mastering][chain][loudness]") {
  constexpr int sample_rate = 48000;
  std::vector<float> samples(static_cast<size_t>(sample_rate));
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = 0.8f * std::sin(2.0f * 3.14159265358979323846f * 440.0f * static_cast<float>(i) /
                                 sample_rate);
  }
  MasteringChainConfig config;
  config.loudness.enabled = true;
  config.loudness.target_lufs = -2.0f;
  config.loudness.ceiling_db = -1.0f;
  const auto result =
      MasteringChain(config).process_mono(samples.data(), samples.size(), sample_rate);
  REQUIRE(result.loudness_target_limited);
  REQUIRE(result.output_lufs < config.loudness.target_lufs - 0.5f);
}

TEST_CASE("Stereo chain uses channel-summed LUFS when limiting a loudness target",
          "[mastering][chain][loudness]") {
  constexpr int sample_rate = 48000;
  std::vector<float> left(static_cast<size_t>(sample_rate));
  std::vector<float> right(left.size());
  for (size_t i = 0; i < left.size(); ++i) {
    const float sample = 0.8f * std::sin(2.0f * sonare::constants::kPi * 440.0f *
                                         static_cast<float>(i) / sample_rate);
    left[i] = sample;
    right[i] = -sample;
  }

  MasteringChainConfig config;
  config.loudness.enabled = true;
  config.loudness.target_lufs = 0.0f;
  config.loudness.ceiling_db = -1.0f;
  const auto result =
      MasteringChain(config).process_stereo(left.data(), right.data(), left.size(), sample_rate);

  REQUIRE(std::isfinite(result.input_lufs));
  REQUIRE(result.loudness_target_limited);
  REQUIRE(result.output_lufs < config.loudness.target_lufs - 0.5f);
}

TEST_CASE("Mono chain measures loudness at the stage input after upstream makeup",
          "[mastering][chain][loudness]") {
  constexpr int sample_rate = 48000;
  constexpr std::size_t length = static_cast<std::size_t>(sample_rate) * 4;
  std::vector<float> samples(length);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    samples[index] = 0.1f * std::sin(sonare::constants::kTwoPi * 440.0f *
                                     static_cast<float>(index) / sample_rate);
  }

  MasteringChainConfig upstream_config;
  upstream_config.dynamics.compressor.enabled = true;
  upstream_config.dynamics.compressor.config.threshold_db = 0.0f;
  upstream_config.dynamics.compressor.config.ratio = 1.0f;
  upstream_config.dynamics.compressor.config.makeup_gain_db = 6.0f;
  const auto upstream =
      MasteringChain(upstream_config).process_mono(samples.data(), samples.size(), sample_rate);

  MasteringChainConfig config = upstream_config;
  config.loudness.enabled = true;
  config.loudness.target_lufs = upstream.output_lufs - 3.0f;
  config.loudness.ceiling_db = -1.0f;
  const auto result =
      MasteringChain(config).process_mono(samples.data(), samples.size(), sample_rate);

  CAPTURE(upstream.report.before.integrated_lufs, upstream.output_lufs,
          result.report.before.integrated_lufs, result.input_lufs, result.applied_gain_db,
          result.output_lufs, config.loudness.target_lufs);
  REQUIRE(upstream.output_lufs > result.report.before.integrated_lufs + 5.0f);
  REQUIRE_FALSE(result.loudness_target_limited);
  REQUIRE_THAT(result.applied_gain_db, WithinAbs(-3.0f, 0.05f));
  REQUIRE_THAT(result.output_lufs, WithinAbs(config.loudness.target_lufs, 0.05f));
}

TEST_CASE("Mono chain leaves silence unchanged when loudness is enabled",
          "[mastering][chain][loudness]") {
  constexpr int sample_rate = 48000;
  std::vector<float> silence(static_cast<std::size_t>(sample_rate) * 4, 0.0f);
  MasteringChainConfig config;
  config.loudness.enabled = true;
  config.loudness.target_lufs = -14.0f;

  const auto result =
      MasteringChain(config).process_mono(silence.data(), silence.size(), sample_rate);

  REQUIRE(result.applied_gain_db == 0.0f);
  REQUIRE_FALSE(result.loudness_target_limited);
  REQUIRE(result.output_true_peak_dbtp == sonare::constants::kFloorDb);
  REQUIRE(std::all_of(result.samples.begin(), result.samples.end(),
                      [](float sample) { return sample == 0.0f; }));
}

TEST_CASE("Named stereo fallback processes mono processors per channel", "[mastering][chain]") {
  std::vector<float> left = {0.1f, 0.2f, 0.3f, 0.4f};
  std::vector<float> right = {0.9f, 0.8f, 0.7f, 0.6f};
  std::vector<Param> params = {{"bitDepth", 2.0}};

  const auto stereo = apply_named_processor_stereo("saturation.bitcrusher", left.data(),
                                                   right.data(), left.size(), 48000, params);
  const auto expected_left =
      apply_named_processor("saturation.bitcrusher", left.data(), left.size(), 48000, params);
  const auto expected_right =
      apply_named_processor("saturation.bitcrusher", right.data(), right.size(), 48000, params);

  REQUIRE(stereo.left.size() == expected_left.samples.size());
  REQUIRE(stereo.right.size() == expected_right.samples.size());
  for (size_t i = 0; i < left.size(); ++i) {
    REQUIRE_THAT(stereo.left[i], WithinAbs(expected_left.samples[i], 1.0e-6f));
    REQUIRE_THAT(stereo.right[i], WithinAbs(expected_right.samples[i], 1.0e-6f));
  }
}

TEST_CASE("Named stereo classical repair applies a shared stereo transfer",
          "[mastering][chain][repair]") {
  std::vector<float> left = {0.2f, 0.05f, 0.05f, 0.0f, -0.05f, -0.05f, 0.05f, 0.05f};
  std::vector<float> right(left.size());
  for (size_t i = 0; i < left.size(); ++i) {
    right[i] = 0.2f * left[i];
  }

  const auto result = apply_named_processor_stereo(
      "repair.dereverbClassical", left.data(), right.data(), left.size(), 48000,
      {{"threshold", 0.04}, {"attenuation", 0.5}, {"nFft", 1024.0}, {"hopLength", 256.0}});

  REQUIRE(result.left.size() == left.size());
  REQUIRE(result.right.size() == right.size());
  bool attenuated = false;
  for (size_t i = 0; i < left.size(); ++i) {
    if (std::abs(left[i]) > 1.0e-6f) {
      REQUIRE_THAT(result.right[i], WithinAbs(0.2f * result.left[i], 1.0e-6f));
    }
    if (std::abs(result.left[i]) < std::abs(left[i])) {
      attenuated = true;
    }
  }
  REQUIRE(attenuated);
}

TEST_CASE("Shared stereo repair transfer preserves signed gain changes",
          "[mastering][chain][repair]") {
  std::vector<float> left = {0.25f, -0.5f};
  std::vector<float> right = {0.125f, -0.25f};

  detail::apply_shared_mono_transfer_repair(left, right, 48000, [](const Audio& mono) {
    std::vector<float> repaired(mono.size());
    for (size_t i = 0; i < repaired.size(); ++i) {
      repaired[i] = -2.0f * mono.data()[i];
    }
    return Audio::from_buffer(repaired.data(), repaired.size(), mono.sample_rate());
  });

  REQUIRE_THAT(left[0], WithinAbs(-0.5f, 1.0e-6f));
  REQUIRE_THAT(right[0], WithinAbs(-0.25f, 1.0e-6f));
  REQUIRE_THAT(left[1], WithinAbs(1.0f, 1.0e-6f));
  REQUIRE_THAT(right[1], WithinAbs(0.5f, 1.0e-6f));
}

TEST_CASE("Shared stereo repair transfer stays bounded at mono zero crossings",
          "[mastering][chain][repair]") {
  // Decorrelated stereo content makes the mono mix cross zero where the
  // channels do not; the spectral repair output is not proportional to the
  // mono mix there, so an unbounded out/in ratio would explode the channels.
  constexpr int sample_rate = 22050;
  std::vector<float> left(static_cast<size_t>(sample_rate / 2));
  std::vector<float> right(left.size());
  float input_peak = 0.0f;
  for (size_t i = 0; i < left.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    left[i] = 0.3f * std::sin(2.0f * 3.14159265358979323846f * 220.0f * t);
    right[i] = 0.3f * std::sin(2.0f * 3.14159265358979323846f * 277.0f * t);
    input_peak = std::max({input_peak, std::abs(left[i]), std::abs(right[i])});
  }

  MasteringChainConfig config;
  config.repair.denoise.enabled = true;
  config.repair.denoise.config.gain_floor = 0.1f;
  MasteringChain chain(config);
  auto result = chain.process_stereo(left.data(), right.data(), left.size(), sample_rate);

  float output_peak = 0.0f;
  for (size_t i = 0; i < result.left.size(); ++i) {
    output_peak = std::max({output_peak, std::abs(result.left[i]), std::abs(result.right[i])});
  }
  REQUIRE(output_peak <= 4.0f * input_peak);
}

TEST_CASE("MasteringChain stereo denoise applies a shared stereo transfer",
          "[mastering][chain][repair]") {
  constexpr int sample_rate = 22050;
  std::vector<float> left(static_cast<size_t>(sample_rate / 4));
  std::vector<float> right(left.size());
  for (size_t i = 0; i < left.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    left[i] = 0.12f * std::sin(2.0f * 3.14159265358979323846f * 440.0f * t) +
              0.02f * std::sin(2.0f * 3.14159265358979323846f * 3000.0f * t);
    right[i] = 0.35f * left[i];
  }

  MasteringChainConfig config;
  config.repair.denoise.enabled = true;
  config.repair.denoise.config.n_fft = 1024;
  config.repair.denoise.config.hop_length = 256;
  config.repair.denoise.config.over_subtraction = 4.0f;
  config.repair.denoise.config.gain_floor = 0.05f;
  MasteringChain chain(config);
  auto result = chain.process_stereo(left.data(), right.data(), left.size(), sample_rate);

  REQUIRE(result.left.size() == left.size());
  REQUIRE(result.right.size() == right.size());
  for (size_t i = 0; i < left.size(); ++i) {
    if (std::abs(result.left[i]) > 1.0e-5f) {
      REQUIRE_THAT(result.right[i], WithinAbs(0.35f * result.left[i], 1.0e-5f));
    }
  }
}

// ---------------------------------------------------------------------------
// StreamingMasteringChain
// ---------------------------------------------------------------------------

// prepare() clears the chain before rebuilding it, and a stage's own prepare()
// can fail for a reason only it can see: a multiband crossover cutoff that sat
// below Nyquist at the old rate can be at or above it at the new one. A device
// switch that fails this way used to leave the object holding whatever stages
// had been built before the throw - typically everything except the
// ceiling-enforcing tail - while still accepting blocks, so the live preview
// kept playing, unlimited, with no error.
TEST_CASE("StreamingMasteringChain prepare leaves no partial chain when a stage throws",
          "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.eq.tilt.enabled = true;                      // stage 1: prepares at any rate
  config.dynamics.multiband_comp.enabled = true;      // stage 5: 2 kHz crossover by default
  config.maximizer.true_peak_limiter.enabled = true;  // the tail that must never go missing
  StreamingMasteringChain chain(config);

  chain.prepare(48000.0, 512, 2);
  REQUIRE(chain.stage_names().size() == 3);
  std::vector<float> left(512, 0.05f);
  std::vector<float> right(512, 0.05f);
  float* channels[] = {left.data(), right.data()};
  REQUIRE_NOTHROW(chain.process_block(channels, 2, 512));

  // 3 kHz puts Nyquist at 1.5 kHz, below the crossover's default 2 kHz split,
  // so the multiband stage throws after eq.tilt has already been built.
  REQUIRE_THROWS_AS(chain.prepare(3000.0, 512, 2), sonare::SonareException);

  // Not "prepared with the stages that happened to succeed": unprepared.
  CHECK(chain.stage_names().empty());
  REQUIRE_THROWS_AS(chain.process_block(channels, 2, 512), sonare::SonareException);
  try {
    chain.process_block(channels, 2, 512);
    FAIL("process_block must reject after a failed prepare");
  } catch (const sonare::SonareException& error) {
    CHECK(error.code() == sonare::ErrorCode::InvalidState);
  }

  // A working chain can still be re-established.
  REQUIRE_NOTHROW(chain.prepare(48000.0, 512, 2));
  CHECK(chain.stage_names().size() == 3);
  REQUIRE_NOTHROW(chain.process_block(channels, 2, 512));
}

// An argument the entry point itself rejects is not a failed re-prepare: it
// never touched a stage, so the chain that was already prepared keeps working.
TEST_CASE("StreamingMasteringChain keeps a prepared chain when prepare rejects its arguments",
          "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.eq.tilt.enabled = true;
  StreamingMasteringChain chain(config);
  chain.prepare(48000.0, 512, 2);
  REQUIRE(chain.stage_names().size() == 1);

  REQUIRE_THROWS_AS(chain.prepare(48000.0, 512, 3), sonare::SonareException);
  REQUIRE_THROWS_AS(chain.prepare(48000.0, 0, 2), sonare::SonareException);
  REQUIRE_THROWS_AS(chain.prepare(0.0, 512, 2), sonare::SonareException);

  CHECK(chain.stage_names().size() == 1);
  std::vector<float> left(512, 0.05f);
  std::vector<float> right(512, 0.05f);
  float* channels[] = {left.data(), right.data()};
  CHECK_NOTHROW(chain.process_block(channels, 2, 512));
}

TEST_CASE("StreamingMasteringChain throws if denoise enabled", "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.repair.denoise.enabled = true;
  REQUIRE_THROWS_AS(StreamingMasteringChain(std::move(config)), sonare::SonareException);
}

// The class doc in chain.h and the three binding docstrings that mirror it name
// the stages this chain supports and the stages it rejects. Both sets are
// derived here from the implementation rather than restated: the stage universe
// comes from the config surface (every switchable stage serializes a
// "<stage>.enabled" key), the rejected set is whichever of those stages makes
// the constructor throw, and the supported set is what prepare() actually
// instantiates, read back from stage_names(). A stage added to prepare() or a
// change to the rejection set turns this red, and the doc lists are the
// expectations it is checked against.
TEST_CASE("StreamingMasteringChain supported and rejected stages match the implementation",
          "[mastering][chain][streaming]") {
  const std::vector<std::string> stage_ids = [] {
    const MasteringChainConfig defaults;
    const auto root = sonare::util::json::parse_strict(chain_config_to_json(defaults));
    const std::string suffix = ".enabled";
    std::vector<std::string> out;
    for (const auto& [key, value] : root["params"].as_object()) {
      (void)value;
      if (key.size() > suffix.size() &&
          key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
        out.push_back(key.substr(0, key.size() - suffix.size()));
      }
    }
    std::sort(out.begin(), out.end());
    return out;
  }();
  REQUIRE(stage_ids.size() >= 12);

  std::vector<std::string> rejected;
  std::vector<std::string> supported;
  for (const std::string& stage : stage_ids) {
    const std::vector<Param> params{{stage + ".enabled", 1.0}};
    MasteringChainConfig config = parse_chain_config_params(params.data(), params.size());
    INFO("stage " << stage);
    try {
      StreamingMasteringChain chain(config);
      // Stereo, so the two stereo-only stages are reachable.
      chain.prepare(48000.0, 512, 2);
      // Enabling exactly one stage must instantiate exactly that stage: this is
      // what makes the list prepare()'s instantiation set rather than "the
      // constructor tolerated the config".
      REQUIRE(chain.stage_names() == std::vector<std::string>{stage});
      supported.push_back(stage);
    } catch (const sonare::SonareException&) {
      rejected.push_back(stage);
    }
  }

  // Every whole-signal repair stage is rejected - all six of them, not just
  // repair.denoise - and so is loudness without a precomputed static gain.
  const std::vector<std::string> kRejected = {
      "loudness",     "repair.declick", "repair.declip",  "repair.decrackle",
      "repair.dehum", "repair.denoise", "repair.dereverb"};
  const std::vector<std::string> kSupported = {"dynamics.compressor",
                                               "dynamics.deesser",
                                               "dynamics.multibandComp",
                                               "dynamics.transientShaper",
                                               "eq.tilt",
                                               "maximizer.truePeakLimiter",
                                               "saturation.exciter",
                                               "saturation.tape",
                                               "spectral.airBand",
                                               "stereo.imager",
                                               "stereo.monoMaker"};
  CHECK(rejected == kRejected);
  CHECK(supported == kSupported);

  // The two stereo-image stages are the only ones prepare() drops on a mono
  // chain, which is the "(stereo only)" qualifier in the same doc.
  const std::vector<Param> stereo_params{
      {"stereo.imager.enabled", 1.0}, {"stereo.monoMaker.enabled", 1.0}, {"eq.tilt.enabled", 1.0}};
  StreamingMasteringChain mono_chain(
      parse_chain_config_params(stereo_params.data(), stereo_params.size()));
  mono_chain.prepare(48000.0, 512, 1);
  CHECK(mono_chain.stage_names() == std::vector<std::string>{"eq.tilt"});
}

TEST_CASE("StreamingMasteringChain throws if loudness enabled", "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.loudness.enabled = true;
  REQUIRE_THROWS_AS(StreamingMasteringChain(std::move(config)), sonare::SonareException);
}

TEST_CASE("StreamingMasteringChain options constructor requires finite gain when loudness enabled",
          "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.loudness.enabled = true;
  // Default options leave loudness_static_gain_db = NaN -> must still throw.
  REQUIRE_THROWS_AS(StreamingMasteringChain(config, StreamingMasteringChainOptions{}),
                    sonare::SonareException);
}

TEST_CASE("StreamingMasteringChain accepts loudness as a precomputed static gain",
          "[mastering][chain][streaming]") {
  // A preset config (every preset enables loudness) must be previewable in the
  // streaming chain once the caller supplies a precomputed static gain.
  MasteringChainConfig config = preset_config(Preset::Pop);
  REQUIRE(config.loudness.enabled);

  StreamingMasteringChainOptions options;
  options.loudness_static_gain_db = 6.0f;
  StreamingMasteringChain chain(config, options);
  chain.prepare(44100.0, 512, 2);

  // The loudness stage appears as a named stage in the streaming chain.
  const auto& names = chain.stage_names();
  REQUIRE(std::find(names.begin(), names.end(), "loudness.optimize") != names.end());

  std::vector<float> left(512, 0.05f);
  std::vector<float> right(512, 0.05f);
  float* channels[] = {left.data(), right.data()};
  REQUIRE_NOTHROW(chain.process_block(channels, 2, 512));

  // The static +6 dB gain (then ceiling limiting) must raise the level versus a
  // chain built from the same config with loudness disabled.
  MasteringChainConfig no_loud = config;
  no_loud.loudness.enabled = false;
  StreamingMasteringChain ref(std::move(no_loud));
  ref.prepare(44100.0, 512, 2);
  std::vector<float> rleft(512, 0.05f);
  std::vector<float> rright(512, 0.05f);
  float* rchannels[] = {rleft.data(), rright.data()};
  ref.process_block(rchannels, 2, 512);

  // After settling, the loudness preview block should be louder than the
  // loudness-disabled reference (static gain applied).
  REQUIRE(std::abs(left[256]) > std::abs(rleft[256]));
}

TEST_CASE("StreamingMasteringChain options constructor ignores gain when loudness disabled",
          "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.eq.tilt.enabled = true;
  StreamingMasteringChainOptions options;
  options.loudness_static_gain_db = 6.0f;  // ignored: loudness not enabled
  StreamingMasteringChain chain(std::move(config), options);
  chain.prepare(44100.0, 512, 1);
  const auto& names = chain.stage_names();
  REQUIRE(std::find(names.begin(), names.end(), "loudness.optimize") == names.end());
}

TEST_CASE("StreamingMasteringChain processes mono blocks", "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.eq.tilt.enabled = true;
  config.eq.tilt.tilt_db = 1.0f;
  StreamingMasteringChain chain(std::move(config));
  chain.prepare(44100.0, 512, 1);
  std::vector<float> block(512, 0.1f);
  float* channels[] = {block.data()};
  chain.process_block(channels, 1, 512);
  REQUIRE(block[0] != 0.1f);
}

TEST_CASE("StreamingMasteringChain flushes AirBand and true-peak latency",
          "[mastering][chain][streaming]") {
  constexpr int kSampleRate = 48000;
  constexpr int kBlockSize = 128;
  std::vector<float> input(2048, 0.0f);
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = 0.25f *
               std::sin(static_cast<float>(i) * sonare::constants::kTwoPi * 14000.0f / kSampleRate);
  }

  MasteringChainConfig config;
  config.spectral.air_band.enabled = true;
  config.spectral.air_band.config = {0.7f, 10000.0f, -60.0f, 6.0f};
  config.maximizer.true_peak_limiter.enabled = true;

  const auto offline = MasteringChain(config).process_mono(input.data(), input.size(), kSampleRate);
  StreamingMasteringChain streaming(config);
  streaming.prepare(kSampleRate, kBlockSize, 1);
  REQUIRE(streaming.latency_samples() > 0);

  std::vector<float> streamed;
  for (size_t offset = 0; offset < input.size(); offset += kBlockSize) {
    std::vector<float> block(input.begin() + static_cast<std::ptrdiff_t>(offset),
                             input.begin() + static_cast<std::ptrdiff_t>(offset + kBlockSize));
    float* channels[] = {block.data()};
    streaming.process_block(channels, 1, static_cast<int>(block.size()));
    streamed.insert(streamed.end(), block.begin(), block.end());
  }
  for (;;) {
    std::vector<float> tail(kBlockSize);
    float* channels[] = {tail.data()};
    const int written = streaming.flush(channels, 1, kBlockSize);
    if (written == 0) break;
    streamed.insert(streamed.end(), tail.begin(), tail.begin() + written);
  }

  // flush() drains the chain's reported latency plus any finite stage tail, so
  // the latency-aligned window always covers the whole offline result.
  const auto latency = static_cast<size_t>(streaming.latency_samples());
  CAPTURE(latency, streamed.size(), offline.samples.size());
  REQUIRE(streamed.size() >= input.size() + latency);
  const auto aligned_begin = streamed.begin() + static_cast<std::ptrdiff_t>(latency);
  std::vector<float> aligned(aligned_begin, aligned_begin + offline.samples.size());

  // Away from the stream edges the two paths run the same processors over the
  // same samples, so removing the reported latency must line them up exactly.
  // Anything else means a stage under-reports its latency or carries block-edge
  // state, which is what this test exists to catch -- a loose whole-buffer
  // tolerance would hide both behind the edge effects described below.
  REQUIRE(aligned.size() > 2 * latency);
  const auto interior = [latency](const std::vector<float>& v) {
    return std::vector<float>(v.begin() + static_cast<std::ptrdiff_t>(latency),
                              v.end() - static_cast<std::ptrdiff_t>(latency));
  };
  CAPTURE(max_abs_difference(interior(aligned), interior(offline.samples)));
  REQUIRE(max_abs_difference(interior(aligned), interior(offline.samples)) < 1.0e-6f);

  // One latency window at each edge is allowed to differ, because the offline
  // chain compensates latency per stage: AirBand's delayed overhang is trimmed
  // off before TruePeakLimiter runs, so the limiter's lookahead reads zeros at
  // the end of the offline stream where the streaming chain feeds it the real
  // continuation, and its gain ramp starts one AirBand latency earlier. Both
  // are end-of-stream artifacts, not drift, and stay far below audibility.
  CAPTURE(max_abs_difference(aligned, offline.samples));
  REQUIRE(max_abs_difference(aligned, offline.samples) < 1.0e-2f);
}

TEST_CASE("StreamingMasteringChain flushes stereo AirBand and true-peak latency",
          "[mastering][chain][streaming]") {
  constexpr int kSampleRate = 48000;
  constexpr int kBlockSize = 128;
  std::vector<float> left(2048, 0.0f);
  std::vector<float> right(2048, 0.0f);
  for (size_t i = 0; i < left.size(); ++i) {
    left[i] = 0.25f *
              std::sin(static_cast<float>(i) * sonare::constants::kTwoPi * 14000.0f / kSampleRate);
  }
  // Keep the planes deliberately different and put a right-only impulse at
  // the end, where only flush can emit its delayed output.
  right.back() = 0.5f;

  MasteringChainConfig config;
  config.spectral.air_band.enabled = true;
  config.spectral.air_band.config = {0.7f, 10000.0f, -60.0f, 6.0f};
  config.maximizer.true_peak_limiter.enabled = true;

  const auto offline =
      MasteringChain(config).process_stereo(left.data(), right.data(), left.size(), kSampleRate);
  StreamingMasteringChain streaming(config);
  streaming.prepare(kSampleRate, kBlockSize, 2);
  REQUIRE(streaming.latency_samples() > 0);

  std::vector<float> streamed_left;
  std::vector<float> streamed_right;
  for (size_t offset = 0; offset < left.size(); offset += kBlockSize) {
    std::vector<float> left_block(left.begin() + static_cast<std::ptrdiff_t>(offset),
                                  left.begin() + static_cast<std::ptrdiff_t>(offset + kBlockSize));
    std::vector<float> right_block(
        right.begin() + static_cast<std::ptrdiff_t>(offset),
        right.begin() + static_cast<std::ptrdiff_t>(offset + kBlockSize));
    float* channels[] = {left_block.data(), right_block.data()};
    streaming.process_block(channels, 2, static_cast<int>(left_block.size()));
    streamed_left.insert(streamed_left.end(), left_block.begin(), left_block.end());
    streamed_right.insert(streamed_right.end(), right_block.begin(), right_block.end());
  }
  for (;;) {
    std::vector<float> left_tail(kBlockSize);
    std::vector<float> right_tail(kBlockSize);
    float* channels[] = {left_tail.data(), right_tail.data()};
    const int written = streaming.flush(channels, 2, kBlockSize);
    if (written == 0) break;
    streamed_left.insert(streamed_left.end(), left_tail.begin(), left_tail.begin() + written);
    streamed_right.insert(streamed_right.end(), right_tail.begin(), right_tail.begin() + written);
  }

  const auto latency = static_cast<size_t>(streaming.latency_samples());
  CAPTURE(latency, streamed_left.size(), streamed_right.size(), offline.left.size(),
          offline.right.size());
  REQUIRE(streamed_left.size() >= left.size() + latency);
  REQUIRE(streamed_right.size() >= right.size() + latency);
  const auto aligned = [latency](const std::vector<float>& samples, size_t length) {
    const auto begin = samples.begin() + static_cast<std::ptrdiff_t>(latency);
    return std::vector<float>(begin, begin + static_cast<std::ptrdiff_t>(length));
  };
  const auto aligned_left = aligned(streamed_left, offline.left.size());
  const auto aligned_right = aligned(streamed_right, offline.right.size());

  REQUIRE(aligned_left.size() > 2 * latency);
  REQUIRE(aligned_right.size() > 2 * latency);
  const auto interior = [latency](const std::vector<float>& samples) {
    return std::vector<float>(samples.begin() + static_cast<std::ptrdiff_t>(latency),
                              samples.end() - static_cast<std::ptrdiff_t>(latency));
  };
  CAPTURE(max_abs_difference(interior(aligned_left), interior(offline.left)),
          max_abs_difference(interior(aligned_right), interior(offline.right)));
  REQUIRE(max_abs_difference(interior(aligned_left), interior(offline.left)) < 1.0e-6f);
  REQUIRE(max_abs_difference(interior(aligned_right), interior(offline.right)) < 1.0e-6f);

  // As with the mono contract above, only the stream edges may differ: the
  // offline runner trims each stage's latency before the next stage, while
  // streaming flush feeds the real continuation through the complete chain.
  CAPTURE(max_abs_difference(aligned_left, offline.left),
          max_abs_difference(aligned_right, offline.right));
  REQUIRE(max_abs_difference(aligned_left, offline.left) < 1.0e-2f);
  REQUIRE(max_abs_difference(aligned_right, offline.right) < 1.0e-2f);
}

TEST_CASE("StreamingMasteringChain stage_names lists enabled stages",
          "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.eq.tilt.enabled = true;
  config.dynamics.compressor.enabled = true;
  StreamingMasteringChain chain(std::move(config));
  chain.prepare(44100.0, 512, 1);
  const auto& names = chain.stage_names();
  REQUIRE(names.size() == 2);
  REQUIRE(names[0] == "eq.tilt");
  REQUIRE(names[1] == "dynamics.compressor");
}

TEST_CASE("StreamingMasteringChain skips stereo stages when mono",
          "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.stereo.imager.enabled = true;
  StreamingMasteringChain chain(std::move(config));
  chain.prepare(44100.0, 512, 1);
  REQUIRE(chain.stage_names().empty());
}

TEST_CASE("StreamingMasteringChain processes stereo with imager", "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.stereo.imager.enabled = true;
  config.stereo.imager.config.width = 1.2f;
  StreamingMasteringChain chain(std::move(config));
  chain.prepare(44100.0, 512, 2);
  REQUIRE(chain.stage_names().size() == 1);
  std::vector<float> left(512, 0.1f);
  std::vector<float> right(512, -0.1f);
  float* channels[] = {left.data(), right.data()};
  REQUIRE_NOTHROW(chain.process_block(channels, 2, 512));
}

TEST_CASE("StreamingMasteringChain reset clears state", "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.dynamics.compressor.enabled = true;
  StreamingMasteringChain chain(std::move(config));
  chain.prepare(44100.0, 512, 1);
  std::vector<float> block(512, 0.5f);
  float* channels[] = {block.data()};
  chain.process_block(channels, 1, 512);
  REQUIRE_NOTHROW(chain.reset());
}

TEST_CASE("StreamingMasteringChain rejects bad num_channels", "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.eq.tilt.enabled = true;
  StreamingMasteringChain chain(std::move(config));
  REQUIRE_THROWS_AS(chain.prepare(44100.0, 512, 0), sonare::SonareException);
  REQUIRE_THROWS_AS(chain.prepare(44100.0, 512, 3), sonare::SonareException);
}

TEST_CASE("StreamingMasteringChain rejects non-finite input before touching processors",
          "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.eq.tilt.enabled = true;
  config.eq.tilt.tilt_db = 1.0f;
  StreamingMasteringChain chain(std::move(config));
  chain.prepare(44100.0, 512, 1);

  // A block carrying a NaN is rejected without running any processor, so the
  // filter state is preserved and a subsequent finite block still processes.
  std::vector<float> bad(512, 0.1f);
  bad[128] = std::numeric_limits<float>::quiet_NaN();
  float* bad_channels[] = {bad.data()};
  REQUIRE_THROWS_AS(chain.process_block(bad_channels, 1, 512), sonare::SonareException);

  // An Inf is likewise rejected.
  std::vector<float> inf_block(512, 0.1f);
  inf_block[0] = std::numeric_limits<float>::infinity();
  float* inf_channels[] = {inf_block.data()};
  REQUIRE_THROWS_AS(chain.process_block(inf_channels, 1, 512), sonare::SonareException);

  // A fully finite block processes normally after the rejections.
  std::vector<float> good(512, 0.1f);
  float* good_channels[] = {good.data()};
  REQUIRE_NOTHROW(chain.process_block(good_channels, 1, 512));

  // A zero-length block returns early without scanning (and without throwing).
  REQUIRE_NOTHROW(chain.process_block(good_channels, 1, 0));
}

TEST_CASE("StreamingMasteringChain rejects oversized block", "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.eq.tilt.enabled = true;
  StreamingMasteringChain chain(std::move(config));
  chain.prepare(44100.0, 256, 1);
  std::vector<float> block(512, 0.1f);
  float* channels[] = {block.data()};
  REQUIRE_THROWS_AS(chain.process_block(channels, 1, 512), sonare::SonareException);
}

// ---------------------------------------------------------------------------
// New repair / dynamics stages
// ---------------------------------------------------------------------------

TEST_CASE("MasteringChain applies new repair stages", "[mastering][chain]") {
  const int sample_rate = 22050;
  std::vector<float> samples(static_cast<size_t>(sample_rate), 0.0f);
  // Mild noise + a few "clicks".
  for (size_t i = 0; i < samples.size(); ++i) {
    const int noise = static_cast<int>((i * 1103515245u + 12345u) & 0xFFFFu) - 32768;
    samples[i] = 0.01f * static_cast<float>(noise) / 32768.0f;
  }
  samples[1000] = 0.95f;
  samples[5000] = -0.95f;
  samples[10000] = 0.9f;

  MasteringChainConfig config;
  config.repair.declick.enabled = true;
  config.repair.dereverb.enabled = true;

  MasteringChain chain(config);
  auto result = chain.process_mono(samples.data(), samples.size(), sample_rate);

  REQUIRE(result.samples.size() == samples.size());
  REQUIRE(std::find(result.stages.begin(), result.stages.end(), "repair.declick") !=
          result.stages.end());
  REQUIRE(std::find(result.stages.begin(), result.stages.end(), "repair.dereverb") !=
          result.stages.end());
}

TEST_CASE("MasteringChain applies new dynamics stages", "[mastering][chain]") {
  const int sample_rate = 22050;
  std::vector<float> samples(static_cast<size_t>(sample_rate), 0.1f);

  MasteringChainConfig config;
  config.dynamics.deesser.enabled = true;
  config.dynamics.transient_shaper.enabled = true;
  config.dynamics.multiband_comp.enabled = true;

  MasteringChain chain(config);
  auto result = chain.process_mono(samples.data(), samples.size(), sample_rate);

  REQUIRE(result.samples.size() == samples.size());
  REQUIRE(std::find(result.stages.begin(), result.stages.end(), "dynamics.deesser") !=
          result.stages.end());
  REQUIRE(std::find(result.stages.begin(), result.stages.end(), "dynamics.transientShaper") !=
          result.stages.end());
  REQUIRE(std::find(result.stages.begin(), result.stages.end(), "dynamics.multibandComp") !=
          result.stages.end());
}

TEST_CASE("StreamingMasteringChain rejects declick", "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.repair.declick.enabled = true;
  REQUIRE_THROWS_AS(StreamingMasteringChain(std::move(config)), sonare::SonareException);
}

TEST_CASE("StreamingMasteringChain rejects dereverb", "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.repair.dereverb.enabled = true;
  REQUIRE_THROWS_AS(StreamingMasteringChain(std::move(config)), sonare::SonareException);
}

TEST_CASE("StreamingMasteringChain supports new dynamics stages", "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.dynamics.deesser.enabled = true;
  config.dynamics.transient_shaper.enabled = true;
  config.dynamics.multiband_comp.enabled = true;

  StreamingMasteringChain chain(std::move(config));
  chain.prepare(44100.0, 512, 1);

  const auto& names = chain.stage_names();
  REQUIRE(std::find(names.begin(), names.end(), "dynamics.deesser") != names.end());
  REQUIRE(std::find(names.begin(), names.end(), "dynamics.transientShaper") != names.end());
  REQUIRE(std::find(names.begin(), names.end(), "dynamics.multibandComp") != names.end());

  std::vector<float> block(512, 0.1f);
  float* channels[] = {block.data()};
  for (int i = 0; i < 4; ++i) {
    REQUIRE_NOTHROW(chain.process_block(channels, 1, 512));
  }
  for (float v : block) {
    REQUIRE(std::isfinite(v));
  }
}

TEST_CASE("parse_chain_config_params handles new repair keys", "[mastering][chain]") {
  Param params[] = {
      {"repair.declick.threshold", 0.5},    {"repair.declip.clipThreshold", 0.9},
      {"repair.decrackle.mode", 1.0},       {"repair.dehum.fundamentalHz", 60.0},
      {"repair.dereverb.attenuation", 0.7},
  };
  auto config = parse_chain_config_params(params, 5);
  REQUIRE(config.repair.declick.enabled);
  REQUIRE_THAT(config.repair.declick.config.threshold, WithinAbs(0.5f, 1e-6f));
  REQUIRE(config.repair.declip.enabled);
  REQUIRE_THAT(config.repair.declip.config.clip_threshold, WithinAbs(0.9f, 1e-6f));
  REQUIRE(config.repair.decrackle.enabled);
  REQUIRE(config.repair.decrackle.config.mode ==
          ::sonare::mastering::repair::DecrackleMode::WaveletShrinkage);
  REQUIRE(config.repair.dehum.enabled);
  REQUIRE_THAT(config.repair.dehum.config.fundamental_hz, WithinAbs(60.0f, 1e-6f));
  REQUIRE(config.repair.dereverb.enabled);
  REQUIRE_THAT(config.repair.dereverb.config.attenuation, WithinAbs(0.7f, 1e-6f));
}

TEST_CASE("parse_chain_config_params handles new dynamics keys", "[mastering][chain]") {
  Param params[] = {
      {"dynamics.deesser.thresholdDb", -30.0},
      {"dynamics.deesser.bandpassQ", 2.25},
      {"dynamics.transientShaper.attackGainDb", 4.0},
      {"dynamics.multibandComp.lowCutoffHz", 200.0},
      {"dynamics.multibandComp.highCutoffHz", 5000.0},
      {"dynamics.multibandComp.midThresholdDb", -22.0},
  };
  auto config = parse_chain_config_params(params, 6);
  REQUIRE(config.dynamics.deesser.enabled);
  REQUIRE_THAT(config.dynamics.deesser.config.threshold_db, WithinAbs(-30.0f, 1e-6f));
  REQUIRE_THAT(config.dynamics.deesser.config.bandpass_q, WithinAbs(2.25f, 1e-6f));
  REQUIRE(config.dynamics.transient_shaper.enabled);
  REQUIRE_THAT(config.dynamics.transient_shaper.config.attack_gain_db, WithinAbs(4.0f, 1e-6f));
  REQUIRE(config.dynamics.multiband_comp.enabled);
  REQUIRE(config.dynamics.multiband_comp.config.crossover.cutoffs_hz.size() >= 2);
  REQUIRE_THAT(config.dynamics.multiband_comp.config.crossover.cutoffs_hz[0],
               WithinAbs(200.0f, 1e-6f));
  REQUIRE_THAT(config.dynamics.multiband_comp.config.crossover.cutoffs_hz[1],
               WithinAbs(5000.0f, 1e-6f));
  REQUIRE(config.dynamics.multiband_comp.config.bands.size() >= 2);
  REQUIRE_THAT(config.dynamics.multiband_comp.config.bands[1].threshold_db,
               WithinAbs(-22.0f, 1e-6f));
}

TEST_CASE("apply_chain_config_overrides toggles new stages independently", "[mastering][chain]") {
  MasteringChainConfig config;
  config.eq.tilt.enabled = true;
  Param overrides[] = {{"dynamics.deesser.enabled", 1.0}};
  apply_chain_config_overrides(config, overrides, 1);
  REQUIRE(config.dynamics.deesser.enabled);
  REQUIRE(config.eq.tilt.enabled);  // unaffected
}

namespace {

constexpr int kAnalysisSampleRate = 44100;

std::vector<float> analysis_sine(float frequency_hz, float amplitude, size_t length) {
  std::vector<float> samples(length);
  for (size_t index = 0; index < length; ++index) {
    samples[index] = amplitude * std::sin(static_cast<float>(index) * frequency_hz *
                                          sonare::constants::kTwoPi / kAnalysisSampleRate);
  }
  return samples;
}

}  // namespace

TEST_CASE("analyze_named_stereo serializes an out-of-phase pair as valid JSON",
          "[mastering][chain][json]") {
  // A fully out-of-phase pair has zero mid energy, so stereo_width() returns
  // +infinity -- exactly the signal this analysis exists to flag. RFC 8259 has
  // no Infinity literal, so the field has to arrive as JSON null instead of an
  // "inf" token that no parser accepts.
  const auto left = analysis_sine(440.0f, 0.5f, static_cast<size_t>(kAnalysisSampleRate) / 4);
  std::vector<float> right(left.size());
  for (size_t index = 0; index < left.size(); ++index) right[index] = -left[index];

  const std::string text = analyze_named_stereo("stereo.monoCompatCheck", left.data(), right.data(),
                                                left.size(), kAnalysisSampleRate, {});
  REQUIRE(text.find("inf") == std::string::npos);

  const auto value = sonare::util::json::parse_strict(text);
  REQUIRE_THAT(value["correlation"].as_number(), WithinAbs(-1.0, 1e-6));
  REQUIRE(value["width"].is_null());
  REQUIRE_THAT(value["monoPeak"].as_number(), WithinAbs(0.0, 1e-6));
  REQUIRE(std::isfinite(value["sideRms"].as_number()));
  REQUIRE(value["likelyMonoCompatible"].as_bool() == false);
}

TEST_CASE("analyze_named_pair serializes a silent source as valid JSON",
          "[mastering][chain][json]") {
  // EBU R128 reports -inf LUFS for a signal below the measurement floor, and
  // metering/lufs.cpp keeps that sentinel deliberately. The serializer, not the
  // meter, is what has to make it representable.
  const std::vector<float> silence(static_cast<size_t>(kAnalysisSampleRate) / 4, 0.0f);
  const auto reference = analysis_sine(440.0f, 0.5f, silence.size());

  const std::string text =
      analyze_named_pair("match.referenceLoudness", silence.data(), reference.data(),
                         silence.size(), reference.size(), kAnalysisSampleRate, {});
  REQUIRE(text.find("inf") == std::string::npos);

  const auto value = sonare::util::json::parse_strict(text);
  REQUIRE(value["sourceLufs"].is_null());
  REQUIRE(std::isfinite(value["referenceLufs"].as_number()));
  // reference_loudness() zeroes the match gain when either reading is
  // non-finite, so this one stays a real number.
  REQUIRE_THAT(value["gainToMatchDb"].as_number(), WithinAbs(0.0, 1e-6));
}

TEST_CASE("named analysis JSON is locale-independent", "[mastering][chain][json][locale]") {
  // A DAW plugin host may run with a comma-decimal locale. Setting LC_NUMERIC
  // alone is not enough to reproduce the failure: a std::ostringstream carries
  // the global C++ locale, not the C one, and writes "-21,7539" only once
  // std::locale::global has been moved. Both are switched here so the test
  // covers the stream path and any C-library formatting alike -- what an
  // unimbued writer produces is `{"sourceLufs":-21,7539}`, a document that
  // parses into a different shape rather than failing outright.
  struct LocaleSwitch {
    std::locale saved_global;
    std::string saved_numeric;
    LocaleSwitch() : saved_global(std::locale()) {
      const char* previous = std::setlocale(LC_NUMERIC, nullptr);
      saved_numeric = previous ? previous : "C";
      // Both glibc and macOS ship de_DE.UTF-8; if none of the spellings is
      // installed the locale stays classic and the assertions still hold.
      for (const char* tag : {"de_DE.UTF-8", "de_DE.utf8", "de_DE"}) {
        try {
          std::locale::global(std::locale(tag));
          std::setlocale(LC_NUMERIC, tag);
          break;
        } catch (const std::runtime_error&) {
          continue;
        }
      }
    }
    ~LocaleSwitch() {
      std::locale::global(saved_global);
      std::setlocale(LC_NUMERIC, saved_numeric.c_str());
    }
  } locale_switch;

  const auto source = analysis_sine(440.0f, 0.5f, static_cast<size_t>(kAnalysisSampleRate) / 4);
  const auto reference = analysis_sine(440.0f, 0.125f, source.size());
  const std::string pair_text =
      analyze_named_pair("match.referenceLoudness", source.data(), reference.data(), source.size(),
                         reference.size(), kAnalysisSampleRate, {});
  const std::string stereo_text =
      analyze_named_stereo("stereo.monoCompatCheck", source.data(), reference.data(), source.size(),
                           kAnalysisSampleRate, {});

  const auto expected = ::sonare::mastering::match::reference_loudness(
      Audio::from_buffer(source.data(), source.size(), kAnalysisSampleRate),
      Audio::from_buffer(reference.data(), reference.size(), kAnalysisSampleRate));

  const auto pair_value = sonare::util::json::parse_strict(pair_text);
  // A comma decimal separator would land here as a truncated integer, so an
  // exact-value check is what catches it rather than a "did it parse" check.
  REQUIRE_THAT(pair_value["sourceLufs"].as_number(),
               WithinAbs(static_cast<double>(expected.source_lufs), 1e-4));
  REQUIRE_THAT(pair_value["gainToMatchDb"].as_number(),
               WithinAbs(static_cast<double>(expected.gain_to_match_db), 1e-4));

  const auto stereo_value = sonare::util::json::parse_strict(stereo_text);
  REQUIRE_THAT(stereo_value["correlation"].as_number(), WithinAbs(1.0, 1e-6));
  REQUIRE(stereo_value["width"].as_number() < 1.0);
}

TEST_CASE("chain progress covers the DSP stages and says so", "[mastering][chain]") {
  // The callback's contract is per-stage, and the documentation now says which
  // stages: `progress` is completed/enabled DSP stages, so it reaches 1.0 as the
  // last enabled stage returns and the trailing output measurement, spectrum and
  // band delta report nothing. A host reading it as a wall-clock fraction sees it
  // sit at 1.0 for one loudness-plus-spectrum pass, which is what the doc has to
  // state rather than imply otherwise.
  constexpr int kSampleRate = 22050;
  std::vector<float> samples(kSampleRate, 0.0f);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = 0.2f * std::sin(2.0f * sonare::constants::kPi * 220.0f * static_cast<float>(i) /
                                 kSampleRate);
  }

  MasteringChainConfig config;
  config.eq.tilt.enabled = true;
  config.eq.tilt.tilt_db = 1.0f;
  config.dynamics.compressor.enabled = true;
  config.dynamics.compressor.config.threshold_db = -24.0f;

  MasteringChain chain(config);
  std::vector<std::pair<float, std::string>> progress;
  chain.set_progress_callback(
      [&](float value, const char* stage) { progress.emplace_back(value, stage ? stage : ""); });
  const auto result = chain.process_mono(samples.data(), samples.size(), kSampleRate);

  // One callback per enabled stage, in order, and nothing after the last one.
  REQUIRE(progress.size() == result.stages.size());
  for (size_t i = 0; i < progress.size(); ++i) {
    CHECK(progress[i].second == result.stages[i]);
    CHECK(progress[i].first ==
          Catch::Approx(static_cast<float>(i + 1) / static_cast<float>(progress.size())));
  }
  REQUIRE(!progress.empty());
  CHECK(progress.back().first == Catch::Approx(1.0f));

  // A config with no enabled stage is the documented special case: exactly one
  // callback, 1.0, named "complete".
  MasteringChain empty_chain{MasteringChainConfig{}};
  std::vector<std::pair<float, std::string>> empty_progress;
  empty_chain.set_progress_callback([&](float value, const char* stage) {
    empty_progress.emplace_back(value, stage ? stage : "");
  });
  const auto empty_result = empty_chain.process_mono(samples.data(), samples.size(), kSampleRate);
  CHECK(empty_result.stages.empty());
  REQUIRE(empty_progress.size() == 1);
  CHECK(empty_progress[0].first == Catch::Approx(1.0f));
  CHECK(empty_progress[0].second == "complete");
}

#if defined(__APPLE__) || defined(__linux__)
namespace {

/// Resident set size of this process, in bytes. Used only by the working-set
/// case below, which is opt-in.
std::size_t resident_bytes();

}  // namespace

// Opt-in: it renders a minute of 96 kHz audio and reads process RSS, so it is
// both slow and environment-sensitive.
TEST_CASE("the mono chain holds the same working-set shape as the stereo chain",
          "[mastering][chain][.][slow]") {
  // The stereo path reduces each input measurement to a 1025-bin spectrum and
  // releases the track-length copy it measured; the mono path used to keep its
  // input Audio alive across every stage so that three track-length buffers were
  // resident at the end. Measured on a 60 s 96 kHz mono track (22 MB per
  // track-length copy): 132.6 MB resident growth before, 88.7 MB after — one
  // whole extra copy of the track, and two by the time the intermediate the
  // spectrum was taken from is counted.
  constexpr int kSampleRate = 96000;
  constexpr std::size_t kLength = 96000u * 60u;
  const std::size_t track_bytes = kLength * sizeof(float);

  std::vector<float> samples(kLength);
  for (std::size_t i = 0; i < kLength; ++i) {
    samples[i] = 0.2f * std::sin(0.01 * static_cast<double>(i));
  }

  const std::size_t before = resident_bytes();
  MasteringChain chain{MasteringChainConfig{}};
  const auto result = chain.process_mono(samples.data(), samples.size(), kSampleRate);
  const std::size_t after = resident_bytes();
  REQUIRE(result.samples.size() == kLength);

  // Both readings are unsigned, and resident size can legitimately fall here:
  // run after other cases, the allocator may return more pages than this chain
  // takes. Subtracting in size_t then wraps to ~2^64 and the case fails with an
  // astronomical "growth" that says nothing about the working set -- which is
  // why it passes standalone and fails inside a full run. A shrinking resident
  // size is trivially within the bound, so clamp at zero rather than measuring
  // the wrap.
  const double growth =
      after > before ? static_cast<double>(after - before) / static_cast<double>(track_bytes) : 0.0;
  CAPTURE(growth);
  // The returned samples are one of these copies and belong to the caller. Four
  // is what the released measurement copies leave behind; six is what holding
  // the input across the chain cost.
  CHECK(growth < 5.0);
}

namespace {

std::size_t resident_bytes() {
#if defined(__APPLE__)
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                &count) != KERN_SUCCESS) {
    return 0;
  }
  return static_cast<std::size_t>(info.resident_size);
#else
  std::FILE* statm = std::fopen("/proc/self/statm", "r");
  if (statm == nullptr) return 0;
  long total = 0;
  long resident = 0;
  const int read = std::fscanf(statm, "%ld %ld", &total, &resident);
  std::fclose(statm);
  if (read != 2) return 0;
  return static_cast<std::size_t>(resident) * static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
#endif
}

}  // namespace
#endif

}  // namespace sonare::mastering::api
