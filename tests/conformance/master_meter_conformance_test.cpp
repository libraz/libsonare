/// @file master_meter_conformance_test.cpp
/// @brief The C ABI reproduces the master-meter readings every surface is held to.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "util/constants.h"
#include "util/json.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

namespace {

using Catch::Matchers::WithinAbs;
using sonare::constants::kTwoPiD;
using sonare::util::json::Value;

Value load_cases() {
  std::ifstream input("tests/conformance/master_meter_reading.json");
  REQUIRE(input.good());
  const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return sonare::util::json::parse_strict(text);
}

void require_reading(const SonareMixMeterSnapshot& snap, const Value& expected, double tol_db,
                     double tol_corr) {
  CHECK_THAT(snap.peak_db_l, WithinAbs(expected["peakDbL"].as_number(), tol_db));
  CHECK_THAT(snap.peak_db_r, WithinAbs(expected["peakDbR"].as_number(), tol_db));
  CHECK_THAT(snap.rms_db_l, WithinAbs(expected["rmsDbL"].as_number(), tol_db));
  CHECK_THAT(snap.rms_db_r, WithinAbs(expected["rmsDbR"].as_number(), tol_db));
  CHECK_THAT(snap.correlation, WithinAbs(expected["correlation"].as_number(), tol_corr));
  CHECK_THAT(snap.true_peak_db_l, WithinAbs(expected["truePeakDbL"].as_number(), tol_db));
  CHECK_THAT(snap.true_peak_db_r, WithinAbs(expected["truePeakDbR"].as_number(), tol_db));
}

}  // namespace

TEST_CASE("The C ABI master meter reads the shared conformance values", "[mixing][conformance]") {
  const Value doc = load_cases();
  const double tol_db = doc["toleranceDb"].as_number();
  const double tol_corr = doc["toleranceCorrelation"].as_number();
  REQUIRE(doc["cases"].size() > 0);
  for (const Value& c : doc["cases"].as_array()) {
    INFO(c["name"].as_string());
    const int sample_rate = static_cast<int>(c["sampleRate"].as_number());
    const int block = static_cast<int>(c["blockSize"].as_number());
    const int blocks = static_cast<int>(c["blocks"].as_number());
    const double freq = c["frequencyHz"].as_number();
    const double amplitude = c["amplitude"].as_number();
    const double phase = c["phaseRad"].as_number();
    const double right_gain = c["rightGain"].as_number();

    SonareMixer* mixer =
        sonare_mixer_from_scene_json(c["scene"].as_string().c_str(), sample_rate, block);
    REQUIRE(mixer != nullptr);
    SonareMixerMasterMeterConfig config{};
    sonare_mixer_master_meter_config_init(&config);
    config.measure_lufs = 0;
    config.true_peak_oversample = static_cast<int32_t>(c["truePeakOversample"].as_number());
    REQUIRE(sonare_mixer_configure_master_meter(mixer, &config) == SONARE_OK);

    std::vector<float> left(static_cast<std::size_t>(block));
    std::vector<float> right(static_cast<std::size_t>(block));
    std::vector<float> out_l(static_cast<std::size_t>(block));
    std::vector<float> out_r(static_cast<std::size_t>(block));
    const float* inputs_l[] = {left.data()};
    const float* inputs_r[] = {right.data()};
    for (int b = 0; b < blocks; ++b) {
      for (int i = 0; i < block; ++i) {
        const double n = static_cast<double>(b) * block + i;
        const double v = amplitude * std::sin(kTwoPiD * freq * n / sample_rate + phase);
        left[static_cast<std::size_t>(i)] = static_cast<float>(v);
        right[static_cast<std::size_t>(i)] = static_cast<float>(v * right_gain);
      }
      REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 1, out_l.data(), out_r.data(),
                                          static_cast<size_t>(block)) == SONARE_OK);
    }
    SonareMixMeterSnapshot snap{};
    REQUIRE(sonare_mixer_master_meter(mixer, &snap) == SONARE_OK);
    require_reading(snap, c["expected"], tol_db, tol_corr);
    REQUIRE(sonare_mixer_flush_meters(mixer) == SONARE_OK);
    REQUIRE(sonare_mixer_master_meter(mixer, &snap) == SONARE_OK);
    require_reading(snap, c["expectedAfterFlush"], tol_db, tol_corr);
    sonare_mixer_destroy(mixer);
  }
}

#endif  // defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)
