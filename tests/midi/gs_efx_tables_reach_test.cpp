/// @file gs_efx_tables_reach_test.cpp
/// @brief Walks the committed gs_efx_tables.h header against a hand-written,
///        fixed enumeration of the 11 conversion classes and fails if any
///        class reaches zero (type, slot) pairs. Runs with no archive present,
///        so it is the CI-side backstop for a derivation script that goes
///        silently empty.
///
/// `make gs-efx-tables-check` only runs where the measurement archive is, which
/// is one machine. This runs everywhere and sees the one failure that matters
/// most: a class that stopped being fed produces exactly what a class that was
/// never wired produces, and both look like a clean run.
///
/// **The eleven classes and the eighteen tables are written out by hand here.**
/// Iterating a list the generator wrote would pass by not looking -- a
/// derivation that dropped a class drops its count with it -- so the header
/// exposes each as a named constant and the enumeration below is the only place
/// that says how many there should be. A class removed from the header is a
/// compile error in this file, which is the point.
///
/// One table is legitimately at zero and is required to be, not excused: the
/// unit's grid carries two delay ladders printed to the same ends, and the one
/// slot citing the second sits on a type the delay claim does not name. See
/// tools/gs/docs/efx-tables.md, "A table no reached slot uses" -- that reading
/// was corroborated at all 35 of the slot's settings, so it is a measured
/// finding and a change to it belongs in an edit to this file.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>

#include "midi/synth/gs_efx_tables.h"

namespace {

namespace s = sonare::midi::synth;

using s::GsEfxSlotConversion;
using s::kGsEfxSlotConversions;

/// One conversion class, named and counted by hand.
struct ClassReach {
  const char* name;
  uint8_t conversion_class;
  int reach;
};

/// The eleven. Written out rather than read from the header's own list.
constexpr std::array<ClassReach, 11> kClasses = {{
    {"rate", s::kGsEfxClassRate, s::kGsEfxReachRate},
    {"delay_time", s::kGsEfxClassDelayTime, s::kGsEfxReachDelayTime},
    {"freq", s::kGsEfxClassFreq, s::kGsEfxReachFreq},
    {"gain", s::kGsEfxClassGain, s::kGsEfxReachGain},
    {"level", s::kGsEfxClassLevel, s::kGsEfxReachLevel},
    {"width", s::kGsEfxClassWidth, s::kGsEfxReachWidth},
    {"wave", s::kGsEfxClassWave, s::kGsEfxReachWave},
    {"pan", s::kGsEfxClassPan, s::kGsEfxReachPan},
    {"balance", s::kGsEfxClassBalance, s::kGsEfxReachBalance},
    {"azimuth", s::kGsEfxClassAzimuth, s::kGsEfxReachAzimuth},
    {"accel", s::kGsEfxClassAccel, s::kGsEfxReachAccel},
}};

/// One table of one class, named and counted by hand. `expected` is the count
/// the header claims; `must_be_zero` marks the one the archive established has
/// no slot, so its zero is pinned rather than tolerated.
struct TableReach {
  const char* name;
  uint8_t conversion_class;
  uint8_t table;
  int expected;
  bool must_be_zero;
};

/// The eighteen, in the header's own declaration order.
constexpr std::array<TableReach, 18> kTables = {{
    {"rate.narrow", s::kGsEfxClassRate, 0, s::kGsEfxTableUseRateNarrow, false},
    {"rate.wide", s::kGsEfxClassRate, 1, s::kGsEfxTableUseRateWide, false},
    {"delay_time.pre_delay", s::kGsEfxClassDelayTime, 0, s::kGsEfxTableUseDelayTimePreDelay, false},
    {"delay_time.time1", s::kGsEfxClassDelayTime, 1, s::kGsEfxTableUseDelayTimeTime1, false},
    {"delay_time.time2", s::kGsEfxClassDelayTime, 2, s::kGsEfxTableUseDelayTimeTime2, true},
    {"delay_time.time3", s::kGsEfxClassDelayTime, 3, s::kGsEfxTableUseDelayTimeTime3, false},
    {"delay_time.time4", s::kGsEfxClassDelayTime, 4, s::kGsEfxTableUseDelayTimeTime4, false},
    {"freq.eq", s::kGsEfxClassFreq, 0, s::kGsEfxTableUseFreqEq, false},
    {"freq.pre_filter", s::kGsEfxClassFreq, 1, s::kGsEfxTableUseFreqPreFilter, false},
    {"freq.damping", s::kGsEfxClassFreq, 2, s::kGsEfxTableUseFreqDamping, false},
    {"gain.tone", s::kGsEfxClassGain, 0, s::kGsEfxTableUseGainTone, false},
    {"level.output", s::kGsEfxClassLevel, 0, s::kGsEfxTableUseLevelOutput, false},
    {"width.section", s::kGsEfxClassWidth, 0, s::kGsEfxTableUseWidthSection, false},
    {"wave.modulator", s::kGsEfxClassWave, 0, s::kGsEfxTableUseWaveModulator, false},
    {"pan.output", s::kGsEfxClassPan, 0, s::kGsEfxTableUsePanOutput, false},
    {"balance.effect", s::kGsEfxClassBalance, 0, s::kGsEfxTableUseBalanceEffect, false},
    {"azimuth.placement", s::kGsEfxClassAzimuth, 0, s::kGsEfxTableUseAzimuthPlacement, false},
    {"accel.rotor", s::kGsEfxClassAccel, 0, s::kGsEfxTableUseAccelRotor, false},
}};

int count_class(uint8_t conversion_class) {
  int found = 0;
  for (const GsEfxSlotConversion& entry : kGsEfxSlotConversions) {
    if (entry.conversion_class == conversion_class) ++found;
  }
  return found;
}

int count_table(uint8_t conversion_class, uint8_t table) {
  int found = 0;
  for (const GsEfxSlotConversion& entry : kGsEfxSlotConversions) {
    if (entry.conversion_class == conversion_class && entry.table == table) ++found;
  }
  return found;
}

class Tally {
 public:
  void same(bool held, const std::string& what) {
    ++count_;
    INFO(what);
    CHECK(held);
  }

