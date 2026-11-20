#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/json.h"

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
    band.sidechain_high_pass_enabled = index % 2 != 0;
    band.sidechain_high_pass_hz = 80.0f + static_cast<float>(index) * 10.0f;
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

// The refusal carries @p code (a refused argument by default) and exactly @p message.
template <typename Fn>
void require_refused_with(Fn&& call, const std::string& message,
                          sonare::ErrorCode code = sonare::ErrorCode::InvalidParameter) {
  try {
    call();
    FAIL("expected a refusal");
  } catch (const sonare::SonareException& error) {
    CHECK(error.code() == code);
    CHECK(std::string(error.what()) == message);
  }
}

void require_same_multiband(const MultibandCompStage& actual, const MultibandCompStage& expected) {
  REQUIRE(actual.enabled == expected.enabled);
  REQUIRE(actual.config.crossover == expected.config.crossover);
  REQUIRE(actual.config.bands.size() == expected.config.bands.size());
  for (std::size_t index = 0; index < expected.config.bands.size(); ++index) {
    CAPTURE(index);
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
    REQUIRE(actual_band.sidechain_high_pass_enabled == expected_band.sidechain_high_pass_enabled);
    REQUIRE(actual_band.sidechain_high_pass_hz == expected_band.sidechain_high_pass_hz);
    REQUIRE(actual_band.pdr_time_ms == expected_band.pdr_time_ms);
    REQUIRE(actual_band.pdr_release_scale == expected_band.pdr_release_scale);
  }
}

// A v2 document whose multiband stage carries @p band_count bands; built by
// editing a writer document, since the writer refuses a count outside 2..64.
std::string v2_document_with_band_count(std::size_t band_count) {
  const std::size_t source_count = std::clamp<std::size_t>(band_count, 2, 64);
  auto root = sonare::util::json::parse_strict(
      chain_config_to_json(make_full_multiband_config(source_count)));
  auto& stage = root.as_object().at("params").as_object().at("dynamics.multibandComp").as_object();
  auto& cutoffs = stage.at("crossover").as_object().at("cutoffsHz").as_array();
  auto& bands = stage.at("bands").as_array();
  while (bands.size() > band_count) {
    bands.pop_back();
    cutoffs.pop_back();
  }
  while (bands.size() < band_count) {
    bands.push_back(bands.back());
  }
  return sonare::util::json::dump(root);
}

}  // namespace

TEST_CASE("preset_names returns all 30 presets", "[mastering][preset]") {
  auto names = preset_names();
  REQUIRE(names.size() == 30);
  const std::vector<std::string> expected = {
      "pop",       "edm",     "acoustic",  "hipHop",         "aiMusic",     "speech",
      "streaming", "youtube", "broadcast", "podcast",        "audiobook",   "cinema",
      "jpop",      "ambient", "lofi",      "classical",      "drumAndBass", "techno",
      "metal",     "trap",    "rnb",       "jazz",           "kpop",        "trance",
      "gameOst",   "vinyl",   "tapeHiss",  "fieldRecording", "voiceMemo",   "shellac78"};
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
  REQUIRE(preset_from_string("vinyl") == Preset::Vinyl);
  REQUIRE(preset_from_string("tapeHiss") == Preset::TapeHiss);
  REQUIRE(preset_from_string("fieldRecording") == Preset::FieldRecording);
  REQUIRE(preset_from_string("voiceMemo") == Preset::VoiceMemo);
  REQUIRE(preset_from_string("shellac78") == Preset::Shellac78);
  REQUIRE_THROWS_AS(preset_from_string("invalid"), sonare::SonareException);
}

TEST_CASE("preset_to_string round-trips", "[mastering][preset]") {
  for (const auto& name : preset_names()) {
    Preset preset = preset_from_string(name);
    REQUIRE(std::string(preset_to_string(preset)) == name);
  }
}

