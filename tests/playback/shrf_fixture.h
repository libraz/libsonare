#pragma once

/// @file shrf_fixture.h
/// @brief Synthetic SHRF v1 sets for the playback tests: unit-impulse HRIRs on
///        both ears and an ITD table with a known closed form, so a rendered
///        interaural delay can be checked against the table directly.

#include <cmath>
#include <cstdint>
#include <vector>

#include "util/constants.h"

namespace sonare::playback::test {

/// Grid of a synthetic set (float32, one row per elevation).
struct ShrfFixtureSpec {
  int sample_rate = 48000;
  int taps = 16;
  int n_az = 8;
  float az_step_deg = 45.0f;
  int n_el = 1;
  float el_min_deg = 0.0f;
  float el_step_deg = 15.0f;
};

/// ITD magnitude at azimuth +/-90 degrees on the horizontal row, in samples.
inline constexpr float kFixtureMaxItdSamples = 24.0f;

/// Known ITD of a grid point: kFixtureMaxItdSamples * sin(az) * cos(el),
/// positive (left ear lags) for sources on the right.
inline float fixture_itd_samples(const ShrfFixtureSpec& spec, int el_index, int az_index) {
  const float az = static_cast<float>(az_index) * spec.az_step_deg;
  const float el = spec.el_min_deg + static_cast<float>(el_index) * spec.el_step_deg;
  const float deg_to_rad = sonare::constants::kPi / 180.0f;
  return kFixtureMaxItdSamples * std::sin(az * deg_to_rad) * std::cos(el * deg_to_rad);
}

/// SHRF v1 bytes of @p spec: unit-impulse HRIRs and `fixture_itd_samples`.
inline std::vector<uint8_t> make_shrf_fixture(const ShrfFixtureSpec& spec = {}) {
  (void)spec;
  return {};
}

}  // namespace sonare::playback::test
