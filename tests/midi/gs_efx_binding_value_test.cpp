/// @file gs_efx_binding_value_test.cpp
/// @brief The binding-row evaluator: designed laws over their printed domain,
///        and the dispatch agreeing with each measured class's own function.
///
/// The dispatch cases name classes by the generated class numbers, which is
/// why they live apart from gs_efx_convert_test.cpp: that file checks the
/// conversions against raw readings and must not include the generated table.

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>

#include "midi/synth/gs_efx_convert.h"
#include "midi/synth/gs_efx_tables.h"

namespace {

using sonare::midi::synth::gs_efx_accel_tau_s;
using sonare::midi::synth::gs_efx_accel_undershoot_hz;
using sonare::midi::synth::gs_efx_corner_hz;
using sonare::midi::synth::gs_efx_delay_ms;
using sonare::midi::synth::gs_efx_freq_hz;
using sonare::midi::synth::gs_efx_gain_db;
using sonare::midi::synth::gs_efx_level_mul;
using sonare::midi::synth::gs_efx_post_gain_db;
using sonare::midi::synth::gs_efx_rate_hz;
using sonare::midi::synth::gs_efx_ratio;
using sonare::midi::synth::gs_efx_width_q;
using sonare::midi::synth::gs_efx_window_ms;
using sonare::midi::synth::GsFreqColumn;
using sonare::midi::synth::GsRateRange;
using sonare::midi::synth::GsShelfSide;
using sonare::midi::synth::GsTimeLadder;

}  // namespace

namespace {

namespace synth = sonare::midi::synth;
using Catch::Approx;

synth::GsEfxDesignedLaw law_of(uint8_t form, float lo, float hi, uint8_t n_states = 0) {
  synth::GsEfxDesignedLaw law{};
  law.form = form;
  law.lo = lo;
  law.hi = hi;
  law.n_states = n_states;
  return law;
}

/// A row that reads a measured class, the way a translated row does.
synth::GsEfxBindingRow class_row(uint8_t conv_class, uint8_t table,
                                 synth::GsEfxOut out = synth::GsEfxOut::kValue) {
  synth::GsEfxBindingRow row{};
  row.kind = synth::kGsEfxRowTranslated;
  row.conv_class = conv_class;
  row.table = table;
  row.out = out;
  return row;
}

/// Every byte the printed domain holds, then a few past it.
template <typename Fn>
void for_each_byte(uint8_t lo, uint8_t hi, Fn&& fn) {
  for (int b = lo; b <= hi; ++b) fn(static_cast<uint8_t>(b));
}

}  // namespace

TEST_CASE("designed linear law meets both endpoints and rises in between", "[gs-efx-designed]") {
  const auto law = law_of(synth::kGsEfxFormLinear, 0.0f, 0.95f);
  CHECK(synth::gs_efx_designed_value(law, 0, 0, 127) == Approx(0.0f));
  CHECK(synth::gs_efx_designed_value(law, 127, 0, 127) == Approx(0.95f));
  CHECK(synth::gs_efx_designed_value(law, 64, 0, 127) == Approx(0.95f * 64.0f / 127.0f));
  float prev = -1.0f;
  for_each_byte(0, 127, [&](uint8_t b) {
    const float v = synth::gs_efx_designed_value(law, b, 0, 127);
    CHECK(v > prev);
    prev = v;
  });
  // A 00-5A domain ends at byte 90, and bytes past it hold the end value.
  CHECK(synth::gs_efx_designed_value(law, 90, 0, 90) == Approx(0.95f));
  CHECK(synth::gs_efx_designed_value(law, 45, 0, 90) == Approx(0.475f));
  CHECK(synth::gs_efx_designed_value(law, 127, 0, 90) == Approx(0.95f));
  // A domain with a nonzero low byte starts at it.
  CHECK(synth::gs_efx_designed_value(law, 0x0F, 0x0F, 0x71) == Approx(0.0f));
  CHECK(synth::gs_efx_designed_value(law, 0, 0x0F, 0x71) == Approx(0.0f));
}

