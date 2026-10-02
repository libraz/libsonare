#pragma once

/// @file shrf_fixture.h
/// @brief Synthetic SHRF v1 sets for the playback tests: unit-impulse HRIRs on
///        both ears and an ITD table with a known closed form, so a rendered
///        interaural delay can be checked against the table directly.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "playback/shrf_format.h"
#include "util/constants.h"

namespace sonare::playback::test {

/// Grid of a synthetic set, one row per elevation.
struct ShrfFixtureSpec {
  int sample_rate = 48000;
  int taps = 16;
  int impulse_tap = 0;
  int n_az = 8;
  float az_step_deg = 45.0f;
  int n_el = 1;
  float el_min_deg = 0.0f;
  float el_step_deg = 15.0f;
  ShrfQuant quant = ShrfQuant::Float32;
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

namespace detail {

inline void push_u16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFFu));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

inline void push_u32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFFu));
}

inline void push_f32(std::vector<uint8_t>& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  push_u32(out, bits);
}

inline void push_i16(std::vector<uint8_t>& out, int16_t value) {
  uint16_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  push_u16(out, bits);
}

}  // namespace detail

/// SHRF v1 bytes of @p spec: unit-impulse HRIRs (tap `spec.impulse_tap` = 1, rest 0,
/// both ears) and `fixture_itd_samples`.
inline std::vector<uint8_t> make_shrf_fixture(const ShrfFixtureSpec& spec = {}) {
  std::vector<uint8_t> bytes;
  bytes.reserve(kShrfHeaderBytes);

  bytes.push_back('S');
  bytes.push_back('H');
  bytes.push_back('R');
  bytes.push_back('F');
  detail::push_u16(bytes, kShrfVersion);
  bytes.push_back(static_cast<uint8_t>(spec.quant));
  bytes.push_back(0);  // reserved
  detail::push_u32(bytes, static_cast<uint32_t>(spec.sample_rate));
  detail::push_u16(bytes, static_cast<uint16_t>(spec.taps));
  detail::push_u16(bytes, static_cast<uint16_t>(spec.n_az));
  detail::push_u16(bytes, static_cast<uint16_t>(spec.n_el));
  detail::push_u16(bytes, 0);  // reserved
  detail::push_f32(bytes, spec.az_step_deg);
  detail::push_f32(bytes, spec.el_min_deg);
  detail::push_f32(bytes, spec.el_step_deg);
  const float scale = spec.quant == ShrfQuant::Int16 ? (1.0f / 32767.0f) : 1.0f;
  detail::push_f32(bytes, scale);

  for (int el = 0; el < spec.n_el; ++el) {
    for (int az = 0; az < spec.n_az; ++az) {
      detail::push_f32(bytes, fixture_itd_samples(spec, el, az));
    }
  }

  for (int el = 0; el < spec.n_el; ++el) {
    for (int az = 0; az < spec.n_az; ++az) {
      for (int ear = 0; ear < 2; ++ear) {
        for (int tap = 0; tap < spec.taps; ++tap) {
          const float value = (tap == spec.impulse_tap) ? 1.0f : 0.0f;
          if (spec.quant == ShrfQuant::Int16) {
            detail::push_i16(bytes, static_cast<int16_t>(std::lround(value / scale)));
          } else {
            detail::push_f32(bytes, value);
          }
        }
      }
    }
  }

  return bytes;
}

}  // namespace sonare::playback::test
