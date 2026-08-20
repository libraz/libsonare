#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"
#include "util/exception.h"

using Catch::Matchers::WithinAbs;

namespace sonare::mastering::api {
namespace {
using sonare::test::peak_abs;
using sonare::test::rms;

std::vector<float> create_preset_fixture(int sample_rate, float seconds) {
  const int count = static_cast<int>(static_cast<float>(sample_rate) * seconds);
  std::vector<float> samples(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    const float t = static_cast<float>(index) / static_cast<float>(sample_rate);
    const float env =
        0.55f + 0.35f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 1.7f * t);
    samples[static_cast<std::size_t>(index)] =
        env * (0.18f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 110.0f * t) +
               0.10f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 440.0f * t) +
               0.04f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 1760.0f * t));
  }
  return samples;
}

float mean_abs(const std::vector<float>& samples) {
  double sum = 0.0;
  for (float value : samples) {
    sum += std::abs(value);
  }
  return static_cast<float>(sum / static_cast<double>(samples.size()));
}

MasteringChainConfig make_full_multiband_config(std::size_t band_count) {
  using sonare::mastering::dynamics::CompressorConfig;
  using sonare::mastering::dynamics::DetectorMode;
  using sonare::mastering::multiband::CrossoverMode;
  using sonare::mastering::multiband::CrossoverSlope;

  MasteringChainConfig config;
  auto& multiband = config.dynamics.multiband_comp;
  multiband.enabled = true;
  multiband.config.crossover.cutoffs_hz.clear();
  for (std::size_t index = 1; index < band_count; ++index) {
    multiband.config.crossover.cutoffs_hz.push_back(100.0f * static_cast<float>(index));
  }
  multiband.config.crossover.slope = CrossoverSlope::LR8;
  multiband.config.crossover.mode = CrossoverMode::Bessel;
  multiband.config.crossover.fir_kernel_size = 257;
  multiband.config.bands.clear();
  multiband.config.bands.resize(band_count);
  for (std::size_t index = 0; index < band_count; ++index) {
    CompressorConfig band;
    band.threshold_db = -30.0f + static_cast<float>(index);
    band.ratio = 1.25f + static_cast<float>(index) * 0.25f;
    band.attack_ms = 2.0f + static_cast<float>(index);
    band.release_ms = 40.0f + static_cast<float>(index) * 5.0f;
    band.knee_db = 1.0f + static_cast<float>(index) * 0.5f;
    band.makeup_gain_db = -1.0f + static_cast<float>(index) * 0.25f;
    band.auto_makeup = index % 2 == 0;
    band.detector = static_cast<DetectorMode>(index % 3);
    band.sidechain_hpf_enabled = index % 2 != 0;
    band.sidechain_hpf_hz = 80.0f + static_cast<float>(index) * 10.0f;
    band.pdr_time_ms = static_cast<float>(index) * 3.0f;
    band.pdr_release_scale = 1.0f + static_cast<float>(index) * 0.1f;
    multiband.config.bands[index] = band;
  }
  return config;
}

void replace_once(std::string& text, const std::string& from, const std::string& to) {
  const auto position = text.find(from);
  REQUIRE(position != std::string::npos);
  text.replace(position, from.size(), to);
}

}  // namespace

TEST_CASE("preset_names returns all 25 presets", "[mastering][preset]") {
  auto names = preset_names();
  REQUIRE(names.size() == 25);
  const std::vector<std::string> expected = {
      "pop",     "edm",       "acoustic",    "hipHop",    "aiMusic", "speech", "streaming",
      "youtube", "broadcast", "podcast",     "audiobook", "cinema",  "jpop",   "ambient",
      "lofi",    "classical", "drumAndBass", "techno",    "metal",   "trap",   "rnb",
      "jazz",    "kpop",      "trance",      "gameOst"};
  for (const auto& name : expected) {
    REQUIRE(std::find(names.begin(), names.end(), name) != names.end());
  }
}

