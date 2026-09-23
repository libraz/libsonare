/// @file gs_efx_convert_test.cpp
/// @brief The GS EFX byte-to-physical-unit conversions, checked against the
///        archive's raw measured readings (never against the generated
///        tables the same derivation produced from the same archive).
///
/// Every expectation below is a number transcribed by hand from a measurement
/// record of unit roland-sc8850-01, and every tolerance is a floor that archive
/// measured for itself -- named in the assertion's own message, because a
/// tolerance nobody can trace is a tolerance that was chosen to pass. A raw
/// reading and a table entry never agree exactly (setting 99 of the wide rate
/// range reads 4.9994 Hz where the ladder holds 5.00), so none of these is an
/// equality except where the conversion's own structure makes it one.
///
/// Reach is an output rather than an inference: the case reports how many
/// comparisons it made, and a derivation that stopped feeding a class shows up
/// as a count that fell rather than as a run that looked exactly like a pass.

#include "midi/synth/gs_efx_convert.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace {

using sonare::midi::synth::gs_efx_accel_tau_s;
using sonare::midi::synth::gs_efx_accel_undershoot_hz;
using sonare::midi::synth::gs_efx_azimuth_deg;
using sonare::midi::synth::gs_efx_balance;
using sonare::midi::synth::gs_efx_corner_hz;
using sonare::midi::synth::gs_efx_delay_ms;
using sonare::midi::synth::gs_efx_enum_index;
using sonare::midi::synth::gs_efx_freq_hz;
using sonare::midi::synth::gs_efx_gain_db;
using sonare::midi::synth::gs_efx_level_mul;
using sonare::midi::synth::gs_efx_pan;
using sonare::midi::synth::gs_efx_post_gain_db;
using sonare::midi::synth::gs_efx_rate_hz;
using sonare::midi::synth::gs_efx_ratio;
using sonare::midi::synth::gs_efx_wave;
using sonare::midi::synth::gs_efx_width_q;
using sonare::midi::synth::gs_efx_window_ms;
using sonare::midi::synth::GsEfxWave;
using sonare::midi::synth::GsFreqColumn;
using sonare::midi::synth::GsRateRange;
using sonare::midi::synth::GsShelfSide;
using sonare::midi::synth::GsTimeLadder;

// Measurement floors, each taken from the archive record or claim named beside it.
constexpr double kRateFloorHz = 0.0028;            // rate: the bend confirmed on a second slot
constexpr double kRateWorstOctaves = 0.01013;      // rate: the claim's own worst residual
constexpr double kTimeFloorMs = 0.042;             // time: four addresses of one type agreeing
constexpr double kTimeRecordFloorMs = 0.0834;      // time: the four-tap record's own floor_ms
constexpr double kFreqFloorOctaves = 0.064;        // freq: one table serving two filter shapes
constexpr double kGainFloorDb = 0.24;              // gain: readings four steps past each edge
constexpr double kLevelFloorDb = 0.042;            // level: the held-out type's worst
constexpr double kWidthFloorOctaves = 2.0 / 12.0;  // width: one reading bin per skirt edge
constexpr double kPanFloorDb = 0.54;               // pan: the two sides read against each other
constexpr double kPanCentreFloorDb = 0.17;         // pan: the median over 342 band readings
constexpr double kBalanceFloorDb = 0.031;          // balance: the four predicted corner steps
constexpr double kBalanceHalfStep = 1.0 / 256.0;   // balance: half a step of its own grid

// The rotary loop's fitted constant and the clock division the archive factored it into.
constexpr double kRotorConstantHz = 0.9768;
constexpr double kRotorStepShift = 32768.0 / 32000.0;

/// Counts every comparison it makes, so the case can report its own reach.
class Tally {
 public:
  void near(double got, double reading, double floor_value, const std::string& what) {
    ++count_;
    INFO(what << ": got " << got << ", archive reads " << reading << ", floor " << floor_value);
    CHECK(std::fabs(got - reading) <= floor_value);
  }

  void same(bool held, const std::string& what) {
    ++count_;
    INFO(what);
    CHECK(held);
  }

  int count() const { return count_; }

 private:
  int count_ = 0;
};

double db(double ratio) { return 20.0 * std::log10(ratio); }

double octaves(double a, double b) { return std::log2(a / b); }

/// What a peaking section of this Q measures between its half-gain points.
double bandwidth_octaves(double q) {
  const double half = 1.0 + 1.0 / (2.0 * q * q);
  return std::log2(half + std::sqrt(half * half - 1.0));
}

struct ByteReading {
  uint8_t value;
  double reading;
};

double pan_balance_db(uint8_t value) {
  float left = 0.0f;
  float right = 0.0f;
  gs_efx_pan(value, &left, &right);
  return db(static_cast<double>(left) / static_cast<double>(right));
}

double pan_left(uint8_t value) {
  float left = 0.0f;
  float right = 0.0f;
  gs_efx_pan(value, &left, &right);
  return left;
}

double pan_right(uint8_t value) {
  float left = 0.0f;
  float right = 0.0f;
  gs_efx_pan(value, &left, &right);
  return right;
}

double balance_direct(uint8_t value) {
  float direct = 0.0f;
  float effect = 0.0f;
  gs_efx_balance(value, &direct, &effect);
  return direct;
}

