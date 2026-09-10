/// @file dereverb_room_test.cpp
/// @brief Pointing a dereverb config at a measured room: the mid-frequency
///        reverberation time the bands reduce to, the mixing time the volume
///        sets, what a partial or failed estimate leaves alone, and the C
///        surface that joins the two.

#include <sonare/sonare_c.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "analysis/acoustic_analyzer.h"
#include "mastering/repair/dereverb_classical.h"

using sonare::mid_frequency_rt60;
using sonare::octave_band_center_hz;
using sonare::mastering::repair::apply_room_measurement;
using sonare::mastering::repair::DereverbClassicalConfig;

namespace {

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

}  // namespace

TEST_CASE("the octave band layout starts at 125 Hz", "[mastering][dereverb_room]") {
  // The mid-frequency average reads bands 2 and 3 by index, so the layout is
  // load-bearing rather than decorative.
  CHECK(octave_band_center_hz(0) == Catch::Approx(125.0f));
  CHECK(octave_band_center_hz(2) == Catch::Approx(500.0f));
  CHECK(octave_band_center_hz(3) == Catch::Approx(1000.0f));
  CHECK(octave_band_center_hz(5) == Catch::Approx(4000.0f));
}

TEST_CASE("the mid-frequency time averages the 500 Hz and 1 kHz octaves",
          "[mastering][dereverb_room]") {
  // Bands: 125, 250, 500, 1k, 2k, 4k. Only the middle pair should count, so the
  // outliers on either side are far from the answer on purpose.
  const std::vector<float> bands = {9.0f, 9.0f, 1.0f, 2.0f, 9.0f, 9.0f};
  CHECK(mid_frequency_rt60(bands) == Catch::Approx(1.5f));
}

TEST_CASE("a band that did not converge is skipped, not averaged in",
          "[mastering][dereverb_room]") {
  // A failed band is NaN rather than absent, so a naive mean returns NaN.
  CHECK(mid_frequency_rt60({0.0f, 0.0f, kNan, 2.0f, 0.0f, 0.0f}) == Catch::Approx(2.0f));
  CHECK(mid_frequency_rt60({0.0f, 0.0f, 1.0f, kNan, 0.0f, 0.0f}) == Catch::Approx(1.0f));
  CHECK(std::isfinite(mid_frequency_rt60({kNan, kNan, kNan, kNan})));
}

TEST_CASE("a narrow estimate falls back to the bands it does have", "[mastering][dereverb_room]") {
  // Both mid bands unusable: a low-band-only estimate is worth less than a
  // mid-band one but more than nothing.
  CHECK(mid_frequency_rt60({0.8f, 1.2f, kNan, kNan, kNan, kNan}) == Catch::Approx(1.0f));
  // Fewer bands than the mid pair needs at all.
  CHECK(mid_frequency_rt60({0.8f, 1.2f}) == Catch::Approx(1.0f));
  // Nothing finite anywhere reports zero rather than a fabricated time.
  CHECK(mid_frequency_rt60({kNan, kNan}) == 0.0f);
  CHECK(mid_frequency_rt60({}) == 0.0f);
}

TEST_CASE("a room measurement sets where the tail is and nothing else",
          "[mastering][dereverb_room]") {
  DereverbClassicalConfig config;
  config.attenuation = 0.9f;
  config.threshold = 0.02f;
  config.over_subtraction = 1.4f;
  config.spectral_floor = 0.05f;
  const DereverbClassicalConfig before = config;

  apply_room_measurement(config, 1.8f, 2500.0f);

  CHECK(config.t60_sec == Catch::Approx(1.8f));
  CHECK(config.late_delay_ms == Catch::Approx(50.0f));  // sqrt(2500)
  // How much to remove is taste, and a measurement has no opinion on it.
  CHECK(config.attenuation == before.attenuation);
  CHECK(config.threshold == before.threshold);
  CHECK(config.over_subtraction == before.over_subtraction);
  CHECK(config.spectral_floor == before.spectral_floor);
}

TEST_CASE("the mixing time follows the room's volume", "[mastering][dereverb_room]") {
  DereverbClassicalConfig small;
  apply_room_measurement(small, 0.4f, 100.0f);
  DereverbClassicalConfig large;
  apply_room_measurement(large, 2.4f, 20000.0f);

  CHECK(small.late_delay_ms == Catch::Approx(10.0f));
  CHECK(large.late_delay_ms == Catch::Approx(141.42f).epsilon(0.01));
  CHECK(small.late_delay_ms < large.late_delay_ms);
}