TEST_CASE("designed log law is geometric between its endpoints", "[gs-efx-designed]") {
  const auto law = law_of(synth::kGsEfxFormLog, 0.05f, 10.0f);
  CHECK(synth::gs_efx_designed_value(law, 0, 0, 127) == Approx(0.05f));
  CHECK(synth::gs_efx_designed_value(law, 127, 0, 127) == Approx(10.0f));
  // The midpoint of a geometric line is the geometric mean.
  CHECK(synth::gs_efx_designed_value(law, 45, 0, 90) == Approx(std::sqrt(0.05f * 10.0f)));
  CHECK(synth::gs_efx_designed_value(law, 90, 0, 90) == Approx(10.0f));
  float prev = 0.0f;
  for_each_byte(0, 90, [&](uint8_t b) {
    const float v = synth::gs_efx_designed_value(law, b, 0, 90);
    CHECK(v > prev);
    prev = v;
  });
}

TEST_CASE("designed db law outputs the linear multiplier of a dB line", "[gs-efx-designed]") {
  const auto law = law_of(synth::kGsEfxFormDb, -24.0f, 12.0f);
  CHECK(synth::gs_efx_designed_value(law, 0, 0, 127) == Approx(std::pow(10.0f, -24.0f / 20.0f)));
  CHECK(synth::gs_efx_designed_value(law, 127, 0, 127) == Approx(std::pow(10.0f, 12.0f / 20.0f)));
  // Halfway along the dB line is -6 dB at the byte the domain's middle names.
  CHECK(synth::gs_efx_designed_value(law, 45, 0, 90) == Approx(std::pow(10.0f, -6.0f / 20.0f)));
  CHECK(synth::gs_efx_designed_value(law, 90, 0, 90) == Approx(std::pow(10.0f, 12.0f / 20.0f)));
  float prev = 0.0f;
  for_each_byte(0, 90, [&](uint8_t b) {
    const float v = synth::gs_efx_designed_value(law, b, 0, 90);
    CHECK(v > prev);
    prev = v;
  });
}

TEST_CASE("designed bipolar law runs minus to plus hi through the domain centre",
          "[gs-efx-designed]") {
  const auto law = law_of(synth::kGsEfxFormBipolar, 0.0f, 1.0f);
  CHECK(synth::gs_efx_designed_value(law, 0, 0, 127) == Approx(-1.0f));
  CHECK(synth::gs_efx_designed_value(law, 127, 0, 127) == Approx(1.0f));
  CHECK(synth::gs_efx_designed_value(law, 0, 0, 90) == Approx(-1.0f));
  CHECK(synth::gs_efx_designed_value(law, 45, 0, 90) == Approx(0.0f).margin(1e-6));
  CHECK(synth::gs_efx_designed_value(law, 90, 0, 90) == Approx(1.0f));
  CHECK(synth::gs_efx_designed_value(law, 0x0F, 0x0F, 0x71) == Approx(-1.0f));
  CHECK(synth::gs_efx_designed_value(law, 0x40, 0x0F, 0x71) == Approx(0.0f).margin(1e-6));
  float prev = -2.0f;
  for_each_byte(0, 127, [&](uint8_t b) {
    const float v = synth::gs_efx_designed_value(law, b, 0, 127);
    CHECK(v > prev);
    prev = v;
  });
}

TEST_CASE("designed enum law returns the state index and clamps at the last state",
          "[gs-efx-designed]") {
  const auto law = law_of(synth::kGsEfxFormEnum, 0.0f, 0.0f, 3);
  CHECK(synth::gs_efx_designed_value(law, 0, 0, 2) == 0.0f);
  CHECK(synth::gs_efx_designed_value(law, 1, 0, 2) == 1.0f);
  CHECK(synth::gs_efx_designed_value(law, 2, 0, 2) == 2.0f);
  CHECK(synth::gs_efx_designed_value(law, 3, 0, 2) == 2.0f);
  CHECK(synth::gs_efx_designed_value(law, 127, 0, 2) == 2.0f);
}

TEST_CASE("drive byte is a gain of 20 log10(v / 48) decibels, with 0 and 2 one state",
          "[gs-efx-designed]") {
  CHECK(synth::gs_efx_drive_db(0) == synth::gs_efx_drive_db(2));
  CHECK(synth::gs_efx_drive_db(1) == synth::gs_efx_drive_db(2));
  CHECK(synth::gs_efx_drive_db(2) == Approx(20.0f * std::log10(2.0f / 48.0f)));
  CHECK(synth::gs_efx_drive_db(2) == Approx(-27.6042f).margin(1e-3));
  CHECK(synth::gs_efx_drive_db(48) == Approx(0.0f).margin(1e-6));
  CHECK(synth::gs_efx_drive_db(127) == Approx(20.0f * std::log10(127.0f / 48.0f)));
  CHECK(synth::gs_efx_drive_db(127) == Approx(8.4514f).margin(1e-3));
  float prev = -1000.0f;
  for_each_byte(2, 127, [&](uint8_t b) {
    const float v = synth::gs_efx_drive_db(b);
    CHECK(v > prev);
    prev = v;
  });
}

