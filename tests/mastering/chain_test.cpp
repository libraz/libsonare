#include "mastering/api/chain.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
#include "mastering/api/insert_factory.h"
#include "mastering/api/internal_processor_runner.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/common/loudness_measure.h"
#include "mastering/match/reference_loudness.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/true_peak_limiter.h"
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

TEST_CASE("MasteringChain runs the repair stages in one order on both paths",
          "[mastering][chain][repair]") {
  // The mono and stereo paths spell their order independently, and the preset
  // goldens only ever call the mono entry point, so a stereo path left in a
  // different order changes nothing any other test reads: the same config would
  // quietly produce two different renders depending on the channel count.
  MasteringChainConfig config;
  config.repair.declip.enabled = true;
  config.repair.declick.enabled = true;
  config.repair.decrackle.enabled = true;
  config.repair.dehum.enabled = true;
  config.repair.denoise.enabled = true;
  config.repair.dereverb.enabled = true;

  // Widest damage first, so each stage sees material the one before it has
  // already made well-formed.
  const std::vector<std::string> kRepairOrder = {"repair.declip",    "repair.declick",
                                                 "repair.decrackle", "repair.dehum",
                                                 "repair.denoise",   "repair.dereverb"};

  constexpr int kSampleRate = 44100;
  std::vector<float> samples(kSampleRate / 4, 0.0f);
  const double step = 2.0 * constants::kPiD * 440.0 / kSampleRate;
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = 0.2f * static_cast<float>(std::sin(step * static_cast<double>(i)));
  }

  MasteringChain mono_chain(config);
  const auto mono = mono_chain.process_mono(samples.data(), samples.size(), kSampleRate);
  MasteringChain stereo_chain(config);
  const auto stereo =
      stereo_chain.process_stereo(samples.data(), samples.data(), samples.size(), kSampleRate);

  REQUIRE(mono.stages.size() >= kRepairOrder.size());
  REQUIRE(stereo.stages.size() >= kRepairOrder.size());
  const auto mono_repair = std::vector<std::string>(
      mono.stages.begin(), mono.stages.begin() + static_cast<std::ptrdiff_t>(kRepairOrder.size()));
  const auto stereo_repair = std::vector<std::string>(
      stereo.stages.begin(),
      stereo.stages.begin() + static_cast<std::ptrdiff_t>(kRepairOrder.size()));
  CHECK(mono_repair == kRepairOrder);
  CHECK(stereo_repair == kRepairOrder);
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

// Each enabled stage carries its own static validator, and the chain runs all of
// them at construction. Before that, a stage rejected its config where the
// processor was built - which for everything from eq.tilt onward is behind the
// six repair stages.
TEST_CASE("MasteringChain rejects every enabled stage's invalid config at construction",
          "[mastering][chain]") {
  auto with_stage = [](auto&& make_invalid) {
    MasteringChainConfig config;
    make_invalid(config);
    return config;
  };

  SECTION("dynamics.deesser") {
    CHECK_THROWS_AS(MasteringChain(with_stage([](MasteringChainConfig& c) {
                      c.dynamics.deesser.enabled = true;
                      c.dynamics.deesser.config.ratio = 0.5f;  // must be >= 1
                    })),
                    sonare::SonareException);
  }
  SECTION("dynamics.transientShaper") {
    CHECK_THROWS_AS(MasteringChain(with_stage([](MasteringChainConfig& c) {
                      c.dynamics.transient_shaper.enabled = true;
                      c.dynamics.transient_shaper.config.fast_release_ms = -1.0f;
                    })),
                    sonare::SonareException);
  }
  SECTION("dynamics.compressor") {
    CHECK_THROWS_AS(MasteringChain(with_stage([](MasteringChainConfig& c) {
                      c.dynamics.compressor.enabled = true;
                      c.dynamics.compressor.config.attack_ms = -1.0f;
                    })),
                    sonare::SonareException);
  }
  SECTION("dynamics.multibandComp band") {
    CHECK_THROWS_AS(MasteringChain(with_stage([](MasteringChainConfig& c) {
                      c.dynamics.multiband_comp.enabled = true;
                      c.dynamics.multiband_comp.config.bands[1].knee_db = -3.0f;
                    })),
                    sonare::SonareException);
  }
  SECTION("dynamics.multibandComp crossover") {
    CHECK_THROWS_AS(MasteringChain(with_stage([](MasteringChainConfig& c) {
                      c.dynamics.multiband_comp.enabled = true;
                      c.dynamics.multiband_comp.config.crossover.cutoffs_hz = {2000.0f, 200.0f};
                    })),
                    sonare::SonareException);
  }
  SECTION("saturation.tape") {
    CHECK_THROWS_AS(MasteringChain(with_stage([](MasteringChainConfig& c) {
                      c.saturation.tape.enabled = true;
                      c.saturation.tape.config.saturation = 2.0f;  // must be in [0, 1]
                    })),
                    sonare::SonareException);
  }
  SECTION("saturation.exciter") {
    CHECK_THROWS_AS(MasteringChain(with_stage([](MasteringChainConfig& c) {
                      c.saturation.exciter.enabled = true;
                      c.saturation.exciter.config.q = 0.0f;
                    })),
                    sonare::SonareException);
  }
  SECTION("spectral.airBand") {
    CHECK_THROWS_AS(MasteringChain(with_stage([](MasteringChainConfig& c) {
                      c.spectral.air_band.enabled = true;
                      c.spectral.air_band.config.dynamic_range_db = -1.0f;
                    })),
                    sonare::SonareException);
  }
  SECTION("stereo.imager") {
    CHECK_THROWS_AS(MasteringChain(with_stage([](MasteringChainConfig& c) {
                      c.stereo.imager.enabled = true;
                      c.stereo.imager.config.width = -1.0f;
                    })),
                    sonare::SonareException);
  }
  SECTION("stereo.monoMaker") {
    CHECK_THROWS_AS(MasteringChain(with_stage([](MasteringChainConfig& c) {
                      c.stereo.mono_maker.enabled = true;
                      c.stereo.mono_maker.config.frequency_hz = 0.0f;
                    })),
                    sonare::SonareException);
  }
  SECTION("maximizer.truePeakLimiter") {
    CHECK_THROWS_AS(MasteringChain(with_stage([](MasteringChainConfig& c) {
                      c.maximizer.true_peak_limiter.enabled = true;
                      c.maximizer.true_peak_limiter.config.lookahead_ms = -1.0f;
                    })),
                    sonare::SonareException);
  }
  SECTION("loudness") {
    CHECK_THROWS_AS(MasteringChain(with_stage([](MasteringChainConfig& c) {
                      c.loudness.enabled = true;
                      c.loudness.release_ms = -1.0f;
                    })),
                    sonare::SonareException);
  }

  // Positive control: every one of those stages enabled with its default config
  // builds and runs. A validator that rejected everything would pass the
  // rejections above on its own.
  MasteringChainConfig valid;
  valid.dynamics.deesser.enabled = true;
  valid.dynamics.transient_shaper.enabled = true;
  valid.dynamics.compressor.enabled = true;
  valid.dynamics.multiband_comp.enabled = true;
  valid.saturation.tape.enabled = true;
  valid.saturation.exciter.enabled = true;
  valid.spectral.air_band.enabled = true;
  valid.stereo.imager.enabled = true;
  valid.stereo.mono_maker.enabled = true;
  valid.maximizer.true_peak_limiter.enabled = true;
  valid.loudness.enabled = true;
  MasteringChain chain(valid);
  std::vector<float> samples(11025, 0.1f);
  CHECK_NOTHROW(chain.process_stereo(samples.data(), samples.data(), samples.size(), 44100));
}

// The stage that surfaced this: Compressor::validate_config used to run where
// chain.cpp builds the compressor, which is after repair has processed the whole
// track.
TEST_CASE("MasteringChain rejects an invalid compressor before the repair stages run",
          "[mastering][chain]") {
  constexpr int kSampleRate = 22050;
  MasteringChainConfig config;
  config.repair.declick.enabled = true;
  config.repair.dehum.enabled = true;
  config.dynamics.compressor.enabled = true;
  config.dynamics.compressor.config.release_ms = -5.0f;
  // Construction throws, so no chain object exists to process anything: the
  // repair stages below cannot have run.
  CHECK_THROWS_AS(MasteringChain(config), sonare::SonareException);

  // The same chain with a valid release runs, and the stage list shows the two
  // repair stages the rejection above skipped.
  config.dynamics.compressor.config.release_ms = 100.0f;
  MasteringChain chain(config);
  std::vector<std::string> stages;
  chain.set_progress_callback([&](float, const char* stage) { stages.emplace_back(stage); });
  std::vector<float> samples(kSampleRate / 4, 0.1f);
  CHECK_NOTHROW(chain.process_mono(samples.data(), samples.size(), kSampleRate));
  REQUIRE(stages.size() == 3);
  CHECK(stages[0] == "repair.declick");
  CHECK(stages[1] == "repair.dehum");
  CHECK(stages[2] == "dynamics.compressor");
}

// Rate-dependent constraints must stay out of validate_mastering_chain_config():
// it runs before a sample rate is known, so a cutoff legal at 48 kHz would be
// rejected there on no evidence.
TEST_CASE("MasteringChain defers the multiband crossover Nyquist check to the rate",
          "[mastering][chain]") {
  MasteringChainConfig config;
  config.dynamics.multiband_comp.enabled = true;
  config.dynamics.multiband_comp.config.crossover.cutoffs_hz = {200.0f, 18000.0f};
  // Construction knows no rate, so the 18 kHz cutoff is accepted here.
  MasteringChain chain(config);

  int stages_run = 0;
  chain.set_progress_callback([&](float, const char*) { ++stages_run; });
  std::vector<float> samples(11025, 0.1f);
  CHECK_THROWS_AS(chain.process_mono(samples.data(), samples.size(), 22050),
                  sonare::SonareException);
  CHECK(stages_run == 0);
  CHECK_NOTHROW(chain.process_mono(samples.data(), samples.size(), 48000));
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

TEST_CASE("a negative maxClickSamples is refused on the chain and named-processor paths",
          "[mastering][chain][validation]") {
  Param chain_params[] = {{"repair.declick.maxClickSamples", -1.0}};
  REQUIRE_THROWS_AS(parse_chain_config_params(chain_params, 1), SonareException);

  const std::vector<float> samples(2048, 0.1f);
  const std::vector<Param> named_params = {{"maxClickSamples", -1.0}};
  REQUIRE_THROWS_AS(
      apply_named_processor("repair.declick", samples.data(), samples.size(), 48000, named_params),
      SonareException);

  Param valid[] = {{"repair.declick.maxClickSamples", 7.0}};
  REQUIRE(parse_chain_config_params(valid, 1).repair.declick.config.max_click_samples == 7u);
}

TEST_CASE("an insert switch value other than 0 or 1 is refused by name at construction",
          "[mastering][chain][validation]") {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const double bad : {2.0, 0.5, -1.0, nan}) {
    INFO("value=" << bad);
    const std::vector<Param> insert_params = {{"driveOn", bad}};
    REQUIRE_THROWS_AS(make_insert_from_params("effects.filter.vowel", insert_params),
                      SonareException);
  }
  const std::vector<Param> drive_on = {{"driveOn", 1.0}};
  REQUIRE(make_insert_from_params("effects.filter.vowel", drive_on) != nullptr);
  const std::vector<Param> drive_off = {{"driveOn", 0.0}};
  REQUIRE(make_insert_from_params("effects.filter.vowel", drive_off) != nullptr);
}

