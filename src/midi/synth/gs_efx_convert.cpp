#include "midi/synth/gs_efx_convert.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>

#include "midi/synth/gs_efx_tables.h"
#include "util/constants.h"

namespace sonare::midi::synth {

namespace {

/// Drive bytes 0 and 2 are one measured state, and 48 is unity gain.
constexpr int kDriveFloorByte = 2;
constexpr float kDriveUnityByte = 48.0f;
/// dB per decade of amplitude.
constexpr float kDbPerDecade = 20.0f;
/// The output-level floor, dB.
constexpr float kLevelFloorDb = -24.0f;

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

/// An output-level byte as dB over the level floor, from the measured multiplier.
float level_db(uint8_t value) noexcept {
  // std::max returns its first argument for the silent byte's -inf.
  return std::max(kLevelFloorDb, kDbPerDecade * std::log10(gs_efx_level_mul(value)));
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

float gs_efx_pan_position(uint8_t value) noexcept {
  using sonare::constants::kInvPi;
  float left = 0.0f;
  float right = 0.0f;
  gs_efx_pan(value, &left, &right);
  // The constant-power pair (cos, sin) of angle (b + 1)·π/4 has this ratio.
  const float position = 4.0f * kInvPi * std::atan2(right, left) - 1.0f;
  return std::clamp(position, -1.0f, 1.0f);
}

float gs_efx_balance_fraction(uint8_t value) noexcept {
  float direct = 0.0f;
  float effect = 0.0f;
  gs_efx_balance(value, &direct, &effect);
  assert(direct + effect > 0.0f);
  return effect / (direct + effect);
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

float gs_efx_drive_db(uint8_t value) noexcept {
  return kDbPerDecade *
         std::log10(static_cast<float>(std::max<int>(value, kDriveFloorByte)) / kDriveUnityByte);
}

float gs_efx_drive_pedal_db(uint8_t value) noexcept {
  return gs_efx_drive_db(value) - gs_efx_drive_db(0);
}

float gs_efx_designed_value(const GsEfxDesignedLaw& law, uint8_t byte, uint8_t byte_lo,
                            uint8_t byte_hi) noexcept {
  if (law.form == kGsEfxFormEnum) {
    assert(law.n_states > 0);
    return static_cast<float>(std::min<int>(byte, law.n_states - 1));
  }
  // A stepped law reads the byte as a state index and places state i at i / (n - 1).
  if (law.n_states > 1 && (law.form == kGsEfxFormLinear || law.form == kGsEfxFormLog)) {
    const GsEfxDesignedLaw continuous{law.form, law.lo, law.hi, 0};
    return gs_efx_designed_value(continuous, byte, 0, static_cast<uint8_t>(law.n_states - 1));
  }
  assert(byte_hi > byte_lo);
  const int inside = std::clamp<int>(byte, byte_lo, byte_hi);
  // Endpoints are returned as written so a law meets its printed ends exactly.
  if (inside == byte_lo && law.form != kGsEfxFormBipolar && law.form != kGsEfxFormDb) {
    return law.lo;
  }
  if (inside == byte_hi && law.form != kGsEfxFormBipolar && law.form != kGsEfxFormDb) {
    return law.hi;
  }
  const double along =
      static_cast<double>(inside - byte_lo) / static_cast<double>(byte_hi - byte_lo);
  switch (law.form) {
    case kGsEfxFormLinear:
      return static_cast<float>(law.lo + (static_cast<double>(law.hi) - law.lo) * along);
    case kGsEfxFormLog:
      assert(law.lo > 0.0f && law.hi > 0.0f);
      return static_cast<float>(law.lo * std::pow(static_cast<double>(law.hi) / law.lo, along));
    case kGsEfxFormDb: {
      const double db = law.lo + (static_cast<double>(law.hi) - law.lo) * along;
      return static_cast<float>(std::pow(10.0, db / kDbPerDecade));
    }
    case kGsEfxFormBipolar:
      return static_cast<float>((2.0 * along - 1.0) * law.hi);
    default:
      break;
  }
  assert(false && "designed law with no form");
  return 0.0f;
}

float gs_efx_binding_value(const GsEfxBindingRow& row, uint8_t byte) noexcept {
  if (row.law.form != kGsEfxFormNone) {
    return gs_efx_designed_value(row.law, byte, row.byte_lo, row.byte_hi);
  }
  switch (row.conv_class) {
    case kGsEfxClassRate:
      return gs_efx_rate_hz(byte, row.table == 1 ? GsRateRange::kWide : GsRateRange::kNarrow);
    case kGsEfxClassDelayTime: {
      constexpr std::array<GsTimeLadder, 5> kLadders = {
          GsTimeLadder::kLadder0, GsTimeLadder::kLadder1, GsTimeLadder::kLadder2,
          GsTimeLadder::kLadder3, GsTimeLadder::kLadder4};
      assert(row.table < kLadders.size());
      return gs_efx_delay_ms(byte, kLadders[row.table]);
    }
    case kGsEfxClassFreq: {
      constexpr std::array<GsFreqColumn, 3> kColumns = {
          GsFreqColumn::kColumn0, GsFreqColumn::kColumn1, GsFreqColumn::kColumn2};
      assert(row.table < kColumns.size());
      return gs_efx_freq_hz(byte, kColumns[row.table]);
    }
    case kGsEfxClassGain:
      return gs_efx_gain_db(byte);
    case kGsEfxClassLevel:
      return level_db(byte);
    case kGsEfxClassWidth:
      return gs_efx_width_q(byte);
    case kGsEfxClassAccel:
      // One divisor table, two quantities: only the undershoot is a frequency.
      return row.out == GsEfxOut::kUndershootHz ? gs_efx_accel_undershoot_hz(byte)
                                                : gs_efx_accel_tau_s(byte);
    case kGsEfxClassPostGain:
      return gs_efx_post_gain_db(byte);
    case kGsEfxClassWindow:
      return gs_efx_window_ms(byte);
    case kGsEfxClassCorner:
      return gs_efx_corner_hz(byte, row.table == 1 ? GsShelfSide::kHigh : GsShelfSide::kLow);
    case kGsEfxClassWave:
      return static_cast<float>(gs_efx_wave(byte));
    case kGsEfxClassAzimuth:
      return static_cast<float>(gs_efx_azimuth_deg(byte));
    case kGsEfxClassPan:
      return gs_efx_pan_position(byte);
    case kGsEfxClassBalance:
      return gs_efx_balance_fraction(byte);
    case kGsEfxRowClassRatio: {
      float units = 0.0f;
      const bool whole =
          gs_efx_ratio(byte, row.byte_lo, row.byte_hi, row.unit_lo, row.unit_hi, &units);
      assert(whole);
      (void)whole;
      // Table 0 is printed in percent and the controls take the fraction;
      // table 1 is printed in semitones, which is what they take.
      return row.table == 0 ? units / 100.0f : units;
    }
    case kGsEfxRowClassDrive:
      // Table 0 is the gain in front of the curve; table 1 is that gain as a pedal's.
      return row.table == 1 ? gs_efx_drive_pedal_db(byte) : gs_efx_drive_db(byte);
    default:
      break;
  }
  assert(false && "binding row with no reader");
  return 0.0f;
}

bool gs_efx_enable_on(const GsEfxEnable& enable, uint8_t byte,
                      uint8_t stage_index_in_rule) noexcept {
  if (enable.mode == kGsEfxEnableSelect) {
    return gs_efx_enum_index(byte, enable.n_stages) == stage_index_in_rule;
  }
  const uint8_t bit = byte & 0x7Fu;
  return ((enable.on_mask[bit >> 5] >> (bit & 31u)) & 1u) != 0;
}

}  // namespace sonare::midi::synth