TEST_CASE("binding_value reads every measured class exactly as its own function does",
          "[gs-efx-designed]") {
  // Bytes that cross the knots, the clamps and the past-the-list conventions.
  constexpr std::array<uint8_t, 9> kBytes = {0, 1, 2, 5, 20, 64, 90, 113, 127};
  for (const uint8_t b : kBytes) {
    INFO("byte " << int(b));
    CHECK(synth::gs_efx_binding_value(class_row(synth::kGsEfxClassRate, 0), b) ==
          gs_efx_rate_hz(b, GsRateRange::kNarrow));
    CHECK(synth::gs_efx_binding_value(class_row(synth::kGsEfxClassRate, 1), b) ==
          gs_efx_rate_hz(b, GsRateRange::kWide));

    constexpr std::array<GsTimeLadder, 5> kLadders = {
        GsTimeLadder::kLadder0, GsTimeLadder::kLadder1, GsTimeLadder::kLadder2,
        GsTimeLadder::kLadder3, GsTimeLadder::kLadder4};
    for (uint8_t t = 0; t < kLadders.size(); ++t) {
      CHECK(synth::gs_efx_binding_value(class_row(synth::kGsEfxClassDelayTime, t), b) ==
            gs_efx_delay_ms(b, kLadders[t]));
    }
    constexpr std::array<GsFreqColumn, 3> kColumns = {
        GsFreqColumn::kColumn0, GsFreqColumn::kColumn1, GsFreqColumn::kColumn2};
    for (uint8_t t = 0; t < kColumns.size(); ++t) {
      CHECK(synth::gs_efx_binding_value(class_row(synth::kGsEfxClassFreq, t), b) ==
            gs_efx_freq_hz(b, kColumns[t]));
    }

    CHECK(synth::gs_efx_binding_value(class_row(synth::kGsEfxClassGain, 0), b) ==
          gs_efx_gain_db(b));
    // Level is a gain in dB over a -24 dB floor, from the measured multiplier.
    CHECK(synth::gs_efx_binding_value(class_row(synth::kGsEfxClassLevel, 0), b) ==
          Approx(std::max(-24.0f, 20.0f * std::log10(gs_efx_level_mul(b)))));
    CHECK(synth::gs_efx_binding_value(class_row(synth::kGsEfxClassWidth, 0), b) ==
          gs_efx_width_q(b));
    CHECK(synth::gs_efx_binding_value(class_row(synth::kGsEfxClassPostGain, 0), b) ==
          gs_efx_post_gain_db(b));
    CHECK(synth::gs_efx_binding_value(class_row(synth::kGsEfxClassWindow, 0), b) ==
          gs_efx_window_ms(b));
    CHECK(synth::gs_efx_binding_value(class_row(synth::kGsEfxClassCorner, 0), b) ==
          gs_efx_corner_hz(b, GsShelfSide::kLow));
    CHECK(synth::gs_efx_binding_value(class_row(synth::kGsEfxClassCorner, 1), b) ==
          gs_efx_corner_hz(b, GsShelfSide::kHigh));
    CHECK(synth::gs_efx_binding_value(class_row(synth::kGsEfxRowClassDrive, 0), b) ==
          synth::gs_efx_drive_db(b));
  }
  // The silent level byte is -inf dB before the floor, which the floor holds.
  CHECK(synth::gs_efx_binding_value(class_row(synth::kGsEfxClassLevel, 0), 0) >= -24.0f);
}

TEST_CASE("binding_value picks the acceleration quantity from the row's out", "[gs-efx-designed]") {
  for (const uint8_t b : {0, 8, 40, 127}) {
    INFO("byte " << int(b));
    const auto make = [](synth::GsEfxOut out) {
      return class_row(synth::kGsEfxClassAccel, 0, out);
    };
    // Both time-constant keys (accelTauS and decelTauS) read the divisor table as a time.
    CHECK(synth::gs_efx_binding_value(make(synth::GsEfxOut::kAccelTau), b) ==
          gs_efx_accel_tau_s(b));
    CHECK(synth::gs_efx_binding_value(make(synth::GsEfxOut::kDecelTau), b) ==
          gs_efx_accel_tau_s(b));
    CHECK(synth::gs_efx_binding_value(make(synth::GsEfxOut::kUndershootHz), b) ==
          gs_efx_accel_undershoot_hz(b));
    // kValue is the default a non-accel row carries; on an accel row it is the time constant.
    CHECK(synth::gs_efx_binding_value(make(synth::GsEfxOut::kValue), b) == gs_efx_accel_tau_s(b));
  }
}

