#include "mastering/saturation/amp_sim.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

namespace {

float peak_abs(const std::vector<float>& samples) {
  float peak = 0.0f;
  for (float sample : samples) peak = std::max(peak, std::abs(sample));
  return peak;
}

}  // namespace

TEST_CASE("AmpSim power ADAA does not replay a bypassed state",
          "[mastering][saturation][boundary]") {
  sonare::mastering::saturation::AmpSimConfig config;
  config.cab = false;
  config.power = 0.5f;
  sonare::mastering::saturation::AmpSim subject(config);
  sonare::mastering::saturation::AmpSim control(config);
  subject.prepare(48000.0, 256, 1);
  control.prepare(48000.0, 256, 1);

  float subject_reentry_peak = 0.0f;
  float control_reentry_peak = 0.0f;
  for (int block = 0; block < 400; ++block) {
    if (block == 100) REQUIRE(subject.set_parameter(6, 0.0f));
    if (block == 300) REQUIRE(subject.set_parameter(6, 0.5f));
    std::vector<float> subject_block(256, block < 100 ? 0.3f : 0.0f);
    auto control_block = subject_block;
    float* subject_channels[] = {subject_block.data()};
    float* control_channels[] = {control_block.data()};
    subject.process(subject_channels, 1, 256);
    control.process(control_channels, 1, 256);
    if (block >= 300) {
      subject_reentry_peak = std::max(subject_reentry_peak, peak_abs(subject_block));
      control_reentry_peak = std::max(control_reentry_peak, peak_abs(control_block));
    }
  }

  REQUIRE(std::isfinite(subject_reentry_peak));
  REQUIRE(std::isfinite(control_reentry_peak));
  REQUIRE(subject_reentry_peak <= control_reentry_peak + 1.0e-5f);
  REQUIRE(subject.non_finite_discard_count() == 0u);
  REQUIRE(control.non_finite_discard_count() == 0u);
}
