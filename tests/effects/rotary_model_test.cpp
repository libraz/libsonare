/// @file rotary_model_test.cpp
/// @brief The rotary's geometric model: two microphones, the horn's angle low-pass, the LR4
///        split, and a drum baffle that leaves the bass below 200 Hz alone.
///
/// Most cases hold both rotors still (rate 0), which parks the horn mouth at azimuth 0 with the
/// microphones at -135 and -45 degrees, so the far (left) microphone sits 135 degrees off the
/// mouth's axis and the near (right) one 45. With `depthMs` 0 the horn turns on the spot and
/// both microphones are the same distance away, so any difference between them is the angle.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "effects/modulation/rotary.h"
#include "rt/biquad_design.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"

namespace {

using sonare::effects::modulation::Rotary;
using sonare::effects::modulation::RotaryConfig;
using sonare::effects::modulation::RotaryModel;
namespace geo = sonare::effects::modulation::rotary_geometry;
using sonare::test::kRate;

constexpr double kSettleS = 0.1;
constexpr double kReadS = 0.1;
constexpr float kSilentDb = -200.0f;

RotaryConfig still_geometric() {
  RotaryConfig config;
  config.model = RotaryModel::kGeometric;
  config.rate_hz = 0.0f;
  config.drum_rate_hz = 0.0f;
  config.depth_ms = 0.0f;
  config.tremolo = 0.0f;
  return config;
}

struct StereoGain {
  double left = 0.0;
  double right = 0.0;
};

/// Steady-state gain of a tone through @p config, per microphone.
StereoGain tone_gain(const RotaryConfig& config, float tone_hz) {
  Rotary rotary(config);
  rotary.prepare(kRate, 4096);
  const int samples = static_cast<int>((kSettleS + kReadS) * kRate);
  std::vector<float> left =
      sonare::test::generate_sine(samples, tone_hz, static_cast<int>(kRate), 0.5f);
  std::vector<float> right = left;
  const auto skip = static_cast<std::size_t>(kSettleS * kRate);
  const double in = sonare::test::rms(left, skip);
  sonare::test::process_stereo(rotary, left, right);
  return {sonare::test::rms(left, skip) / in, sonare::test::rms(right, skip) / in};
}

/// |H| of the TPT Butterworth low-pass the horn is heard through, at this rate.
double angle_lp_gain(double tone_hz, double corner_hz) {
  const double pi = sonare::constants::kPiD;
  const double omega = std::tan(pi * tone_hz / kRate) / std::tan(pi * corner_hz / kRate);
  return 1.0 / std::sqrt(1.0 + std::pow(omega, 4.0));
}

double horn_peak_gain(double tone_hz) {
  const float w0 = sonare::rt::frequency_to_w0(geo::kHornPeakHz, kRate);
  const sonare::rt::BiquadCoeffs peak = sonare::rt::rbj_peak(w0, geo::kHornPeakQ, geo::kHornPeakDb);
  return sonare::rt::biquad_magnitude(
      peak, static_cast<float>(sonare::constants::kTwoPiD * tone_hz / kRate));
}

std::vector<float> render(RotaryConfig config, std::vector<float>& right) {
  Rotary rotary(config);
  rotary.prepare(kRate, 4096);
  const int samples = static_cast<int>(0.5 * kRate);
  std::vector<float> left =
      sonare::test::generate_sine(samples, 440.0f, static_cast<int>(kRate), 0.3f);
  const std::vector<float> over =
      sonare::test::generate_sine(samples, 3100.0f, static_cast<int>(kRate), 0.2f);
  for (std::size_t i = 0; i < left.size(); ++i) left[i] += over[i];
  right = left;
  sonare::test::process_stereo(rotary, left, right);
  return left;
}

/// Peak-to-trough swing, in dB, of the 20 ms RMS envelope of a tone through a turning baffle.
double baffle_swing_db(RotaryConfig config, float tone_hz) {
  Rotary rotary(config);
  rotary.prepare(kRate, 4096);
  const int samples = static_cast<int>(1.5 * kRate);
  std::vector<float> left =
      sonare::test::generate_sine(samples, tone_hz, static_cast<int>(kRate), 0.5f);
  std::vector<float> right = left;
  sonare::test::process_stereo(rotary, left, right);
  const auto window = static_cast<std::size_t>(0.02 * kRate);
  double lo = 1e30;
  double hi = 0.0;
  for (std::size_t start = static_cast<std::size_t>(0.25 * kRate); start + window <= left.size();
       start += window) {
    const double level = sonare::test::rms(left.data() + start, window);
    lo = std::min(lo, level);
    hi = std::max(hi, level);
  }
  return 20.0 * std::log10(hi / lo);
}

}  // namespace