TEST_CASE("preset_kind matches loudness.enabled, exactly five restoration presets",
          "[mastering][preset]") {
  std::size_t restoration_count = 0;
  for (const auto& name : preset_names()) {
    CAPTURE(name);
    const auto preset = preset_from_string(name);
    const auto config = preset_config(preset);
    const bool is_mastering = preset_kind(preset) == PresetKind::Mastering;
    REQUIRE(is_mastering == config.loudness.enabled);
    if (!is_mastering) ++restoration_count;
  }
  REQUIRE(restoration_count == 5);
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

TEST_CASE("the default chain document is version 2 with tokens and a structured multiband record",
          "[mastering][preset][json]") {
  const auto config = MasteringChainConfig{};
  const std::string json = chain_config_to_json(config);
  const auto root = sonare::util::json::parse_strict(json);
  REQUIRE(root["version"].as_int() == 2);
  const auto& params = root["params"].as_object();

  REQUIRE(params.at("dynamics.compressor.detector").as_string() == "rms");
  REQUIRE(params.at("repair.decrackle.mode").is_string());
  REQUIRE(params.at("repair.dehum.mode").is_string());
  REQUIRE(params.at("repair.denoise.mode").is_string());
  REQUIRE(params.at("repair.denoise.noiseEstimator").is_string());
  for (const auto& [key, value] : params) {
    CAPTURE(key);
    REQUIRE_FALSE(value.is_null());
    REQUIRE(key.find("cutoff0Hz") == std::string::npos);
    REQUIRE(key.find("cutoff1Hz") == std::string::npos);
  }

  const auto& multiband = params.at("dynamics.multibandComp").as_object();
  const auto& cutoffs = multiband.at("crossover").as_object().at("cutoffsHz").as_array();
  REQUIRE(cutoffs.size() == 2);
  REQUIRE(cutoffs[0].as_number() == 120.0);
  REQUIRE(cutoffs[1].as_number() == 2000.0);
  REQUIRE(multiband.at("bands").as_array().size() == 3);

  REQUIRE(chain_config_to_json(chain_config_from_json(json)) == json);
}

TEST_CASE("v2 multiband JSON preserves four and five full compressor bands",
          "[mastering][preset][json]") {
  for (const std::size_t band_count : {std::size_t{4}, std::size_t{5}}) {
    CAPTURE(band_count);
    const auto config = make_full_multiband_config(band_count);
    const std::string json = chain_config_to_json(config);
    REQUIRE(json.find("\"version\":2") != std::string::npos);
    REQUIRE(json.find("\"dynamics.multibandComp\":{") != std::string::npos);
    REQUIRE(json.find("\"dynamics.multibandComp.bands.0.ratio\"") == std::string::npos);

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
      REQUIRE(actual_band.sidechain_high_pass_enabled == expected_band.sidechain_high_pass_enabled);
      // A record carries the corner only while the high-pass is on.
      if (expected_band.sidechain_high_pass_enabled) {
        REQUIRE(actual_band.sidechain_high_pass_hz == expected_band.sidechain_high_pass_hz);
      }
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
  REQUIRE(json.find("\"dynamics.multibandComp.cutoff0Hz\"") == std::string::npos);
  REQUIRE(chain_config_to_json(chain_config_from_json(json)) == json);
}

TEST_CASE("v2 multiband JSON rejects malformed structured fields", "[mastering][preset][json]") {
  const std::string valid = chain_config_to_json(make_full_multiband_config(4));

  SECTION("unknown field") {
    std::string malformed = valid;
    replace_once(malformed, "\"bands\":", "\"unknown\":[] ,\"bands\":");
    REQUIRE_THROWS_AS(chain_config_from_json(malformed), sonare::SonareException);
  }

  SECTION("a partial record reads as its dotted keys") {
    const auto partial =
        chain_config_from_json(R"({"params":{"dynamics.multibandComp":{"enabled":true}}})");
    REQUIRE(partial.dynamics.multiband_comp.enabled);
    REQUIRE(partial.dynamics.multiband_comp.config.bands.size() ==
            MasteringChainConfig{}.dynamics.multiband_comp.config.bands.size());
  }

  SECTION("non-finite field") {
    std::string malformed = valid;
    replace_once(malformed, "\"cutoffsHz\":[100,200,300]", "\"cutoffsHz\":[null,200,300]");
    REQUIRE_THROWS_AS(chain_config_from_json(malformed), sonare::SonareException);
  }

  SECTION("a number for a token") {
    std::string malformed = valid;
    const auto slope = malformed.find("\"slope\":\"");
    REQUIRE(slope != std::string::npos);
    const auto value = slope + std::string("\"slope\":").size();
    malformed.replace(value, malformed.find('"', value + 1) + 1 - value, "1.5");
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
    malformed.insert(params_end, ",\"dynamics.multibandComp.bands.0.ratio\":2");
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

TEST_CASE("a chain document is read the same however its paths are spelled",
          "[mastering][preset][json]") {
  const MasteringChainConfig dotted = chain_config_from_json(
      R"({"params":{"dynamics.compressor.enabled":true,"dynamics.compressor.ratio":3,)"
      R"("dynamics.compressor.detector":"peak"}})");
  const MasteringChainConfig nested = chain_config_from_json(
      R"({"version":2,"params":{"dynamics":{"compressor":{"enabled":true,"ratio":3,)"
      R"("detector":"peak"}}}})");
  CHECK(chain_config_to_json(dotted) == chain_config_to_json(nested));
  CHECK(dotted.dynamics.compressor.config.ratio == 3.0f);
  CHECK(dotted.dynamics.compressor.config.detector == dynamics::DetectorMode::Peak);

  SECTION("the writer names every enum value and states version 2") {
    const std::string json = chain_config_to_json(make_full_multiband_config(4));
    CHECK(json.find("\"version\":2") != std::string::npos);
    CHECK(json.find("\"dynamics.compressor.detector\":\"") != std::string::npos);
    CHECK(json.find("\"repair.denoise.mode\":\"") != std::string::npos);
    CHECK(json.find("\"slope\":\"") != std::string::npos);
    CHECK(json.find("\"mode\":\"") != std::string::npos);
    REQUIRE(chain_config_to_json(chain_config_from_json(json)) == json);
  }
}

TEST_CASE("a chain document refuses what it cannot mean, naming the path",
          "[mastering][preset][json]") {
  const auto refused = [](const std::string& document, const std::string& fragment) {
    INFO(document);
    try {
      (void)chain_config_from_json(document);
      FAIL("expected a refusal");
    } catch (const sonare::SonareException& error) {
      CHECK(error.code() == sonare::ErrorCode::InvalidFormat);
      CHECK_THAT(std::string(error.what()), Catch::Matchers::ContainsSubstring(fragment));
    }
    try {
      (void)chain_config_flat_params(document);
      FAIL("expected the flat reader to refuse too");
    } catch (const sonare::SonareException& error) {
      CHECK(error.code() == sonare::ErrorCode::InvalidFormat);
    }
  };

  refused(R"({"version":1,"params":{}})", "version must be 2, or left out");
  refused(R"({"version":"2","params":{}})", "version must be 2, or left out");
  refused(R"({"params":{"eq.tilt.tiltDb":1,"eq":{"tilt":{"tiltDb":2}}}})",
          "params.eq.tilt.tiltDb must not be given twice");
  refused(R"({"params":{"dynamics.multibandComp.cutoff0Hz":100,)"
          R"("dynamics.multibandComp.crossover.cutoffsHz":[200,300]}})",
          "must not be given twice");
  refused(R"({"params":{"eq.tilt.tiltDb":null}})",
          "params.eq.tilt.tiltDb must be a number, a boolean or a token");
  refused(R"({"params":{"dynamics.compressor.detector":1}})",
          "params.dynamics.compressor.detector must be one of ");
  refused(R"({"params":{"dynamics.compressor.detector":"loud"}})",
          "params.dynamics.compressor.detector must be one of ");
  refused(R"({"params":{"eq.tilt.tiltDb":"high"}})",
          "params.eq.tilt.tiltDb must be a number or a boolean");
  refused(R"({"params":{"repair.declip.clipThresholdDb":0.5}})",
          "params.repair.declip.clipThresholdDb must be finite and in [-890, 0]");
  refused(R"({"params":{"repair.declip.enabled":false,"repair.declip.clipThresholdDb":3}})",
          "params.repair.declip.clipThresholdDb must be finite and in [-890, 0]");
  refused(R"({"params":{"dynamics.multibandComp.lowCutoffHz":200}})",
          "renamed to params.dynamics.multibandComp.crossover.cutoffsHz.0");
  refused(R"({"params":{"dynamics.multibandComp.highCutoffHz":2000}})",
          "renamed to params.dynamics.multibandComp.crossover.cutoffsHz.1");
  refused(R"({"params":{"loudness":{"ceilingDb":-1}}})", "renamed to params.loudness.ceilingDbtp");
  refused(R"({"params":{"dynamics.deesser.bandpassQ":2}})",
          "unknown key params.dynamics.deesser.bandpassQ (renamed to "
          "params.dynamics.deesser.bandPassQ)");
  refused(R"({"params":{"maximizer.truePeakLimiter.ceilingDb":-1}})",
          "renamed to params.maximizer.truePeakLimiter.ceilingDbtp");
  refused(R"({"params":{},"extra":1})", "unknown key extra");

  // Annotation keys pass at any depth, and a non-positive threshold is the declipper's own.
  const MasteringChainConfig annotated =
      chain_config_from_json(R"({"$comment":"x","params":{"x-ui":1,"repair":{"$note":"y","declip":)"
                             R"({"clipThresholdDb":-0.5}}}})");
  CHECK_THAT(sonare::linear_to_db(annotated.repair.declip.config.clip_threshold),
             WithinAbs(-0.5, 1e-4));
}

TEST_CASE("every document entry reads the same document and refuses as a document",
          "[mastering][preset][json]") {
  const MasteringChainConfig over =
      chain_config_overrides_from_json(preset_config(Preset::Pop), R"({"params":{"eq":{"tilt":)"
                                                                   R"({"tiltDb":1.5}}}})");
  CHECK(over.eq.tilt.enabled);
  CHECK(over.eq.tilt.tilt_db == 1.5f);
  CHECK(
      streaming_chain_config_from_json(R"({"params":{}})").repair.denoise.config.noise_estimator ==
      repair::DenoiseNoiseEstimator::Spp);

  for (const auto& read : std::vector<std::function<void(const std::string&)>>{
           [](const std::string& d) { (void)chain_config_from_json(d); },
           [](const std::string& d) { (void)streaming_chain_config_from_json(d); },
           [](const std::string& d) {
             (void)chain_config_overrides_from_json(preset_config(Preset::Pop), d);
           }}) {
    require_refused_with([&] { read(R"({"params":{"eq.tilt.noSuchKey":1}})"); },
                         "unknown key params.eq.tilt.noSuchKey", sonare::ErrorCode::InvalidFormat);
    require_refused_with([&] { read(R"({"params":{"eq.tilt.tiltDb":null}})"); },
                         "params.eq.tilt.tiltDb must be a number, a boolean or a token",
                         sonare::ErrorCode::InvalidFormat);
  }
}

TEST_CASE("a binding's parameter object is refused under the root its caller wrote",
          "[mastering][preset][json]") {
  const ChainParamsOrigin overrides = chain_params_argument("overrides");
  const auto refusal_of = [&](const std::string& document) {
    try {
      (void)chain_config_overrides_from_json(preset_config(Preset::Pop), document, overrides);
    } catch (const sonare::SonareParameterRefusal& refusal) {
      return refusal;
    }
    FAIL("expected a parameter refusal");
    throw std::logic_error("unreachable");
  };

  const sonare::SonareParameterRefusal unknown =
      refusal_of(R"({"params":{"limiter":{"ceilingDb":-1}}})");
  CHECK(unknown.code() == sonare::ErrorCode::InvalidParameter);
  CHECK(unknown.shape_error());
  CHECK(unknown.subject() == "overrides.limiter.ceilingDb");

  const sonare::SonareParameterRefusal token =
      refusal_of(R"({"params":{"repair.denoise.noiseEstimator":"bogus"}})");
  CHECK(token.code() == sonare::ErrorCode::InvalidParameter);
  CHECK_FALSE(token.shape_error());
  CHECK(std::string(token.what()) ==
        "overrides.repair.denoise.noiseEstimator must be one of quantile, mcra, imcra, spp");

  const sonare::SonareParameterRefusal ordinal =
      refusal_of(R"({"params":{"repair.denoise.noiseEstimator":2}})");
  CHECK(ordinal.shape_error());
  CHECK(std::string(ordinal.what()) ==
        "overrides.repair.denoise.noiseEstimator must be one of quantile, mcra, imcra, spp");

  require_refused_with(
      [&] {
        (void)chain_config_from_json(R"({"params":{"eq.tilt.tiltDb":null}})",
                                     chain_params_argument("config"));
      },
      "config.eq.tilt.tiltDb must be a number, a boolean or a token");
}

TEST_CASE("a multiband chain stage carries between two and 64 bands", "[mastering][preset][json]") {
  const std::string too_few =
      "multiband stage needs at least two bands; one band is the compressor stage";
  const std::string too_many = "multiband bands count must be between 2 and 64";

  SECTION("the shared validator") {
    require_refused_with(
        [] {
          validate_chain_multiband_config(
              make_full_multiband_config(1).dynamics.multiband_comp.config);
        },
        too_few);
    require_refused_with(
        [] {
          validate_chain_multiband_config(
              make_full_multiband_config(65).dynamics.multiband_comp.config);
        },
        too_many);
    validate_chain_multiband_config(make_full_multiband_config(2).dynamics.multiband_comp.config);
  }

  SECTION("the JSON writer") {
    require_refused_with([] { chain_config_to_json(make_full_multiband_config(1)); }, too_few);
  }

  SECTION("the JSON reader") {
    const std::string one_band = v2_document_with_band_count(1);
    REQUIRE(one_band.find("\"cutoffsHz\":[]") != std::string::npos);
    // The document reader walks the record as dotted paths, so it names the path it stopped at.
    require_refused_with(
        [&] { chain_config_from_json(one_band); },
        "params.dynamics.multibandComp.crossover.cutoffsHz must not be an empty list",
        sonare::ErrorCode::InvalidFormat);
    require_refused_with(
        [] { chain_config_from_json(v2_document_with_band_count(65)); },
        "params.dynamics.multibandComp.bands.64.attackMs is past the end of the band "
        "list",
        sonare::ErrorCode::InvalidFormat);
    REQUIRE(chain_config_from_json(v2_document_with_band_count(64))
                .dynamics.multiband_comp.config.bands.size() == 64);
  }
}

TEST_CASE("a chain config document's flat params parse to the document's config",
          "[mastering][preset][json]") {
  std::vector<std::pair<std::string, MasteringChainConfig>> cases = {
      {"default", MasteringChainConfig{}},
      {"four bands", make_full_multiband_config(4)},
      {"five bands", make_full_multiband_config(5)},
  };
  for (const auto& name : preset_names()) {
    cases.emplace_back(name, preset_config(preset_from_string(name)));
  }

  for (const auto& [name, source] : cases) {
    CAPTURE(name);
    const std::string text = chain_config_to_json(source);
    const auto root = sonare::util::json::parse_strict(text);
    REQUIRE(root["version"].as_int() == 2);

    const auto flat = chain_config_flat_params(text);
    REQUIRE_FALSE(flat.empty());
    std::vector<Param> params;
    params.reserve(flat.size());
    for (const auto& entry : flat) {
      CAPTURE(entry.key);
      if (entry.is_bool) REQUIRE((entry.value == 0.0 || entry.value == 1.0));
      params.push_back(Param{entry.key, entry.value});
    }
    const auto enabled = std::find_if(flat.begin(), flat.end(), [](const auto& entry) {
      return entry.key == "dynamics.multibandComp.enabled";
    });
    REQUIRE(enabled != flat.end());
    REQUIRE(enabled->is_bool);
    const auto cutoff = std::find_if(flat.begin(), flat.end(), [](const auto& entry) {
      return entry.key == "dynamics.multibandComp.crossover.cutoffsHz.0";
    });
    REQUIRE(cutoff != flat.end());
    REQUIRE_FALSE(cutoff->is_bool);

    const auto from_flat = parse_chain_config_params(params.data(), params.size());
    const auto from_json = chain_config_from_json(text);
    require_same_multiband(from_flat.dynamics.multiband_comp, from_json.dynamics.multiband_comp);
    // The canonical writer emits every stage's every field, so equal text is equal fields.
    REQUIRE(chain_config_to_json(from_flat) == chain_config_to_json(from_json));
    REQUIRE(chain_config_to_json(from_flat) == text);
  }
}

TEST_CASE("chain config flat params refuse what has no flat spelling",
          "[mastering][preset][json]") {
  const std::string no_params = "params must be given";
  require_refused_with([] { chain_config_flat_params(R"({"version":2})"); }, no_params,
                       sonare::ErrorCode::InvalidFormat);
  require_refused_with([] { chain_config_flat_params(R"({"params":[1]})"); },
                       "params must be an object", sonare::ErrorCode::InvalidFormat);
  REQUIRE(chain_config_flat_params(R"({"params":{}})").empty());

  require_refused_with([] { chain_config_flat_params(R"({"params":{"eq.tilt.tiltDb":"steep"}})"); },
                       "params.eq.tilt.tiltDb must be a number or a boolean",
                       sonare::ErrorCode::InvalidFormat);
  require_refused_with([] { chain_config_flat_params(R"({"params":{"eq.tilt.tiltDb":null}})"); },
                       "params.eq.tilt.tiltDb must be a number, a boolean or a token",
                       sonare::ErrorCode::InvalidFormat);
  require_refused_with(
      [] {
        chain_config_flat_params(
            R"({"params":{"dynamics.multibandComp":{"bands":[{"ratio":"x"}]}}})");
      },
      "params.dynamics.multibandComp.bands.0.ratio must be a number or a boolean",
      sonare::ErrorCode::InvalidFormat);
  require_refused_with(
      [] {
        chain_config_flat_params(
            R"({"params":{"dynamics.multibandComp":{"crossover":{"cutoffsHz":[]}}}})");
      },
      "params.dynamics.multibandComp.crossover.cutoffsHz must not be an empty list",
      sonare::ErrorCode::InvalidFormat);
}

TEST_CASE("chain config flat params resolve an enum name as the validating reader does",
          "[mastering][preset][json]") {
  const std::string text =
      R"({"params":{"repair.denoise.enabled":true,"repair.denoise.noiseEstimator":"mcra"}})";
  const auto flat = chain_config_flat_params(text);
  REQUIRE(flat.size() == 2u);
  REQUIRE(flat[1].key == "repair.denoise.noiseEstimator");
  REQUIRE_FALSE(flat[1].is_bool);
  const auto expected = mastering_enum_value("", "repair.denoise.noiseEstimator", "mcra");
  REQUIRE(expected.has_value());
  REQUIRE(flat[1].value == *expected);
  REQUIRE_THROWS_AS(chain_config_flat_params(R"({"params":{"repair.denoise.noiseEstimator":"x"}})"),
                    sonare::SonareException);
}

TEST_CASE("chain config JSON rejects malformed input", "[mastering][preset][json]") {
  REQUIRE_THROWS_AS(chain_config_from_json("{}"), sonare::SonareException);
  REQUIRE_THROWS_AS(chain_config_from_json("{\"version\":1.5,\"params\":{}}"),
                    sonare::SonareException);
  REQUIRE_THROWS_AS(chain_config_from_json("{\"version\":1,\"params\":{}}"),
                    sonare::SonareException);
  REQUIRE_THROWS_AS(chain_config_from_json("{\"version\":2,\"params\":{\"unknown.key\":1}}"),
                    sonare::SonareException);
  REQUIRE_THROWS_AS(chain_config_from_json("{\"version\":2,\"params\":{\"eq.tilt.enabled\":}}"),
                    sonare::SonareException);
  REQUIRE_THROWS_AS(
      chain_config_from_json("{\"version\":2,\"params\":{\"repair.denoise.noiseEstimator\":99}}"),
      sonare::SonareException);
}

TEST_CASE("chain config JSON rejects unknown top-level keys", "[mastering][preset][json]") {
  // Allowed top-level keys: version, params and annotation keys.
  REQUIRE_THROWS_AS(chain_config_from_json("{\"version\":2,\"params\":{},\"extra\":1}"),
                    sonare::SonareException);
  // Legitimate documents must still parse cleanly — guards against an over-tight
  // allowlist regression.
  REQUIRE_NOTHROW(chain_config_from_json("{\"version\":2,\"params\":{}}"));
}

TEST_CASE("chain config JSON rejects duplicate top-level keys", "[mastering][preset][json]") {
  // Strict parser: a duplicate `"version"` key would otherwise silently take
  // the last-write value and could hide a bumped/incompatible version in the
  // first occurrence. Fail fast instead.
  REQUIRE_THROWS_AS(chain_config_from_json("{\"version\":2,\"version\":2,\"params\":{}}"),
                    sonare::SonareException);
  REQUIRE_THROWS_AS(chain_config_from_json("{\"version\":2,\"params\":{},\"params\":{}}"),
                    sonare::SonareException);
}

TEST_CASE("chain config JSON rejects a document exceeding the JSON byte budget",
          "[mastering][preset][json][resource]") {
  // One JSON string value past the 1 MiB chain-config byte budget. The
  // refusal has to come from the byte precheck, not from the parser walking a
  // document this size into json::Value nodes first.
  const std::string oversized = "{\"pad\":\"" + std::string(1024u * 1024u + 1024u, 'a') + "\"}";
  try {
    chain_config_from_json(oversized);
    FAIL("an over-budget chain config document was accepted");
  } catch (const sonare::SonareException& error) {
    const std::string message = error.what();
    INFO(message);
    REQUIRE(error.code() == sonare::ErrorCode::InvalidFormat);
    REQUIRE(message.find("byte budget") != std::string::npos);
  }

  // An ordinary chain config document still round-trips, so the refusal above
  // is the size and not some other change to chain config parsing.
  const std::string ordinary = chain_config_to_json(MasteringChainConfig{});
  REQUIRE(chain_config_to_json(chain_config_from_json(ordinary)) == ordinary);
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
    REQUIRE_THAT(config.loudness.ceiling_dbtp, WithinAbs(item.ceiling, 1e-6f));
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

TEST_CASE("repair-oriented presets enable only their named repair stages", "[mastering][preset]") {
  auto assert_repair_only = [](const MasteringChainConfig& config) {
    REQUIRE_FALSE(config.eq.tilt.enabled);
    REQUIRE_FALSE(config.dynamics.deesser.enabled);
    REQUIRE_FALSE(config.dynamics.transient_shaper.enabled);
    REQUIRE_FALSE(config.dynamics.compressor.enabled);
    REQUIRE_FALSE(config.dynamics.multiband_comp.enabled);
    REQUIRE_FALSE(config.saturation.tape.enabled);
    REQUIRE_FALSE(config.saturation.exciter.enabled);
    REQUIRE_FALSE(config.spectral.air_band.enabled);
    REQUIRE_FALSE(config.stereo.imager.enabled);
    REQUIRE_FALSE(config.stereo.mono_maker.enabled);
    REQUIRE_FALSE(config.maximizer.true_peak_limiter.enabled);
    REQUIRE_FALSE(config.loudness.enabled);
  };

  const auto vinyl = preset_config(Preset::Vinyl);
  REQUIRE(vinyl.repair.declick.enabled);
  REQUIRE(vinyl.repair.decrackle.enabled);
  REQUIRE(vinyl.repair.denoise.enabled);
  REQUIRE_FALSE(vinyl.repair.declip.enabled);
  REQUIRE_FALSE(vinyl.repair.dehum.enabled);
  REQUIRE_FALSE(vinyl.repair.dereverb.enabled);
  assert_repair_only(vinyl);

  const auto tape_hiss = preset_config(Preset::TapeHiss);
  REQUIRE(tape_hiss.repair.denoise.enabled);
  REQUIRE_FALSE(tape_hiss.repair.declick.enabled);
  REQUIRE_FALSE(tape_hiss.repair.declip.enabled);
  REQUIRE_FALSE(tape_hiss.repair.decrackle.enabled);
  REQUIRE_FALSE(tape_hiss.repair.dehum.enabled);
  REQUIRE_FALSE(tape_hiss.repair.dereverb.enabled);
  assert_repair_only(tape_hiss);

  const auto field_recording = preset_config(Preset::FieldRecording);
  REQUIRE(field_recording.repair.denoise.enabled);
  REQUIRE(field_recording.repair.dehum.enabled);
  REQUIRE(field_recording.repair.dehum.config.adaptive);
  REQUIRE(field_recording.repair.dereverb.enabled);
  REQUIRE_FALSE(field_recording.repair.declick.enabled);
  REQUIRE_FALSE(field_recording.repair.declip.enabled);
  REQUIRE_FALSE(field_recording.repair.decrackle.enabled);
  assert_repair_only(field_recording);

  const auto voice_memo = preset_config(Preset::VoiceMemo);
  REQUIRE(voice_memo.repair.declip.enabled);
  REQUIRE(voice_memo.repair.denoise.enabled);
  REQUIRE(voice_memo.repair.dereverb.enabled);
  REQUIRE_FALSE(voice_memo.repair.declick.enabled);
  REQUIRE_FALSE(voice_memo.repair.decrackle.enabled);
  REQUIRE_FALSE(voice_memo.repair.dehum.enabled);
  assert_repair_only(voice_memo);

  const auto shellac78 = preset_config(Preset::Shellac78);
  REQUIRE(shellac78.repair.declick.enabled);
  REQUIRE(shellac78.repair.decrackle.enabled);
  REQUIRE(shellac78.repair.denoise.enabled);
  REQUIRE_FALSE(shellac78.repair.declip.enabled);
  REQUIRE_FALSE(shellac78.repair.dehum.enabled);
  REQUIRE_FALSE(shellac78.repair.dereverb.enabled);
  assert_repair_only(shellac78);

  // shellac78 deviates from its defaults where vinyl does not: heavier damage
  // needs a wider click cap, a more sensitive crackle threshold, and a deeper
  // noise floor.
  REQUIRE(shellac78.repair.declick.config.max_click_samples >
          vinyl.repair.declick.config.max_click_samples);
  REQUIRE(shellac78.repair.decrackle.config.threshold < vinyl.repair.decrackle.config.threshold);
  REQUIRE(shellac78.repair.denoise.config.reduction_db > vinyl.repair.denoise.config.reduction_db);
}

TEST_CASE("all 30 presets process a deterministic fixture with valid output",
          "[.][slow][mastering][preset]") {
  constexpr int sample_rate = 44100;
  const auto fixture = create_preset_fixture(sample_rate, 1.25f);

  const auto names = preset_names();
  REQUIRE(names.size() == 30);
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
  REQUIRE(names.size() == 30);
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

TEST_CASE("every preset's params JSON is version 2", "[mastering][preset][json]") {
  for (const auto& name : preset_names()) {
    CAPTURE(name);
    const auto config = preset_config(preset_from_string(name));
    const std::string json = chain_config_to_json(config);
    const auto root = sonare::util::json::parse_strict(json);
    REQUIRE(root["version"].as_int() == 2);
  }
}

TEST_CASE("applying a preset's own params JSON as overrides is a no-op",
          "[mastering][preset][json]") {
  // masteringPresetParams(preset) is chain_config_to_json(preset_config(preset)).
  // Re-applying that exact document as overrides on top of the same
  // preset must change nothing: every value it carries is already the value
  // the preset set.
  constexpr int sample_rate = 44100;
  const auto fixture = create_preset_fixture(sample_rate, 0.5f);

  for (const auto& name : preset_names()) {
    CAPTURE(name);
    const auto preset = preset_from_string(name);
    const std::string json = chain_config_to_json(preset_config(preset));
    const auto flat = chain_config_flat_params(json);

    std::vector<Param> overrides;
    overrides.reserve(flat.size());
    for (const auto& entry : flat) {
      overrides.push_back(Param{entry.key, entry.value});
    }

    const auto baseline = master_audio_mono(preset, fixture.data(), fixture.size(), sample_rate);
    const auto overridden = master_audio_mono(preset, fixture.data(), fixture.size(), sample_rate,
                                              overrides.data(), overrides.size());

    REQUIRE(overridden.samples.size() == baseline.samples.size());
    REQUIRE(overridden.samples == baseline.samples);
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
  // A dropped override is a silent no-op, so assert the override moved the
  // achieved loudness AND moved it the right way: "merely different" also
  // passes for a sign error.
  const std::vector<float> samples = create_preset_fixture(44100, 1.0f);

  const auto defaults = master_audio_mono(Preset::Pop, samples.data(), samples.size(), 44100);

  Param loud_override[] = {{"loudness.targetLufs", -8.0}};
  const auto loud =
      master_audio_mono(Preset::Pop, samples.data(), samples.size(), 44100, loud_override, 1);

  Param quiet_override[] = {{"loudness.targetLufs", -24.0}};
  const auto quiet =
      master_audio_mono(Preset::Pop, samples.data(), samples.size(), 44100, quiet_override, 1);

  WARN("output LUFS: quiet=" << quiet.output_lufs << " default=" << defaults.output_lufs
                             << " loud=" << loud.output_lufs);
  CHECK(std::isfinite(loud.output_lufs));
  CHECK(std::isfinite(quiet.output_lufs));
  CHECK(loud.output_lufs > quiet.output_lufs + 3.0f);
  CHECK(loud.output_lufs > defaults.output_lufs + 1.5f);
  CHECK(quiet.output_lufs < defaults.output_lufs - 1.5f);
  CHECK(loud.samples.size() == samples.size());
  CHECK(quiet.samples.size() == samples.size());
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

TEST_CASE("a cutoff list override resizes a preset's multiband bands by index",
          "[mastering][preset][multiband]") {
  using sonare::mastering::dynamics::CompressorConfig;
  auto base = preset_config(Preset::Pop);
  auto& base_bands = base.dynamics.multiband_comp.config.bands;
  REQUIRE(base_bands.size() == 3);
  // Distinct per-band values, so "kept by index" cannot pass by every band being the default.
  base_bands[0].ratio = 3.0f;
  base_bands[1].knee_db = 2.0f;
  base_bands[2].threshold_db = -25.0f;
  const auto preset_bands = base_bands;

  const auto require_band = [](const CompressorConfig& actual, const CompressorConfig& expected) {
    CHECK(actual.threshold_db == expected.threshold_db);
    CHECK(actual.ratio == expected.ratio);
    CHECK(actual.attack_ms == expected.attack_ms);
    CHECK(actual.release_ms == expected.release_ms);
    CHECK(actual.knee_db == expected.knee_db);
    CHECK(actual.makeup_gain_db == expected.makeup_gain_db);
    CHECK(actual.auto_makeup == expected.auto_makeup);
    CHECK(actual.detector == expected.detector);
    CHECK(actual.sidechain_high_pass_enabled == expected.sidechain_high_pass_enabled);
    CHECK(actual.sidechain_high_pass_hz == expected.sidechain_high_pass_hz);
    CHECK(actual.pdr_time_ms == expected.pdr_time_ms);
    CHECK(actual.pdr_release_scale == expected.pdr_release_scale);
  };

  SECTION("four cutoffs grow three bands to five") {
    auto config = base;
    const Param overrides[] = {
        {"dynamics.multibandComp.bands.1.ratio", 5.0},
        {"dynamics.multibandComp.crossover.cutoffsHz.0", 100.0},
        {"dynamics.multibandComp.crossover.cutoffsHz.1", 800.0},
        {"dynamics.multibandComp.crossover.cutoffsHz.2", 3000.0},
        {"dynamics.multibandComp.crossover.cutoffsHz.3", 9000.0},
    };
    apply_chain_config_overrides(config, overrides, std::size(overrides));
    const auto& multiband = config.dynamics.multiband_comp;
    CHECK(multiband.enabled);
    REQUIRE(multiband.config.crossover.cutoffs_hz ==
            std::vector<float>{100.0f, 800.0f, 3000.0f, 9000.0f});
    REQUIRE(multiband.config.bands.size() == 5);
    require_band(multiband.config.bands[0], preset_bands[0]);
    auto edited = preset_bands[1];
    edited.ratio = 5.0f;
    require_band(multiband.config.bands[1], edited);
    require_band(multiband.config.bands[2], preset_bands[2]);
    require_band(multiband.config.bands[3], CompressorConfig{});
    require_band(multiband.config.bands[4], CompressorConfig{});

    // The masterAudio path takes the same overrides and runs the five-band stage.
    const auto fixture = create_preset_fixture(44100, 0.25f);
    const auto result = master_audio_mono(Preset::Pop, fixture.data(), fixture.size(), 44100,
                                          overrides, std::size(overrides));
    CHECK(std::find(result.stages.begin(), result.stages.end(), "dynamics.multibandComp") !=
          result.stages.end());
  }

  SECTION("one cutoff shrinks three bands to two") {
    auto config = base;
    const Param overrides[] = {{"dynamics.multibandComp.crossover.cutoffsHz.0", 500.0}};
    apply_chain_config_overrides(config, overrides, 1);
    const auto& bands = config.dynamics.multiband_comp.config.bands;
    REQUIRE(config.dynamics.multiband_comp.config.crossover.cutoffs_hz ==
            std::vector<float>{500.0f});
    REQUIRE(bands.size() == 2);
    require_band(bands[0], preset_bands[0]);
    require_band(bands[1], preset_bands[1]);
  }
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