double balance_effect(uint8_t value) {
  float direct = 0.0f;
  float effect = 0.0f;
  gs_efx_balance(value, &direct, &effect);
  return effect;
}

}  // namespace

TEST_CASE("gs_efx_convert reproduces the archive's measured readings", "[gs-efx-convert]") {
  Tally tally;

  // --- rate: two printed ranges over one byte domain -----------------------
  // Wide range, read every setting on one slot: 0.05 Hz a step to 99, then
  // 0.10 to 119, then 0.50 to 125, with 126 and 127 holding the last entry.
  const ByteReading kWideCurve[] = {
      {0, 0.05},    {4, 0.25},    {8, 0.4485},   {32, 1.6505},  {64, 3.2493},
      {95, 4.7998}, {99, 4.9994}, {100, 5.0989}, {119, 7.0009}, {120, 7.5008},
      {125, 9.999}, {126, 9.999}, {127, 9.9993},
  };
  for (const auto& r : kWideCurve) {
    tally.near(gs_efx_rate_hz(r.value, GsRateRange::kWide), r.reading, kRateFloorHz,
               "rate wide, setting " + std::to_string(r.value) + " (01 21 / 40 03 07 curve)");
  }

  // The bend was confirmed on a second slot, which is where the 0.0028 Hz floor
  // beside every rate assertion above comes from.
  const ByteReading kWideSecondSlot[] = {{99, 4.9992}, {119, 7.0004}, {127, 9.9992}};
  for (const auto& r : kWideSecondSlot) {
    tally.near(gs_efx_rate_hz(r.value, GsRateRange::kWide), r.reading, kRateFloorHz,
               "rate wide, setting " + std::to_string(r.value) + " (02 01 / 40 03 09)");
  }

  const ByteReading kWideThirdSlot[] = {{32, 1.6526}, {110, 6.101}, {127, 9.9985}};
  for (const auto& r : kWideThirdSlot) {
    tally.near(gs_efx_rate_hz(r.value, GsRateRange::kWide), r.reading, kRateFloorHz,
               "rate wide, setting " + std::to_string(r.value) + " (01 20 / 40 03 04)");
  }

  const ByteReading kNarrow[] = {
      {32, 1.65}, {110, 5.5497}, {127, 6.3994}, {110, 5.5494}, {127, 6.399}};
  for (const auto& r : kNarrow) {
    tally.near(gs_efx_rate_hz(r.value, GsRateRange::kNarrow), r.reading, kRateFloorHz,
               "rate narrow, setting " + std::to_string(r.value) + " (04 00 / 04 03)");
  }

  // The slot the rate claim scores its own worst residual on, so it carries the
  // claim's worst figure rather than the bend-confirmation floor.
  const ByteReading kNarrowWorstSlot[] = {{110, 5.5434}, {127, 6.3953}};
  for (const auto& r : kNarrowWorstSlot) {
    const double floor_hz = r.reading * (std::exp2(kRateWorstOctaves) - 1.0);
    tally.near(gs_efx_rate_hz(r.value, GsRateRange::kNarrow), r.reading, floor_hz,
               "rate narrow, setting " + std::to_string(r.value) + " (05 00 / 40 03 0F)");
  }

  // --- delay time: five ladders, each entry cut to a whole 32000 Hz sample --
  // Setting 0 of a ladder that starts at zero is left out: the records read it
  // at 0.10 ms, which is their own search floor rather than a delay.
  const ByteReading kLadder500[] = {
      {1, 0.0833},  {2, 0.1875},  {8, 0.7708},  {25, 2.5},    {32, 3.1875}, {50, 5.0},  {51, 5.5},
      {60, 10.0},   {61, 11.0},   {64, 14.0},   {70, 20.0},   {90, 40.0},   {91, 50.0}, {96, 100.0},
      {100, 140.0}, {116, 300.0}, {117, 320.0}, {126, 500.0}, {127, 500.0},
  };
  for (const auto& r : kLadder500) {
    tally.near(gs_efx_delay_ms(r.value, GsTimeLadder::kLadder3), r.reading, kTimeFloorMs,
               "delay 0-500m, setting " + std::to_string(r.value) + " (01 50 / 01 51 / 02 08)");
  }

  const ByteReading kLadder100[] = {
      {2, 0.1875}, {4, 0.375}, {8, 0.7708}, {25, 2.5},   {50, 5.0},   {51, 5.5},    {60, 10.0},
      {64, 14.0},  {99, 49.0}, {100, 50.0}, {101, 52.0}, {110, 70.0}, {125, 100.0}, {127, 100.0},
  };
  for (const auto& r : kLadder100) {
    tally.near(gs_efx_delay_ms(r.value, GsTimeLadder::kLadder0), r.reading, kTimeFloorMs,
               "pre-delay 0-100m, setting " + std::to_string(r.value) + " (01 40 / 40 03 03)");
  }

  const ByteReading kLadder200[] = {{0, 200.0},  {1, 205.0},  {25, 325.0},      {64, 520.0},
                                    {70, 550.0}, {71, 560.0}, {115, 1000.0208}, {127, 1000.0208}};
  for (const auto& r : kLadder200) {
    tally.near(gs_efx_delay_ms(r.value, GsTimeLadder::kLadder1), r.reading, kTimeFloorMs,
               "delay 200-1000, setting " + std::to_string(r.value) + " (01 52 / 40 03 03)");
  }

  const ByteReading kLadder200FourTap[] = {{72, 569.9583}, {82, 670.0625}, {110, 950.0625}};
  for (const auto& r : kLadder200FourTap) {
    tally.near(gs_efx_delay_ms(r.value, GsTimeLadder::kLadder1), r.reading, kTimeRecordFloorMs,
               "delay 200-1000, setting " + std::to_string(r.value) + " (01 53 tap 1)");
  }

  const ByteReading kLadder635[] = {{1, 5.0},    {2, 10.0},    {10, 50.0},   {50, 250.0},
                                    {64, 320.0}, {100, 500.0}, {126, 630.0}, {127, 635.0}};
  for (const auto& r : kLadder635) {
    tally.near(gs_efx_delay_ms(r.value, GsTimeLadder::kLadder4), r.reading, kTimeFloorMs,
               "delay 0-635m, setting " + std::to_string(r.value) + " (04 00 / 40 03 13)");
  }

  // The fifth ladder shares a printed range with the 200-1000 one above and is a
  // different column: the archive reads the two fifty milliseconds apart at 80.
  tally.near(static_cast<double>(gs_efx_delay_ms(80, GsTimeLadder::kLadder1)) -
                 static_cast<double>(gs_efx_delay_ms(80, GsTimeLadder::kLadder2)),
             50.0, kTimeFloorMs, "the two 200-1000 ladders, setting 80 (time claim)");

  // That slot's delay glides, and approached from above the record returns its
  // column times 127/128 -- the same one-byte ratio every other entry carries.
  const ByteReading kLadderGliding[] = {{0, 198.4375}, {60, 496.1042}};
  for (const auto& r : kLadderGliding) {
    const double ratio = 127.0 / 128.0;
    tally.near(static_cast<double>(gs_efx_delay_ms(r.value, GsTimeLadder::kLadder2)) * ratio,
               r.reading, kTimeFloorMs,
               "delay 200-1000 second column, setting " + std::to_string(r.value) + " (01 54)");
  }

  // --- freq: the top four bits index sixteen entries -----------------------
  const ByteReading kFreqEqA[] = {{0, 198.7}, {8, 240.6}, {16, 278.7}, {24, 398.0}};
  for (const auto& r : kFreqEqA) {
    tally.near(octaves(gs_efx_freq_hz(r.value, GsFreqColumn::kColumn0), r.reading), 0.0,
               kFreqFloorOctaves,
               "freq EQ column, setting " + std::to_string(r.value) + " (01 00 / 40 03 0A)");
  }

  // Setting 0 is left out of the two multi-effect takes: they read entry 0 at
  // 178-179 Hz where the two mid-section takes read 198, which is 0.15 octaves
  // apart and the one place in this class the records disagree past the floor.
  const ByteReading kFreqEqB[] = {{16, 279.3},  {32, 485.8},  {48, 783.0},   {64, 1231.6},
                                  {80, 1962.2}, {96, 3079.0}, {112, 4863.5}, {127, 6061.2}};
  for (const auto& r : kFreqEqB) {
    tally.near(octaves(gs_efx_freq_hz(r.value, GsFreqColumn::kColumn0), r.reading), 0.0,
               kFreqFloorOctaves,
               "freq EQ column, setting " + std::to_string(r.value) + " (04 01 / 40 03 0D)");
  }

  const ByteReading kFreqEqC[] = {{16, 280.6}, {64, 1226.1}, {127, 6063.0}};
  for (const auto& r : kFreqEqC) {
    tally.near(octaves(gs_efx_freq_hz(r.value, GsFreqColumn::kColumn0), r.reading), 0.0,
               kFreqFloorOctaves,
               "freq EQ column, setting " + std::to_string(r.value) + " (04 03 / 40 03 08)");
  }

  // Eight settings share an entry, and the entry either side of the plateau is a
  // different one: 279.1, 278.7, 278.8, 279.0 at 16, 17, 20, 23 against 240.6 at 15.
  const ByteReading kFreqPlateau[] = {{17, 278.7}, {20, 278.8}, {23, 279.0}, {15, 240.6}};
  for (const auto& r : kFreqPlateau) {
    tally.near(octaves(gs_efx_freq_hz(r.value, GsFreqColumn::kColumn0), r.reading), 0.0,
               kFreqFloorOctaves,
               "freq EQ plateau, setting " + std::to_string(r.value) + " (01 00 / 40 03 07)");
  }
  tally.same(
      gs_efx_freq_hz(16, GsFreqColumn::kColumn0) == gs_efx_freq_hz(23, GsFreqColumn::kColumn0),
      "freq EQ: settings 16 and 23 are one entry (stride eight)");
  tally.same(
      gs_efx_freq_hz(15, GsFreqColumn::kColumn0) != gs_efx_freq_hz(16, GsFreqColumn::kColumn0),
      "freq EQ: setting 15 is the entry before the plateau");

  // --- gain: one decibel a step over a window, nearest edge outside it ------
  const ByteReading kGainMultiA[] = {{58, -5.92}, {70, 5.9}, {76, 12.07}};
  for (const auto& r : kGainMultiA) {
    tally.near(gs_efx_gain_db(r.value), r.reading, kGainFloorDb,
               "gain, setting " + std::to_string(r.value) + " (04 01 / 40 03 0F)");
  }
  const ByteReading kGainMultiB[] = {{58, -6.04}, {70, 6.05}, {76, 11.99}};
  for (const auto& r : kGainMultiB) {
    tally.near(gs_efx_gain_db(r.value), r.reading, kGainFloorDb,
               "gain, setting " + std::to_string(r.value) + " (04 03 / 40 03 0A)");
  }
  const ByteReading kGainSection[] = {{52, -12.11}, {54, -9.84}, {64, 0.14},
                                      {73, 8.85},   {74, 9.79},  {76, 11.92}};
  for (const auto& r : kGainSection) {
    tally.near(gs_efx_gain_db(r.value), r.reading, kGainFloorDb,
               "gain, setting " + std::to_string(r.value) + " (01 00 / 40 03 0C)");
  }

  // Outside the window the unit returns the window's own end. The evidence is a
  // pair of readings that agree, so the conversion has to return one value twice.
  struct ClampPair {
    uint8_t outside;
    uint8_t edge;
    const char* source;
  };
  const ClampPair kGainClamps[] = {
      {0, 52, "04 01: -11.67 against -11.73 at the edge"},
      {40, 52, "04 01: -11.79 against -11.73 at the edge"},
      {90, 76, "04 01: 12.06 against 12.07 at the edge"},
      {127, 76, "04 01: 12.15 against 12.07 at the edge"},
      {50, 52, "01 00: -12.10 against -12.11 at the edge"},
      {78, 76, "01 00: 11.95 against 11.92 at the edge"},
  };
  for (const auto& c : kGainClamps) {
    tally.same(
        gs_efx_gain_db(c.outside) == gs_efx_gain_db(c.edge),
        std::string("gain clamps at setting ") + std::to_string(c.outside) + " (" + c.source + ")");
  }

  // --- level: a stored numerator over 127 ----------------------------------
  // The records hold absolute levels, so each is read against its own take's
  // setting 127; that removes the chain and leaves the table's own shape.
  struct LevelReading {
    uint8_t value;
    double relative_db;
    const char* source;
  };
  const LevelReading kLevels[] = {
      {4, -85.82 + 53.31, "01 00 / 40 03 16"},   {8, -79.83 + 53.31, "01 00 / 40 03 16"},
      {16, -73.80 + 53.31, "01 00 / 40 03 16"},  {32, -67.78 + 53.31, "01 00 / 40 03 16"},
      {64, -60.91 + 53.31, "01 00 / 40 03 16"},  {96, -56.60 + 53.31, "01 00 / 40 03 16"},
      {8, -87.60 + 61.12, "01 40 / 40 03 16"},   {32, -75.55 + 61.12, "01 40 / 40 03 16"},
      {64, -68.71 + 61.12, "01 40 / 40 03 16"},  {8, -72.57 + 46.06, "01 11 at drive 96"},
      {32, -60.53 + 46.06, "01 11 at drive 96"}, {8, -81.26 + 54.76, "01 11 at drive 0"},
      {32, -69.19 + 54.76, "01 11 at drive 0"},
  };
  for (const auto& r : kLevels) {
    tally.near(db(gs_efx_level_mul(r.value) / gs_efx_level_mul(127)), r.relative_db, kLevelFloorDb,
               std::string("level, setting ") + std::to_string(r.value) + " (" + r.source + ")");
  }
  // Reading the byte straight as the multiplier is out by 3.53 dB at setting 9,
  // which is the claim's own worst case and the reason a table is stored at all.
  tally.near(db((9.0 / 127.0) / gs_efx_level_mul(9)), 3.53, kLevelFloorDb,
             "level: the byte read straight, setting 9 (level claim)");
  tally.same(gs_efx_level_mul(127) == 1.0f, "level: the table reaches full scale at 127");

  // --- width: five entries, read between the section's half-gain points ----
  struct WidthReading {
    uint8_t value;
    double below_hz;
    double above_hz;
    const char* source;
  };
  const WidthReading kWidths[] = {
      {0, 280.6, 5039.7, "01 00 / 40 03 08"},
      {1, 500.0, 2996.6, "01 00 / 40 03 08"},
      {2, 749.2, 2000.0, "01 00 / 40 03 08"},
      {3, 943.9, 1587.4, "01 00 / 40 03 08"},
      {4, 1059.5, 1414.2, "01 00 / 40 03 08"},
      {0, 222.7, 4000.0, "04 01 / 40 03 0E"},
      {1, 396.9, 2378.4, "04 01 / 40 03 0E"},
      {2, 594.6, 1587.4, "04 01 / 40 03 0E"},
      {3, 749.2, 1259.9, "04 01 / 40 03 0E"},
      {4, 840.9, 1122.5, "04 01 / 40 03 0E"},
      {5, 280.6, 5039.7, "01 00: past the table, entry 0"},
      {64, 280.6, 5039.7, "01 00: past the table, entry 0"},
      {127, 297.3, 5039.7, "01 00: past the table, entry 0"},
  };
  for (const auto& r : kWidths) {
    tally.near(bandwidth_octaves(gs_efx_width_q(r.value)), octaves(r.above_hz, r.below_hz),
               kWidthFloorOctaves,
               std::string("width, setting ") + std::to_string(r.value) + " (" + r.source + ")");
  }

  // --- wave: five fixed shapes, told apart by their harmonics --------------
  tally.same(gs_efx_wave(0) == GsEfxWave::kTriangle,
             "wave 0: third harmonic 0.0897 where a triangle has a ninth (01 25 / 40 03 03)");
  tally.same(gs_efx_wave(1) == GsEfxWave::kSquare,
             "wave 1: third 0.3178 and fifth 0.1690, a square's thirds and fifths");
  tally.same(gs_efx_wave(2) == GsEfxWave::kSine,
             "wave 2: third 0.0586 and fifth 0.0015, neither a triangle nor a square");
  tally.same(gs_efx_wave(3) == GsEfxWave::kSawUp,
             "wave 3: climbs 0.9062 of its cycle (01 25 / 01 26 / 04 06 alike)");
  tally.same(gs_efx_wave(4) == GsEfxWave::kSawDown,
             "wave 4: climbs 0.0781 of its cycle, the same shape run the other way");
  tally.same(gs_efx_wave(5) == gs_efx_wave(0), "wave: past the printed list, entry 0");

  // --- pan: one multiplier a side, under a term added after it -------------
  // The broadband readings carry that added term, so they are compared only
  // through the middle of the sweep, which is where the claim says they agree.
  struct PanReading {
    uint8_t value;
    double balance_db;
    const char* source;
  };
  const PanReading kPans[] = {
      {8, 13.09, "broadband voice"}, {8, 13.04, "piano voice"}, {32, 4.51, "01 01"},
      {32, 4.46, "01 03"},           {64, 0.06, "01 01"},       {64, -0.14, "01 03"},
      {96, -4.56, "01 01"},          {96, -4.94, "01 03"},
  };
  for (const auto& r : kPans) {
    tally.near(pan_balance_db(r.value), r.balance_db, kPanFloorDb,
               std::string("pan, setting ") + std::to_string(r.value) + " (" + r.source + ")");
  }
  // The centre entry sits 2.44 dB under its end -- neither the 3.01 of a
  // constant-power pair nor the 6.02 of a straight linear crossfade.
  tally.near(db(pan_left(64) / pan_left(0)), -2.44, kPanCentreFloorDb,
             "pan: the centre entry under its end (40 03 15)");
  tally.same(std::fabs(db(pan_left(64) / pan_left(0)) + 3.01) > kPanCentreFloorDb,
             "pan: the centre is not a constant-power pair");
  tally.same(std::fabs(db(pan_left(64) / pan_left(0)) + 6.02) > kPanCentreFloorDb,
             "pan: the centre is not a linear crossfade");
  // The two sides are one table read in opposite directions, 0.54 dB apart at worst.
  tally.near(db(pan_left(32) / pan_right(95)), 0.0, kPanFloorDb,
             "pan: setting 32 left against setting 95 right");
  tally.near(db(pan_left(64) / pan_right(63)), 0.0, kPanFloorDb,
             "pan: setting 64 left against setting 63 right");

  // --- balance: two truncated ramps that meet at full ----------------------
  // Four steps over the effect half's corner, each an uneven size because the
  // ramp is truncated onto its grid; reflecting through 127 instead of 128
  // would put them 0.175-0.200 dB out, well over the floor used here.
  struct BalanceStep {
    uint8_t from;
    uint8_t to;
    double step_db;
  };
  const BalanceStep kBalanceSteps[] = {
      {60, 61, 0.2075}, {61, 62, 0.1350}, {62, 63, 0.1975}, {63, 64, 0.1850}};
  for (const auto& s : kBalanceSteps) {
    tally.near(db(balance_effect(s.to) / balance_effect(s.from)), s.step_db, kBalanceFloorDb,
               "balance effect half, step " + std::to_string(s.from) + "->" + std::to_string(s.to) +
                   " (01 50 / 40 03 12)");
  }
  tally.same(balance_effect(65) == balance_effect(64),
             "balance effect half: rises for the last time at 64 (01 57: -49.07 both)");
  tally.same(balance_effect(0) == 0.0f,
             "balance effect half: nothing at 0 (01 57 late reads its own floor)");

  const ByteReading kBalanceDirect[] = {
      {65, 0.976096}, {72, 0.832505}, {108, 0.117816}, {113, 0.015789}};
  for (const auto& r : kBalanceDirect) {
    tally.near(balance_direct(r.value) / balance_direct(64), r.reading, kBalanceHalfStep,
               "balance direct half, setting " + std::to_string(r.value) +
                   " (01 57 early, against its own setting 64)");
  }
  tally.same(balance_direct(0) == balance_direct(64),
             "balance direct half: flat to 64 (01 57: -48.42 against -48.43)");
  tally.same(balance_direct(114) == 0.0f,
             "balance direct half: nothing from 114 on (01 57: -109.5)");
  tally.same(balance_direct(127) == 0.0f, "balance direct half: still nothing at 127");

  // --- azimuth: rounded to a quarter turn, then clamped --------------------
  struct AzimuthReading {
    uint8_t value;
    int degrees;
    const char* source;
  };
  const AzimuthReading kAzimuths[] = {
      {0, -180, "01 70: 0.13 dB apart, a pole"},    {5, -180, "01 70: 0.14 dB, still the pole"},
      {6, -168, "01 71: 2.07 dB, off the pole"},    {35, -84, "01 70: 13.17 dB"},
      {63, 0, "01 70: 0.15 dB, the centre"},        {91, 84, "01 70: -12.81 dB, the mirror of 35"},
      {123, 180, "01 70: 0.12 dB, the other pole"}, {127, 180, "01 71: 0.12 dB"},
  };
  for (const auto& r : kAzimuths) {
    tally.same(gs_efx_azimuth_deg(r.value) == r.degrees,
               std::string("azimuth, setting ") + std::to_string(r.value) + " (" + r.source + ")");
  }
  tally.same(gs_efx_azimuth_deg(35) == -gs_efx_azimuth_deg(91),
             "azimuth: the table is a mirror (13.17 dB against -12.81 dB)");
  tally.same(gs_efx_azimuth_deg(0) == -gs_efx_azimuth_deg(123),
             "azimuth: the two poles are one place, half a turn either way");

  // --- acceleration: four bits, held as a time constant in seconds ---------
  // The rotor stops short of the rate it was aimed at by the loop's constant
  // over that entry's divisor, so the reading is a rate gap and is compared as
  // one. The target is itself a reading: the same entry reached from above.
  struct AccelReading {
    uint8_t value;
    double target_hz;
    double reached_hz;
    const char* source;
  };
  const AccelReading kAccels[] = {
      {64, 0.8488, 0.7401, "01 22 / 40 03 05 aimed lowest"},
      {80, 0.8488, 0.7673, "01 22 / 40 03 05 aimed lowest"},
      {96, 0.8488, 0.8180, "01 22 / 40 03 05 aimed lowest"},
      {104, 0.8488, 0.8335, "01 22 / 40 03 05 aimed lowest"},
      {112, 0.8488, 0.8410, "01 22 / 40 03 05 aimed lowest"},
      {120, 0.8488, 0.8430, "01 22 / 40 03 05 aimed lowest"},
      {127, 0.8488, 0.8430, "01 22 / 40 03 05 aimed lowest"},
      {64, 2.0491, 1.9425, "01 22 / 40 03 05 aimed a fifth up"},
  };
  for (const auto& r : kAccels) {
    const double predicted_gap =
        kRotorConstantHz * kRotorStepShift / static_cast<double>(gs_efx_accel_tau_s(r.value));
    tally.near(predicted_gap, r.target_hz - r.reached_hz, kRateFloorHz,
               std::string("accel, setting ") + std::to_string(r.value) + " (" + r.source + ")");
  }
  tally.same(gs_efx_accel_tau_s(120) == gs_efx_accel_tau_s(127),
             "accel: eight settings share an entry (both read 0.843 Hz)");

  // The same byte also sets how far short of its target the rotor stops on the
  // way up, and the record publishes that distance per entry with the source of
  // the rate it was subtracted from named beside it. Each carries its own take's
  // resolution as its floor, because the distances run from three tenths of a
  // hertz to five thousandths and one figure over all of them is two orders out
  // at one end. Entry 7 is left out: the record itself publishes it as 1.23
  // times its take's own resolution, which is the one entry it calls outside.
  struct UndershootReading {
    uint8_t value;
    double distance_hz;
    double take_floor_hz;
    const char* aim;
  };
  const UndershootReading kUndershoots[] = {
      {16, 0.32370, 0.0022, "aimed by the fitted rate table"},
      {24, 0.24420, 0.0007, "aimed at 0.8488, landed on from above"},
      {32, 0.19870, 0.0084, "aimed by the fitted rate table"},
      {40, 0.16560, 0.0084, "aimed by the fitted rate table"},
      {48, 0.14330, 0.0078, "aimed by the fitted rate table"},
      {64, 0.10870, 0.0006, "aimed at a rate the unit stated"},
      {72, 0.09680, 0.0028, "aimed at a rate the unit stated"},
      {80, 0.08150, 0.0013, "aimed at a rate the unit stated"},
      {88, 0.06110, 0.0008, "aimed at a rate the unit stated"},
      {96, 0.03080, 0.0003, "aimed at a rate the unit stated"},
      {104, 0.01530, 0.0002, "aimed at a rate the unit stated"},
      {112, 0.00780, 0.0003, "aimed at a rate the unit stated"},
      {120, 0.00580, 0.0004, "the entry off the doubling, 168 give or take 12"},
  };
  for (const auto& r : kUndershoots) {
    tally.near(
        gs_efx_accel_undershoot_hz(r.value), r.distance_hz, r.take_floor_hz,
        std::string("accel undershoot, setting ") + std::to_string(r.value) + " (" + r.aim + ")");
  }

  // Settings inside one entry return one distance, which is what says the byte
  // is read through four bits and not whole. Both pairs are power-on bytes.
  const ByteReading kUndershootWithinEntry[] = {
      {24, 0.2454}, {31, 0.2450}, {88, 0.0601}, {95, 0.0597}};
  for (const auto& r : kUndershootWithinEntry) {
    tally.near(gs_efx_accel_undershoot_hz(r.value), r.reading, kRateFloorHz,
               "accel undershoot within one entry, setting " + std::to_string(r.value) +
                   " (01 22 / 40 03 05 and 40 03 09)");
  }
  tally.same(gs_efx_accel_undershoot_hz(24) == gs_efx_accel_undershoot_hz(31),
             "accel undershoot: settings 24 and 31 are one entry");
  tally.same(gs_efx_accel_undershoot_hz(88) == gs_efx_accel_undershoot_hz(95),
             "accel undershoot: settings 88 and 95 are one entry");

  // The two figures a rotor's shortfall was taken for a property of that rotor
  // are where its own power-on acceleration byte sits in this one table.
  tally.near(gs_efx_accel_undershoot_hz(24), 0.245, kRateFloorHz,
             "accel undershoot: the low rotor at its power-on byte 24");
  tally.near(gs_efx_accel_undershoot_hz(88), 0.062, kRateFloorHz,
             "accel undershoot: the high rotor at its power-on byte 88");
  tally.same(gs_efx_accel_undershoot_hz(24) >= 0.244f && gs_efx_accel_undershoot_hz(24) <= 0.246f,
             "accel undershoot: inside the 0.244-0.246 Hz the low rotor held across its rate byte");

  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 40);
}