TEST_CASE("an unusable measurement leaves its own field alone", "[mastering][dereverb_room]") {
  // A half-converged estimate must configure the half it measured rather than
  // overwriting the other with a zero.
  const DereverbClassicalConfig defaults;

  DereverbClassicalConfig no_time;
  apply_room_measurement(no_time, 0.0f, 900.0f);
  CHECK(no_time.t60_sec == defaults.t60_sec);
  CHECK(no_time.late_delay_ms == Catch::Approx(30.0f));

  DereverbClassicalConfig no_volume;
  apply_room_measurement(no_volume, 1.1f, 0.0f);
  CHECK(no_volume.t60_sec == Catch::Approx(1.1f));
  CHECK(no_volume.late_delay_ms == defaults.late_delay_ms);

  DereverbClassicalConfig nothing;
  apply_room_measurement(nothing, kNan, kNan);
  CHECK(nothing.t60_sec == defaults.t60_sec);
  CHECK(nothing.late_delay_ms == defaults.late_delay_ms);
}

TEST_CASE("the mixing time is bounded at both ends", "[mastering][dereverb_room]") {
  // A cupboard has no late field to separate; nothing mixes past a second.
  DereverbClassicalConfig tiny;
  apply_room_measurement(tiny, 0.2f, 0.25f);
  CHECK(tiny.late_delay_ms >= 1.0f);

  DereverbClassicalConfig vast;
  apply_room_measurement(vast, 6.0f, 4.0e8f);
  CHECK(vast.late_delay_ms == Catch::Approx(1000.0f));
}

TEST_CASE("the C surface joins a room estimate to a dereverb config",
          "[mastering][dereverb_room]") {
  std::vector<float> bands = {9.0f, 9.0f, 1.0f, 2.0f, 9.0f, 9.0f};
  SonareRoomEstimate estimate{};
  estimate.volume = 2500.0f;
  estimate.rt60_bands = bands.data();
  estimate.band_count = bands.size();

  SonareDereverbClassicalConfig config{};
  config.attenuation = 0.9f;
  config.n_fft = 1024;
  config.hop_length = 256;

  REQUIRE(sonare_mastering_repair_dereverb_config_for_room(&estimate, &config) == SONARE_OK);
  CHECK(config.t60_sec == Catch::Approx(1.5f));
  CHECK(config.late_delay_ms == Catch::Approx(50.0f));
  // Read AND written: the taste fields the caller set survive.
  CHECK(config.attenuation == Catch::Approx(0.9f));
  CHECK(config.n_fft == 1024);

  CHECK(sonare_mastering_repair_dereverb_config_for_room(nullptr, &config) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_mastering_repair_dereverb_config_for_room(&estimate, nullptr) ==
        SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("the C surface takes every config field literally", "[mastering][dereverb_room]") {
  // The dereverb call itself reads a NULL config as "library defaults"; this
  // one has no such rule, so a caller must not zero-initialize and expect them.
  std::vector<float> bands = {0.0f, 0.0f, 1.0f, 1.0f};
  SonareRoomEstimate estimate{};
  estimate.volume = 400.0f;
  estimate.rt60_bands = bands.data();
  estimate.band_count = bands.size();

  SonareDereverbClassicalConfig config{};
  REQUIRE(sonare_mastering_repair_dereverb_config_for_room(&estimate, &config) == SONARE_OK);
  CHECK(config.t60_sec == Catch::Approx(1.0f));
  CHECK(config.late_delay_ms == Catch::Approx(20.0f));
  // Untouched fields stay at the zeros the caller left, not at any default.
  CHECK(config.attenuation == 0.0f);
  CHECK(config.n_fft == 0);
  CHECK(config.spectral_floor == 0.0f);
}

TEST_CASE("an estimate with no bands at all is accepted and changes nothing",
          "[mastering][dereverb_room]") {
  // sonare_estimate_room leaves rt60_bands NULL when neither estimate produced
  // any bands, which must not be read as a zero reverberation time.
  SonareRoomEstimate estimate{};
  estimate.volume = 0.0f;
  estimate.rt60_bands = nullptr;
  estimate.band_count = 0;

  SonareDereverbClassicalConfig config{};
  SonareDereverbClassicalConfig before = config;
  REQUIRE(sonare_mastering_repair_dereverb_config_for_room(&estimate, &config) == SONARE_OK);
  CHECK(config.t60_sec == before.t60_sec);
  CHECK(config.late_delay_ms == before.late_delay_ms);
}