TEST_CASE("model 0 is the classic path, and switching away and back does not disturb it",
          "[rotary-model]") {
  RotaryConfig config;
  std::vector<float> right_plain;
  const std::vector<float> left_plain = render(config, right_plain);

  Rotary rotary(config);
  rotary.prepare(kRate, 4096);
  REQUIRE(rotary.set_parameter(13, 1.0f));
  REQUIRE(rotary.set_parameter(13, 0.0f));
  const int samples = static_cast<int>(0.5 * kRate);
  std::vector<float> left =
      sonare::test::generate_sine(samples, 440.0f, static_cast<int>(kRate), 0.3f);
  const std::vector<float> over =
      sonare::test::generate_sine(samples, 3100.0f, static_cast<int>(kRate), 0.2f);
  for (std::size_t i = 0; i < left.size(); ++i) left[i] += over[i];
  std::vector<float> right = left;
  sonare::test::process_stereo(rotary, left, right);
  CHECK(left == left_plain);
  CHECK(right == right_plain);

  // The geometric model is a different sound, not a relabelled one.
  config.model = RotaryModel::kGeometric;
  std::vector<float> right_geo;
  const std::vector<float> left_geo = render(config, right_geo);
  CHECK(sonare::test::max_abs_difference(left_geo, left_plain) > 0.05f);
}

TEST_CASE("the two microphones hear the turning horn differently, and coincide at spread 0",
          "[rotary-model]") {
  RotaryConfig config;
  config.model = RotaryModel::kGeometric;
  std::vector<float> right;
  const std::vector<float> left = render(config, right);
  CHECK(sonare::test::max_abs_difference(left, right) > 0.05f);

  config.stereo_spread = 0.0f;
  std::vector<float> right_same;
  const std::vector<float> left_same = render(config, right_same);
  CHECK(left_same == right_same);
}

TEST_CASE("the horn loses its top as its mouth turns away from the microphone", "[rotary-model]") {
  RotaryConfig config = still_geometric();
  config.drum_level_db = kSilentDb;
  const double far_corner = geo::horn_corner_hz(-sonare::constants::kInvSqrt2);
  const double near_corner = geo::horn_corner_hz(sonare::constants::kInvSqrt2);
  REQUIRE(far_corner < near_corner);
  for (const float tone : {1500.0f, 3000.0f, 6000.0f}) {
    const StereoGain gain = tone_gain(config, tone);
    const double measured_db = 20.0 * std::log10(gain.left / gain.right);
    const double predicted_db =
        20.0 * std::log10(angle_lp_gain(tone, far_corner) / angle_lp_gain(tone, near_corner));
    CAPTURE(tone, measured_db, predicted_db);
    CHECK(measured_db == Catch::Approx(predicted_db).margin(0.1));
  }
  // At 6 kHz the far microphone is well down.
  const StereoGain top = tone_gain(config, 6000.0f);
  CHECK(20.0 * std::log10(top.left / top.right) < -15.0);
}

TEST_CASE("the LR4 halves add to a flat response", "[rotary-model]") {
  RotaryConfig drum_only = still_geometric();
  drum_only.horn_level_db = kSilentDb;
  RotaryConfig horn_only = still_geometric();
  horn_only.drum_level_db = kSilentDb;
  const double near_corner = geo::horn_corner_hz(sonare::constants::kInvSqrt2);
  for (const float tone : {150.0f, 400.0f, 800.0f, 1300.0f, 2500.0f}) {
    const double drum = tone_gain(drum_only, tone).right;
    // The horn band with the horn's own peak and angle low-pass divided out: the LR4 high half.
    const double horn = tone_gain(horn_only, tone).right /
                        (horn_peak_gain(tone) * angle_lp_gain(tone, near_corner));
    CAPTURE(tone, drum, horn);
    CHECK(drum + horn == Catch::Approx(1.0).margin(0.01));
  }
  // At the crossover each half is -6 dB, which a one-pole split would not give.
  CHECK(tone_gain(drum_only, 800.0f).right == Catch::Approx(0.5).margin(0.01));
}

TEST_CASE("the drum baffle modulates above 200 Hz and leaves the bass below it alone",
          "[rotary-model]") {
  RotaryConfig config = still_geometric();
  config.drum_rate_hz = 4.0f;
  config.tremolo = 1.0f;
  config.horn_level_db = kSilentDb;
  const double bass = baffle_swing_db(config, 50.0f);
  const double band = baffle_swing_db(config, 500.0f);
  CAPTURE(bass, band);
  CHECK(bass < 0.3);
  CHECK(band > 0.6 * geo::kBaffleDepthDb);

  // The classic model's drum tremolo reaches the same bass, so the bound above can fail.
  RotaryConfig classic;
  classic.rate_hz = 0.0f;
  classic.drum_rate_hz = 4.0f;
  classic.depth_ms = 0.0f;
  classic.tremolo = 1.0f;
  classic.horn_level_db = kSilentDb;
  CHECK(baffle_swing_db(classic, 50.0f) > 3.0);
}

TEST_CASE("model is realtime id 13 and refuses values that name no model", "[rotary-model]") {
  Rotary rotary;
  rotary.prepare(kRate, 256);
  CHECK(rotary.set_parameter(13, 1.0f));
  CHECK(rotary.set_parameter(13, 0.0f));
  CHECK_FALSE(rotary.set_parameter(13, 2.0f));
  CHECK_FALSE(rotary.set_parameter(13, 0.5f));
  CHECK_FALSE(rotary.set_parameter(13, -1.0f));
  CHECK_FALSE(rotary.set_parameter(13, std::nanf("")));
  bool listed = false;
  for (const auto& descriptor : rotary.parameter_descriptors()) {
    if (descriptor.id == 13) listed = descriptor.key == "model";
  }
  CHECK(listed);
}
