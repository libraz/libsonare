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

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "core/audio.h"
#include "mastering/api/chain.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/processor_params.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/sidechain_router.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/multiband/multiband_compressor.h"
#include "mastering/multiband/multiband_expander.h"
#include "mastering/multiband/multiband_limiter.h"
#include "mastering/multiband/multiband_saturation.h"
#include "mastering/repair/declip.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/hard_clipper.h"
#include "mastering/saturation/soft_clipper.h"
#include "mastering/saturation/tape.h"
#include "mastering/saturation/waveshaper.h"
#include "mastering/spectral/presence_enhancer.h"
#include "rt/aliasing_control.h"
#include "util/constants.h"
#include "util/exception.h"

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

TEST_CASE("mastering flat parameters reject non-finite and unrepresentable values",
          "[mastering][params][numeric]") {
  using sonare::SonareException;

  const double invalid_values[] = {std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity(),
                                   -std::numeric_limits<double>::infinity(), 1.0e300, -1.0e300};
  for (double value : invalid_values) {
    INFO("value=" << value);
    REQUIRE_THROWS_AS(make_map({{"delayMs", value}}), SonareException);
    REQUIRE_THROWS_AS(
        parse_chain_config_params(std::vector<Param>{{"stereo.imager.width", value}}.data(), 1),
        SonareException);
  }

  REQUIRE_THROWS_AS(make_insert("stereo.haasEnhancer", R"({"delayMs":1e300})"), SonareException);
}

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

TEST_CASE("final.dither rejects an out-of-range dither type", "[mastering][final][param_wiring]") {
  // final::DitherType has 4 members (None, Rpdf, Tpdf, NoiseShaped). Before the
  // fix an out-of-range int silently fell through the DSP's if-chain tail and
  // ran as NoiseShaped instead of raising InvalidParameter.
  const std::vector<float> silence(256, 0.0f);
  REQUIRE_THROWS_AS(
      sonare::mastering::api::apply_named_processor("final.dither", silence.data(), silence.size(),
                                                    48000, {{"type", 4.0}, {"targetBits", 16.0}}),
      sonare::SonareException);
}

