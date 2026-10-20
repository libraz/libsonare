#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

#include "mastering/spectral/air_band.h"
#include "mastering/spectral/low_end_focus.h"
#include "mastering/spectral/presence_enhancer.h"
#include "mastering/spectral/spectral_shaper.h"
#include "mastering/stereo/auto_pan.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/stereo_balance.h"
#include "util/exception.h"

namespace {

using sonare::ErrorCode;
using sonare::SonareException;

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
constexpr float kInfF = std::numeric_limits<float>::infinity();
constexpr float kNanF = std::numeric_limits<float>::quiet_NaN();

template <typename Fn>
bool throws_invalid_parameter(Fn&& fn) {
  try {
    fn();
  } catch (const SonareException& e) {
    return e.code() == ErrorCode::InvalidParameter;
  }
  return false;
}

template <typename Processor>
void check_prepare_domain(Processor& processor) {
  REQUIRE(throws_invalid_parameter([&] { processor.prepare(kInf, 512); }));
  REQUIRE(throws_invalid_parameter([&] { processor.prepare(kNan, 512); }));
  REQUIRE(throws_invalid_parameter([&] { processor.prepare(0.3, 512); }));
  REQUIRE(throws_invalid_parameter([&] { processor.prepare(48000.0, -1); }));
  REQUIRE_NOTHROW(processor.prepare(48000.0, 512));
}

}  // namespace

TEST_CASE("prepare refuses non-finite and sub-sample rates like the shared validator",
          "[mastering][spectral][prepare]") {
  SECTION("reference sibling (Imager)") {
    sonare::mastering::stereo::Imager imager;
    check_prepare_domain(imager);
  }
  SECTION("AirBand") {
    sonare::mastering::spectral::AirBand processor;
    check_prepare_domain(processor);
    REQUIRE(throws_invalid_parameter([&] { processor.prepare(kInf, 512, 2); }));
    REQUIRE(throws_invalid_parameter([&] { processor.prepare(0.3, 512, 2); }));
    REQUIRE_NOTHROW(processor.prepare(48000.0, 512, 2));
  }
  SECTION("LowEndFocus") {
    sonare::mastering::spectral::LowEndFocus processor;
    check_prepare_domain(processor);
  }
  SECTION("PresenceEnhancer") {
    sonare::mastering::spectral::PresenceEnhancer processor;
    check_prepare_domain(processor);
  }
  SECTION("SpectralShaper") {
    sonare::mastering::spectral::SpectralShaper processor;
    check_prepare_domain(processor);
  }
}

TEST_CASE("AirBand refuses a NaN dynamic range", "[mastering][spectral][config]") {
  sonare::mastering::spectral::AirBandConfig config;
  config.dynamic_range_db = kNanF;
  REQUIRE(throws_invalid_parameter([&] { sonare::mastering::spectral::AirBand band(config); }));

  sonare::mastering::spectral::AirBand band;
  REQUIRE(throws_invalid_parameter([&] { band.set_config(config); }));

  config.dynamic_range_db = 6.0f;
  REQUIRE_NOTHROW(band.set_config(config));
}

TEST_CASE("AutoPan refuses non-finite rate and phase", "[mastering][stereo][config]") {
  using sonare::mastering::stereo::AutoPan;
  using sonare::mastering::stereo::AutoPanConfig;

  AutoPan pan;
  AutoPanConfig config;

  config.rate_hz = kInfF;
  REQUIRE(throws_invalid_parameter([&] { pan.set_config(config); }));
  REQUIRE(throws_invalid_parameter([&] { AutoPan built(config); }));

  config = AutoPanConfig{};
  config.rate_hz = kNanF;
  REQUIRE(throws_invalid_parameter([&] { pan.set_config(config); }));

  config = AutoPanConfig{};
  config.phase = kNanF;
  REQUIRE(throws_invalid_parameter([&] { pan.set_config(config); }));

  config = AutoPanConfig{};
  config.phase = kInfF;
  REQUIRE(throws_invalid_parameter([&] { pan.set_config(config); }));

  config = AutoPanConfig{};
  config.depth = kNanF;
  REQUIRE(throws_invalid_parameter([&] { pan.set_config(config); }));

  config = AutoPanConfig{};
  config.rate_hz = 2.0f;
  config.phase = 0.25f;
  REQUIRE_NOTHROW(pan.set_config(config));
}

TEST_CASE("StereoBalance refuses a NaN balance", "[mastering][stereo][config]") {
  using sonare::mastering::stereo::StereoBalance;
  using sonare::mastering::stereo::StereoBalanceConfig;

  StereoBalance balance;
  StereoBalanceConfig config;

  config.balance = kNanF;
  REQUIRE(throws_invalid_parameter([&] { balance.set_config(config); }));
  REQUIRE(throws_invalid_parameter([&] { StereoBalance built(config); }));

  config.balance = kInfF;
  REQUIRE(throws_invalid_parameter([&] { balance.set_config(config); }));

  config.balance = 0.5f;
  REQUIRE_NOTHROW(balance.set_config(config));
}
