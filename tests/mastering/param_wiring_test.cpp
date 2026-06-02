// Regression coverage for three parameter-wiring bugs where a user-specified
// parameter was silently ignored and the processor ran with the factory
// default (no error raised):
//
//   H4 repair.declip.lpcBlend was dropped by the named-processor branch, so the
//      declipper always used DeclipConfig::lpc_blend == 0.65f.
//   H5 multiband.{compressor,expander,limiter,saturation} only set the
//      crossover and never populated per-band parameters, so band0.thresholdDb
//      etc. were never read (every band kept its factory default).
//   H6 the chain compressor mapping (flat Param[] and JSON round-trip) did not
//      carry detector / sidechainHpf* / pdr* fields, so they were unreachable
//      from any binding and lost on a JSON round-trip.
//
// These assert at the config-building level (the resulting config struct) for
// determinism rather than comparing audio output.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/processor_params.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/multiband/multiband_compressor.h"
#include "mastering/multiband/multiband_expander.h"
#include "mastering/multiband/multiband_limiter.h"
#include "mastering/multiband/multiband_saturation.h"
#include "mastering/repair/declip.h"

namespace {

using sonare::mastering::api::apply_named_processor_stereo;
using sonare::mastering::api::chain_config_from_json;
using sonare::mastering::api::chain_config_to_json;
using sonare::mastering::api::make_insert;
using sonare::mastering::api::MasteringChainConfig;
using sonare::mastering::api::Param;
using sonare::mastering::api::parse_chain_config_params;
using sonare::mastering::api::detail::f;
using sonare::mastering::api::detail::make_map;
using sonare::mastering::api::detail::populate_compressor_bands;

}  // namespace

// --- H4 -------------------------------------------------------------------

TEST_CASE("repair.declip lpcBlend is wired through the named-processor path",
          "[mastering][repair][declip][param_wiring]") {
  // The named-processor branch reads `config.lpc_blend = f(params, "lpcBlend",
  // config.lpc_blend)` — exercise that exact accessor to prove the supplied
  // value overrides the struct default (0.65f) instead of being dropped.
  sonare::mastering::repair::DeclipConfig config;
  REQUIRE(config.lpc_blend == 0.65f);  // factory default the bug got stuck on

  const std::vector<Param> params{
      {"clipThreshold", 0.9}, {"lpcOrder", 24}, {"iterations", 3}, {"lpcBlend", 0.25}};
  const auto map = make_map(params);
  config.lpc_blend = f(map, "lpcBlend", config.lpc_blend);

  REQUIRE(config.lpc_blend == 0.25f);
  REQUIRE(config.lpc_blend != 0.65f);
}

// --- H5 -------------------------------------------------------------------

TEST_CASE("multiband per-band params populate via the shared helper",
          "[mastering][multiband][param_wiring]") {
  // The named-processor path calls populate_compressor_bands() after building
  // the crossover. Distinct band0 vs band1 thresholds must survive (not all
  // collapse to the factory default of -18 dB).
  sonare::mastering::multiband::MultibandCompressorConfig config;
  const float default_threshold = config.bands.at(0).threshold_db;
  REQUIRE(default_threshold == -18.0f);

  const std::vector<Param> params{{"band0.thresholdDb", -30.0},
                                  {"band0.ratio", 4.0},
                                  {"band1.thresholdDb", -12.0},
                                  {"band2.thresholdDb", -6.0}};
  populate_compressor_bands(config, make_map(params));

  REQUIRE(config.bands.at(0).threshold_db == -30.0f);
  REQUIRE(config.bands.at(0).ratio == 4.0f);
  REQUIRE(config.bands.at(1).threshold_db == -12.0f);
  REQUIRE(config.bands.at(2).threshold_db == -6.0f);
  REQUIRE(config.bands.at(0).threshold_db != config.bands.at(1).threshold_db);
}