TEST_CASE("preset_from_string maps known names", "[mastering][preset]") {
  REQUIRE(preset_from_string("pop") == Preset::Pop);
  REQUIRE(preset_from_string("edm") == Preset::EDM);
  REQUIRE(preset_from_string("acoustic") == Preset::Acoustic);
  REQUIRE(preset_from_string("hipHop") == Preset::HipHop);
  REQUIRE(preset_from_string("aiMusic") == Preset::AIMusic);
  REQUIRE(preset_from_string("speech") == Preset::Speech);
  REQUIRE(preset_from_string("streaming") == Preset::Streaming);
  REQUIRE(preset_from_string("youtube") == Preset::YouTube);
  REQUIRE(preset_from_string("broadcast") == Preset::Broadcast);
  REQUIRE(preset_from_string("podcast") == Preset::Podcast);
  REQUIRE(preset_from_string("audiobook") == Preset::Audiobook);
  REQUIRE(preset_from_string("cinema") == Preset::Cinema);
  REQUIRE(preset_from_string("jpop") == Preset::JPop);
  REQUIRE(preset_from_string("ambient") == Preset::Ambient);
  REQUIRE(preset_from_string("lofi") == Preset::Lofi);
  REQUIRE(preset_from_string("classical") == Preset::Classical);
  REQUIRE(preset_from_string("drumAndBass") == Preset::DrumAndBass);
  REQUIRE(preset_from_string("techno") == Preset::Techno);
  REQUIRE(preset_from_string("metal") == Preset::Metal);
  REQUIRE(preset_from_string("trap") == Preset::Trap);
  REQUIRE(preset_from_string("rnb") == Preset::RnB);
  REQUIRE(preset_from_string("jazz") == Preset::Jazz);
  REQUIRE(preset_from_string("kpop") == Preset::KPop);
  REQUIRE(preset_from_string("trance") == Preset::Trance);
  REQUIRE(preset_from_string("gameOst") == Preset::GameOst);
  REQUIRE_THROWS_AS(preset_from_string("invalid"), sonare::SonareException);
}

TEST_CASE("preset_to_string round-trips", "[mastering][preset]") {
  for (const auto& name : preset_names()) {
    Preset preset = preset_from_string(name);
    REQUIRE(std::string(preset_to_string(preset)) == name);
  }
}

TEST_CASE("preset chain configs round-trip through JSON", "[mastering][preset][json]") {
  for (const auto& name : preset_names()) {
    CAPTURE(name);
    const auto config = preset_config(preset_from_string(name));
    const std::string json = chain_config_to_json(config);
    const auto parsed = chain_config_from_json(json);
    REQUIRE(chain_config_to_json(parsed) == json);
  }
}

