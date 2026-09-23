#include "midi/synth/gs_efx_convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "midi/synth/gs_efx_tables.h"

namespace sonare::midi::synth {

namespace {

/// Keeps a ladder knot that lands on a whole sample from falling one ulp short of it.
constexpr double kSampleCutGuard = 1e-6;

/// Which shape each of the five printed wave bytes selects, in byte order.
constexpr std::array<GsEfxWave, 5> kWaveByByte = {{
    GsEfxWave::kTriangle,
    GsEfxWave::kSquare,
    GsEfxWave::kSine,
    GsEfxWave::kSawUp,
    GsEfxWave::kSawDown,
}};

/// Reads a piecewise-linear table, holding the end value outside the knots.
template <std::size_t N>
float from_breakpoints(const std::array<GsEfxBreakpoint, N>& knots, uint8_t value) noexcept {
  if (value <= knots.front().setting) return knots.front().value;
  for (std::size_t i = 1; i < N; ++i) {
    if (value > knots[i].setting) continue;
    const double lo = knots[i - 1].value;
    const double span = static_cast<double>(knots[i].setting - knots[i - 1].setting);
    const double rise = static_cast<double>(knots[i].value) - lo;
    const double along = static_cast<double>(value - knots[i - 1].setting);
    return static_cast<float>(lo + rise * along / span);
  }
  return knots.back().value;
}

/// Cuts a ladder entry back to a whole sample of the unit's own clock.
float cut_to_whole_samples(float ms) noexcept {
  const double per_ms = static_cast<double>(kGsEfxUnitClockHz) / 1000.0;
  const double samples = std::floor(static_cast<double>(ms) * per_ms + kSampleCutGuard);
  return static_cast<float>(samples / per_ms);
}

/// One balance ramp: the byte's distance from its corner over the printed width,
/// truncated onto the stored grid and clamped at both ends.
float balance_ramp(int distance) noexcept {
  if (distance <= 0) return 0.0f;
  const int steps = static_cast<int>(
      std::floor(static_cast<double>(distance) * kGsEfxBalanceGrid / kGsEfxBalanceWidth));
  return std::min(1.0f, static_cast<float>(steps) / static_cast<float>(kGsEfxBalanceGrid));
}

/// The top four bits of a seven-bit byte, which is what a 16-entry table indexes by.
std::size_t top_four_bits(uint8_t value) noexcept {
  return std::min<std::size_t>(static_cast<std::size_t>(value) >> 3, 15);
}

/// The rotary loop's step rate, the unit's clock over 2^15. Both acceleration
/// conversions are this over the entry's divisor or the entry's divisor over
/// this, so neither can carry a constant the other does not.
double accel_step_hz() noexcept {
  return static_cast<double>(kGsEfxUnitClockHz) / static_cast<double>(kGsEfxAccelShift);
}

}  // namespace

float gs_efx_rate_hz(uint8_t value, GsRateRange range) noexcept {
  return range == GsRateRange::kNarrow ? from_breakpoints(kGsEfxRateNarrow, value)
                                       : from_breakpoints(kGsEfxRateWide, value);
}

float gs_efx_delay_ms(uint8_t value, GsTimeLadder ladder) noexcept {
  switch (ladder) {
    case GsTimeLadder::kLadder1:
      return cut_to_whole_samples(from_breakpoints(kGsEfxDelayTime1, value));
    case GsTimeLadder::kLadder2:
      return cut_to_whole_samples(from_breakpoints(kGsEfxDelayTime2, value));
    case GsTimeLadder::kLadder3:
      return cut_to_whole_samples(from_breakpoints(kGsEfxDelayTime3, value));
    case GsTimeLadder::kLadder4:
      return cut_to_whole_samples(from_breakpoints(kGsEfxDelayTime4, value));
    case GsTimeLadder::kLadder0:
      break;
  }
  return cut_to_whole_samples(from_breakpoints(kGsEfxDelayPreDelay, value));
}

float gs_efx_freq_hz(uint8_t value, GsFreqColumn column) noexcept {
  const std::size_t entry = top_four_bits(value);
  switch (column) {
    case GsFreqColumn::kColumn1:
      return kGsEfxFreqPreFilter[entry];
    case GsFreqColumn::kColumn2:
      return kGsEfxFreqDamping[entry];
    case GsFreqColumn::kColumn0:
      break;
  }
  return kGsEfxFreqEq[entry];
}

float gs_efx_gain_db(uint8_t value) noexcept {
  const int inside = std::clamp<int>(value, kGsEfxGainWindowLo, kGsEfxGainWindowHi);
  return static_cast<float>(inside - kGsEfxGainOffset) * kGsEfxGainDbPerStep;
}

float gs_efx_level_mul(uint8_t value) noexcept {
  const std::size_t entry = std::min<std::size_t>(value, kGsEfxLevelNumerator.size() - 1);
  return static_cast<float>(kGsEfxLevelNumerator[entry]) /
         static_cast<float>(kGsEfxLevelDenominator);
}

float gs_efx_width_q(uint8_t value) noexcept {
  const std::size_t entry = value < kGsEfxWidth.size() ? value : 0;
  return kGsEfxWidth[entry];
}

GsEfxWave gs_efx_wave(uint8_t value) noexcept {
  // Past the printed list the unit keeps whatever state it was in, which a pure
  // conversion cannot see; its sibling small table (width) returns entry 0 there.
  const std::size_t entry = value < kWaveByByte.size() ? value : 0;
  return kWaveByByte[entry];
}

void gs_efx_pan(uint8_t value, float* left, float* right) noexcept {
  if (left != nullptr) *left = from_breakpoints(kGsEfxPanLeft, value);
  if (right != nullptr) *right = from_breakpoints(kGsEfxPanRight, value);
}

void gs_efx_balance(uint8_t value, float* direct, float* effect) noexcept {
  // The archive's records put the 114 corner on the direct half and the 14 corner
  // on the effect half; gs_efx_tables.h names the two constants the other way round.
  const int n = static_cast<int>(value);
  if (direct != nullptr) *direct = balance_ramp(kGsEfxBalanceCornerEffect - n);
  if (effect != nullptr) *effect = balance_ramp(n - kGsEfxBalanceCornerDirect);
}

int gs_efx_azimuth_deg(uint8_t value) noexcept {
  // Add two (the rounding of the divide by four), shift, then recentre: the byte's
  // 128 values leave 32 quarter-turns, so the middle one is the clamp plus one.
  const int position = ((static_cast<int>(value) + 2) >> 2) - (kGsEfxAzimuthClamp + 1);
  return std::clamp(position, -kGsEfxAzimuthClamp, kGsEfxAzimuthClamp) *
         kGsEfxAzimuthDegreesPerPosition;
}

float gs_efx_accel_tau_s(uint8_t value) noexcept {
  return static_cast<float>(kGsEfxAccelDivisor[top_four_bits(value)] / accel_step_hz());
}

float gs_efx_accel_undershoot_hz(uint8_t value) noexcept {
  return static_cast<float>(accel_step_hz() / kGsEfxAccelDivisor[top_four_bits(value)]);
}

float gs_efx_post_gain_db(uint8_t value) noexcept {
  return static_cast<float>(gs_efx_enum_index(value, kGsEfxPostGainSettings)) *
         kGsEfxPostGainDbPerStep;
}

float gs_efx_window_ms(uint8_t value) noexcept {
  return kGsEfxWindowMs[static_cast<std::size_t>(
      gs_efx_enum_index(value, static_cast<int>(kGsEfxWindowMs.size())))];
}

float gs_efx_corner_hz(uint8_t value, GsShelfSide side) noexcept {
  const auto& states = side == GsShelfSide::kLow ? kGsEfxCornerLow : kGsEfxCornerHigh;
  return states[static_cast<std::size_t>(
      gs_efx_enum_index(value, static_cast<int>(states.size())))];
}

bool gs_efx_ratio(uint8_t value, int lo_byte, int hi_byte, int lo_unit, int hi_unit,
                  float* out) noexcept {
  if (out == nullptr || hi_byte <= lo_byte) return false;
  const int steps = hi_byte - lo_byte;
  const int span = hi_unit - lo_unit;
  // The refusal, and the whole reason this takes endpoints rather than a table.
  if (span % steps != 0) return false;
  const int per_byte = span / steps;  // exact, by the check above
  const int clamped = std::clamp(static_cast<int>(value), lo_byte, hi_byte);
  const int units = lo_unit + (clamped - lo_byte) * per_byte;
  *out = static_cast<float>(units);
  return true;
}

int gs_efx_enum_index(uint8_t value, int count) noexcept {
  return (count > 0 && static_cast<int>(value) < count) ? static_cast<int>(value) : 0;
}

}  // namespace sonare::midi::synth