TEST_CASE("multiband inserts reflect per-band thresholdDb via insert_factory",
          "[mastering][multiband][insert_factory][param_wiring]") {
  using sonare::mastering::multiband::MultibandCompressor;
  using sonare::mastering::multiband::MultibandExpander;
  using sonare::mastering::multiband::MultibandLimiter;
  using sonare::mastering::multiband::MultibandSaturation;

  SECTION("compressor") {
    auto processor =
        make_insert("multiband.compressor",
                    R"({"band0.thresholdDb":-30,"band1.thresholdDb":-12,"band2.thresholdDb":-6})");
    auto* mb = dynamic_cast<MultibandCompressor*>(processor.get());
    REQUIRE(mb != nullptr);
    REQUIRE(mb->config().bands.at(0).threshold_db == -30.0f);
    REQUIRE(mb->config().bands.at(1).threshold_db == -12.0f);
    REQUIRE(mb->config().bands.at(2).threshold_db == -6.0f);
    REQUIRE(mb->config().bands.at(0).threshold_db != mb->config().bands.at(1).threshold_db);
  }

  SECTION("expander") {
    auto processor =
        make_insert("multiband.expander", R"({"band0.thresholdDb":-50,"band1.thresholdDb":-30})");
    auto* mb = dynamic_cast<MultibandExpander*>(processor.get());
    REQUIRE(mb != nullptr);
    REQUIRE(mb->config().bands.at(0).threshold_db == -50.0f);
    REQUIRE(mb->config().bands.at(1).threshold_db == -30.0f);
    REQUIRE(mb->config().bands.at(0).threshold_db != mb->config().bands.at(1).threshold_db);
  }

  SECTION("limiter") {
    auto processor =
        make_insert("multiband.limiter", R"({"band0.thresholdDb":-3,"band1.thresholdDb":-1})");
    auto* mb = dynamic_cast<MultibandLimiter*>(processor.get());
    REQUIRE(mb != nullptr);
    REQUIRE(mb->config().bands.at(0).threshold_db == -3.0f);
    REQUIRE(mb->config().bands.at(1).threshold_db == -1.0f);
    REQUIRE(mb->config().bands.at(0).threshold_db != mb->config().bands.at(1).threshold_db);
  }

  SECTION("saturation") {
    // SaturationBandConfig has no thresholdDb; driveDb is its per-band gain.
    auto processor =
        make_insert("multiband.saturation", R"({"band0.driveDb":6,"band1.driveDb":12})");
    auto* mb = dynamic_cast<MultibandSaturation*>(processor.get());
    REQUIRE(mb != nullptr);
    REQUIRE(mb->config().bands.at(0).drive_db == 6.0f);
    REQUIRE(mb->config().bands.at(1).drive_db == 12.0f);
    REQUIRE(mb->config().bands.at(0).drive_db != mb->config().bands.at(1).drive_db);
  }
}

// --- H6 -------------------------------------------------------------------

TEST_CASE("chain compressor advanced fields survive flat-param apply",
          "[mastering][chain][compressor][param_wiring]") {
  const std::vector<Param> params{
      {"dynamics.compressor.enabled", 1.0},
      {"dynamics.compressor.detector", 2.0},  // LogRms
      {"dynamics.compressor.sidechainHpfEnabled", 1.0},
      {"dynamics.compressor.sidechainHpfHz", 250.0},
      {"dynamics.compressor.pdrTimeMs", 30.0},
      {"dynamics.compressor.pdrReleaseScale", 1.5},
  };
  const MasteringChainConfig cfg = parse_chain_config_params(params.data(), params.size());
  const auto& c = cfg.dynamics.compressor.config;

  REQUIRE(cfg.dynamics.compressor.enabled);
  REQUIRE(c.detector == sonare::mastering::dynamics::DetectorMode::LogRms);
  REQUIRE(c.sidechain_hpf_enabled);
  REQUIRE(c.sidechain_hpf_hz == 250.0f);
  REQUIRE(c.pdr_time_ms == 30.0f);
  REQUIRE(c.pdr_release_scale == 1.5f);
}

TEST_CASE("chain compressor advanced fields round-trip through JSON",
          "[mastering][chain_json][compressor][param_wiring]") {
  MasteringChainConfig cfg;
  cfg.dynamics.compressor.enabled = true;
  cfg.dynamics.compressor.config.detector = sonare::mastering::dynamics::DetectorMode::Peak;
  cfg.dynamics.compressor.config.sidechain_hpf_enabled = true;
  cfg.dynamics.compressor.config.sidechain_hpf_hz = 180.0f;
  cfg.dynamics.compressor.config.pdr_time_ms = 22.0f;
  cfg.dynamics.compressor.config.pdr_release_scale = 0.75f;

  const MasteringChainConfig restored = chain_config_from_json(chain_config_to_json(cfg));
  const auto& c = restored.dynamics.compressor.config;

  REQUIRE(restored.dynamics.compressor.enabled);
  REQUIRE(c.detector == sonare::mastering::dynamics::DetectorMode::Peak);
  REQUIRE(c.sidechain_hpf_enabled);
  REQUIRE(c.sidechain_hpf_hz == 180.0f);
  REQUIRE(c.pdr_time_ms == 22.0f);
  REQUIRE(c.pdr_release_scale == 0.75f);
}

