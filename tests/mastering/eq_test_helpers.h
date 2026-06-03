/// @file eq_test_helpers.h
/// @brief Shared helpers for mastering EQ tests.

#pragma once

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "mastering/eq/api_style.h"
#include "mastering/eq/band_pass.h"
#include "mastering/eq/cut_filter.h"
#include "mastering/eq/dynamic_eq.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/graphic_eq.h"
#include "mastering/eq/linear_phase.h"
#include "mastering/eq/mid_side_eq.h"
#include "mastering/eq/minimum_phase.h"
#include "mastering/eq/parametric.h"
#include "mastering/eq/pultec.h"
#include "mastering/eq/shelving.h"
#include "mastering/eq/spectrum_engine.h"
#include "mastering/eq/spectrum_registry.h"
#include "mastering/eq/tilt.h"
#include "rt/biquad_design.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"

using Catch::Matchers::WithinAbs;
using namespace sonare::mastering::eq;

namespace {

using sonare::constants::kButterworthQ;
using sonare::constants::kPiD;
using sonare::test::max_abs_difference;
using sonare::test::peak_abs;
using sonare::test::process;
using sonare::test::process_stereo;
using sonare::test::rms_tail;

[[maybe_unused]] std::vector<float> sine(float frequency_hz, int sample_rate, int samples,
                                         float amplitude = 0.25f) {
  std::vector<float> out(static_cast<size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    out[static_cast<size_t>(i)] =
        amplitude * static_cast<float>(std::sin(2.0 * kPiD * frequency_hz * i / sample_rate));
  }
  return out;
}

[[maybe_unused]] float kernel_magnitude_at(const std::vector<float>& kernel, double frequency_hz,
                                           double sample_rate) {
  std::complex<double> response{0.0, 0.0};
  const double omega = 2.0 * kPiD * frequency_hz / sample_rate;
  for (size_t n = 0; n < kernel.size(); ++n) {
    response += static_cast<double>(kernel[n]) *
                std::exp(std::complex<double>(0.0, -omega * static_cast<double>(n)));
  }
  return static_cast<float>(std::abs(response));
}

}  // namespace