TEST_CASE("gs_efx_ratio reads a slot only where the printed ends force one step",
          "[gs-efx-convert]") {
  Tally tally;

  // The three printed ranges whose ends admit exactly one step. Each spans a
  // whole number of units per byte, which is what separates reading them from
  // fitting them -- no table was measured for any of these slots.
  struct Forced {
    int lo_byte, hi_byte, lo_unit, hi_unit, per_byte;
    const char* printed;
  };
  constexpr std::array<Forced, 3> kForced = {{
      {15, 113, -98, 98, 2, "0F-71"},
      {14, 114, -100, 100, 2, "0E-72"},
      {0, 90, 0, 180, 2, "00-5A"},
  }};

  for (const auto& r : kForced) {
    const std::string at = std::string("printed ") + r.printed;
    float lo = 0.0F;
    float hi = 0.0F;
    float mid = 0.0F;
    tally.same(gs_efx_ratio(static_cast<uint8_t>(r.lo_byte), r.lo_byte, r.hi_byte, r.lo_unit,
                            r.hi_unit, &lo),
               at + ": the low end is read");
    tally.same(gs_efx_ratio(static_cast<uint8_t>(r.hi_byte), r.lo_byte, r.hi_byte, r.lo_unit,
                            r.hi_unit, &hi),
               at + ": the high end is read");
    tally.same(lo == static_cast<float>(r.lo_unit), at + ": the low end returns its printed unit");
    tally.same(hi == static_cast<float>(r.hi_unit), at + ": the high end returns its printed unit");

    const int one_up = r.lo_byte + 1;
    tally.same(gs_efx_ratio(static_cast<uint8_t>(one_up), r.lo_byte, r.hi_byte, r.lo_unit,
                            r.hi_unit, &mid),
               at + ": one byte above the low end is read");
    tally.same(mid == static_cast<float>(r.lo_unit + r.per_byte),
               at + ": one byte moves exactly one step");

    // Outside the range there is no reading to take, so the nearer end stands.
    float under = 0.0F;
    float over = 0.0F;
    if (r.lo_byte > 0) {
      tally.same(gs_efx_ratio(0, r.lo_byte, r.hi_byte, r.lo_unit, r.hi_unit, &under) &&
                     under == static_cast<float>(r.lo_unit),
                 at + ": a byte below the range clamps to the low end");
    }
    tally.same(gs_efx_ratio(127, r.lo_byte, r.hi_byte, r.lo_unit, r.hi_unit, &over) &&
                   over == static_cast<float>(r.hi_unit),
               at + ": a byte above the range clamps to the high end");
  }

  // The refusal. A span the byte count does not divide has no step the ends
  // force, and the function must not pick the nearest one -- nothing
  // downstream could tell the result from a measured conversion.
  float written = -1.0F;
  tally.same(!gs_efx_ratio(50, 15, 113, 0, 100, &written),
             "a span of 100 over 98 steps is refused rather than rounded");
  tally.same(written == -1.0F, "a refused reading writes nothing");
  tally.same(!gs_efx_ratio(50, 15, 113, -98, 98, nullptr), "no output means no reading");
  tally.same(!gs_efx_ratio(50, 113, 113, -98, 98, &written), "an empty range is refused");
  tally.same(!gs_efx_ratio(50, 113, 15, -98, 98, &written), "a reversed range is refused");
  tally.same(written == -1.0F, "none of the four refusals wrote");

  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 24);
}