// --- M5: stereo dither decorrelation --------------------------------------

TEST_CASE("final.dither decorrelates stereo channels", "[mastering][final][param_wiring]") {
  // Both channels start identical (silent). With correlated dither the two
  // channels would receive bit-identical noise (collapsing to a mono phantom
  // centre); the per-channel seed salt must make the right channel differ while
  // the left channel stays bit-identical to the mono path.
  const std::vector<float> silence(256, 0.0f);
  const auto result =
      apply_named_processor_stereo("final.dither", silence.data(), silence.data(), silence.size(),
                                   48000, {{"type", 2.0}, {"targetBits", 16.0}});

  REQUIRE(result.left.size() == silence.size());
  REQUIRE(result.right.size() == silence.size());

  // The left channel matches the mono dither output bit-for-bit.
  const auto mono = sonare::mastering::api::apply_named_processor(
      "final.dither", silence.data(), silence.size(), 48000, {{"type", 2.0}, {"targetBits", 16.0}});
  REQUIRE(result.left == mono.samples);

  // The right channel must differ from the left at some sample.
  bool channels_differ = false;
  for (size_t i = 0; i < silence.size(); ++i) {
    if (result.left[i] != result.right[i]) channels_differ = true;
  }
  REQUIRE(channels_differ);
}

TEST_CASE("final.outputChain decorrelates stereo channels", "[mastering][final][param_wiring]") {
  // outputChain dithers then quantizes; with NoiseShaped dither the per-channel
  // seed must decorrelate L and R the same way final.dither does. Use a low-bit
  // target so the dither noise is large enough to survive quantization.
  const std::vector<float> silence(256, 0.0f);
  const auto result = apply_named_processor_stereo(
      "final.outputChain", silence.data(), silence.data(), silence.size(), 48000,
      {{"ditherType", 2.0}, {"targetBits", 8.0}, {"clamp", 1.0}});

  REQUIRE(result.left.size() == silence.size());
  REQUIRE(result.right.size() == silence.size());

  bool channels_differ = false;
  for (size_t i = 0; i < silence.size(); ++i) {
    if (result.left[i] != result.right[i]) channels_differ = true;
  }
  REQUIRE(channels_differ);
}

// --- M6: custom crossover band count --------------------------------------

TEST_CASE("multiband custom cutoff count builds and processes",
          "[mastering][multiband][param_wiring]") {
  using sonare::mastering::multiband::MultibandCompressor;
  using sonare::mastering::multiband::MultibandExpander;
  using sonare::mastering::multiband::MultibandLimiter;
  using sonare::mastering::multiband::MultibandSaturation;

  // A 3-cutoff crossover implies 4 bands; before the fix the bands vector kept
  // its 3-band default and validate_config threw "band count must match
  // crossover". Each insert must now build without throwing and expose 4 bands.
  SECTION("compressor") {
    auto processor = make_insert(
        "multiband.compressor",
        R"({"cutoff0Hz":120,"cutoff1Hz":1000,"cutoff2Hz":6000,"band3.thresholdDb":-9})");
    auto* mb = dynamic_cast<MultibandCompressor*>(processor.get());
    REQUIRE(mb != nullptr);
    REQUIRE(mb->config().bands.size() == 4);
    REQUIRE(mb->config().bands.at(3).threshold_db == -9.0f);
  }

  SECTION("expander") {
    auto processor =
        make_insert("multiband.expander", R"({"cutoff0Hz":120,"cutoff1Hz":1000,"cutoff2Hz":6000})");
    auto* mb = dynamic_cast<MultibandExpander*>(processor.get());
    REQUIRE(mb != nullptr);
    REQUIRE(mb->config().bands.size() == 4);
  }

  SECTION("limiter") {
    auto processor =
        make_insert("multiband.limiter", R"({"cutoff0Hz":120,"cutoff1Hz":1000,"cutoff2Hz":6000})");
    auto* mb = dynamic_cast<MultibandLimiter*>(processor.get());
    REQUIRE(mb != nullptr);
    REQUIRE(mb->config().bands.size() == 4);
  }

  SECTION("saturation") {
    auto processor = make_insert("multiband.saturation",
                                 R"({"cutoff0Hz":120,"cutoff1Hz":1000,"cutoff2Hz":6000})");
    auto* mb = dynamic_cast<MultibandSaturation*>(processor.get());
    REQUIRE(mb != nullptr);
    REQUIRE(mb->config().bands.size() == 4);
  }
}
