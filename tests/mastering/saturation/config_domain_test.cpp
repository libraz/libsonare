#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_predicate.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "mastering/saturation/soft_clipper.h"
#include "mastering/saturation/tape.h"
#include "mastering/saturation/waveshaper.h"
#include "util/exception.h"

namespace {

using namespace sonare::mastering::saturation;

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

template <typename Processor>
bool processes_finite(Processor& processor) {
  processor.prepare(48000.0, 256);
  std::vector<float> block(256);
  for (size_t i = 0; i < block.size(); ++i)
    block[i] = 0.5f * std::sin(0.05f * static_cast<float>(i));
  float* channels[] = {block.data()};
  processor.process(channels, 1, 256);
  for (float sample : block) {
    if (!std::isfinite(sample)) return false;
  }
  return true;
}

bool is_invalid_parameter(const sonare::SonareException& error) {
  return error.code() == sonare::ErrorCode::InvalidParameter;
}

}  // namespace

TEST_CASE("SoftClipper refuses a non-finite ceiling", "[mastering][saturation][boundary]") {
  SoftClipper control{SoftClipperConfig{}};
  REQUIRE(processes_finite(control));

  SoftClipperConfig config;
  config.ceiling = kInf;
  REQUIRE_THROWS_MATCHES(SoftClipper(config), sonare::SonareException,
                         Catch::Matchers::Predicate<sonare::SonareException>(is_invalid_parameter));
  config.ceiling = kNaN;
  REQUIRE_THROWS_MATCHES(SoftClipper(config), sonare::SonareException,
                         Catch::Matchers::Predicate<sonare::SonareException>(is_invalid_parameter));
}

TEST_CASE("Tape refuses a non-finite bias", "[mastering][saturation][boundary]") {
  Tape control{TapeConfig{}};
  REQUIRE(processes_finite(control));

  for (float bad : {kNaN, kInf, -kInf}) {
    TapeConfig config;
    config.bias = bad;
    REQUIRE_THROWS_MATCHES(
        Tape(config), sonare::SonareException,
        Catch::Matchers::Predicate<sonare::SonareException>(is_invalid_parameter));
  }
}

TEST_CASE("Waveshaper refuses a non-finite bias", "[mastering][saturation][boundary]") {
  Waveshaper control{WaveshaperConfig{}};
  REQUIRE(processes_finite(control));

  for (float bad : {kNaN, kInf, -kInf}) {
    WaveshaperConfig config;
    config.bias = bad;
    REQUIRE_THROWS_MATCHES(
        Waveshaper(config), sonare::SonareException,
        Catch::Matchers::Predicate<sonare::SonareException>(is_invalid_parameter));
  }
}