TEST_CASE("post gain and the splice window reproduce the archive's readings", "[gs-efx-convert]") {
  Tally tally;

  // Post gain: each set's level at settings 1-3 against its own setting 0, so
  // the stage in front of the byte cancels. Floor: the spread the claim reads
  // over its nine steps.
  constexpr double kPostGainFloorDb = 0.09;
  struct PostGainSet {
    std::array<double, 4> heard_db;
    const char* source;
  };
  const PostGainSet kPostGainSets[] = {
      {{-45.29, -39.27, -33.24, -27.3}, "01 30 / 40 03 05, the compressor"},
      {{-51.63, -45.61, -39.59, -33.64}, "01 31 / 40 03 06, threshold at the top"},
      {{-80.55, -74.52, -68.5, -62.54}, "01 31 / 40 03 06, threshold at nought"},
  };
  for (const auto& set : kPostGainSets) {
    for (uint8_t setting = 1; setting < 4; ++setting) {
      tally.near(gs_efx_post_gain_db(setting) - gs_efx_post_gain_db(0),
                 set.heard_db[setting] - set.heard_db[0], kPostGainFloorDb,
                 "post gain, setting " + std::to_string(setting) + " (" + set.source + ")");
    }
  }
  tally.same(gs_efx_post_gain_db(0) == 0.0f, "post gain: setting 0 adds nothing");
  tally.same(gs_efx_post_gain_db(4) == gs_efx_post_gain_db(0),
             "post gain: past the four printed settings, the first");

  // Window: both shifters of 01 60 read at the octave. Floor: the worst the two
  // shifters disagree by, four point six parts in ten thousand.
  constexpr double kWindowFloorRelative = 4.6e-4;
  const std::array<double, 5> kFirstShifterMs = {31.9977, 42.6545, 63.9853, 85.2988, 127.9926};
  const std::array<double, 5> kSecondShifterMs = {31.9988, 42.6585, 63.9734, 85.3046, 128.0516};
  for (uint8_t state = 0; state < 5; ++state) {
    for (const auto* reading : {&kFirstShifterMs, &kSecondShifterMs}) {
      const double ms = (*reading)[state];
      tally.near(gs_efx_window_ms(state), ms, ms * kWindowFloorRelative,
                 "window, state " + std::to_string(state) + " (01 60 / 40 03 0B)");
    }
  }

  // 01 61 reads a distance a constant multiple of the window, so its five means
  // are held to the ratio the windows stand in. Floor: four takes of one state.
  constexpr double kFeedbackShifterRepeatsMs = 0.0209;
  const std::array<double, 5> kFeedbackShifterMs = {28.505, 38.0, 57.0, 75.995, 114.026};
  for (uint8_t state = 1; state < 5; ++state) {
    const double predicted = kFeedbackShifterMs[0] * gs_efx_window_ms(state) / gs_efx_window_ms(0);
    tally.near(predicted, kFeedbackShifterMs[state], kFeedbackShifterRepeatsMs,
               "window ratio, state " + std::to_string(state) + " (01 61 / 40 03 07)");
  }
  tally.same(gs_efx_window_ms(5) == gs_efx_window_ms(0),
             "window: past the five printed states, the first");

  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 25);
}

