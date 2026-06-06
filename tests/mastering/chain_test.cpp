#include "mastering/api/chain.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "mastering/api/audio_utils.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/common/loudness_measure.h"
#include "util/exception.h"

using Catch::Matchers::WithinAbs;

namespace sonare::mastering::api {

TEST_CASE("MasteringChain passes through with empty config (mono)", "[mastering][chain]") {
  std::vector<float> samples(44100, 0.1f);
  MasteringChainConfig config;
  MasteringChain chain(config);
  auto result = chain.process_mono(samples.data(), samples.size(), 44100);
  REQUIRE(result.samples.size() == samples.size());
  REQUIRE(result.sample_rate == 44100);
  REQUIRE(result.stages.empty());
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

TEST_CASE("StreamingMasteringChain throws if denoise enabled", "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.repair.denoise.enabled = true;
  REQUIRE_THROWS_AS(StreamingMasteringChain(std::move(config)), sonare::SonareException);
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

}  // namespace sonare::mastering::api