TEST_CASE("a ceiling above full scale is refused by construction and by the streaming setter",
          "[mastering][chain][validation]") {
  const std::vector<float> samples(4096, 0.1f);
  REQUIRE_THROWS_AS(
      sonare::mastering::maximizer::loudness_optimize(
          Audio::from_buffer(samples.data(), samples.size(), 48000), {-14.0f, 1.0f, 4}),
      SonareException);
  REQUIRE_THROWS_AS(sonare::mastering::maximizer::TruePeakLimiter({1.0f, 1.0f, 20.0f, 4}),
                    SonareException);

  MasteringChainConfig config;
  config.maximizer.true_peak_limiter.enabled = true;
  config.maximizer.true_peak_limiter.config.ceiling_db = 1.0f;
  REQUIRE_THROWS_AS(MasteringChain(config), SonareException);

  config.maximizer.true_peak_limiter.config.ceiling_db = -1.0f;
  StreamingMasteringChain streaming(config);
  streaming.prepare(48000.0, 512, 1);
  REQUIRE_THROWS_AS(streaming.set_parameter("maximizer.truePeakLimiter.ceilingDb", 1.0),
                    SonareException);
  REQUIRE_THROWS_AS(streaming.set_parameter("maximizer.truePeakLimiter.ceilingDb",
                                            std::numeric_limits<double>::quiet_NaN()),
                    SonareException);
  REQUIRE_NOTHROW(streaming.set_parameter("maximizer.truePeakLimiter.ceilingDb", -3.0));
}

TEST_CASE("parse_chain_config_params rejects unknown keys", "[mastering][chain]") {
  Param params[] = {{"nonexistent.key", 0.0}};
  REQUIRE_THROWS_AS(parse_chain_config_params(params, 1), sonare::SonareException);
}