  int count() const { return count_; }

 private:
  int count_ = 0;
};

}  // namespace

TEST_CASE("every conversion class reaches at least one (type, slot) pair", "[gs-efx-reach]") {
  Tally tally;

  int class_total = 0;
  for (const ClassReach& row : kClasses) {
    const std::string label = std::string("class ") + row.name;
    tally.same(row.reach > 0, label + " reaches no (type, slot) pair");
    // The named count against the conversion list itself: a count left standing
    // while the entries behind it went away says nothing on its own.
    tally.same(count_class(row.conversion_class) == row.reach,
               label + " count disagrees with the conversion list");
    class_total += row.reach;
  }
  tally.same(class_total == s::kGsEfxMeasured,
             "the eleven class counts do not add up to the header's measured count");
  tally.same(static_cast<int>(kGsEfxSlotConversions.size()) == s::kGsEfxMeasured,
             "the conversion list is not as long as the header's measured count");

  int table_total = 0;
  for (const TableReach& row : kTables) {
    const std::string label = std::string("table ") + row.name;
    tally.same(count_table(row.conversion_class, row.table) == row.expected,
               label + " count disagrees with the conversion list");
    if (row.must_be_zero) {
      tally.same(row.expected == 0, label + " is the measured zero and is no longer zero");
    } else {
      tally.same(row.expected > 0, label + " reaches no (type, slot) pair");
    }
    table_total += row.expected;
  }
  tally.same(table_total == s::kGsEfxMeasured,
             "the eighteen table counts do not add up to the header's measured count");

  // The block these counts are a part of. Measured alone reads as an amount
  // understood; against printed it reads as what it is, a fraction, and a
  // header that lost the distinction fails here rather than quietly halving
  // the denominator every count is read against.
  tally.same(s::kGsEfxMeasured > 0, "the tables give no conversion at all");
  tally.same(s::kGsEfxMeasured < s::kGsEfxPrinted,
             "the measured count is not inside the printed parameter block");

  // The derivation's own identity, so a header swapped for another unit's is a
  // failure here rather than a silent change of what every count means.
  tally.same(s::kGsEfxUnitId == "roland-sc8850-01",
             "the tables are not the unit this enumeration was written against");
  tally.same(!s::kGsEfxArchiveRevision.empty(), "the tables carry no archive revision");

  WARN("comparisons: " << tally.count());
  REQUIRE(tally.count() >= 60);
}