TEST_CASE("default multiband chain keeps the canonical v1 JSON", "[mastering][preset][json]") {
  const auto config = MasteringChainConfig{};
  const std::string expected =
      "{\"params\":{\"dynamics.compressor.attackMs\":10,\"dynamics.compressor.autoMakeup\":false,"
      "\"dynamics.compressor.detector\":1,\"dy"
      "namics.compressor.enabled\":false,\"dynamics.compressor.kneeDb\":0,\"dynamics.compressor."
      "makeupGainDb\":0,\"dynamics.compresso"
      "r.pdrReleaseScale\":1,\"dynamics.compressor.pdrTimeMs\":0,\"dynamics.compressor.ratio\":2,"
      "\"dynamics.compressor.releaseMs\":100"
      ",\"dynamics.compressor.sidechainHpfEnabled\":false,\"dynamics.compressor.sidechainHpfHz\":"
      "100,\"dynamics.compressor.threshold"
      "Db\":-18,\"dynamics.deesser.attackMs\":1,\"dynamics.deesser.bandpassQ\":1.5,\"dynamics."
      "deesser.enabled\":false,\"dynamics.deesse"
      "r.frequencyHz\":6000,\"dynamics.deesser.rangeDb\":12,\"dynamics.deesser.ratio\":4,"
      "\"dynamics.deesser.releaseMs\":60,\"dynamics.d"
      "eesser.thresholdDb\":-24,\"dynamics.multibandComp.enabled\":false,\"dynamics.multibandComp."
      "highAttackMs\":10,\"dynamics.multib"
      "andComp.highCutoffHz\":2000,\"dynamics.multibandComp.highRatio\":2,\"dynamics.multibandComp."
      "highReleaseMs\":100,\"dynamics.mul"
      "tibandComp.highThresholdDb\":-18,\"dynamics.multibandComp.lowAttackMs\":10,\"dynamics."
      "multibandComp.lowCutoffHz\":120,\"dynami"
      "cs.multibandComp.lowRatio\":2,\"dynamics.multibandComp.lowReleaseMs\":100,\"dynamics."
      "multibandComp.lowThresholdDb\":-18,\"dyna"
      "mics.multibandComp.midAttackMs\":10,\"dynamics.multibandComp.midRatio\":2,\"dynamics."
      "multibandComp.midReleaseMs\":100,\"dynami"
      "cs.multibandComp.midThresholdDb\":-18,\"dynamics.transientShaper.attackGainDb\":3,"
      "\"dynamics.transientShaper.enabled\":false,"
      "\"dynamics.transientShaper.fastAttackMs\":0,\"dynamics.transientShaper.fastReleaseMs\":20,"
      "\"dynamics.transientShaper.gainSmoo"
      "thingMs\":0,\"dynamics.transientShaper.lookaheadMs\":0,\"dynamics.transientShaper."
      "maxGainDb\":12,\"dynamics.transientShaper.se"
      "nsitivity\":1,\"dynamics.transientShaper.slowAttackMs\":15,\"dynamics.transientShaper."
      "slowReleaseMs\":200,\"dynamics.transient"
      "Shaper.sustainGainDb\":0,\"eq.tilt.enabled\":false,\"eq.tilt.pivotHz\":1000,\"eq.tilt."
      "tiltDb\":0,\"loudness.applyGainAtInputRate"
      "\":false,\"loudness.ceilingDb\":-1,\"loudness.enabled\":false,\"loudness.releaseMs\":50,"
      "\"loudness.targetLufs\":-14,\"loudness.tru"
      "ePeakOversample\":4,\"maximizer.truePeakLimiter.applyGainAtInputRate\":false,\"maximizer."
      "truePeakLimiter.ceilingDb\":-1,\"maxi"
      "mizer.truePeakLimiter.enabled\":false,\"maximizer.truePeakLimiter.lookaheadMs\":1,"
      "\"maximizer.truePeakLimiter.oversampleFact"
      "or\":4,\"maximizer.truePeakLimiter.releaseMs\":50,\"repair.declick.enabled\":false,\"repair."
      "declick.lpcOrder\":20,\"repair.decli"
      "ck.maxClickSamples\":8,\"repair.declick.neighborRatio\":4,\"repair.declick.residualRatio\":"
      "8,\"repair.declick.threshold\":0.800"
      "00001192092896,\"repair.declip.clipThreshold\":0.98000001907348633,\"repair.declip."
      "enabled\":false,\"repair.declip.iterations"
      "\":2,\"repair.declip.lpcBlend\":0.64999997615814209,\"repair.declip.lpcOrder\":36,\"repair."
      "decrackle.enabled\":false,\"repair.de"
      "crackle.levels\":4,\"repair.decrackle.mode\":0,\"repair.decrackle.threshold\":0."
      "40000000596046448,\"repair.dehum.adaptation\":0"
      ".25,\"repair.dehum.adaptive\":false,\"repair.dehum.enabled\":false,\"repair.dehum."
      "frameSize\":2048,\"repair.dehum.fundamentalHz"
      "\":50,\"repair.dehum.harmonics\":4,\"repair.dehum.pllBandwidth\":0.0099999997764825821,"
      "\"repair.dehum.q\":20,\"repair.dehum.sear"
      "chRangeHz\":2,\"repair.denoise.ddAlpha\":0.98000001907348633,\"repair.denoise.enabled\":"
      "false,\"repair.denoise.gainFloor\":0.05"
      "000000074505806,\"repair.denoise.gainSmoothing\":true,\"repair.denoise.hopLength\":256,"
      "\"repair.denoise.mode\":0,\"repair.denoi"
      "se.nFft\":1024,\"repair.denoise.noiseEstimationQuantile\":0.10000000149011612,\"repair."
      "denoise.noiseEstimator\":0,\"repair.den"
      "oise.overSubtraction\":2,\"repair.denoise.spectralFloor\":0.05000000074505806,\"repair."
      "denoise.speechPresenceGain\":true,\"rep"
      "air.dereverb.attenuation\":0.5,\"repair.dereverb.enabled\":false,\"repair.dereverb."
      "hopLength\":256,\"repair.dereverb.lateDelay"
      "Ms\":50,\"repair.dereverb.nFft\":1024,\"repair.dereverb.overSubtraction\":1,\"repair."
      "dereverb.spectralFloor\":0.079999998211860"
      "657,\"repair.dereverb.t60Sec\":0.40000000596046448,\"repair.dereverb.threshold\":0."
      "05000000074505806,\"repair.dereverb.wpeEna"
      "bled\":false,\"repair.dereverb.wpeIterations\":2,\"repair.dereverb.wpeStrength\":0."
      "69999998807907104,\"repair.dereverb.wpeTaps"
      "\":3,\"saturation.exciter.aliasing\":0,\"saturation.exciter.amount\":0.25,\"saturation."
      "exciter.driveDb\":6,\"saturation."
      "exciter.enabled\":false,\"saturation.excit"
      "er.evenOddMix\":0.5,\"saturation.exciter.frequencyHz\":3000,\"saturation.exciter.q\":1,"
      "\"saturation.tape.bias\":0,\"saturation.t"
      "ape.driveDb\":3,\"saturation.tape.enabled\":false,\"saturation.tape.gapLoss\":0."
      "20000000298023224,\"saturation.tape.headBumpDb"
      "\":1.5,\"saturation.tape.hysteresis\":0.20000000298023224,\"saturation.tape.outputGainDb\":"
      "0,\"saturation.tape.oversampleFactor\":1,\"saturation.tape.saturation\":0.5"
      ",\"saturation.tape.speedIps\":15,\"spectral.airBand.amount\":0.25,\"spectral.airBand."
      "dynamicRangeDb\":3,\"spectral.airBand.dyna"
      "micThresholdDb\":-36,\"spectral.airBand.enabled\":false,\"spectral.airBand."
      "shelfFrequencyHz\":12000,\"stereo.imager.decorrelat"
      "ionAmount\":0,\"stereo.imager.enabled\":false,\"stereo.imager.outputGainDb\":0,\"stereo."
      "imager.preserveEnergy\":true,\"stereo.im"
      "ager.width\":1,\"stereo.monoMaker.amount\":1,\"stereo.monoMaker.enabled\":false,\"stereo."
      "monoMaker.frequencyHz\":120},\"version\""
      ":1}";
  REQUIRE(chain_config_to_json(config) == expected);
  REQUIRE(chain_config_to_json(chain_config_from_json(expected)) == expected);
}