TEST_CASE("A saved denoise floor loads as the depth it stood for", "[mastering][chain]") {
  // The knob is a depth in dB; documents written while it was a linear floor
  // carry that spelling, so loading one has to convert rather than ignore it.
  Param legacy[] = {{"repair.denoise.gainFloor", 0.1}};
  REQUIRE_THAT(parse_chain_config_params(legacy, 1).repair.denoise.config.reduction_db,
               WithinAbs(20.0f, 1e-4f));

  // The short spelling normalizes onto the same key, so it converts too.
  Param short_legacy[] = {{"repair.gainFloor", 0.1}};
  REQUIRE_THAT(parse_chain_config_params(short_legacy, 1).repair.denoise.config.reduction_db,
               WithinAbs(20.0f, 1e-4f));

  // A floor above unity was refused before. As a depth it is negative, so the
  // conversion carries the old boundary rather than widening it, and the
  // refusal names the stage because the repair stages are checked before any
  // of them has touched the track.
  Param out_of_range[] = {{"repair.denoise.gainFloor", 1.5}};
  std::string refusal;
  try {
    parse_chain_config_params(out_of_range, 1);
    FAIL("a linear floor above unity must not survive the conversion");
  } catch (const SonareException& error) {
    refusal = error.what();
  }
  REQUIRE(refusal.find("repair.denoise") != std::string::npos);
  REQUIRE(refusal.find("reduction_db") != std::string::npos);

  // The current spelling is taken as written, with no conversion in the way.
  Param current[] = {{"repair.denoise.reductionDb", 12.0}};
  REQUIRE_THAT(parse_chain_config_params(current, 1).repair.denoise.config.reduction_db,
               WithinAbs(12.0f, 1e-6f));

  // The short spelling exists for the current name too, alongside the three
  // other repair shorthands, so the two spellings do not disagree on which
  // names are accepted.
  Param current_short[] = {{"repair.reductionDb", 12.0}};
  REQUIRE_THAT(parse_chain_config_params(current_short, 1).repair.denoise.config.reduction_db,
               WithinAbs(12.0f, 1e-6f));
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
  cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz = {1200.0f};
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

// mastering-012: the offline runner processes in
// internal::kOfflineProcessorBlockSize (~1.49 s at 44.1 kHz) blocks, and
// dynamics.compressor/deesser/multibandComp used to report
// last_gain_reduction_db() -- the FINAL block's instantaneous value -- while
// the limiter stages already used the whole-program minimum. A program that
// only compresses in its first block, with the compressor fully released by
// the time the last (mostly silent) block finishes, used to read back near
// 0 dB regardless of the real mid-song reduction.
TEST_CASE(
    "dynamics.compressor's reported gain reduction reflects the whole program, not the "
    "final block",
    "[mastering][chain]") {
  using sonare::mastering::api::internal::kOfflineProcessorBlockSize;
  constexpr int kSampleRate = 44100;
  // Three blocks: loud, then two full blocks of silence. last_gain_reduction_db()
  // reports the worst reduction WITHIN its own process() call, so one silent
  // block after the loud one still carries the release tail crossing that
  // block boundary; a second silent block is needed for the envelope to fully
  // settle before the "final block" the old code read from begins.
  const std::size_t total = static_cast<std::size_t>(kOfflineProcessorBlockSize) * 3;

  std::vector<float> samples(total, 0.0f);
  // Loud enough (threshold_db -18, ratio 2) to force real gain reduction, only
  // in the first block; the render ends two full blocks (~2.97 s) of silence
  // later, so the compressor's release (100 ms default) has long recovered by
  // the time the final block's own worst-reduction figure is taken.
  for (std::size_t i = 0; i < static_cast<std::size_t>(kOfflineProcessorBlockSize); ++i) {
    samples[i] =
        0.9f * std::sin(sonare::constants::kTwoPi * 300.0f * static_cast<float>(i) / kSampleRate);
  }

  MasteringChainConfig config;
  config.dynamics.compressor.enabled = true;
  const auto result =
      MasteringChain(config).process_mono(samples.data(), samples.size(), kSampleRate);

  float compressor_reduction = 0.0f;
  for (const auto& entry : result.stage_gain_reductions) {
    if (entry.stage == "dynamics.compressor") compressor_reduction = entry.gain_reduction_db;
  }
  CAPTURE(compressor_reduction);
  // The final block is silent, so the old last-block-only report would read
  // back near 0 dB; the whole-program minimum must show the real reduction.
  REQUIRE(compressor_reduction < -3.0f);
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
  // A burst followed by a decaying tail, and long enough for the late-reverb
  // estimate to have a frame to look back at: the spectral subtraction only
  // engages past `lateDelayMs` (50 ms, nine 256-sample hops at 48 kHz), so a
  // handful of samples produces a bit-for-bit passthrough and the attenuation
  // check below would be measuring nothing but STFT round-trip rounding.
  constexpr size_t kLength = 8192;
  constexpr int kSr = 48000;
  std::vector<float> left(kLength);
  for (size_t i = 0; i < kLength; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSr);
    const float excitation = i < 64 ? 0.8f : 0.0f;
    const float tail =
        0.35f * std::sin(sonare::constants::kTwoPi * 640.0f * t) * std::exp(-t / 0.12f);
    left[i] = excitation + tail;
  }
  std::vector<float> right(left.size());
  for (size_t i = 0; i < left.size(); ++i) {
    right[i] = 0.2f * left[i];
  }

  const auto result = apply_named_processor_stereo(
      "repair.dereverbClassical", left.data(), right.data(), left.size(), kSr,
      {{"threshold", 0.04}, {"attenuation", 0.5}, {"nFft", 1024.0}, {"hopLength", 256.0}});

  REQUIRE(result.left.size() == left.size());
  REQUIRE(result.right.size() == right.size());
  bool attenuated = false;
  for (size_t i = 0; i < left.size(); ++i) {
    if (std::abs(left[i]) > 1.0e-6f) {
      REQUIRE_THAT(result.right[i], WithinAbs(0.2f * result.left[i], 1.0e-6f));
    }
    // A margin, not `<`: the transfer has to actually take level out, rather
    // than land a rounding step below the input.
    if (std::abs(result.left[i]) < 0.99f * std::abs(left[i])) {
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

TEST_CASE("MasteringChain stereo denoise keeps the output peak bounded",
          "[mastering][chain][repair]") {
  // The chain no longer repairs a mono mix and scales the pair by the per-sample
  // out/in ratio, so the unbounded ratio at a mix zero crossing this once
  // guarded is gone. The bound stays because a masked resynthesis is an
  // overlap-add of modified frames and is not bounded by the input peak a priori.
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
  config.repair.denoise.config.reduction_db = 20.0f;
  MasteringChain chain(config);
  auto result = chain.process_stereo(left.data(), right.data(), left.size(), sample_rate);

  float output_peak = 0.0f;
  for (size_t i = 0; i < result.left.size(); ++i) {
    output_peak = std::max({output_peak, std::abs(result.left[i]), std::abs(result.right[i])});
  }
  REQUIRE(output_peak <= 4.0f * input_peak);
}

TEST_CASE("MasteringChain stereo denoise preserves a constant inter-channel ratio",
          "[mastering][chain][repair]") {
  // Named for what it checks. It does NOT witness that the mask was shared:
  // every gain this denoiser computes is a power ratio, so a scaled pair gets
  // the same mask from a per-channel pass too -- measured at 4.282e-09 against
  // this case's own 1.0e-05 tolerance. The linked mask is witnessed by
  // "builds one mask from both channels" below, which uses an unscaled pair.
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
  config.repair.denoise.config.reduction_db = 26.0f;
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

namespace {

/// @brief Reproducible uniform noise in [-1, 1), so each channel carries its own floor.
class ChainLcg {
 public:
  explicit ChainLcg(uint32_t seed) : state_(seed) {}
  float next() {
    state_ = state_ * 1664525u + 1013904223u;
    return static_cast<float>(state_ >> 8) / 8388608.0f - 1.0f;
  }

 private:
  uint32_t state_;
};

float worst_gap(const std::vector<float>& a, const std::vector<float>& b) {
  const size_t count = std::min(a.size(), b.size());
  float worst = 0.0f;
  for (size_t i = 0; i < count; ++i) worst = std::max(worst, std::abs(a[i] - b[i]));
  return worst;
}

}  // namespace

TEST_CASE("MasteringChain stereo denoise builds one mask from both channels",
          "[mastering][chain][repair]") {
  constexpr int sample_rate = 22050;
  const size_t length = static_cast<size_t>(sample_rate / 4);
  // Two different programmes with their own noise floors, deliberately not a
  // scaled pair. Every gain this denoiser computes is a power ratio, so scaling
  // a channel leaves its whole mask unchanged and a scaled pair cannot separate
  // one shared mask from two private ones.
  std::vector<float> left(length);
  std::vector<float> right(length);
  ChainLcg rng(2468u);
  for (size_t i = 0; i < length; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    left[i] = 0.30f * std::sin(sonare::constants::kTwoPi * 440.0f * t) + 0.02f * rng.next();
    right[i] = 0.10f * std::sin(sonare::constants::kTwoPi * 3100.0f * t) + 0.06f * rng.next();
  }
  float peak = 0.0f;
  for (size_t i = 0; i < length; ++i) {
    peak = std::max({peak, std::abs(left[i]), std::abs(right[i])});
  }

  MasteringChainConfig config;
  config.repair.denoise.enabled = true;
  config.repair.denoise.config.n_fft = 1024;
  config.repair.denoise.config.hop_length = 256;
  MasteringChain chain(config);

  const auto stereo = chain.process_stereo(left.data(), right.data(), length, sample_rate);
  // Per channel: the same chain, one channel at a time. This IS the
  // implementation a linked mask has to be distinguishable from.
  const auto mono_left = chain.process_mono(left.data(), length, sample_rate);
  const auto mono_right = chain.process_mono(right.data(), length, sample_rate);

  // An STFT frame sums n_fft products in float, twice over for analysis and
  // synthesis, so a difference this size is rounding rather than a decision.
  const float rounding = 2.0f * std::sqrt(1024.0f) * std::numeric_limits<float>::epsilon() * peak;
  const float left_gap = worst_gap(stereo.left, mono_left.samples);
  const float right_gap = worst_gap(stereo.right, mono_right.samples);
  INFO("left gap " << left_gap << " right gap " << right_gap << " rounding " << rounding);
  // The mask is built from the channel-summed power, so each channel's output
  // depends on the other channel. Per-channel processing makes both gaps zero.
  CHECK(left_gap > 100.0f * rounding);
  CHECK(right_gap > 100.0f * rounding);

  // Non-vacuity: the stage has to have done something at all. Without this the
  // gaps above could be produced by a stage that only runs in one of the modes.
  CHECK(worst_gap(stereo.left, left) > 100.0f * rounding);

  // The control that attributes the gaps to denoise. With the stage off, every
  // remaining stage in this config is disabled, so the stereo and mono paths
  // have to agree bit for bit -- if they do not, something else links the
  // channels and the gaps above are not evidence about denoise.
  MasteringChainConfig off = config;
  off.repair.denoise.enabled = false;
  MasteringChain bypass(off);
  const auto bypass_stereo = bypass.process_stereo(left.data(), right.data(), length, sample_rate);
  const auto bypass_mono = bypass.process_mono(left.data(), length, sample_rate);
  REQUIRE(bypass_stereo.left.size() == bypass_mono.samples.size());
  CHECK(std::memcmp(bypass_stereo.left.data(), bypass_mono.samples.data(),
                    bypass_stereo.left.size() * sizeof(float)) == 0);
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

  // Five repair stages need the whole signal, and so does loudness without a
  // precomputed static gain. repair.denoise is in this list only because the
  // sweep enables each stage at its defaults, and the default noise estimator is
  // the whole-signal one; the case below drives the other side of that.
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

TEST_CASE("StreamingMasteringChain takes repair.denoise only with a causal noise estimator",
          "[mastering][chain][streaming]") {
  // Both sides are driven. Asserting only the refusal would still pass if the
  // stage could never be built at all, and asserting only the acceptance would
  // still pass if the whole-signal estimator were being swapped out silently --
  // which is the outcome the refusal exists to prevent.
  const auto chain_with = [](double estimator) {
    const std::vector<Param> params{{"repair.denoise.enabled", 1.0},
                                    {"repair.denoise.noiseEstimator", estimator}};
    return parse_chain_config_params(params.data(), params.size());
  };

  // 0 is the quantile estimator, which ranks every frame of the whole signal.
  // Caught by hand rather than with REQUIRE_THROWS_AS because what the refusal
  // has to carry is the message, not only the type.
  try {
    StreamingMasteringChain rejected(chain_with(0.0));
    FAIL("the quantile estimator was accepted");
  } catch (const sonare::SonareException& error) {
    // The message has to name the field and the values that work, or a caller
    // learns only that the stage is unavailable.
    const std::string what = error.what();
    CAPTURE(what);
    CHECK(what.find("noiseEstimator") != std::string::npos);
    CHECK(what.find("spp") != std::string::npos);
  }

  // 1 mcra, 2 imcra, 3 spp: all three are recursive in time.
  for (const double estimator : {1.0, 2.0, 3.0}) {
    CAPTURE(estimator);
    StreamingMasteringChain chain(chain_with(estimator));
    chain.prepare(48000.0, 512, 2);
    CHECK(chain.stage_names() == std::vector<std::string>{"repair.denoise"});
    CHECK(chain.latency_samples() > 0);
  }
}

TEST_CASE("a streaming chain config defaults repair.denoise to a causal estimator",
          "[mastering][chain][streaming]") {
  const std::vector<Param> enabled_only{{"repair.denoise.enabled", 1.0}};

  // The offline parse keeps the quantile default, which a stream refuses by name.
  const MasteringChainConfig offline =
      parse_chain_config_params(enabled_only.data(), enabled_only.size());
  CHECK(offline.repair.denoise.config.noise_estimator ==
        sonare::mastering::repair::DenoiseNoiseEstimator::Quantile);
  REQUIRE_THROWS_AS(StreamingMasteringChain(offline), sonare::SonareException);

  // The streaming parse starts on the estimator the realtime denoise insert defaults to, so
  // enabling the stage alone prepares.
  const MasteringChainConfig streaming =
      parse_streaming_chain_config_params(enabled_only.data(), enabled_only.size());
  CHECK(streaming.repair.denoise.config.noise_estimator ==
        sonare::mastering::repair::DenoiseNoiseEstimator::Spp);
  StreamingMasteringChain chain(streaming);
  chain.prepare(48000.0, 512, 2);
  CHECK(chain.stage_names() == std::vector<std::string>{"repair.denoise"});

  // An explicit estimator still wins, and the whole-signal one is still refused by name.
  const std::vector<Param> explicit_quantile{{"repair.denoise.enabled", 1.0},
                                             {"repair.denoise.noiseEstimator", 0.0}};
  REQUIRE_THROWS_AS(StreamingMasteringChain(parse_streaming_chain_config_params(
                        explicit_quantile.data(), explicit_quantile.size())),
                    sonare::SonareException);
  const std::vector<Param> explicit_mcra{{"repair.denoise.enabled", 1.0},
                                         {"repair.denoise.noiseEstimator", 1.0}};
  CHECK(parse_streaming_chain_config_params(explicit_mcra.data(), explicit_mcra.size())
            .repair.denoise.config.noise_estimator ==
        sonare::mastering::repair::DenoiseNoiseEstimator::Mcra);
}

TEST_CASE("a chain config document takes an enum-valued key by its name", "[mastering][chain]") {
  const auto by_name = chain_config_from_json(
      R"({"version":1,"params":{"repair.denoise.enabled":true,)"
      R"("repair.denoise.noiseEstimator":"imcra","repair.denoise.mode":"mmseStsa"}})");
  const auto by_number =
      chain_config_from_json(R"({"version":1,"params":{"repair.denoise.enabled":true,)"
                             R"("repair.denoise.noiseEstimator":2,"repair.denoise.mode":1}})");
  CHECK(by_name.repair.denoise.config.noise_estimator ==
        sonare::mastering::repair::DenoiseNoiseEstimator::Imcra);
  CHECK(by_name.repair.denoise.config.mode == sonare::mastering::repair::DenoiseMode::MmseStsa);
  CHECK(chain_config_to_json(by_name) == chain_config_to_json(by_number));

  // An unknown name names the key and the valid ones; a string for a number key is a wrong type.
  try {
    (void)chain_config_from_json(
        R"({"version":1,"params":{"repair.denoise.noiseEstimator":"nope"}})");
    FAIL("an unknown enum name was accepted");
  } catch (const sonare::SonareException& error) {
    CHECK(std::string(error.what()).find("quantile, mcra, imcra, spp") != std::string::npos);
  }
  CHECK_THROWS_AS(chain_config_from_json(R"({"version":1,"params":{"loudness.targetLufs":"x"}})"),
                  sonare::SonareException);
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

// mastering-013: release_ms=0 is the documented "use the library default"
// sentinel (LoudnessStage::release_ms). The offline chain resolves it before
// building its post-gain true-peak limiter (loudness_release_ms(), chain.cpp);
// the streaming chain used to pass the raw sentinel straight through, so its
// limiter ran with an effectively instantaneous release instead of
// kDefaultLoudnessReleaseMs. A config built with the sentinel and one built
// with the default spelled out explicitly must therefore process identically.
TEST_CASE(
    "StreamingMasteringChain resolves the loudness release_ms=0 sentinel like the offline "
    "chain",
    "[mastering][chain][streaming]") {
  constexpr int kSampleRate = 48000;
  constexpr int kBlockSize = 256;
  constexpr int kSamples = 4096;

  auto burst_then_quiet = []() {
    // A loud transient burst forces real gain reduction; the long quiet tail
    // that follows is where a 0 ms and a 50 ms release audibly disagree.
    std::vector<float> signal(static_cast<size_t>(kSamples), 0.02f);
    for (int i = 0; i < 200; ++i) {
      signal[static_cast<size_t>(i)] = 0.95f;
    }
    return signal;
  };

  auto process = [&](float release_ms) {
    MasteringChainConfig config;
    config.loudness.enabled = true;
    config.loudness.target_lufs = -6.0f;
    config.loudness.ceiling_db = -0.3f;
    config.loudness.release_ms = release_ms;
    config.loudness.max_limiter_gain_reduction_db = 24.0f;

    StreamingMasteringChainOptions options;
    options.loudness_static_gain_db = 18.0f;  // drive the limiter hard

    StreamingMasteringChain chain(config, options);
    chain.prepare(kSampleRate, kBlockSize, 1);

    std::vector<float> signal = burst_then_quiet();
    for (int offset = 0; offset < kSamples; offset += kBlockSize) {
      float* channel = signal.data() + offset;
      float* channels[] = {channel};
      chain.process_block(channels, 1, kBlockSize);
    }
    return signal;
  };

  const auto sentinel = process(0.0f);
  const auto explicit_default = process(sonare::mastering::maximizer::kDefaultLoudnessReleaseMs);

  REQUIRE(sentinel.size() == explicit_default.size());
  CHECK(max_abs_difference(sentinel, explicit_default) == 0.0f);
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

TEST_CASE("StreamingMasteringChain preserves fractional-rate Nyquist validation",
          "[mastering][chain][streaming]") {
  constexpr double kSampleRate = 44100.5;
  constexpr float kNearNyquist = 22050.1f;

  SECTION("tilt pivot") {
    MasteringChainConfig config;
    config.eq.tilt.enabled = true;
    config.eq.tilt.tilt_db = 1.0f;
    config.eq.tilt.pivot_hz = kNearNyquist;
    StreamingMasteringChain chain(config);
    REQUIRE_NOTHROW(chain.prepare(kSampleRate, 128, 1));
    // The pivot is valid below 44100.5 / 2, while truncating the prepared rate
    // to 44100 would incorrectly reject this unrelated realtime-safe change.
    REQUIRE_NOTHROW(chain.set_parameter("eq.tilt.tiltDb", 2.0));
    REQUIRE(chain.config().eq.tilt.tilt_db == Catch::Approx(2.0f));
  }

  SECTION("multiband crossover") {
    MasteringChainConfig config;
    config.dynamics.multiband_comp.enabled = true;
    config.dynamics.multiband_comp.config.crossover.cutoffs_hz = {120.0f, kNearNyquist};
    StreamingMasteringChain chain(config);
    REQUIRE_NOTHROW(chain.prepare(kSampleRate, 128, 1));
    // The crossover is also valid only with the fractional Nyquist. Changing
    // a band threshold must retain that prepared configuration and validate it
    // against the exact rate.
    REQUIRE_NOTHROW(chain.set_parameter("dynamics.multibandComp.lowThresholdDb", -24.0));
    REQUIRE(chain.config().dynamics.multiband_comp.config.bands[0].threshold_db ==
            Catch::Approx(-24.0f));
  }
}

TEST_CASE("StreamingMasteringChain repeats an existing clamped processor value safely",
          "[mastering][chain][streaming]") {
  constexpr double kSampleRate = 48000.0;
  constexpr int kBlockSize = 64;
  MasteringChainConfig config;
  config.dynamics.deesser.enabled = true;
  // DeEsser accepts this construction-time value and clamps only its
  // effective filter cutoff at prepare/setter time. Repeating it must remain a
  // no-op so the running filter state is not rebuilt or rejected by the live
  // setter's stricter automation domain.
  config.dynamics.deesser.config.frequency_hz = 5.0f;

  StreamingMasteringChain repeated(config);
  StreamingMasteringChain control(config);
  repeated.prepare(kSampleRate, kBlockSize, 1);
  control.prepare(kSampleRate, kBlockSize, 1);
  std::vector<float> first(kBlockSize);
  for (int i = 0; i < kBlockSize; ++i) {
    first[static_cast<size_t>(i)] =
        0.7f * std::sin(static_cast<float>(i) * sonare::constants::kTwoPi * 8000.0f /
                        static_cast<float>(kSampleRate));
  }
  auto control_first = first;
  float* first_channels[] = {first.data()};
  float* control_first_channels[] = {control_first.data()};
  repeated.process_block(first_channels, 1, kBlockSize);
  control.process_block(control_first_channels, 1, kBlockSize);
  REQUIRE_NOTHROW(repeated.set_parameter("dynamics.deesser.frequencyHz", 5.0));

  std::vector<float> next(kBlockSize);
  for (int i = 0; i < kBlockSize; ++i) {
    next[static_cast<size_t>(i)] =
        0.7f * std::sin(static_cast<float>(kBlockSize + i) * sonare::constants::kTwoPi * 8000.0f /
                        static_cast<float>(kSampleRate));
  }
  auto control_next = next;
  float* next_channels[] = {next.data()};
  float* control_next_channels[] = {control_next.data()};
  repeated.process_block(next_channels, 1, kBlockSize);
  control.process_block(control_next_channels, 1, kBlockSize);
  REQUIRE(max_abs_difference(next, control_next) == 0.0f);
}

TEST_CASE("StreamingMasteringChain repeats values at the multiband kernel limit",
          "[mastering][chain][streaming]") {
  constexpr double kSampleRate = 48000.0;
  constexpr int kBlockSize = 64;
  MasteringChainConfig config;
  config.dynamics.multiband_comp.enabled = true;
  // The kernel limit is the chain's own, so it holds even in an IIR mode that ignores the kernel.
  config.dynamics.multiband_comp.config.crossover.fir_kernel_size = kMaxFirKernelSize + 1;
  REQUIRE_THROWS_AS(StreamingMasteringChain(config), SonareException);
  config.dynamics.multiband_comp.config.crossover.fir_kernel_size = kMaxFirKernelSize;

  StreamingMasteringChain repeated(config);
  StreamingMasteringChain control(config);
  repeated.prepare(kSampleRate, kBlockSize, 1);
  control.prepare(kSampleRate, kBlockSize, 1);
  std::vector<float> first(kBlockSize, 0.2f);
  auto control_first = first;
  float* first_channels[] = {first.data()};
  float* control_first_channels[] = {control_first.data()};
  repeated.process_block(first_channels, 1, kBlockSize);
  control.process_block(control_first_channels, 1, kBlockSize);
  REQUIRE_NOTHROW(repeated.set_parameter("dynamics.multibandComp.lowThresholdDb", -18.0));

  std::vector<float> next(kBlockSize, -0.15f);
  auto control_next = next;
  float* next_channels[] = {next.data()};
  float* control_next_channels[] = {control_next.data()};
  repeated.process_block(next_channels, 1, kBlockSize);
  control.process_block(control_next_channels, 1, kBlockSize);
  REQUIRE(max_abs_difference(next, control_next) == 0.0f);
}

TEST_CASE("StreamingMasteringChain exposes aliases for a two-band multiband config",
          "[mastering][chain][streaming]") {
  constexpr double kSampleRate = 48000.0;
  constexpr int kBlockSize = 64;
  MasteringChainConfig config;
  config.dynamics.multiband_comp.enabled = true;
  config.dynamics.multiband_comp.config.crossover.cutoffs_hz = {1200.0f};
  config.dynamics.multiband_comp.config.bands.resize(2);

  StreamingMasteringChain changed(config);
  StreamingMasteringChain continuity(config);
  StreamingMasteringChain control(config);
  changed.prepare(kSampleRate, kBlockSize, 1);
  continuity.prepare(kSampleRate, kBlockSize, 1);
  control.prepare(kSampleRate, kBlockSize, 1);
  std::vector<float> first(kBlockSize, 0.35f);
  auto continuity_first = first;
  auto control_first = first;
  float* first_channels[] = {first.data()};
  float* continuity_first_channels[] = {continuity_first.data()};
  float* control_first_channels[] = {control_first.data()};
  changed.process_block(first_channels, 1, kBlockSize);
  continuity.process_block(continuity_first_channels, 1, kBlockSize);
  control.process_block(control_first_channels, 1, kBlockSize);

  REQUIRE_NOTHROW(changed.set_parameter("dynamics.multibandComp.lowThresholdDb", -30.0));
  REQUIRE_NOTHROW(continuity.set_parameter("dynamics.multibandComp.lowThresholdDb", -18.0));
  REQUIRE_NOTHROW(continuity.set_parameter("dynamics.multibandComp.midThresholdDb", -18.0));
  REQUIRE(changed.config().dynamics.multiband_comp.config.bands[0].threshold_db ==
          Catch::Approx(-30.0f));

  std::vector<float> next(kBlockSize, -0.25f);
  auto continuity_next = next;
  auto control_next = next;
  float* next_channels[] = {next.data()};
  float* continuity_next_channels[] = {continuity_next.data()};
  float* control_next_channels[] = {control_next.data()};
  changed.process_block(next_channels, 1, kBlockSize);
  continuity.process_block(continuity_next_channels, 1, kBlockSize);
  control.process_block(control_next_channels, 1, kBlockSize);
  REQUIRE(max_abs_difference(continuity_next, control_next) == 0.0f);
  REQUIRE(max_abs_difference(next, control_next) > 1.0e-6f);
}

TEST_CASE("chain parameter lookup preserves non-default boolean fields",
          "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.dynamics.compressor.enabled = true;
  config.dynamics.compressor.config.auto_makeup = true;
  config.dynamics.compressor.config.sidechain_hpf_enabled = true;
  REQUIRE(chain_config_parameter_value(config, "dynamics.compressor.autoMakeup") ==
          std::optional<double>{1.0});
  REQUIRE(chain_config_parameter_value(config, "dynamics.compressor.sidechainHpfEnabled") ==
          std::optional<double>{1.0});
}

TEST_CASE("StreamingMasteringChain preserves typed double parameter semantics",
          "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.dynamics.compressor.enabled = true;
  config.dynamics.compressor.config.detector = dynamics::DetectorMode::Rms;
  StreamingMasteringChain chain(config);
  StreamingMasteringChain control(config);
  chain.prepare(48000.0, 64, 1);
  control.prepare(48000.0, 64, 1);

  std::vector<float> warmup(64);
  for (int i = 0; i < 64; ++i) {
    warmup[static_cast<size_t>(i)] =
        0.75f * std::sin(static_cast<float>(i) * sonare::constants::kTwoPi * 440.0f / 48000.0f);
  }
  auto control_warmup = warmup;
  float* warmup_channels[] = {warmup.data()};
  float* control_warmup_channels[] = {control_warmup.data()};
  chain.process_block(warmup_channels, 1, 64);
  control.process_block(control_warmup_channels, 1, 64);

  // Enum validation must see the original double. Narrowing first would turn
  // this fractional selector into the legal value 1 and silently accept it.
  CHECK_THROWS_AS(chain.set_parameter("dynamics.compressor.detector", 1.00000001), SonareException);
  REQUIRE(chain.config().dynamics.compressor.config.detector ==
          config.dynamics.compressor.config.detector);

  std::vector<float> continuation(64, 0.2f);
  auto control_continuation = continuation;
  float* continuation_channels[] = {continuation.data()};
  float* control_continuation_channels[] = {control_continuation.data()};
  chain.process_block(continuation_channels, 1, 64);
  control.process_block(control_continuation_channels, 1, 64);
  REQUIRE(max_abs_difference(continuation, control_continuation) == 0.0f);

  // A non-zero double below float's range still means true for a boolean
  // chain field. Passing the narrowed float zero would incorrectly no-op.
  const double tiny_nonzero = std::ldexp(1.0, -150);
  REQUIRE_NOTHROW(chain.set_parameter("dynamics.compressor.autoMakeup", tiny_nonzero));
  REQUIRE(chain.config().dynamics.compressor.config.auto_makeup);
}

TEST_CASE("StreamingMasteringChain treats signed zero release as unchanged",
          "[mastering][chain][streaming]") {
  constexpr double kSampleRate = 48000.0;
  constexpr int kBlockSize = 127;
  MasteringChainConfig config;
  config.maximizer.true_peak_limiter.enabled = true;
  config.maximizer.true_peak_limiter.config.ceiling_db = -6.0f;
  config.maximizer.true_peak_limiter.config.release_ms = 0.0f;

  StreamingMasteringChain repeated(config);
  StreamingMasteringChain control(config);
  repeated.prepare(kSampleRate, kBlockSize, 1);
  control.prepare(kSampleRate, kBlockSize, 1);
  auto make_block = [](int first_sample, float amplitude) {
    std::vector<float> block(kBlockSize);
    for (int i = 0; i < kBlockSize; ++i) {
      const float phase = static_cast<float>(sonare::constants::kTwoPiD) * 440.0f *
                          static_cast<float>(first_sample + i) / static_cast<float>(kSampleRate);
      block[static_cast<size_t>(i)] = amplitude * std::sin(phase);
    }
    block[31] = 0.98f;
    return block;
  };
  auto prime = make_block(0, 0.9f);
  auto control_prime = prime;
  float* prime_channels[] = {prime.data()};
  float* control_prime_channels[] = {control_prime.data()};
  repeated.process_block(prime_channels, 1, kBlockSize);
  control.process_block(control_prime_channels, 1, kBlockSize);
  REQUIRE_NOTHROW(repeated.set_parameter("maximizer.truePeakLimiter.releaseMs", -0.0));
  REQUIRE_FALSE(std::signbit(repeated.config().maximizer.true_peak_limiter.config.release_ms));

  std::vector<float> repeated_output;
  std::vector<float> control_output;
  for (int block_index = 1; block_index <= 3; ++block_index) {
    auto next = make_block(block_index * kBlockSize, 0.35f);
    auto control_next = next;
    float* next_channels[] = {next.data()};
    float* control_next_channels[] = {control_next.data()};
    repeated.process_block(next_channels, 1, kBlockSize);
    control.process_block(control_next_channels, 1, kBlockSize);
    repeated_output.insert(repeated_output.end(), next.begin(), next.end());
    control_output.insert(control_output.end(), control_next.begin(), control_next.end());
  }
  REQUIRE(max_abs_difference(repeated_output, control_output) == 0.0f);

  const auto drain = [](StreamingMasteringChain& chain) {
    std::vector<float> output;
    for (int call = 0; call < 4096; ++call) {
      std::vector<float> block(kBlockSize, -7.0f);
      float* channels[] = {block.data()};
      const int written = chain.flush(channels, 1, kBlockSize);
      if (written == 0) return output;
      output.insert(output.end(), block.begin(), block.begin() + written);
    }
    throw std::runtime_error("StreamingMasteringChain flush did not terminate");
  };
  const auto repeated_tail = drain(repeated);
  const auto control_tail = drain(control);
  REQUIRE(repeated_tail.size() == control_tail.size());
  REQUIRE(max_abs_difference(repeated_tail, control_tail) == 0.0f);
}

TEST_CASE("StreamingMasteringChain changes realtime parameters without rebuilding DSP",
          "[mastering][chain][streaming]") {
  constexpr double kSampleRate = 48000.0;
  constexpr int kBlockSize = 128;

  auto sine_block = [](int first_sample, float frequency, float amplitude) {
    std::vector<float> block(kBlockSize);
    for (int i = 0; i < kBlockSize; ++i) {
      block[static_cast<size_t>(i)] =
          amplitude *
          std::sin(static_cast<float>(sonare::constants::kTwoPiD) * frequency *
                   static_cast<float>(first_sample + i) / static_cast<float>(kSampleRate));
    }
    return block;
  };
  const auto process_mono = [](StreamingMasteringChain& chain, std::vector<float>& block) {
    float* channels[] = {block.data()};
    chain.process_block(channels, 1, static_cast<int>(block.size()));
  };
  const auto process_stereo = [](StreamingMasteringChain& chain, std::vector<float>& left,
                                 std::vector<float>& right) {
    float* channels[] = {left.data(), right.data()};
    chain.process_block(channels, 2, static_cast<int>(left.size()));
  };
  const auto drain_mono = [](StreamingMasteringChain& chain) {
    std::vector<float> output;
    for (int call = 0; call < 4096; ++call) {
      std::vector<float> block(kBlockSize, -7.0f);
      float* channels[] = {block.data()};
      const int written = chain.flush(channels, 1, kBlockSize);
      if (written == 0) return output;
      output.insert(output.end(), block.begin(), block.begin() + written);
    }
    throw std::runtime_error("StreamingMasteringChain flush did not terminate");
  };

  SECTION("mono true-peak ceiling retains the running lookahead") {
    MasteringChainConfig config;
    config.maximizer.true_peak_limiter.enabled = true;
    config.maximizer.true_peak_limiter.config.ceiling_db = -1.0f;

    StreamingMasteringChain changed(config);
    StreamingMasteringChain continuity(config);
    StreamingMasteringChain control(config);
    changed.prepare(kSampleRate, kBlockSize, 1);
    continuity.prepare(kSampleRate, kBlockSize, 1);
    control.prepare(kSampleRate, kBlockSize, 1);

    auto first = sine_block(0, 440.0f, 0.98f);
    auto first_continuity = first;
    auto first_control = first;
    process_mono(changed, first);
    process_mono(continuity, first_continuity);
    process_mono(control, first_control);

    REQUIRE_NOTHROW(changed.set_parameter("maximizer.truePeakLimiter.ceilingDb", -12.0));
    // Setting the current value is the continuity probe: a setter that
    // re-prepared or reset the limiter would diverge from the untouched chain.
    REQUIRE_NOTHROW(continuity.set_parameter("maximizer.truePeakLimiter.ceilingDb", -1.0));
    REQUIRE(changed.config().maximizer.true_peak_limiter.config.ceiling_db ==
            Catch::Approx(-12.0f));
    REQUIRE(changed.latency_samples() == control.latency_samples());

    std::vector<float> changed_output;
    std::vector<float> continuity_output;
    std::vector<float> control_output;
    for (int block_index = 1; block_index <= 3; ++block_index) {
      auto next = sine_block(block_index * kBlockSize, 440.0f, 0.98f);
      auto next_continuity = next;
      auto next_control = next;
      process_mono(changed, next);
      process_mono(continuity, next_continuity);
      process_mono(control, next_control);
      changed_output.insert(changed_output.end(), next.begin(), next.end());
      continuity_output.insert(continuity_output.end(), next_continuity.begin(),
                               next_continuity.end());
      control_output.insert(control_output.end(), next_control.begin(), next_control.end());
    }
    CHECK(max_abs_difference(changed_output, control_output) > 1.0e-4f);
    REQUIRE(max_abs_difference(continuity_output, control_output) == 0.0f);

    const auto continuity_tail = drain_mono(continuity);
    const auto control_tail = drain_mono(control);
    REQUIRE(continuity_tail.size() == control_tail.size());
    REQUIRE_FALSE(continuity_tail.empty());
    REQUIRE(max_abs_difference(continuity_tail, control_tail) < 1.0e-6f);
  }

  SECTION("repeating limiter release preserves adaptive history") {
    constexpr int kReleaseBlockSize = 127;
    MasteringChainConfig config;
    config.maximizer.true_peak_limiter.enabled = true;
    config.maximizer.true_peak_limiter.config.ceiling_db = -6.0f;
    config.maximizer.true_peak_limiter.config.release_ms = 120.0f;

    StreamingMasteringChain repeated(config);
    StreamingMasteringChain control(config);
    repeated.prepare(kSampleRate, kReleaseBlockSize, 1);
    control.prepare(kSampleRate, kReleaseBlockSize, 1);

    auto make_block = [](int first_sample, float amplitude) {
      std::vector<float> block(kReleaseBlockSize);
      for (int i = 0; i < kReleaseBlockSize; ++i) {
        const float phase = static_cast<float>(sonare::constants::kTwoPiD) * 440.0f *
                            static_cast<float>(first_sample + i) / static_cast<float>(kSampleRate);
        block[static_cast<size_t>(i)] = amplitude * std::sin(phase);
      }
      // A sharp peak makes the adaptive release path observable after the
      // intentionally off-phase (127 * 4 is not divisible by its interval).
      block[31] = 0.98f;
      return block;
    };
    auto prime = make_block(0, 0.9f);
    auto control_prime = prime;
    process_mono(repeated, prime);
    process_mono(control, control_prime);
    REQUIRE_NOTHROW(repeated.set_parameter("maximizer.truePeakLimiter.releaseMs", 120.0));

    std::vector<float> repeated_output;
    std::vector<float> control_output;
    for (int block_index = 1; block_index <= 3; ++block_index) {
      auto next = make_block(block_index * kReleaseBlockSize, 0.35f);
      auto control_next = next;
      process_mono(repeated, next);
      process_mono(control, control_next);
      repeated_output.insert(repeated_output.end(), next.begin(), next.end());
      control_output.insert(control_output.end(), control_next.begin(), control_next.end());
    }
    REQUIRE(max_abs_difference(repeated_output, control_output) == 0.0f);

    const auto drain = [&](StreamingMasteringChain& chain) {
      std::vector<float> output;
      for (int call = 0; call < 4096; ++call) {
        std::vector<float> block(kReleaseBlockSize, -7.0f);
        float* channels[] = {block.data()};
        const int written = chain.flush(channels, 1, kReleaseBlockSize);
        if (written == 0) return output;
        output.insert(output.end(), block.begin(), block.begin() + written);
      }
      throw std::runtime_error("StreamingMasteringChain flush did not terminate");
    };
    const auto repeated_tail = drain(repeated);
    const auto control_tail = drain(control);
    REQUIRE(repeated_tail.size() == control_tail.size());
    REQUIRE_FALSE(repeated_tail.empty());
    REQUIRE(max_abs_difference(repeated_tail, control_tail) == 0.0f);
  }

  SECTION("stereo true-peak ceiling changes both channels") {
    MasteringChainConfig config;
    config.maximizer.true_peak_limiter.enabled = true;
    config.maximizer.true_peak_limiter.config.ceiling_db = -1.0f;

    StreamingMasteringChain changed(config);
    StreamingMasteringChain control(config);
    changed.prepare(kSampleRate, kBlockSize, 2);
    control.prepare(kSampleRate, kBlockSize, 2);

    auto first_left = sine_block(0, 700.0f, 0.9f);
    auto first_right = sine_block(0, 1100.0f, 0.85f);
    auto first_control_left = first_left;
    auto first_control_right = first_right;
    process_stereo(changed, first_left, first_right);
    process_stereo(control, first_control_left, first_control_right);
    REQUIRE_NOTHROW(changed.set_parameter("maximizer.truePeakLimiter.ceilingDb", -12.0));

    std::vector<float> changed_left;
    std::vector<float> changed_right;
    std::vector<float> control_left;
    std::vector<float> control_right;
    for (int block_index = 1; block_index <= 3; ++block_index) {
      auto left = sine_block(block_index * kBlockSize, 700.0f, 0.9f);
      auto right = sine_block(block_index * kBlockSize, 1100.0f, 0.85f);
      auto control_block_left = left;
      auto control_block_right = right;
      process_stereo(changed, left, right);
      process_stereo(control, control_block_left, control_block_right);
      changed_left.insert(changed_left.end(), left.begin(), left.end());
      changed_right.insert(changed_right.end(), right.begin(), right.end());
      control_left.insert(control_left.end(), control_block_left.begin(), control_block_left.end());
      control_right.insert(control_right.end(), control_block_right.begin(),
                           control_block_right.end());
    }
    REQUIRE(max_abs_difference(changed_left, control_left) > 1.0e-4f);
    REQUIRE(max_abs_difference(changed_right, control_right) > 1.0e-4f);
  }

  SECTION("stereo compressor threshold changes both channels") {
    MasteringChainConfig config;
    config.dynamics.compressor.enabled = true;
    config.dynamics.compressor.config.threshold_db = -12.0f;
    config.dynamics.compressor.config.ratio = 8.0f;
    config.dynamics.compressor.config.release_ms = 300.0f;

    StreamingMasteringChain changed(config);
    StreamingMasteringChain continuity(config);
    StreamingMasteringChain control(config);
    changed.prepare(kSampleRate, kBlockSize, 2);
    continuity.prepare(kSampleRate, kBlockSize, 2);
    control.prepare(kSampleRate, kBlockSize, 2);
    auto first_left = sine_block(0, 700.0f, 0.8f);
    auto first_right = sine_block(0, 1100.0f, 0.75f);
    auto first_continuity_left = first_left;
    auto first_continuity_right = first_right;
    auto first_control_left = first_left;
    auto first_control_right = first_right;
    process_stereo(changed, first_left, first_right);
    process_stereo(continuity, first_continuity_left, first_continuity_right);
    process_stereo(control, first_control_left, first_control_right);
    REQUIRE_NOTHROW(changed.set_parameter("dynamics.compressor.thresholdDb", -36.0));
    REQUIRE_NOTHROW(continuity.set_parameter("dynamics.compressor.thresholdDb", -12.0));

    std::vector<float> changed_left;
    std::vector<float> changed_right;
    std::vector<float> continuity_left;
    std::vector<float> continuity_right;
    std::vector<float> control_left;
    std::vector<float> control_right;
    for (int block_index = 1; block_index <= 3; ++block_index) {
      auto left = sine_block(block_index * kBlockSize, 700.0f, 0.8f);
      auto right = sine_block(block_index * kBlockSize, 1100.0f, 0.75f);
      auto continuity_block_left = left;
      auto continuity_block_right = right;
      auto control_block_left = left;
      auto control_block_right = right;
      process_stereo(changed, left, right);
      process_stereo(continuity, continuity_block_left, continuity_block_right);
      process_stereo(control, control_block_left, control_block_right);
      changed_left.insert(changed_left.end(), left.begin(), left.end());
      changed_right.insert(changed_right.end(), right.begin(), right.end());
      continuity_left.insert(continuity_left.end(), continuity_block_left.begin(),
                             continuity_block_left.end());
      continuity_right.insert(continuity_right.end(), continuity_block_right.begin(),
                              continuity_block_right.end());
      control_left.insert(control_left.end(), control_block_left.begin(), control_block_left.end());
      control_right.insert(control_right.end(), control_block_right.begin(),
                           control_block_right.end());
    }
    REQUIRE(max_abs_difference(changed_left, control_left) > 1.0e-5f);
    REQUIRE(max_abs_difference(changed_right, control_right) > 1.0e-5f);
    REQUIRE(changed.config().dynamics.compressor.config.threshold_db == Catch::Approx(-36.0f));
    REQUIRE(max_abs_difference(continuity_left, control_left) == 0.0f);
    REQUIRE(max_abs_difference(continuity_right, control_right) == 0.0f);
  }

  SECTION("stereo tilt keeps both channel histories") {
    MasteringChainConfig config;
    config.eq.tilt.enabled = true;
    config.eq.tilt.tilt_db = 1.0f;
    config.eq.tilt.pivot_hz = 1000.0f;

    StreamingMasteringChain changed(config);
    StreamingMasteringChain continuity(config);
    StreamingMasteringChain control(config);
    changed.prepare(kSampleRate, kBlockSize, 2);
    continuity.prepare(kSampleRate, kBlockSize, 2);
    control.prepare(kSampleRate, kBlockSize, 2);

    auto left = sine_block(0, 8000.0f, 0.4f);
    auto right = sine_block(0, 500.0f, 0.4f);
    auto continuity_left = left;
    auto continuity_right = right;
    auto control_left = left;
    auto control_right = right;
    process_stereo(changed, left, right);
    process_stereo(continuity, continuity_left, continuity_right);
    process_stereo(control, control_left, control_right);

    REQUIRE_NOTHROW(changed.set_parameter("eq.tilt.tiltDb", 8.0));
    REQUIRE_NOTHROW(continuity.set_parameter("eq.tilt.tiltDb", 1.0));

    auto next_left = sine_block(kBlockSize, 8000.0f, 0.4f);
    auto next_right = sine_block(kBlockSize, 500.0f, 0.4f);
    auto next_continuity_left = next_left;
    auto next_continuity_right = next_right;
    auto next_control_left = next_left;
    auto next_control_right = next_right;
    process_stereo(changed, next_left, next_right);
    process_stereo(continuity, next_continuity_left, next_continuity_right);
    process_stereo(control, next_control_left, next_control_right);

    REQUIRE(max_abs_difference(next_continuity_left, next_control_left) == 0.0f);
    REQUIRE(max_abs_difference(next_continuity_right, next_control_right) == 0.0f);
    REQUIRE(max_abs_difference(next_left, next_control_left) > 1.0e-4f);
    REQUIRE(max_abs_difference(next_right, next_control_right) > 1.0e-4f);
    CHECK(changed.config().eq.tilt.tilt_db == Catch::Approx(8.0f));
  }

  SECTION("reprepare uses the updated configuration") {
    MasteringChainConfig config;
    config.eq.tilt.enabled = true;
    config.eq.tilt.tilt_db = 1.0f;
    config.eq.tilt.pivot_hz = 1000.0f;
    StreamingMasteringChain changed(config);
    changed.prepare(kSampleRate, kBlockSize, 2);
    REQUIRE_NOTHROW(changed.set_parameter("eq.tilt.tiltDb", 8.0));
    config.eq.tilt.tilt_db = 8.0f;
    StreamingMasteringChain expected(config);
    expected.prepare(kSampleRate, kBlockSize, 2);
    changed.prepare(kSampleRate, kBlockSize, 2);
    auto actual_left = sine_block(0, 8000.0f, 0.4f);
    auto actual_right = sine_block(0, 500.0f, 0.4f);
    auto expected_left = actual_left;
    auto expected_right = actual_right;
    process_stereo(changed, actual_left, actual_right);
    process_stereo(expected, expected_left, expected_right);
    REQUIRE(max_abs_difference(actual_left, expected_left) < 1.0e-6f);
    REQUIRE(max_abs_difference(actual_right, expected_right) < 1.0e-6f);
    REQUIRE(changed.config().eq.tilt.tilt_db == Catch::Approx(8.0f));
  }

  SECTION("multiband aliases target the realized band descriptor") {
    MasteringChainConfig config;
    config.dynamics.multiband_comp.enabled = true;
    StreamingMasteringChain chain(config);
    chain.prepare(kSampleRate, kBlockSize, 1);
    const std::vector<std::pair<const char*, double>> values = {
        {"dynamics.multibandComp.lowThresholdDb", -30.0},
        {"dynamics.multibandComp.lowRatio", 3.0},
        {"dynamics.multibandComp.lowAttackMs", 5.0},
        {"dynamics.multibandComp.lowReleaseMs", 80.0},
        {"dynamics.multibandComp.midThresholdDb", -27.0},
        {"dynamics.multibandComp.midRatio", 4.0},
        {"dynamics.multibandComp.midAttackMs", 7.0},
        {"dynamics.multibandComp.midReleaseMs", 90.0},
        {"dynamics.multibandComp.highThresholdDb", -24.0},
        {"dynamics.multibandComp.highRatio", 5.0},
        {"dynamics.multibandComp.highAttackMs", 9.0},
        {"dynamics.multibandComp.highReleaseMs", 100.0},
    };
    for (const auto& [key, value] : values) {
      REQUIRE_NOTHROW(chain.set_parameter(key, value));
    }
    const auto& bands = chain.config().dynamics.multiband_comp.config.bands;
    REQUIRE(bands[0].threshold_db == Catch::Approx(-30.0f));
    REQUIRE(bands[0].ratio == Catch::Approx(3.0f));
    REQUIRE(bands[0].attack_ms == Catch::Approx(5.0f));
    REQUIRE(bands[0].release_ms == Catch::Approx(80.0f));
    REQUIRE(bands[1].threshold_db == Catch::Approx(-27.0f));
    REQUIRE(bands[1].ratio == Catch::Approx(4.0f));
    REQUIRE(bands[1].attack_ms == Catch::Approx(7.0f));
    REQUIRE(bands[1].release_ms == Catch::Approx(90.0f));
    REQUIRE(bands[2].threshold_db == Catch::Approx(-24.0f));
    REQUIRE(bands[2].ratio == Catch::Approx(5.0f));
    REQUIRE(bands[2].attack_ms == Catch::Approx(9.0f));
    REQUIRE(bands[2].release_ms == Catch::Approx(100.0f));
    REQUIRE_THROWS_AS(chain.set_parameter("dynamics.multibandComp.lowCutoffHz", 250.0),
                      SonareException);
    REQUIRE_THROWS_AS(chain.set_parameter("dynamics.multibandComp.lowMakeupGainDb", 2.0),
                      SonareException);
    REQUIRE_THROWS_AS(chain.set_parameter("dynamics.multibandComp.band3ThresholdDb", -20.0),
                      SonareException);
  }

  SECTION("loudness aliases keep the static gain and resolve release sentinel") {
    MasteringChainConfig config;
    config.loudness.enabled = true;
    StreamingMasteringChainOptions options;
    options.loudness_static_gain_db = 0.0f;
    StreamingMasteringChain chain(config, options);
    chain.prepare(kSampleRate, kBlockSize, 1);
    REQUIRE_NOTHROW(chain.set_parameter("loudness.ceilingDb", -2.0));
    REQUIRE_NOTHROW(chain.set_parameter("loudness.releaseMs", 0.0));
    REQUIRE(chain.config().loudness.ceiling_db == Catch::Approx(-2.0f));
    REQUIRE(chain.config().loudness.release_ms == Catch::Approx(0.0f));
  }
}

TEST_CASE("StreamingMasteringChain rejects unsafe parameters without touching history",
          "[mastering][chain][streaming]") {
  constexpr double kSampleRate = 48000.0;
  constexpr int kBlockSize = 64;
  MasteringChainConfig config;
  config.maximizer.true_peak_limiter.enabled = true;

  StreamingMasteringChain chain(config);
  StreamingMasteringChain control(config);
  chain.prepare(kSampleRate, kBlockSize, 1);
  control.prepare(kSampleRate, kBlockSize, 1);
  std::vector<float> prime(kBlockSize, 0.75f);
  auto control_prime = prime;
  float* prime_channels[] = {prime.data()};
  float* control_prime_channels[] = {control_prime.data()};
  chain.process_block(prime_channels, 1, kBlockSize);
  control.process_block(control_prime_channels, 1, kBlockSize);

  std::vector<float> first_tail(8, -1.0f);
  std::vector<float> control_first_tail(8, -1.0f);
  float* first_tail_channels[] = {first_tail.data()};
  float* control_first_tail_channels[] = {control_first_tail.data()};
  const int first_written = chain.flush(first_tail_channels, 1, 8);
  const int control_first_written = control.flush(control_first_tail_channels, 1, 8);
  REQUIRE(first_written == control_first_written);
  REQUIRE(first_written == 8);
  for (int i = 0; i < first_written; ++i) {
    CHECK(first_tail[static_cast<size_t>(i)] ==
          Catch::Approx(control_first_tail[static_cast<size_t>(i)]).margin(1.0e-6f));
  }

  REQUIRE_THROWS_AS(chain.set_parameter("maximizer.truePeakLimiter.lookaheadMs", 4.0),
                    SonareException);
  REQUIRE_THROWS_AS(chain.set_parameter("maximizer.truePeakLimiter.ceilingDb",
                                        std::numeric_limits<double>::quiet_NaN()),
                    SonareException);
  REQUIRE_THROWS_AS(chain.set_parameter("unknown.stage.parameter", -1.0), SonareException);
  REQUIRE_THROWS_AS(chain.set_parameter("maximizer.truePeakLimiter.ceilingDb",
                                        std::numeric_limits<double>::max()),
                    SonareException);

  MasteringChainConfig clamp_config;
  clamp_config.saturation.tape.enabled = true;
  clamp_config.saturation.exciter.enabled = true;
  StreamingMasteringChain clamp_chain(clamp_config);
  clamp_chain.prepare(kSampleRate, kBlockSize, 1);
  const float denormal = std::numeric_limits<float>::denorm_min();
  REQUIRE_THROWS_AS(clamp_chain.set_parameter("saturation.tape.speedIps", denormal),
                    SonareException);
  REQUIRE_THROWS_AS(clamp_chain.set_parameter("saturation.exciter.frequencyHz", denormal),
                    SonareException);
  REQUIRE_THROWS_AS(clamp_chain.set_parameter("saturation.exciter.q", denormal), SonareException);
  REQUIRE(clamp_chain.config().saturation.tape.config.speed_ips ==
          Catch::Approx(clamp_config.saturation.tape.config.speed_ips));
  REQUIRE(clamp_chain.config().saturation.exciter.config.frequency_hz ==
          Catch::Approx(clamp_config.saturation.exciter.config.frequency_hz));
  REQUIRE(clamp_chain.config().saturation.exciter.config.q ==
          Catch::Approx(clamp_config.saturation.exciter.config.q));

  const auto drain = [](StreamingMasteringChain& current) {
    std::vector<float> output;
    for (int call = 0; call < 4096; ++call) {
      std::vector<float> block(kBlockSize, -1.0f);
      float* channels[] = {block.data()};
      const int written = current.flush(channels, 1, kBlockSize);
      if (written == 0) return output;
      output.insert(output.end(), block.begin(), block.begin() + written);
    }
    throw std::runtime_error("StreamingMasteringChain flush did not terminate");
  };
  const auto tail = drain(chain);
  const auto control_tail = drain(control);
  REQUIRE(tail.size() == control_tail.size());
  REQUIRE_FALSE(tail.empty());
  REQUIRE(max_abs_difference(tail, control_tail) < 1.0e-6f);

  MasteringChainConfig disabled_config;
  StreamingMasteringChain disabled(disabled_config);
  disabled.prepare(kSampleRate, kBlockSize, 1);
  REQUIRE_THROWS_AS(disabled.set_parameter("dynamics.compressor.thresholdDb", -30.0),
                    SonareException);

  MasteringChainConfig stereo_config;
  stereo_config.stereo.imager.enabled = true;
  StreamingMasteringChain mono(stereo_config);
  mono.prepare(kSampleRate, kBlockSize, 1);
  REQUIRE_THROWS_AS(mono.set_parameter("stereo.imager.width", 1.5), SonareException);
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
  constexpr double kRate = 44100.0;
  constexpr int kBlock = 512;

  // A loud burst to leave state behind, and a quiet probe whose rendering
  // reveals it: compressor envelope, limiter lookahead and biquad memory all
  // colour the probe's head differently when the burst preceded it.
  std::vector<float> prime(kBlock);
  std::vector<float> probe(kBlock);
  for (int i = 0; i < kBlock; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kRate);
    prime[static_cast<size_t>(i)] =
        0.9f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 1000.0f * t);
    probe[static_cast<size_t>(i)] =
        0.05f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 100.0f * t);
  }

  const auto render_probe = [&](const MasteringChainConfig& config, bool with_prime,
                                bool with_reset) {
    StreamingMasteringChain chain{config};
    chain.prepare(kRate, kBlock, 1);
    if (with_prime) {
      std::vector<float> burst = prime;
      float* channels[] = {burst.data()};
      chain.process_block(channels, 1, kBlock);
    }
    if (with_reset) chain.reset();
    std::vector<float> out = probe;
    float* channels[] = {out.data()};
    chain.process_block(channels, 1, kBlock);
    return out;
  };

  const auto check_stage = [&](const MasteringChainConfig& config) {
    const std::vector<float> fresh = render_probe(config, false, false);
    const std::vector<float> after_reset = render_probe(config, true, true);
    const std::vector<float> without_reset = render_probe(config, true, false);
    // Non-vacuity: unless the burst is still audible in the un-reset render,
    // the bit-identity below would hold for a reset() that did nothing.
    CHECK(max_abs_difference(without_reset, fresh) > 0.0f);
    CHECK(max_abs_difference(after_reset, fresh) == 0.0f);
  };

  SECTION("compressor") {
    MasteringChainConfig config;
    config.dynamics.compressor.enabled = true;
    config.dynamics.compressor.config.threshold_db = -30.0f;
    config.dynamics.compressor.config.ratio = 8.0f;
    config.dynamics.compressor.config.release_ms = 500.0f;
    check_stage(config);
  }

  SECTION("true peak limiter") {
    MasteringChainConfig config;
    config.maximizer.true_peak_limiter.enabled = true;
    config.maximizer.true_peak_limiter.config.ceiling_db = -6.0f;
    check_stage(config);
  }

  SECTION("tilt eq") {
    MasteringChainConfig config;
    config.eq.tilt.enabled = true;
    config.eq.tilt.tilt_db = 6.0f;
    check_stage(config);
  }
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
  // A DAW plugin host may run with a comma-decimal locale. LC_NUMERIC is the
  // half that reaches the serializer -- util::json formats through snprintf and
  // folds the separator back onto "." itself -- so it is the one that has to
  // move for this to mean anything. std::locale::global is switched alongside
  // it to cover any stream-based formatting a caller mixes in. What a writer
  // without the fold produces is `{"sourceLufs":-21,7539}`, a document that
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

TEST_CASE("StreamingMasteringChain rejects null planes before processing",
          "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.maximizer.true_peak_limiter.enabled = true;
  StreamingMasteringChain chain(config);
  chain.prepare(48000.0, 64, 2);
  std::vector<float> left(64, 0.25f);
  const auto original = left;
  float* channels[] = {left.data(), nullptr};
  REQUIRE_THROWS_AS(chain.process_block(channels, 2, 64), sonare::SonareException);
  CHECK(left == original);
  REQUIRE_THROWS_AS(chain.process_block(nullptr, 2, 1), sonare::SonareException);
  REQUIRE_NOTHROW(chain.process_block(nullptr, 2, 0));
}

TEST_CASE("StreamingMasteringChain rejected input preserves an in-progress drain",
          "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.maximizer.true_peak_limiter.enabled = true;
  StreamingMasteringChain chain(config);
  StreamingMasteringChain reference(config);
  constexpr int kBlock = 64;
  chain.prepare(48000.0, kBlock, 1);
  reference.prepare(48000.0, kBlock, 1);
  std::vector<float> actual(kBlock, 0.2f);
  std::vector<float> expected = actual;
  float* actual_channels[] = {actual.data()};
  float* expected_channels[] = {expected.data()};
  chain.process_block(actual_channels, 1, kBlock);
  reference.process_block(expected_channels, 1, kBlock);
  const int first = chain.flush(actual_channels, 1, kBlock);
  REQUIRE(first == reference.flush(expected_channels, 1, kBlock));
  REQUIRE(first > 0);
  REQUIRE(first < chain.latency_samples());
  CHECK(actual == expected);

  actual[0] = std::numeric_limits<float>::quiet_NaN();
  REQUIRE_THROWS_AS(chain.process_block(actual_channels, 1, kBlock), sonare::SonareException);
  int total = first;
  for (;;) {
    const int written = chain.flush(actual_channels, 1, kBlock);
    const int expected_written = reference.flush(expected_channels, 1, kBlock);
    REQUIRE(written == expected_written);
    if (written == 0) break;
    CHECK(std::equal(actual.begin(), actual.begin() + written, expected.begin()));
    total += written;
  }
  // The flush covers the latency and then the oversampled limiter's ring.
  sonare::mastering::maximizer::TruePeakLimiter limiter(config.maximizer.true_peak_limiter.config);
  limiter.prepare(48000.0, kBlock);
  CHECK(total == chain.latency_samples() + limiter.tail_samples());
}

TEST_CASE("StreamingMasteringChain rejects non-finite rates without losing preparation",
          "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.maximizer.true_peak_limiter.enabled = true;
  StreamingMasteringChain chain(config);
  StreamingMasteringChain reference(config);
  chain.prepare(48000.0, 64, 1);
  reference.prepare(48000.0, 64, 1);
  const int latency = chain.latency_samples();
  REQUIRE(latency > 0);
  for (double rate :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    REQUIRE_THROWS_AS(chain.prepare(rate, 64, 1), sonare::SonareException);
    CHECK(chain.latency_samples() == latency);
    std::vector<float> block(64, 0.2f);
    std::vector<float> expected = block;
    float* channels[] = {block.data()};
    float* expected_channels[] = {expected.data()};
    REQUIRE_NOTHROW(chain.process_block(channels, 1, 64));
    reference.process_block(expected_channels, 1, 64);
    CHECK(block == expected);
  }
}

// ---------------------------------------------------------------------------
// Typed multiband over the flat chain params
// ---------------------------------------------------------------------------

namespace {

using sonare::mastering::dynamics::CompressorConfig;

constexpr const char* kMb = "dynamics.multibandComp.";

std::string mb_key(const std::string& rest) { return kMb + rest; }

void require_same_band(const CompressorConfig& actual, const CompressorConfig& expected) {
  CHECK(actual.threshold_db == expected.threshold_db);
  CHECK(actual.ratio == expected.ratio);
  CHECK(actual.attack_ms == expected.attack_ms);
  CHECK(actual.release_ms == expected.release_ms);
  CHECK(actual.knee_db == expected.knee_db);
  CHECK(actual.makeup_gain_db == expected.makeup_gain_db);
  CHECK(actual.auto_makeup == expected.auto_makeup);
  CHECK(actual.detector == expected.detector);
  CHECK(actual.sidechain_hpf_enabled == expected.sidechain_hpf_enabled);
  CHECK(actual.sidechain_hpf_hz == expected.sidechain_hpf_hz);
  CHECK(actual.pdr_time_ms == expected.pdr_time_ms);
  CHECK(actual.pdr_release_scale == expected.pdr_release_scale);
}

void require_same_multiband(const MultibandCompStage& actual, const MultibandCompStage& expected) {
  REQUIRE(actual.enabled == expected.enabled);
  REQUIRE(actual.config.crossover == expected.config.crossover);
  REQUIRE(actual.config.bands.size() == expected.config.bands.size());
  for (size_t index = 0; index < expected.config.bands.size(); ++index) {
    CAPTURE(index);
    require_same_band(actual.config.bands[index], expected.config.bands[index]);
  }
}

// A four-band stage whose every field differs from its default.
MultibandCompStage four_band_stage() {
  using sonare::mastering::dynamics::DetectorMode;
  using sonare::mastering::multiband::CrossoverMode;
  using sonare::mastering::multiband::CrossoverSlope;
  MultibandCompStage stage;
  stage.enabled = true;
  stage.config.crossover.cutoffs_hz = {150.0f, 1500.0f, 6000.0f};
  stage.config.crossover.slope = CrossoverSlope::LR8;
  stage.config.crossover.mode = CrossoverMode::Bessel;
  stage.config.crossover.fir_kernel_size = 257;
  stage.config.bands.assign(4, CompressorConfig{});
  for (size_t index = 0; index < 4; ++index) {
    auto& band = stage.config.bands[index];
    const auto step = static_cast<float>(index);
    band.threshold_db = -30.0f + step;
    band.ratio = 1.5f + 0.5f * step;
    band.attack_ms = 3.0f + step;
    band.release_ms = 60.0f + 10.0f * step;
    band.knee_db = 1.0f + step;
    band.makeup_gain_db = 0.5f * step;
    band.auto_makeup = index % 2 == 1;
    band.detector = static_cast<DetectorMode>(index % 3);
    band.sidechain_hpf_enabled = index % 2 == 0;
    band.sidechain_hpf_hz = 70.0f + 5.0f * step;
    band.pdr_time_ms = 2.0f * step;
    band.pdr_release_scale = 1.0f + 0.25f * step;
  }
  return stage;
}

// The same stage as flat keys in the array spelling.
std::vector<Param> array_spelling(const MultibandCompStage& stage) {
  std::vector<Param> params;
  params.push_back({mb_key("enabled"), stage.enabled ? 1.0 : 0.0});
  const auto& crossover = stage.config.crossover;
  for (size_t index = 0; index < crossover.cutoffs_hz.size(); ++index) {
    params.push_back({mb_key("crossover.cutoffsHz." + std::to_string(index)),
                      static_cast<double>(crossover.cutoffs_hz[index])});
  }
  params.push_back({mb_key("crossover.slope"), static_cast<double>(crossover.slope)});
  params.push_back({mb_key("crossover.mode"), static_cast<double>(crossover.mode)});
  params.push_back(
      {mb_key("crossover.firKernelSize"), static_cast<double>(crossover.fir_kernel_size)});
  for (size_t index = 0; index < stage.config.bands.size(); ++index) {
    const auto& band = stage.config.bands[index];
    const std::string prefix = "bands." + std::to_string(index) + ".";
    params.push_back({mb_key(prefix + "thresholdDb"), band.threshold_db});
    params.push_back({mb_key(prefix + "ratio"), band.ratio});
    params.push_back({mb_key(prefix + "attackMs"), band.attack_ms});
    params.push_back({mb_key(prefix + "releaseMs"), band.release_ms});
    params.push_back({mb_key(prefix + "kneeDb"), band.knee_db});
    params.push_back({mb_key(prefix + "makeupGainDb"), band.makeup_gain_db});
    params.push_back({mb_key(prefix + "autoMakeup"), band.auto_makeup ? 1.0 : 0.0});
    params.push_back({mb_key(prefix + "detector"), static_cast<double>(band.detector)});
    params.push_back(
        {mb_key(prefix + "sidechainHpfEnabled"), band.sidechain_hpf_enabled ? 1.0 : 0.0});
    params.push_back({mb_key(prefix + "sidechainHpfHz"), band.sidechain_hpf_hz});
    params.push_back({mb_key(prefix + "pdrTimeMs"), band.pdr_time_ms});
    params.push_back({mb_key(prefix + "pdrReleaseScale"), band.pdr_release_scale});
  }
  return params;
}

MasteringChainConfig parse(const std::vector<Param>& params) {
  return parse_chain_config_params(params.data(), params.size());
}

// The refusal is InvalidParameter and its message carries every one of @p needles.
template <typename Fn>
void require_refused(Fn&& call, std::initializer_list<const char*> needles) {
  try {
    call();
    FAIL("expected an InvalidParameter refusal");
  } catch (const SonareException& error) {
    CHECK(error.code() == ErrorCode::InvalidParameter);
    const std::string message = error.what();
    CAPTURE(message);
    for (const char* needle : needles) {
      CHECK(message.find(needle) != std::string::npos);
    }
  }
}

}  // namespace

TEST_CASE("flat multiband array keys parse to the v2 JSON document's stage",
          "[mastering][chain][multiband]") {
  const MultibandCompStage stage = four_band_stage();
  MasteringChainConfig document_config;
  document_config.dynamics.multiband_comp = stage;
  const std::string json = chain_config_to_json(document_config);
  REQUIRE(json.find("\"version\":2") != std::string::npos);
  const MasteringChainConfig from_document = chain_config_from_json(json);

  const MasteringChainConfig from_flat = parse(array_spelling(stage));
  require_same_multiband(from_flat.dynamics.multiband_comp, from_document.dynamics.multiband_comp);
  require_same_multiband(from_flat.dynamics.multiband_comp, stage);
}

TEST_CASE("flat multiband scalar spellings and shorthand edit in place",
          "[mastering][chain][multiband]") {
  SECTION("shorthand and scalar keys keep the three-band split") {
    const auto config = parse({{mb_key("lowCutoffHz"), 200.0},
                               {mb_key("cutoff1Hz"), 5000.0},
                               {mb_key("midThresholdDb"), -22.0},
                               {mb_key("band2.kneeDb"), 3.0},
                               {mb_key("slope"), 2.0}});
    const auto& multiband = config.dynamics.multiband_comp;
    REQUIRE(multiband.enabled);
    REQUIRE(multiband.config.crossover.cutoffs_hz == std::vector<float>{200.0f, 5000.0f});
    REQUIRE(multiband.config.bands.size() == 3);
    CHECK(multiband.config.bands[1].threshold_db == -22.0f);
    CHECK(multiband.config.bands[2].knee_db == 3.0f);
    CHECK(multiband.config.crossover.slope == multiband::CrossoverSlope::LR8);
  }

  SECTION("bands.<i>.<field> is band<i>.<field>") {
    const auto array = parse({{mb_key("bands.1.ratio"), 4.0}, {mb_key("crossover.mode"), 1.0}});
    const auto scalar = parse({{mb_key("band1.ratio"), 4.0}, {mb_key("mode"), 1.0}});
    require_same_multiband(array.dynamics.multiband_comp, scalar.dynamics.multiband_comp);
    CHECK(array.dynamics.multiband_comp.config.bands[1].ratio == 4.0f);
  }

  SECTION("two spellings of one field in one call are refused by name") {
    require_refused([] { parse({{mb_key("lowRatio"), 2.0}, {mb_key("bands.0.ratio"), 3.0}}); },
                    {"dynamics.multibandComp.lowRatio", "dynamics.multibandComp.bands.0.ratio"});
    require_refused([] { parse({{mb_key("band0.ratio"), 2.0}, {mb_key("lowRatio"), 3.0}}); },
                    {"dynamics.multibandComp.band0.ratio", "dynamics.multibandComp.lowRatio"});
    require_refused([] { parse({{mb_key("crossover.slope"), 1.0}, {mb_key("slope"), 2.0}}); },
                    {"dynamics.multibandComp.crossover.slope", "dynamics.multibandComp.slope"});
    require_refused([] { parse({{mb_key("lowCutoffHz"), 100.0}, {mb_key("cutoff0Hz"), 90.0}}); },
                    {"dynamics.multibandComp.lowCutoffHz", "dynamics.multibandComp.cutoff0Hz"});
  }

  SECTION("the same spelling twice stays last-wins") {
    const auto config = parse({{mb_key("bands.0.ratio"), 2.0}, {mb_key("bands.0.ratio"), 3.0}});
    CHECK(config.dynamics.multiband_comp.config.bands[0].ratio == 3.0f);
  }

  SECTION("the cutoff list is refused beside any scalar cutoff spelling") {
    for (const char* scalar : {"lowCutoffHz", "highCutoffHz", "cutoff0Hz", "cutoff1Hz"}) {
      CAPTURE(scalar);
      const std::string scalar_key = mb_key(scalar);
      require_refused(
          [&] {
            parse({{mb_key("crossover.cutoffsHz.0"), 100.0},
                   {scalar_key, 1000.0},
                   {mb_key("crossover.cutoffsHz.1"), 2000.0}});
          },
          {scalar_key.c_str(), "crossover.cutoffsHz"});
    }
  }
}

TEST_CASE("flat multiband list rules hold in any key order", "[mastering][chain][multiband]") {
  SECTION("bands before or after the cutoff list give one config") {
    const std::vector<Param> bands = {{mb_key("bands.3.ratio"), 6.0},
                                      {mb_key("bands.0.thresholdDb"), -24.0}};
    const std::vector<Param> crossover = {{mb_key("crossover.cutoffsHz.2"), 8000.0},
                                          {mb_key("crossover.cutoffsHz.0"), 200.0},
                                          {mb_key("crossover.cutoffsHz.1"), 2000.0}};
    std::vector<Param> bands_first = bands;
    bands_first.insert(bands_first.end(), crossover.begin(), crossover.end());
    std::vector<Param> crossover_first = crossover;
    crossover_first.insert(crossover_first.end(), bands.begin(), bands.end());

    const auto first = parse(bands_first);
    const auto second = parse(crossover_first);
    require_same_multiband(first.dynamics.multiband_comp, second.dynamics.multiband_comp);
    const auto& config = first.dynamics.multiband_comp.config;
    REQUIRE(config.crossover.cutoffs_hz == std::vector<float>{200.0f, 2000.0f, 8000.0f});
    REQUIRE(config.bands.size() == 4);
    CHECK(config.bands[3].ratio == 6.0f);
    CHECK(config.bands[0].threshold_db == -24.0f);
  }

  SECTION("cutoff indices must run from 0 without a gap") {
    require_refused(
        [] {
          parse(
              {{mb_key("crossover.cutoffsHz.0"), 100.0}, {mb_key("crossover.cutoffsHz.2"), 300.0}});
        },
        {"crossover cutoff indices must be contiguous from 0: "
         "dynamics.multibandComp.crossover.cutoffsHz.2"});
    require_refused([] { parse({{mb_key("crossover.cutoffsHz.1"), 100.0}}); },
                    {"crossover cutoff indices must be contiguous from 0: "
                     "dynamics.multibandComp.crossover.cutoffsHz.1"});
  }

  SECTION("a band index past the resulting count is refused in either order") {
    const std::vector<Param> band = {{mb_key("bands.2.ratio"), 2.0}};
    const std::vector<Param> list = {{mb_key("crossover.cutoffsHz.0"), 1000.0}};
    std::vector<Param> band_first = band;
    band_first.insert(band_first.end(), list.begin(), list.end());
    std::vector<Param> list_first = list;
    list_first.insert(list_first.end(), band.begin(), band.end());
    for (const auto* params : {&band_first, &list_first}) {
      require_refused([&] { parse(*params); },
                      {"multiband band index out of range: dynamics.multibandComp.bands.2.ratio"});
    }
  }

  SECTION("structural limits are refused whether or not the stage is enabled") {
    for (const double enabled : {1.0, 0.0}) {
      CAPTURE(enabled);
      const Param on{mb_key("enabled"), enabled};
      std::vector<Param> too_many = {on};
      for (int index = 0; index < 64; ++index) {
        too_many.push_back({mb_key("crossover.cutoffsHz." + std::to_string(index)),
                            20.0 + 10.0 * static_cast<double>(index)});
      }
      require_refused([&] { parse(too_many); }, {"multiband bands count must be between 1 and 64"});
      require_refused(
          [&] {
            parse({on,
                   {mb_key("crossover.cutoffsHz.0"), 2000.0},
                   {mb_key("crossover.cutoffsHz.1"), 200.0}});
          },
          {"crossover cutoffs must be strictly ascending"});
      require_refused([&] { parse({on, {mb_key("crossover.cutoffsHz.0"), -5.0}}); },
                      {"crossover cutoffs must be finite and positive"});
      require_refused([&] { parse({on, {mb_key("cutoff0Hz"), 0.0}}); },
                      {"crossover cutoffs must be finite and positive"});
      require_refused([&] { parse({on, {mb_key("crossover.slope"), 3.0}}); },
                      {"crossover slope is out of range"});
      require_refused([&] { parse({on, {mb_key("mode"), 4.0}}); },
                      {"crossover mode is out of range"});
      require_refused([&] { parse({on, {mb_key("crossover.firKernelSize"), 70000.0}}); },
                      {"crossover FIR kernel size is out of range"});
    }
  }

  SECTION("64 bands is the limit, not past it") {
    std::vector<Param> params;
    for (int index = 0; index < 63; ++index) {
      params.push_back({mb_key("crossover.cutoffsHz." + std::to_string(index)),
                        20.0 + 10.0 * static_cast<double>(index)});
    }
    params.push_back({mb_key("bands.63.ratio"), 3.0});
    const auto config = parse(params);
    REQUIRE(config.dynamics.multiband_comp.config.bands.size() == 64);
    CHECK(config.dynamics.multiband_comp.config.bands[63].ratio == 3.0f);
  }
}

TEST_CASE("a typed crossover alone enables the multiband stage", "[mastering][chain][multiband]") {
  const std::vector<Param> list = {{mb_key("crossover.cutoffsHz.0"), 200.0},
                                   {mb_key("crossover.cutoffsHz.1"), 2000.0},
                                   {mb_key("crossover.cutoffsHz.2"), 8000.0}};
  const auto parsed = parse(list);
  CHECK(parsed.dynamics.multiband_comp.enabled);
  CHECK(parsed.dynamics.multiband_comp.config.bands.size() == 4);
  // The same rule the shorthand follows.
  CHECK(parse({{mb_key("lowCutoffHz"), 200.0}}).dynamics.multiband_comp.enabled);

  MasteringChainConfig base;
  REQUIRE_FALSE(base.dynamics.multiband_comp.enabled);
  apply_chain_config_overrides(base, list.data(), list.size());
  CHECK(base.dynamics.multiband_comp.enabled);

  std::vector<Param> switched_off = list;
  switched_off.push_back({mb_key("enabled"), 0.0});
  CHECK_FALSE(parse(switched_off).dynamics.multiband_comp.enabled);
}

TEST_CASE("StreamingMasteringChain retargets every multiband band live",
          "[mastering][chain][streaming][multiband]") {
  constexpr double kSampleRate = 48000.0;
  constexpr int kBlockSize = 256;
  MasteringChainConfig config;
  config.dynamics.multiband_comp.enabled = true;
  config.dynamics.multiband_comp.config.crossover.cutoffs_hz = {200.0f, 1000.0f, 4000.0f, 10000.0f};
  config.dynamics.multiband_comp.config.bands.assign(5, CompressorConfig{});

  // 6 kHz sits in band 3 (4..10 kHz).
  const auto block = [&](int offset) {
    std::vector<float> samples(kBlockSize);
    for (int index = 0; index < kBlockSize; ++index) {
      samples[static_cast<size_t>(index)] =
          0.5f * std::sin(sonare::constants::kTwoPi * 6000.0f * static_cast<float>(offset + index) /
                          static_cast<float>(kSampleRate));
    }
    return samples;
  };
  const auto run = [&](StreamingMasteringChain& chain) {
    std::vector<float> out;
    for (int offset = 0; offset < 8 * kBlockSize; offset += kBlockSize) {
      auto samples = block(offset);
      float* channels[] = {samples.data()};
      chain.process_block(channels, 1, kBlockSize);
      out.insert(out.end(), samples.begin(), samples.end());
    }
    return out;
  };

  SECTION("band3 thresholdDb and band4 makeupGainDb") {
    MasteringChainConfig target = config;
    target.dynamics.multiband_comp.config.bands[3].threshold_db = -30.0f;
    target.dynamics.multiband_comp.config.bands[4].makeup_gain_db = 2.0f;
    StreamingMasteringChain changed(config);
    StreamingMasteringChain expected(target);
    StreamingMasteringChain control(config);
    changed.prepare(kSampleRate, kBlockSize, 1);
    expected.prepare(kSampleRate, kBlockSize, 1);
    control.prepare(kSampleRate, kBlockSize, 1);
    REQUIRE_NOTHROW(changed.set_parameter(mb_key("band3.thresholdDb"), -30.0));
    REQUIRE_NOTHROW(changed.set_parameter(mb_key("band4.makeupGainDb"), 2.0));
    const auto& bands = changed.config().dynamics.multiband_comp.config.bands;
    CHECK(bands[3].threshold_db == -30.0f);
    CHECK(bands[4].makeup_gain_db == 2.0f);

    const auto changed_out = run(changed);
    const auto expected_out = run(expected);
    const auto control_out = run(control);
    CHECK(max_abs_difference(changed_out, expected_out) < 1.0e-6f);
    // The edit is audible, so the agreement above is not two untouched chains.
    CHECK(max_abs_difference(changed_out, control_out) > 1.0e-3f);
  }

  SECTION("makeupGainDb is live on the shorthand bands too, beside the aliases") {
    StreamingMasteringChain chain(config);
    chain.prepare(kSampleRate, kBlockSize, 1);
    REQUIRE_NOTHROW(chain.set_parameter(mb_key("band0.makeupGainDb"), 1.5));
    REQUIRE_NOTHROW(chain.set_parameter(mb_key("lowThresholdDb"), -21.0));
    REQUIRE_NOTHROW(chain.set_parameter(mb_key("highRatio"), 3.0));
    const auto& bands = chain.config().dynamics.multiband_comp.config.bands;
    CHECK(bands[0].makeup_gain_db == 1.5f);
    CHECK(bands[0].threshold_db == -21.0f);
    CHECK(bands[2].ratio == 3.0f);
    // Structural keys stay off the live surface.
    CHECK_THROWS_AS(chain.set_parameter(mb_key("cutoff0Hz"), 250.0), SonareException);
    CHECK_THROWS_AS(chain.set_parameter(mb_key("band5.thresholdDb"), -20.0), SonareException);
  }

  SECTION("a repeated value is a no-op") {
    StreamingMasteringChain repeated(config);
    StreamingMasteringChain control(config);
    repeated.prepare(kSampleRate, kBlockSize, 1);
    control.prepare(kSampleRate, kBlockSize, 1);
    auto first = block(0);
    auto control_first = first;
    float* first_channels[] = {first.data()};
    float* control_channels[] = {control_first.data()};
    repeated.process_block(first_channels, 1, kBlockSize);
    control.process_block(control_channels, 1, kBlockSize);
    const double current = config.dynamics.multiband_comp.config.bands[3].threshold_db;
    REQUIRE_NOTHROW(repeated.set_parameter(mb_key("band3.thresholdDb"), current));
    auto next = block(kBlockSize);
    auto control_next = next;
    float* next_channels[] = {next.data()};
    float* control_next_channels[] = {control_next.data()};
    repeated.process_block(next_channels, 1, kBlockSize);
    control.process_block(control_next_channels, 1, kBlockSize);
    CHECK(max_abs_difference(next, control_next) == 0.0f);
  }
}

}  // namespace sonare::mastering::api
