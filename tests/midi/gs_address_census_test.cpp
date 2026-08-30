/// @file gs_address_census_test.cpp
/// @brief How much of the GS address space that real files actually use the
///        address table has taken a position on.
///
/// docs/gs.md says an address with no row is a defect rather than a silence,
/// and that coverage is a number the table and its test produce. This is that
/// number, measured against addresses somebody else wrote: a hand-authored
/// corpus can only contain what its author already knew about, so it would
/// prove the table covers the table.
///
/// The census is generated from a corpus that is not in the repository
/// (tools/gs/docs/census.md). It is committed, so this runs in a fresh clone.
///
/// The gate is a ratchet, not a pass/fail on completeness: the ceilings only
/// ever move down, and a change that leaves a newly reachable address unrowed
/// pushes the count past one of them.

#include "midi/gs_address_census.inc"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "midi/synth/gs_address_table.h"

namespace {

using sonare::midi::synth::gs_lookup_address;
using sonare::midi::synth::gs_lookup_range;
using sonare::test::kGsCensusAddresses;

bool covered(uint32_t addr) {
  return gs_lookup_address(addr) != nullptr || gs_lookup_range(addr) != nullptr;
}

std::string hex(uint32_t addr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02X %02X %02X", addr >> 16, (addr >> 8) & 0xFF, addr & 0xFF);
  return buf;
}

/// Distinct census addresses no row claims. Lower this with the change that
/// lowers it; it must never rise.
///
/// Both ceilings are relative to one census. Refreshing it over a larger corpus
/// raises them by surfacing addresses nobody had seen, which is the census
/// working rather than coverage regressing — re-record them in the same change,
/// and compare the RATIO across a refresh, never the count.
constexpr size_t kUnrowedAddressCeiling = 3428;

/// Corpus file-touches (an address counted once per file that wrote it) no row
/// claims. Weighted, because one address reached by 1300 files is not the same
/// size of gap as one reached by 2 — and the two numbers move independently, so
/// a change that rows a rare address while breaking a common one is caught by
/// the second ceiling even though the first went down.
constexpr uint64_t kUnrowedFileTouchCeiling = 25907;

/// A ceiling nobody lowers stops describing the table and starts describing
/// whenever it was last written, so a run far enough under one fails too, with
/// the number to write. The slack is wide enough that a phase in progress does
/// not trip it and narrow enough that a finished phase does.
constexpr size_t kUnrowedAddressSlack = 120;
/// The heaviest single row still missing is 1344 touches, so 1500 let a whole
/// row land without asking for a re-record. This slack's failure mode is
/// silence, and the cost of tripping it is one number.
constexpr uint64_t kUnrowedFileTouchSlack = 500;

}  // namespace

TEST_CASE("GS address table coverage against a corpus of real files", "[midi][synth][gs]") {
  // A lookup that answered everything would report perfect coverage. An address
  // the table cannot hold a row for keeps that from reading as success.
  REQUIRE_FALSE(covered(0x7F7F7F));
  // And one it does hold keeps a lookup that answers nothing from reading as a
  // ceiling that is merely generous.
  REQUIRE(covered(0x400004));  // MASTER VOLUME, 580 corpus files

  size_t unrowed = 0;
  uint64_t unrowed_touches = 0;
  uint64_t total_touches = 0;
  std::vector<sonare::test::GsCensusAddress> worst;

  for (const sonare::test::GsCensusAddress& entry : kGsCensusAddresses) {
    total_touches += entry.files;
    if (covered(entry.addr)) continue;
    ++unrowed;
    unrowed_touches += entry.files;
    worst.push_back(entry);
  }

  std::sort(worst.begin(), worst.end(),
            [](const sonare::test::GsCensusAddress& a, const sonare::test::GsCensusAddress& b) {
              return a.files > b.files;
            });

  std::string report;
  for (size_t i = 0; i < worst.size() && i < 20; ++i) {
    report += "\n  " + hex(worst[i].addr) + "  " + std::to_string(worst[i].files) + " files";
  }
  INFO("census: " << kGsCensusAddresses.size() << " addresses, " << total_touches
                  << " file-touches; unrowed " << unrowed << " / " << unrowed_touches
                  << ". Widest gaps:" << report);

  CHECK(unrowed <= kUnrowedAddressCeiling);
  CHECK(unrowed_touches <= kUnrowedFileTouchCeiling);

  INFO("coverage improved: set kUnrowedAddressCeiling = "
       << unrowed << " and kUnrowedFileTouchCeiling = " << unrowed_touches);
  CHECK(unrowed + kUnrowedAddressSlack >= kUnrowedAddressCeiling);
  CHECK(unrowed_touches + kUnrowedFileTouchSlack >= kUnrowedFileTouchCeiling);
}