TEST_CASE("v2 multiband JSON preserves four and five full compressor bands",
          "[mastering][preset][json]") {
  for (const std::size_t band_count : {std::size_t{4}, std::size_t{5}}) {
    CAPTURE(band_count);
    const auto config = make_full_multiband_config(band_count);
    const std::string json = chain_config_to_json(config);
    REQUIRE(json.find("\"version\":2") != std::string::npos);
    REQUIRE(json.find("\"dynamics.multibandComp\":{") != std::string::npos);
    REQUIRE(json.find("\"dynamics.multibandComp.lowRatio\"") == std::string::npos);

    const auto restored = chain_config_from_json(json);
    const auto& expected = config.dynamics.multiband_comp;
    const auto& actual = restored.dynamics.multiband_comp;
    REQUIRE(actual.enabled == expected.enabled);
    REQUIRE(actual.config.crossover == expected.config.crossover);
    REQUIRE(actual.config.bands.size() == expected.config.bands.size());
    for (std::size_t index = 0; index < expected.config.bands.size(); ++index) {
      const auto& expected_band = expected.config.bands[index];
      const auto& actual_band = actual.config.bands[index];
      REQUIRE(actual_band.threshold_db == expected_band.threshold_db);
      REQUIRE(actual_band.ratio == expected_band.ratio);
      REQUIRE(actual_band.attack_ms == expected_band.attack_ms);
      REQUIRE(actual_band.release_ms == expected_band.release_ms);
      REQUIRE(actual_band.knee_db == expected_band.knee_db);
      REQUIRE(actual_band.makeup_gain_db == expected_band.makeup_gain_db);
      REQUIRE(actual_band.auto_makeup == expected_band.auto_makeup);
      REQUIRE(actual_band.detector == expected_band.detector);
      REQUIRE(actual_band.sidechain_hpf_enabled == expected_band.sidechain_hpf_enabled);
      REQUIRE(actual_band.sidechain_hpf_hz == expected_band.sidechain_hpf_hz);
      REQUIRE(actual_band.pdr_time_ms == expected_band.pdr_time_ms);
      REQUIRE(actual_band.pdr_release_scale == expected_band.pdr_release_scale);
    }
    REQUIRE(chain_config_to_json(restored) == json);
  }
}

