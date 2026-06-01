// Targeted coverage for audit fixes around the insert factory, JSON
// round-tripping, and presets. The data-driven set_parameter_test.cpp already
// exercises the set_parameter contract for every name in insert_factory_names();
// these cases pin the specific behaviors changed by the audit pass:
//   * the modulation/delay effects are now registered and buildable,
//   * "maximizer.loudnessOptimize" is intentionally NOT a streaming insert,
//   * chain JSON round-trips the full repair.denoise field set,
//   * the streaming preset matches the pop preset.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/presets.h"
#include "rt/processor_base.h"

namespace {

using sonare::mastering::api::chain_config_from_json;
using sonare::mastering::api::chain_config_to_json;
using sonare::mastering::api::insert_factory_names;
using sonare::mastering::api::make_insert;
using sonare::mastering::api::MasteringChainConfig;
using sonare::mastering::api::Preset;
using sonare::mastering::api::preset_config;

bool ListContains(const std::vector<std::string>& names, const std::string& target) {
  for (const auto& name : names) {
    if (name == target) return true;
  }
  return false;
}

}  // namespace

#ifdef SONARE_WITH_FX
TEST_CASE("Modulation and delay effects are registered as inserts",
          "[mastering][insert_factory][effects]") {
  const auto names = insert_factory_names();
  for (const char* name : {"effects.modulation.chorus", "effects.modulation.flanger",
                           "effects.modulation.phaser", "effects.delay.stereo"}) {
    DYNAMIC_SECTION(name) {
      REQUIRE(ListContains(names, name));
      auto processor = make_insert(name, "{}");
      REQUIRE(processor != nullptr);
    }
  }
}

TEST_CASE("Modulation/delay inserts read their JSON params",
          "[mastering][insert_factory][effects]") {
  // Constructing with explicit params must not throw and must yield a processor;
  // the numeric mapping is validated indirectly via the set_parameter contract
  // test, here we just guard the param-reading code path.
  REQUIRE(make_insert("effects.modulation.chorus",
                      R"({"rateHz":1.2,"depthMs":4,"centerDelayMs":10,"dryWet":0.4})") != nullptr);
  REQUIRE(
      make_insert("effects.modulation.flanger",
                  R"({"rateHz":0.5,"depthMs":1,"centerDelayMs":2,"feedback":0.4,"dryWet":0.6})") !=
      nullptr);
  REQUIRE(make_insert("effects.modulation.phaser",
                      R"({"rateHz":0.3,"minHz":200,"maxHz":2000,"stages":6,"dryWet":0.5})") !=
          nullptr);
  REQUIRE(
      make_insert(
          "effects.delay.stereo",
          R"({"delayTimeLMs":120,"delayTimeRMs":180,"feedback":0.3,"pingPong":1,"dryWet":0.5})") !=
      nullptr);
}
#endif  // SONARE_WITH_FX

TEST_CASE("maximizer.loudnessOptimize is not a streaming insert",
          "[mastering][insert_factory][maximizer]") {
  // LUFS normalization is offline-only; the insert factory must not silently
  // degrade it to a true-peak limiter. The honest streaming name is
  // "maximizer.truePeakLimiter", which remains available.
  REQUIRE_FALSE(ListContains(insert_factory_names(), "maximizer.loudnessOptimize"));
  REQUIRE(make_insert("maximizer.loudnessOptimize", "{}") == nullptr);
  REQUIRE(make_insert("maximizer.truePeakLimiter", "{}") != nullptr);
}

TEST_CASE("chain JSON round-trips the full repair.denoise field set",
          "[mastering][chain_json][repair]") {
  MasteringChainConfig cfg;
  cfg.repair.denoise.enabled = true;
  cfg.repair.denoise.config.mode = sonare::mastering::repair::DenoiseMode::SpectralSubtraction;
  cfg.repair.denoise.config.noise_estimator =
      sonare::mastering::repair::DenoiseNoiseEstimator::Imcra;
  cfg.repair.denoise.config.n_fft = 2048;
  cfg.repair.denoise.config.hop_length = 512;
  cfg.repair.denoise.config.dd_alpha = 0.95f;
  cfg.repair.denoise.config.gain_floor = 0.08f;
  cfg.repair.denoise.config.over_subtraction = 3.5f;
  cfg.repair.denoise.config.spectral_floor = 0.12f;
  cfg.repair.denoise.config.noise_estimation_quantile = 0.2f;
  cfg.repair.denoise.config.speech_presence_gain = false;
  cfg.repair.denoise.config.gain_smoothing = false;

  const MasteringChainConfig restored = chain_config_from_json(chain_config_to_json(cfg));
  const auto& d = restored.repair.denoise.config;
  REQUIRE(restored.repair.denoise.enabled);
  REQUIRE(d.mode == sonare::mastering::repair::DenoiseMode::SpectralSubtraction);
  REQUIRE(d.noise_estimator == sonare::mastering::repair::DenoiseNoiseEstimator::Imcra);
  REQUIRE(d.n_fft == 2048);
  REQUIRE(d.hop_length == 512);
  REQUIRE(d.dd_alpha == 0.95f);
  REQUIRE(d.gain_floor == 0.08f);
  REQUIRE(d.over_subtraction == 3.5f);
  REQUIRE(d.spectral_floor == 0.12f);
  REQUIRE(d.noise_estimation_quantile == 0.2f);
  REQUIRE_FALSE(d.speech_presence_gain);
  REQUIRE_FALSE(d.gain_smoothing);
}

TEST_CASE("streaming preset equals pop preset", "[mastering][presets]") {
  const MasteringChainConfig pop = preset_config(Preset::Pop);
  const MasteringChainConfig streaming = preset_config(Preset::Streaming);
  REQUIRE(streaming.loudness.enabled == pop.loudness.enabled);
  REQUIRE(streaming.loudness.target_lufs == pop.loudness.target_lufs);
  REQUIRE(streaming.loudness.ceiling_db == pop.loudness.ceiling_db);
  REQUIRE(streaming.maximizer.true_peak_limiter.config.ceiling_db ==
          pop.maximizer.true_peak_limiter.config.ceiling_db);
  REQUIRE(streaming.dynamics.compressor.config.threshold_db ==
          pop.dynamics.compressor.config.threshold_db);
}