TEST_CASE("an equaliser corner byte reproduces the archive's half-gain points",
          "[gs-efx-convert]") {
  Tally tally;

  // 01 00, each corner byte read at its two printed states: the half-gain point
  // of the shelf's deviation at full boost. Floor: one band of the
  // twelfth-octave set the points are read on.
  constexpr double kCornerFloorOctaves = 1.0 / 12.0;
  const std::array<double, 2> kLowHz = {118.0, 222.7};
  const std::array<double, 2> kHighHz = {6727.2, 11313.7};
  for (uint8_t setting = 0; setting < 2; ++setting) {
    tally.near(std::log2(gs_efx_corner_hz(setting, GsShelfSide::kLow) / kLowHz[setting]), 0.0,
               kCornerFloorOctaves,
               "low corner, setting " + std::to_string(setting) + " (40 03 03)");
    tally.near(std::log2(gs_efx_corner_hz(setting, GsShelfSide::kHigh) / kHighHz[setting]), 0.0,
               kCornerFloorOctaves,
               "high corner, setting " + std::to_string(setting) + " (40 03 05)");
  }
  for (const GsShelfSide side : {GsShelfSide::kLow, GsShelfSide::kHigh}) {
    tally.same(gs_efx_corner_hz(1, side) > gs_efx_corner_hz(0, side) * 1.5f,
               "corner: the second state sits well above the first");
  }

  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 6);
}

TEST_CASE("gs_efx_enum_index returns the first state past the printed list", "[gs-efx-convert]") {
  Tally tally;

  tally.same(gs_efx_enum_index(0, 5) == 0, "the first state");
  tally.same(gs_efx_enum_index(4, 5) == 4, "the last state of five");
  // Past the list the unit keeps the state it was in, which a pure conversion
  // cannot see. A slot printing a list never takes such a byte, so entry 0
  // answers only where no write rule stands in front of the conversion.
  tally.same(gs_efx_enum_index(5, 5) == 0, "one past the list");
  tally.same(gs_efx_enum_index(127, 5) == 0, "far past the list");
  tally.same(gs_efx_enum_index(0, 2) == 0, "a two-state switch, off");
  tally.same(gs_efx_enum_index(1, 2) == 1, "a two-state switch, on");
  tally.same(gs_efx_enum_index(2, 2) == 0, "a two-state switch, past its list");
  tally.same(gs_efx_enum_index(3, 0) == 0, "an empty list has no state but the first");

  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 8);
}