TEST_CASE("advanced three-band multiband settings select v2", "[mastering][preset][json]") {
  auto config = MasteringChainConfig{};
  config.dynamics.multiband_comp.config.crossover.slope =
      sonare::mastering::multiband::CrossoverSlope::LR8;
  const std::string json = chain_config_to_json(config);
  REQUIRE(json.find("\"version\":2") != std::string::npos);
  REQUIRE(json.find("\"dynamics.multibandComp.lowCutoffHz\"") == std::string::npos);
  REQUIRE(chain_config_to_json(chain_config_from_json(json)) == json);
}

TEST_CASE("v2 multiband JSON rejects malformed structured fields", "[mastering][preset][json]") {
  const std::string valid = chain_config_to_json(make_full_multiband_config(4));

  SECTION("unknown field") {
    std::string malformed = valid;
    replace_once(malformed, "\"bands\":", "\"unknown\":[] ,\"bands\":");
    REQUIRE_THROWS_AS(chain_config_from_json(malformed), sonare::SonareException);
  }

  SECTION("missing field") {
    REQUIRE_THROWS_AS(chain_config_from_json(
                          R"({"version":2,"params":{"dynamics.multibandComp":{"enabled":true}}})"),
                      sonare::SonareException);
  }

  SECTION("non-finite field") {
    std::string malformed = valid;
    replace_once(malformed, "\"cutoffsHz\":[100,200,300]", "\"cutoffsHz\":[null,200,300]");
    REQUIRE_THROWS_AS(chain_config_from_json(malformed), sonare::SonareException);
  }

  SECTION("fractional enum") {
    std::string malformed = valid;
    replace_once(malformed, "\"slope\":2", "\"slope\":1.5");
    REQUIRE_THROWS_AS(chain_config_from_json(malformed), sonare::SonareException);
  }

  SECTION("non-ascending cutoffs") {
    std::string malformed = valid;
    replace_once(malformed, "\"cutoffsHz\":[100,200,300]", "\"cutoffsHz\":[200,100,300]");
    REQUIRE_THROWS_AS(chain_config_from_json(malformed), sonare::SonareException);
  }

  SECTION("flat and structured multiband keys are not mixed") {
    std::string malformed = valid;
    const auto params_end = malformed.rfind("},\"version\":2}");
    REQUIRE(params_end != std::string::npos);
    malformed.insert(params_end, ",\"dynamics.multibandComp.lowRatio\":2");
    REQUIRE_THROWS_AS(chain_config_from_json(malformed), sonare::SonareException);
  }

  SECTION("writer rejects more than 64 bands") {
    REQUIRE_THROWS_AS(chain_config_to_json(make_full_multiband_config(65)),
                      sonare::SonareException);
  }

  SECTION("writer rejects an oversized FIR kernel") {
    auto config = make_full_multiband_config(4);
    config.dynamics.multiband_comp.config.crossover.fir_kernel_size = 65'537;
    REQUIRE_THROWS_AS(chain_config_to_json(config), sonare::SonareException);
  }
}