TEST_CASE("final.outputChain rejects an out-of-range dither type",
          "[mastering][final][param_wiring]") {
  const std::vector<float> silence(256, 0.0f);
  REQUIRE_THROWS_AS(sonare::mastering::api::apply_named_processor(
                        "final.outputChain", silence.data(), silence.size(), 48000,
                        {{"ditherType", 4.0}, {"targetBits", 8.0}, {"clamp", 1.0}}),
                    sonare::SonareException);
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

// --- M-1: standalone loudnessOptimize honored only a subset of its params -----
//
// The per-processor maximizer.loudnessOptimize branch (mono and stereo) dropped
// releaseMs / applyGainAtInputRate, so the standalone optimizer limited with the
// factory defaults while the identical settings inside a chain used the supplied
// values. All three loudness-stage call sites (standalone mono, standalone
// stereo, in-chain) now build the true-peak limiter through the shared
// loudness_limiter_config() helper, so they stay in lockstep on every field.

TEST_CASE("loudness_limiter_config maps all four loudness limiter fields",
          "[mastering][maximizer][param_wiring]") {
  const auto cfg = sonare::mastering::maximizer::loudness_limiter_config(-2.5f, 8, 173.0f, true);
  REQUIRE(cfg.ceiling_db == -2.5f);
  REQUIRE(cfg.oversample_factor == 8);
  REQUIRE(cfg.release_ms == 173.0f);  // dropped by the standalone path before the fix
  REQUIRE(cfg.apply_gain_at_input_rate == true);
}

TEST_CASE("maximizer.loudnessOptimize standalone paths pass every loudness param through",
          "[mastering][maximizer][param_wiring]") {
  constexpr int sr = 48000;
  constexpr std::size_t n = 4096;
  // Quiet sine so loudness normalization applies positive gain and the true-peak
  // limiter actually runs (its release / input-rate path is what release_ms and
  // applyGainAtInputRate select).
  std::vector<float> left(n);
  std::vector<float> right(n);
  for (std::size_t k = 0; k < n; ++k) {
    const float t = static_cast<float>(k) / static_cast<float>(sr);
    left[k] = 0.05f * std::sin(2.0f * 3.14159265358979323846f * 220.0f * t);
    right[k] = 0.05f * std::sin(2.0f * 3.14159265358979323846f * 221.0f * t);
  }

  // Non-default everything, including a release time and the detect-only path
  // the old standalone branch ignored.
  const std::vector<Param> params{{"targetLufs", -16.0},
                                  {"ceilingDb", -1.5},
                                  {"truePeakOversample", 4.0},
                                  {"releaseMs", 200.0},
                                  {"applyGainAtInputRate", 1.0}};

  SECTION("mono path matches a direct loudness_optimize() with the same config") {
    sonare::mastering::maximizer::LoudnessOptimizeConfig config;
    config.target_lufs = -16.0f;
    config.ceiling_db = -1.5f;
    config.true_peak_oversample = 4;
    config.release_ms = 200.0f;
    config.apply_gain_at_input_rate = true;
    const auto direct = sonare::mastering::maximizer::loudness_optimize(
        sonare::Audio::from_buffer(left.data(), n, sr), config);
    const std::vector<float> expected(direct.audio.data(),
                                      direct.audio.data() + direct.audio.size());

    const auto dispatched = sonare::mastering::api::apply_named_processor(
        "maximizer.loudnessOptimize", left.data(), n, sr, params);
    REQUIRE(dispatched.samples == expected);
    REQUIRE(dispatched.latency_samples == 0);
  }

  SECTION("stereo path matches the identical in-chain loudness stage") {
    const auto standalone = apply_named_processor_stereo("maximizer.loudnessOptimize", left.data(),
                                                         right.data(), n, sr, params);

    MasteringChainConfig cfg;
    cfg.loudness.enabled = true;
    cfg.loudness.target_lufs = -16.0f;
    cfg.loudness.ceiling_db = -1.5f;
    cfg.loudness.true_peak_oversample = 4;
    cfg.loudness.release_ms = 200.0f;
    cfg.loudness.apply_gain_at_input_rate = true;
    sonare::mastering::api::MasteringChain chain(cfg);
    const auto chained = chain.process_stereo(left.data(), right.data(), n, sr);

    REQUIRE(standalone.left.size() == chained.left.size());
    REQUIRE(standalone.left == chained.left);
    REQUIRE(standalone.right == chained.right);
    REQUIRE(standalone.latency_samples == 0);
  }
}

// --- Nonlinear stage controls the parameter surface has to carry --------------
//
// The clippers' antialiasing mode, the tape core's oversampling factor and the
// multiband saturator's per-band algorithm and bypass switch are config fields
// the DSP already honours. The flat parameter surface is the only configuration
// path every binding funnels through, so a field it does not read is a field no
// caller can set. These cases drive each field from that surface and check both
// the resulting config and the processed audio.

namespace {

using sonare::mastering::api::apply_named_processor;
using sonare::mastering::api::insert_param_names;
using sonare::mastering::multiband::MultibandSaturation;
using sonare::mastering::multiband::SaturationType;
using sonare::rt::AliasingControl;

constexpr int kNonlinearSampleRate = 22050;
constexpr std::size_t kNonlinearFrames = 5512;

// One tone per band of the default 120 / 2000 Hz crossover, hot enough that a
// saturating stage moves the output well clear of numeric noise.
std::vector<float> three_band_signal() {
  std::vector<float> out(kNonlinearFrames);
  for (std::size_t k = 0; k < kNonlinearFrames; ++k) {
    const float t = static_cast<float>(k) / static_cast<float>(kNonlinearSampleRate);
    out[k] = 0.3f * std::sin(sonare::constants::kTwoPi * 60.0f * t) +
             0.3f * std::sin(sonare::constants::kTwoPi * 800.0f * t) +
             0.3f * std::sin(sonare::constants::kTwoPi * 6000.0f * t);
  }
  return out;
}

std::vector<float> run_insert(const std::string& name, const std::string& json_params,
                              const std::vector<float>& input) {
  auto processor = make_insert(name, json_params);
  REQUIRE(processor != nullptr);
  processor->prepare(static_cast<double>(kNonlinearSampleRate), static_cast<int>(input.size()), 1);
  std::vector<float> out = input;
  float* channels[] = {out.data()};
  processor->process(channels, 1, static_cast<int>(out.size()));
  return out;
}

// Params selecting one antialiasing mode, spelled as the enum's ordinal.
std::string aliasing_json(AliasingControl mode) {
  return R"({"aliasing":)" + std::to_string(static_cast<int>(mode)) + "}";
}

// A declared mode must either configure the processor or be rejected outright.
// Quietly keeping the default — what happened while `aliasing` was absent from
// the parameter surface — is the failure this guards against.
template <typename Processor>
void require_aliasing_reaches(const char* name, AliasingControl mode) {
  CAPTURE(name);
  CAPTURE(static_cast<int>(mode));
  const std::string json = aliasing_json(mode);
  std::unique_ptr<sonare::rt::ProcessorBase> processor;
  try {
    processor = make_insert(name, json);
  } catch (const sonare::SonareException&) {
    return;  // the processor does not implement this mode and says so
  }
  auto* typed = dynamic_cast<Processor*>(processor.get());
  REQUIRE(typed != nullptr);
  REQUIRE(typed->config().aliasing == mode);
}

}  // namespace

TEST_CASE("nonlinear stage controls appear in the published parameter names",
          "[mastering][saturation][multiband][param_wiring]") {
  // insert_param_names() is the discovery surface every binding exposes; a key
  // missing from it is a key no caller can find, whatever the DSP supports.
  const auto contains = [](const std::vector<std::string>& names, const std::string& key) {
    return std::find(names.begin(), names.end(), key) != names.end();
  };

  REQUIRE(contains(insert_param_names("saturation.tape"), "oversampleFactor"));
  REQUIRE(contains(insert_param_names("saturation.hardClipper"), "aliasing"));
  REQUIRE(contains(insert_param_names("saturation.softClipper"), "aliasing"));
  REQUIRE(contains(insert_param_names("saturation.waveshaper"), "aliasing"));
  REQUIRE(contains(insert_param_names("saturation.exciter"), "aliasing"));
  REQUIRE(contains(insert_param_names("spectral.presenceEnhancer"), "aliasing"));

  const auto multiband = insert_param_names("multiband.saturation");
  REQUIRE(contains(multiband, "band0.type"));
  REQUIRE(contains(multiband, "band0.enabled"));
}

TEST_CASE("saturation.tape oversampleFactor is reachable from the parameter surface",
          "[mastering][saturation][param_wiring]") {
  using sonare::mastering::saturation::Tape;

  const auto input = three_band_signal();
  std::vector<std::vector<float>> outputs;
  for (const int factor : {1, 2, 4}) {
    CAPTURE(factor);
    const std::string json = R"({"driveDb":18,"oversampleFactor":)" + std::to_string(factor) + "}";
    auto processor = make_insert("saturation.tape", json);
    auto* tape = dynamic_cast<Tape*>(processor.get());
    REQUIRE(tape != nullptr);
    REQUIRE(tape->config().oversample_factor == factor);

    const auto result = apply_named_processor(
        "saturation.tape", input.data(), input.size(), kNonlinearSampleRate,
        {{"driveDb", 18.0}, {"oversampleFactor", static_cast<double>(factor)}});
    REQUIRE(result.samples.size() == input.size());
    outputs.push_back(result.samples);
  }

  // Latency-compensated by the offline runner, so the difference is the
  // oversampled core, not an alignment shift.
  REQUIRE(outputs.at(0) != outputs.at(1));
  REQUIRE(outputs.at(1) != outputs.at(2));
  REQUIRE(outputs.at(0) != outputs.at(2));
}

TEST_CASE("saturation.tape rejects an oversample factor its core cannot run",
          "[mastering][saturation][param_wiring]") {
  REQUIRE_THROWS_AS(make_insert("saturation.tape", R"({"oversampleFactor":3})"),
                    sonare::SonareException);
}

TEST_CASE("saturation clippers see every declared antialiasing mode",
          "[mastering][saturation][param_wiring]") {
  using sonare::mastering::saturation::HardClipper;
  using sonare::mastering::saturation::SoftClipper;
  using sonare::mastering::saturation::Waveshaper;

  const AliasingControl modes[] = {AliasingControl::None, AliasingControl::Adaa1,
                                   AliasingControl::Adaa2, AliasingControl::Oversample4x};
  for (const AliasingControl mode : modes) {
    require_aliasing_reaches<HardClipper>("saturation.hardClipper", mode);
    require_aliasing_reaches<SoftClipper>("saturation.softClipper", mode);
    require_aliasing_reaches<Waveshaper>("saturation.waveshaper", mode);
  }
}

TEST_CASE("saturation clippers reject an antialiasing mode outside the declared set",
          "[mastering][saturation][param_wiring]") {
  const std::string undeclared =
      std::to_string(static_cast<int>(AliasingControl::Oversample4x) + 1);
  for (const char* name :
       {"saturation.hardClipper", "saturation.softClipper", "saturation.waveshaper"}) {
    CAPTURE(name);
    REQUIRE_THROWS_AS(make_insert(name, R"({"aliasing":)" + undeclared + "}"),
                      sonare::SonareException);
    REQUIRE_THROWS_AS(make_insert(name, R"({"aliasing":-1})"), sonare::SonareException);
  }
}

TEST_CASE("saturation clipper antialiasing mode changes the processed audio",
          "[mastering][saturation][param_wiring]") {
  const auto input = three_band_signal();

  const auto soft_direct =
      run_insert("saturation.softClipper", R"({"driveDb":18,"aliasing":0})", input);
  const auto soft_adaa1 =
      run_insert("saturation.softClipper", R"({"driveDb":18,"aliasing":1})", input);
  REQUIRE(soft_direct != soft_adaa1);

  // A ceiling below the signal peak so the hard clipper actually clips.
  const auto hard_direct =
      run_insert("saturation.hardClipper", R"({"ceiling":0.2,"aliasing":0})", input);
  const auto hard_adaa2 =
      run_insert("saturation.hardClipper", R"({"ceiling":0.2,"aliasing":2})", input);
  REQUIRE(hard_direct != hard_adaa2);
}

TEST_CASE("harmonic generators reach the antialiasing modes they implement",
          "[mastering][saturation][spectral][param_wiring]") {
  using sonare::mastering::saturation::Exciter;
  using sonare::mastering::spectral::PresenceEnhancer;

  for (const AliasingControl mode : {AliasingControl::None, AliasingControl::Oversample4x}) {
    CAPTURE(static_cast<int>(mode));
    const std::string json = aliasing_json(mode);

    auto exciter_insert = make_insert("saturation.exciter", json);
    auto* exciter = dynamic_cast<Exciter*>(exciter_insert.get());
    REQUIRE(exciter != nullptr);
    REQUIRE(exciter->config().aliasing == mode);

    auto presence_insert = make_insert("spectral.presenceEnhancer", json);
    auto* presence = dynamic_cast<PresenceEnhancer*>(presence_insert.get());
    REQUIRE(presence != nullptr);
    REQUIRE(presence->config().aliasing == mode);
  }
}

TEST_CASE("harmonic generators reject the antialiasing modes they do not implement",
          "[mastering][saturation][spectral][param_wiring]") {
  // Neither harmonic generator has an ADAA antiderivative wired up, so unlike
  // the clippers both Adaa1 and Adaa2 must be refused rather than run as None.
  // The refusal has to arrive as a SonareException, which is what every facade
  // knows how to translate.
  const std::string undeclared =
      std::to_string(static_cast<int>(AliasingControl::Oversample4x) + 1);
  for (const char* name : {"saturation.exciter", "spectral.presenceEnhancer"}) {
    CAPTURE(name);
    REQUIRE_THROWS_AS(make_insert(name, aliasing_json(AliasingControl::Adaa1)),
                      sonare::SonareException);
    REQUIRE_THROWS_AS(make_insert(name, aliasing_json(AliasingControl::Adaa2)),
                      sonare::SonareException);
    REQUIRE_THROWS_AS(make_insert(name, R"({"aliasing":)" + undeclared + "}"),
                      sonare::SonareException);
    REQUIRE_THROWS_AS(make_insert(name, R"({"aliasing":-1})"), sonare::SonareException);
  }
}

TEST_CASE("harmonic generator oversampling changes the processed audio",
          "[mastering][saturation][spectral][param_wiring]") {
  const auto input = three_band_signal();

  const auto exciter_direct =
      run_insert("saturation.exciter", R"({"amount":1,"driveDb":18,"aliasing":0})", input);
  const auto exciter_oversampled =
      run_insert("saturation.exciter", R"({"amount":1,"driveDb":18,"aliasing":3})", input);
  REQUIRE(exciter_direct != exciter_oversampled);

  const auto presence_direct =
      run_insert("spectral.presenceEnhancer", R"({"amount":1,"drive":8,"aliasing":0})", input);
  const auto presence_oversampled =
      run_insert("spectral.presenceEnhancer", R"({"amount":1,"drive":8,"aliasing":3})", input);
  REQUIRE(presence_direct != presence_oversampled);
}

TEST_CASE("dynamics.sidechainRouter lookaheadMs is reachable from the parameter surface",
          "[mastering][dynamics][param_wiring]") {
  // Found by the table coverage guard: SidechainRouterConfig::lookahead_ms had
  // no table row, so the router's lookahead -- and the latency it reports --
  // could not be set from any binding, while the sibling ducking processor
  // exposed the same field.
  using sonare::mastering::dynamics::SidechainRouter;

  auto processor = make_insert("dynamics.sidechainRouter", R"({"lookaheadMs":8})");
  auto* router = dynamic_cast<SidechainRouter*>(processor.get());
  REQUIRE(router != nullptr);
  REQUIRE(router->config().lookahead_ms == 8.0f);

  // The lookahead has to reach the DSP, not just the config: it is what the
  // stage reports as latency.
  processor->prepare(static_cast<double>(kNonlinearSampleRate), 1024, 1);
  REQUIRE(processor->latency_samples() > 0);

  auto without = make_insert("dynamics.sidechainRouter", "{}");
  REQUIRE(without != nullptr);
  without->prepare(static_cast<double>(kNonlinearSampleRate), 1024, 1);
  REQUIRE(without->latency_samples() == 0);
}

TEST_CASE("saturation.exciter aliasing survives a chain JSON round trip",
          "[mastering][chain_json][saturation][param_wiring]") {
  // The exciter is a chain stage, so its key has to survive serialization as
  // well as construction; a dropped key would silently reset it to None.
  MasteringChainConfig cfg;
  cfg.saturation.exciter.enabled = true;
  cfg.saturation.exciter.config.aliasing = AliasingControl::Oversample4x;

  const MasteringChainConfig restored = chain_config_from_json(chain_config_to_json(cfg));
  REQUIRE(restored.saturation.exciter.config.aliasing == AliasingControl::Oversample4x);

  const std::vector<Param> params{
      {"saturation.exciter.enabled", 1.0},
      {"saturation.exciter.aliasing", static_cast<int>(AliasingControl::Oversample4x)}};
  const MasteringChainConfig parsed = parse_chain_config_params(params.data(), params.size());
  REQUIRE(parsed.saturation.exciter.config.aliasing == AliasingControl::Oversample4x);
}

TEST_CASE("multiband.saturation per-band type reaches each band",
          "[mastering][multiband][param_wiring]") {
  auto processor =
      make_insert("multiband.saturation", R"({"band0.type":1,"band1.type":2,"band2.type":3})");
  auto* mb = dynamic_cast<MultibandSaturation*>(processor.get());
  REQUIRE(mb != nullptr);
  REQUIRE(mb->config().bands.at(0).type == SaturationType::Tape);
  REQUIRE(mb->config().bands.at(1).type == SaturationType::Tube);
  REQUIRE(mb->config().bands.at(2).type == SaturationType::Exciter);

  auto defaulted = make_insert("multiband.saturation", "{}");
  auto* mb_default = dynamic_cast<MultibandSaturation*>(defaulted.get());
  REQUIRE(mb_default != nullptr);
  REQUIRE(mb_default->config().bands.at(0).type == SaturationType::SoftClip);
}

TEST_CASE("multiband.saturation rejects a band type outside the declared set",
          "[mastering][multiband][param_wiring]") {
  const std::string undeclared = std::to_string(static_cast<int>(SaturationType::Exciter) + 1);
  REQUIRE_THROWS_AS(make_insert("multiband.saturation", R"({"band0.type":)" + undeclared + "}"),
                    sonare::SonareException);
  REQUIRE_THROWS_AS(make_insert("multiband.saturation", R"({"band0.type":-1})"),
                    sonare::SonareException);
}

TEST_CASE("multiband.saturation band algorithms each produce a different output",
          "[mastering][multiband][param_wiring]") {
  const auto input = three_band_signal();
  std::vector<std::vector<float>> outputs;
  for (int type = 0; type <= static_cast<int>(SaturationType::Exciter); ++type) {
    outputs.push_back(
        run_insert("multiband.saturation",
                   R"({"band1.driveDb":18,"band1.type":)" + std::to_string(type) + "}", input));
  }

  for (std::size_t a = 0; a + 1 < outputs.size(); ++a) {
    for (std::size_t b = a + 1; b < outputs.size(); ++b) {
      CAPTURE(a, b);
      REQUIRE(outputs.at(a) != outputs.at(b));
    }
  }
}

TEST_CASE("multiband.saturation band enabled=false keeps the band out of the sum",
          "[mastering][multiband][param_wiring]") {
  auto processor = make_insert("multiband.saturation", R"({"band0.enabled":0})");
  auto* mb = dynamic_cast<MultibandSaturation*>(processor.get());
  REQUIRE(mb != nullptr);
  REQUIRE_FALSE(mb->config().bands.at(0).enabled);
  REQUIRE(mb->config().bands.at(1).enabled);

  // A disabled band contributes nothing of its own: its drive cannot reach the
  // output at all, while the same drive on the enabled band does.
  const auto input = three_band_signal();
  const auto off_driven =
      run_insert("multiband.saturation", R"({"band1.enabled":0,"band1.driveDb":24})", input);
  const auto off_clean =
      run_insert("multiband.saturation", R"({"band1.enabled":0,"band1.driveDb":0})", input);
  const auto on_driven =
      run_insert("multiband.saturation", R"({"band1.enabled":1,"band1.driveDb":24})", input);

  REQUIRE(off_driven == off_clean);
  REQUIRE(off_driven != on_driven);
}
