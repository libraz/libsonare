#include "mastering/api/chain.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <stdexcept>
#include <vector>

#include "mastering/api/named_processor.h"

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
  REQUIRE_THROWS_AS(parse_chain_config_params(params, 1), std::invalid_argument);
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

// ---------------------------------------------------------------------------
// StreamingMasteringChain
// ---------------------------------------------------------------------------

TEST_CASE("StreamingMasteringChain throws if denoise enabled", "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.repair.denoise.enabled = true;
  REQUIRE_THROWS_AS(StreamingMasteringChain(std::move(config)), std::invalid_argument);
}

TEST_CASE("StreamingMasteringChain throws if loudness enabled", "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.loudness.enabled = true;
  REQUIRE_THROWS_AS(StreamingMasteringChain(std::move(config)), std::invalid_argument);
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
  REQUIRE_THROWS_AS(chain.prepare(44100.0, 512, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(chain.prepare(44100.0, 512, 3), std::invalid_argument);
}

TEST_CASE("StreamingMasteringChain rejects oversized block", "[mastering][chain][streaming]") {
  MasteringChainConfig config;
  config.eq.tilt.enabled = true;
  StreamingMasteringChain chain(std::move(config));
  chain.prepare(44100.0, 256, 1);
  std::vector<float> block(512, 0.1f);
  float* channels[] = {block.data()};
  REQUIRE_THROWS_AS(chain.process_block(channels, 1, 512), std::invalid_argument);
}

}  // namespace sonare::mastering::api