TEST_CASE("chain config JSON rejects malformed input", "[mastering][preset][json]") {
  REQUIRE_THROWS_AS(chain_config_from_json("{}"), sonare::SonareException);
  REQUIRE_THROWS_AS(chain_config_from_json("{\"version\":1.5,\"params\":{}}"),
                    sonare::SonareException);
  REQUIRE_THROWS_AS(chain_config_from_json("{\"version\":2,\"params\":{}}"),
                    sonare::SonareException);
  REQUIRE_THROWS_AS(chain_config_from_json("{\"version\":1,\"params\":{\"unknown.key\":1}}"),
                    sonare::SonareException);
  REQUIRE_THROWS_AS(chain_config_from_json("{\"version\":1,\"params\":{\"eq.tilt.enabled\":}}"),
                    sonare::SonareException);
  REQUIRE_THROWS_AS(
      chain_config_from_json("{\"version\":1,\"params\":{\"repair.denoise.noiseEstimator\":99}}"),
      sonare::SonareException);
}

TEST_CASE("chain config JSON rejects unknown top-level keys", "[mastering][preset][json]") {
  // Allowed top-level keys: {"version", "params"}. The validator delegates the
  // check to util::json::schema::has_allowed_keys, which surfaces the offending
  // field name in the error so callers can act on it.
  REQUIRE_THROWS_AS(chain_config_from_json("{\"version\":1,\"params\":{},\"extra\":1}"),
                    sonare::SonareException);
  // Legitimate documents must still parse cleanly — guards against an over-tight
  // allowlist regression.
  REQUIRE_NOTHROW(chain_config_from_json("{\"version\":1,\"params\":{}}"));
}

TEST_CASE("chain config JSON rejects duplicate top-level keys", "[mastering][preset][json]") {
  // Strict parser: a duplicate `"version"` key would otherwise silently take
  // the last-write value and could hide a bumped/incompatible version in the
  // first occurrence. Fail fast instead.
  REQUIRE_THROWS_AS(chain_config_from_json("{\"version\":1,\"version\":2,\"params\":{}}"),
                    sonare::SonareException);
  REQUIRE_THROWS_AS(chain_config_from_json("{\"version\":1,\"params\":{},\"params\":{}}"),
                    sonare::SonareException);
}

TEST_CASE("preset_config(Pop) has expected enabled stages", "[mastering][preset]") {
  auto config = preset_config(Preset::Pop);
  REQUIRE(config.dynamics.compressor.enabled);
  REQUIRE(config.loudness.enabled);
  REQUIRE_THAT(config.loudness.target_lufs, WithinAbs(-14.0f, 1e-6f));
  // The loudness stage's built-in limiter guarantees the ceiling; presets do not
  // additionally enable the standalone maximizer limiter (avoids double limiting).
  REQUIRE_FALSE(config.maximizer.true_peak_limiter.enabled);
}

TEST_CASE("preset_config(AIMusic) enables denoise and air_band", "[mastering][preset]") {
  auto config = preset_config(Preset::AIMusic);
  REQUIRE(config.repair.denoise.enabled);
  REQUIRE(config.spectral.air_band.enabled);
}

TEST_CASE("preset_config(Speech) enables denoise and loudness target=-16", "[mastering][preset]") {
  auto config = preset_config(Preset::Speech);
  REQUIRE(config.repair.denoise.enabled);
  REQUIRE(config.loudness.enabled);
  REQUIRE_THAT(config.loudness.target_lufs, WithinAbs(-16.0f, 1e-6f));
}

TEST_CASE("new platform and genre presets expose planned loudness targets", "[mastering][preset]") {
  struct ExpectedPreset {
    Preset preset;
    float lufs;
    float ceiling;
  };
  const ExpectedPreset expected[] = {
      {Preset::Streaming, -14.0f, -1.0f},  {Preset::YouTube, -14.0f, -1.0f},
      {Preset::Broadcast, -23.0f, -1.0f},  {Preset::Podcast, -16.0f, -1.5f},
      {Preset::Audiobook, -18.0f, -3.0f},  {Preset::Cinema, -27.0f, -2.0f},
      {Preset::JPop, -9.0f, -0.5f},        {Preset::Ambient, -18.0f, -1.0f},
      {Preset::Lofi, -11.0f, -1.0f},       {Preset::Classical, -23.0f, -2.0f},
      {Preset::DrumAndBass, -8.0f, -0.3f}, {Preset::Techno, -9.0f, -0.4f},
      {Preset::Metal, -9.0f, -0.5f},       {Preset::Trap, -9.0f, -0.5f},
      {Preset::RnB, -12.0f, -1.0f},        {Preset::Jazz, -18.0f, -1.5f},
      {Preset::KPop, -8.0f, -0.5f},        {Preset::Trance, -8.5f, -0.4f},
      {Preset::GameOst, -16.0f, -1.0f},
  };

  for (const auto& item : expected) {
    auto config = preset_config(item.preset);
    REQUIRE(config.loudness.enabled);
    REQUIRE_FALSE(config.maximizer.true_peak_limiter.enabled);
    REQUIRE_THAT(config.loudness.target_lufs, WithinAbs(item.lufs, 1e-6f));
    REQUIRE_THAT(config.loudness.ceiling_db, WithinAbs(item.ceiling, 1e-6f));
  }
}

