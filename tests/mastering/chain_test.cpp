#include "mastering/api/chain.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "mastering/api/named_processor.h"
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