TEST_CASE("binding_value reads a ratio row between its own endpoints", "[gs-efx-designed]") {
  // Percent (table 0): -98..+98 over bytes 0F..71, returned as a fraction.
  synth::GsEfxBindingRow percent = class_row(synth::kGsEfxRowClassRatio, 0);
  percent.byte_lo = 0x0F;
  percent.byte_hi = 0x71;
  percent.unit_lo = -98;
  percent.unit_hi = 98;
  // Semitones (table 1): -24..+12 over bytes 28..4C, returned as printed.
  synth::GsEfxBindingRow semis = class_row(synth::kGsEfxRowClassRatio, 1);
  semis.byte_lo = 0x28;
  semis.byte_hi = 0x4C;
  semis.unit_lo = -24;
  semis.unit_hi = 12;
  for (int b = 0; b <= 127; ++b) {
    const uint8_t byte = static_cast<uint8_t>(b);
    float units = 0.0f;
    REQUIRE(gs_efx_ratio(byte, 0x0F, 0x71, -98, 98, &units));
    CHECK(synth::gs_efx_binding_value(percent, byte) == units / 100.0f);
    REQUIRE(gs_efx_ratio(byte, 0x28, 0x4C, -24, 12, &units));
    CHECK(synth::gs_efx_binding_value(semis, byte) == units);
  }
  CHECK(synth::gs_efx_binding_value(percent, 0x71) == Approx(0.98f));
  CHECK(synth::gs_efx_binding_value(semis, 0x28) == -24.0f);
}

TEST_CASE("binding_value reads an invented row through its own law", "[gs-efx-designed]") {
  synth::GsEfxBindingRow row{};
  row.kind = synth::kGsEfxRowDesigned;
  row.law = law_of(synth::kGsEfxFormLog, 200.0f, 8000.0f);
  row.byte_lo = 0;
  row.byte_hi = 127;
  CHECK(synth::gs_efx_binding_value(row, 0) == Approx(200.0f));
  CHECK(synth::gs_efx_binding_value(row, 127) == Approx(8000.0f));
  row.byte_hi = 90;
  CHECK(synth::gs_efx_binding_value(row, 90) == Approx(8000.0f));
  CHECK(synth::gs_efx_binding_value(row, 127) == Approx(8000.0f));
  CHECK(synth::gs_efx_binding_value(row, 45) == Approx(std::sqrt(200.0f * 8000.0f)));
}

TEST_CASE("enable_on reads an on-state mask and a selector", "[gs-efx-designed]") {
  synth::GsEfxEnable sw{};
  sw.mode = synth::kGsEfxEnableStages;
  sw.n_stages = 1;
  sw.on_mask[0] = 0x2u;         // state 1
  sw.on_mask[1] = 0x1u;         // byte 32
  sw.on_mask[3] = 0x80000000u;  // byte 127
  CHECK_FALSE(synth::gs_efx_enable_on(sw, 0, 0));
  CHECK(synth::gs_efx_enable_on(sw, 1, 0));
  CHECK_FALSE(synth::gs_efx_enable_on(sw, 2, 0));
  CHECK(synth::gs_efx_enable_on(sw, 32, 0));
  CHECK_FALSE(synth::gs_efx_enable_on(sw, 33, 0));
  CHECK(synth::gs_efx_enable_on(sw, 127, 0));
  CHECK_FALSE(synth::gs_efx_enable_on(sw, 126, 0));

  synth::GsEfxEnable pick{};
  pick.mode = synth::kGsEfxEnableSelect;
  pick.n_stages = 3;
  for (uint8_t stage = 0; stage < 3; ++stage) {
    for (uint8_t b = 0; b < 3; ++b) {
      CHECK(synth::gs_efx_enable_on(pick, b, stage) == (b == stage));
    }
    // Past the printed states the byte reads as state 0.
    CHECK(synth::gs_efx_enable_on(pick, 3, stage) == (stage == 0));
    CHECK(synth::gs_efx_enable_on(pick, 127, stage) == (stage == 0));
  }
}