TEST_CASE("new presets enable characteristic stages", "[mastering][preset]") {
  REQUIRE(preset_config(Preset::Podcast).dynamics.deesser.enabled);
  REQUIRE(preset_config(Preset::Audiobook).repair.declick.enabled);
  REQUIRE(preset_config(Preset::Ambient).stereo.imager.config.width > 1.0f);
  REQUIRE(preset_config(Preset::Lofi).saturation.tape.enabled);
  REQUIRE(preset_config(Preset::Classical).dynamics.compressor.config.ratio < 1.3f);
  REQUIRE(preset_config(Preset::DrumAndBass).dynamics.transient_shaper.enabled);
  REQUIRE(preset_config(Preset::Techno).saturation.tape.enabled);
  REQUIRE(preset_config(Preset::Metal).spectral.air_band.enabled);
  REQUIRE(preset_config(Preset::Trap).saturation.tape.config.drive_db > 2.0f);
  REQUIRE(preset_config(Preset::RnB).dynamics.compressor.config.attack_ms >= 10.0f);
  REQUIRE(preset_config(Preset::Jazz).dynamics.compressor.config.ratio < 1.5f);
  REQUIRE(preset_config(Preset::KPop).spectral.air_band.enabled);
  REQUIRE(preset_config(Preset::Trance).stereo.imager.config.width > 1.2f);
  REQUIRE(preset_config(Preset::GameOst).stereo.imager.enabled);
}

TEST_CASE("all 25 presets process a deterministic fixture with valid output",
          "[.][slow][mastering][preset]") {
  constexpr int sample_rate = 44100;
  const auto fixture = create_preset_fixture(sample_rate, 1.25f);

  const auto names = preset_names();
  REQUIRE(names.size() == 25);
  for (const auto& name : names) {
    CAPTURE(name);
    auto result =
        master_audio_mono(preset_from_string(name), fixture.data(), fixture.size(), sample_rate);

    REQUIRE(result.samples.size() == fixture.size());
    REQUIRE_FALSE(result.stages.empty());
    REQUIRE(std::isfinite(result.input_lufs));
    REQUIRE(std::isfinite(result.output_lufs));
    REQUIRE(std::isfinite(result.applied_gain_db));

    const float output_mean_abs = mean_abs(result.samples);
    const float output_rms = rms(result.samples);
    const float output_peak = peak_abs(result.samples);
    REQUIRE(std::isfinite(output_mean_abs));
    REQUIRE(std::isfinite(output_rms));
    REQUIRE(std::isfinite(output_peak));
    REQUIRE(output_mean_abs > 1e-4f);
    REQUIRE(output_rms > 1e-4f);
    REQUIRE(output_peak <= 1.05f);

    bool all_samples_finite = true;
    for (float sample : result.samples) {
      all_samples_finite = all_samples_finite && std::isfinite(sample);
    }
    REQUIRE(all_samples_finite);
  }
}

TEST_CASE("all presets master a three-minute stereo fixture with bounded memory",
          "[.][slow][memory][mastering][preset]") {
  constexpr int sample_rate = 44100;
  const auto left = create_preset_fixture(sample_rate, 3.0f * 60.0f);
  std::vector<float> right = left;
  for (size_t index = 0; index < right.size(); ++index) right[index] *= 0.83f;

  const auto names = preset_names();
  REQUIRE(names.size() == 25);
  for (const auto& name : names) {
    CAPTURE(name);
    const auto result = master_audio_stereo(preset_from_string(name), left.data(), right.data(),
                                            left.size(), sample_rate);
    REQUIRE(result.left.size() == left.size());
    REQUIRE(result.right.size() == right.size());
    REQUIRE(std::isfinite(result.output_lufs));
    REQUIRE(std::all_of(result.left.begin(), result.left.end(),
                        [](float sample) { return std::isfinite(sample); }));
    REQUIRE(std::all_of(result.right.begin(), result.right.end(),
                        [](float sample) { return std::isfinite(sample); }));
  }
}

