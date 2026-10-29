#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "mastering/dynamics/transient_shaper.h"
#include "mastering/dynamics/vocal_rider.h"
#include "util/exception.h"

namespace {

using sonare::ErrorCode;
using sonare::SonareException;
using sonare::mastering::dynamics::TransientShaper;
using sonare::mastering::dynamics::TransientShaperConfig;
using sonare::mastering::dynamics::VocalRider;
using sonare::mastering::dynamics::VocalRiderConfig;

template <typename Operation>
bool throws_invalid_parameter(Operation operation) {
  try {
    operation();
  } catch (const SonareException& error) {
    return error.code() == ErrorCode::InvalidParameter;
  }
  return false;
}

template <typename Processor>
void process(Processor& processor, std::vector<float>& samples) {
  float* channels[] = {samples.data()};
  processor.process(channels, 1, static_cast<int>(samples.size()));
}

template <typename Processor, typename Config>
void check_non_finite_field(Config valid, float Config::*field, const char* field_name) {
  const std::array<float, 3> invalid_values{std::numeric_limits<float>::quiet_NaN(),
                                            std::numeric_limits<float>::infinity(),
                                            -std::numeric_limits<float>::infinity()};
  const std::vector<float> warm_input(128, 0.4f);
  const std::vector<float> next_input(128, -0.2f);

  for (float value : invalid_values) {
    DYNAMIC_SECTION(field_name << " rejects " << value) {
      CAPTURE(field_name, value);
      Config invalid = valid;
      invalid.*field = value;
      CHECK(throws_invalid_parameter([&] { Processor rejected(invalid); }));

      Processor actual(valid);
      Processor control(valid);
      actual.prepare(48000.0, 128);
      control.prepare(48000.0, 128);
      std::vector<float> actual_warm = warm_input;
      std::vector<float> control_warm = warm_input;
      process(actual, actual_warm);
      process(control, control_warm);

      CHECK(throws_invalid_parameter([&] { actual.set_config(invalid); }));
      CHECK(actual.config().*field == valid.*field);

      std::vector<float> actual_next = next_input;
      std::vector<float> control_next = next_input;
      process(actual, actual_next);
      process(control, control_next);
      CHECK(actual_next == control_next);
    }
  }
}

template <typename Processor, typename Config>
void check_finite_values(Config base, float Config::*field, float positive, float negative) {
  for (float value : {positive, negative}) {
    Config config = base;
    config.*field = value;
    Processor processor(config);
    processor.prepare(48000.0, 128);
    processor.set_config(config);
    REQUIRE(processor.config().*field == value);
    std::vector<float> samples(128, 0.25f);
    process(processor, samples);
    for (float sample : samples) REQUIRE(std::isfinite(sample));
  }
}

}  // namespace

TEST_CASE("dynamics configs reject non-finite gain and level fields", "[dynamics-config-finite]") {
  SECTION("TransientShaper attack and sustain gains") {
    const TransientShaperConfig valid{};
    check_non_finite_field<TransientShaper>(valid, &TransientShaperConfig::attack_gain_db,
                                            "attack_gain_db");
    check_non_finite_field<TransientShaper>(valid, &TransientShaperConfig::sustain_gain_db,
                                            "sustain_gain_db");
    check_finite_values<TransientShaper>(valid, &TransientShaperConfig::attack_gain_db, 4.0f,
                                         -4.0f);
    check_finite_values<TransientShaper>(valid, &TransientShaperConfig::sustain_gain_db, 4.0f,
                                         -4.0f);
  }

  SECTION("VocalRider target, noise floor and output gain") {
    const VocalRiderConfig valid{};
    check_non_finite_field<VocalRider>(valid, &VocalRiderConfig::target_db, "target_db");
    check_non_finite_field<VocalRider>(valid, &VocalRiderConfig::noise_floor_db, "noise_floor_db");
    check_non_finite_field<VocalRider>(valid, &VocalRiderConfig::output_gain_db, "output_gain_db");
    check_finite_values<VocalRider>(valid, &VocalRiderConfig::target_db, -15.0f, -24.0f);
    check_finite_values<VocalRider>(valid, &VocalRiderConfig::noise_floor_db, -50.0f, -70.0f);
    check_finite_values<VocalRider>(valid, &VocalRiderConfig::output_gain_db, 3.0f, -3.0f);
  }
}