TEST_CASE("preset_config(Streaming) is an intentional alias of Pop", "[mastering][preset]") {
  // make_streaming() deliberately returns make_pop() (see presets.cpp). This
  // test documents that contract: the two configs must be byte-for-byte equal
  // (compared via their canonical JSON form). If a genuinely distinct streaming
  // voicing is introduced, this assertion is the intended trip-wire to update.
  const auto streaming = preset_config(Preset::Streaming);
  const auto pop = preset_config(Preset::Pop);
  REQUIRE(chain_config_to_json(streaming) == chain_config_to_json(pop));
}

TEST_CASE("master_audio_mono runs Pop preset on dummy signal", "[mastering][preset]") {
  std::vector<float> samples(44100, 0.1f);
  auto result = master_audio_mono(Preset::Pop, samples.data(), samples.size(), 44100);
  REQUIRE(result.samples.size() == samples.size());
  REQUIRE_FALSE(result.stages.empty());
}

TEST_CASE("master_audio_mono applies overrides", "[mastering][preset]") {
  std::vector<float> samples(44100, 0.1f);
  Param overrides[] = {{"loudness.targetLufs", -10.0}};
  REQUIRE_NOTHROW(
      master_audio_mono(Preset::Pop, samples.data(), samples.size(), 44100, overrides, 1));
}

TEST_CASE("apply_chain_config_overrides updates fields in-place", "[mastering][preset]") {
  auto config = preset_config(Preset::Pop);
  REQUIRE_THAT(config.loudness.target_lufs, WithinAbs(-14.0f, 1e-6f));

  Param overrides[] = {{"loudness.targetLufs", -10.0}};
  apply_chain_config_overrides(config, overrides, 1);

  REQUIRE_THAT(config.loudness.target_lufs, WithinAbs(-10.0f, 1e-6f));
  // Pop preset's compressor was already enabled; it should still be enabled
  // since we didn't touch the dynamics module.
  REQUIRE(config.dynamics.compressor.enabled);
}

TEST_CASE("apply_chain_config_overrides can disable a module via enabled=0",
          "[mastering][preset]") {
  auto config = preset_config(Preset::Pop);
  REQUIRE(config.dynamics.compressor.enabled);

  Param overrides[] = {{"dynamics.compressor.enabled", 0.0}};
  apply_chain_config_overrides(config, overrides, 1);

  REQUIRE_FALSE(config.dynamics.compressor.enabled);
}

TEST_CASE("master_audio_stereo runs EDM preset", "[mastering][preset]") {
  std::vector<float> left(22050, 0.1f);
  std::vector<float> right(22050, -0.1f);
  auto result = master_audio_stereo(Preset::EDM, left.data(), right.data(), left.size(), 44100);
  REQUIRE(result.left.size() == left.size());
  REQUIRE(result.right.size() == right.size());
}

TEST_CASE("preset_config(AIMusic) enables full repair trio", "[mastering][preset]") {
  auto config = preset_config(Preset::AIMusic);
  REQUIRE(config.repair.denoise.enabled);
  REQUIRE(config.repair.declick.enabled);
  REQUIRE(config.repair.dereverb.enabled);
}

TEST_CASE("preset_config(Speech) enables deesser", "[mastering][preset]") {
  auto config = preset_config(Preset::Speech);
  REQUIRE(config.dynamics.deesser.enabled);
}

TEST_CASE("preset_config(Pop) enables transient_shaper", "[mastering][preset]") {
  auto config = preset_config(Preset::Pop);
  REQUIRE(config.dynamics.transient_shaper.enabled);
  REQUIRE_THAT(config.dynamics.transient_shaper.config.attack_gain_db, WithinAbs(2.0f, 1e-6f));
}

}  // namespace sonare::mastering::api
