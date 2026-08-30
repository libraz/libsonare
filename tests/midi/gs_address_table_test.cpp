/// @file gs_address_table_test.cpp
/// @brief The GS address table and the SysEx frame/decode layer that walks it:
///        every defined row decodes, every kIgnore/kAccept row carries a
///        reason, no two rows claim an address, a malformed frame is refused,
///        and an address no row claims is counted rather than dropped.
///
/// The coverage check at the end of the first case is the point of the file: a
/// row added to the table without a decode test here fails by name.

#include "midi/synth/gs_address_table.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "midi/synth/gs_layer.h"
#include "midi/synth/gs_system_effects.h"

namespace {

using sonare::midi::synth::gs_address_block_index;
using sonare::midi::synth::gs_address_offset;
using sonare::midi::synth::gs_decode_writes;
using sonare::midi::synth::gs_lookup_address;
using sonare::midi::synth::gs_param_name;
using sonare::midi::synth::gs_part_block_to_channel;
using sonare::midi::synth::gs_sysex_frame;
using sonare::midi::synth::gs_value_in_range;
using sonare::midi::synth::GsAddressEntry;
using sonare::midi::synth::GsAddressRange;
using sonare::midi::synth::GsFrame;
using sonare::midi::synth::GsLevel;
using sonare::midi::synth::GsParam;
using sonare::midi::synth::GsSysExKind;
using sonare::midi::synth::GsWrite;
using sonare::midi::synth::kGsAddressTable;
using sonare::midi::synth::kGsCommandDt1;
using sonare::midi::synth::kGsCommandRq1;
using sonare::midi::synth::kGsModelId;
using sonare::midi::synth::kGsUndefinedRanges;
using sonare::midi::synth::kRolandManufacturerId;
using sonare::midi::synth::parse_gs_sysex;

/// Which table rows a decode test has exercised, filled by check_row().
std::array<bool, kGsAddressTable.size()> g_covered{};
std::array<bool, kGsUndefinedRanges.size()> g_range_covered{};

std::string hex24(uint32_t addr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02X %02X %02X", (addr >> 16) & 0xFFu, (addr >> 8) & 0xFFu,
                addr & 0xFFu);
  return buf;
}

/// A Roland SysEx message, framed, with a correct checksum.
std::vector<uint8_t> roland_message(uint32_t addr, const std::vector<uint8_t>& data,
                                    uint8_t command = kGsCommandDt1, uint8_t model = kGsModelId,
                                    uint8_t device = 0x10,
                                    uint8_t manufacturer = kRolandManufacturerId) {
  std::vector<uint8_t> msg{0xF0, manufacturer, device, model, command};
  msg.push_back(static_cast<uint8_t>((addr >> 16) & 0x7Fu));
  msg.push_back(static_cast<uint8_t>((addr >> 8) & 0x7Fu));
  msg.push_back(static_cast<uint8_t>(addr & 0x7Fu));
  msg.insert(msg.end(), data.begin(), data.end());
  uint32_t sum = 0;
  for (size_t i = 5; i < msg.size(); ++i) {
    sum += msg[i];
  }
  msg.push_back(static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu));
  msg.push_back(0xF7);
  return msg;
}

struct Decoded {
  std::vector<GsWrite> writes;
  uint32_t unknown = 0;
  size_t total = 0;
};

Decoded decode(const std::vector<uint8_t>& msg) {
  Decoded out;
  const GsFrame frame = gs_sysex_frame(msg.data(), msg.size());
  std::vector<GsWrite> buffer(256);
  out.total = gs_decode_writes(frame, buffer.data(), buffer.size(), &out.unknown);
  buffer.resize(std::min(out.total, buffer.size()));
  out.writes = buffer;
  return out;
}

/// Decodes one data byte at @p addr, asserts where it landed, and marks the row
/// it landed on as covered.
void check_row(uint32_t addr, uint8_t value, GsParam param, uint8_t index = 0, uint8_t part = 0) {
  INFO("address " << hex24(addr));
  const Decoded decoded = decode(roland_message(addr, {value}));
  REQUIRE(decoded.writes.size() == 1);
  const GsWrite& write = decoded.writes[0];
  CHECK(write.param == param);
  CHECK(write.index == index);
  CHECK(write.part == part);
  CHECK(write.value == value);
  CHECK(write.addr == addr);
  CHECK(decoded.unknown == 0);

  if (const GsAddressEntry* entry = gs_lookup_address(addr)) {
    g_covered[static_cast<size_t>(entry - kGsAddressTable.data())] = true;
  }
  for (size_t i = 0; i < kGsUndefinedRanges.size(); ++i) {
    const uint32_t base = addr & ~kGsUndefinedRanges[i].mask;
    if (base >= kGsUndefinedRanges[i].lo_addr && base <= kGsUndefinedRanges[i].hi_addr) {
      g_range_covered[i] = true;
    }
  }
}

}  // namespace

TEST_CASE("GS address table: every row decodes", "[midi][gs][address]") {
  // System (00 00 xx / 00 01 xx).
  check_row(0x00007F, 0x00, GsParam::kSystemModeSet);
  check_row(0x00010A, 0x01, GsParam::kChannelMsgRxPort, 0x0A);
  check_row(0x00011A, 0x01, GsParam::kChannelMsgRxPort, 0x1A);

  // System parameters (40 00 xx). MASTER TUNE is one four-nibble parameter, so
  // its bytes come back as one param with the nibble position as the index.
  check_row(0x400000, 0x00, GsParam::kMasterTune, 0);
  check_row(0x400003, 0x0F, GsParam::kMasterTune, 3);
  check_row(0x400004, 0x7F, GsParam::kMasterVolume);
  check_row(0x400005, 0x40, GsParam::kMasterKeyShift);
  check_row(0x400006, 0x40, GsParam::kMasterPan);
  check_row(0x40007F, 0x00, GsParam::kModeSet);

  // Patch common (40 01 xx). PATCH NAME is one 16-byte field, so a byte in the
  // middle of it comes back as the same param with its position as the index.
  check_row(0x400100, 0x20, GsParam::kPatchName, 0);
  check_row(0x400107, 0x41, GsParam::kPatchName, 7);
  check_row(0x40010F, 0x7F, GsParam::kPatchName, 15);
  check_row(0x400110, 0x00, GsParam::kUndefined);
  check_row(0x40012F, 0x00, GsParam::kUndefined);

  // Reverb / chorus / delay (40 01 30-5A).
  check_row(0x400130, 0x04, GsParam::kReverbMacro);
  check_row(0x400131, 0x04, GsParam::kReverbCharacter);
  check_row(0x400132, 0x00, GsParam::kReverbPreLpf);
  check_row(0x400133, 0x40, GsParam::kReverbLevel);
  check_row(0x400134, 0x40, GsParam::kReverbTime);
  check_row(0x400135, 0x00, GsParam::kReverbDelayFeedback);
  check_row(0x400136, 0x00, GsParam::kUndefined);
  check_row(0x400137, 0x00, GsParam::kReverbPredelay);
  check_row(0x400138, 0x02, GsParam::kChorusMacro);
  check_row(0x400139, 0x00, GsParam::kChorusPreLpf);
  check_row(0x40013A, 0x40, GsParam::kChorusLevel);
  check_row(0x40013B, 0x08, GsParam::kChorusFeedback);
  check_row(0x40013C, 0x50, GsParam::kChorusDelay);
  check_row(0x40013D, 0x03, GsParam::kChorusRate);
  check_row(0x40013E, 0x13, GsParam::kChorusDepth);
  check_row(0x40013F, 0x00, GsParam::kChorusSendToReverb);
  check_row(0x400140, 0x00, GsParam::kChorusSendToDelay);
  check_row(0x400141, 0x00, GsParam::kUndefined);
  check_row(0x40014F, 0x00, GsParam::kUndefined);
  check_row(0x400150, 0x00, GsParam::kDelayMacro);
  check_row(0x400151, 0x00, GsParam::kDelayPreLpf);
  check_row(0x400152, 0x61, GsParam::kDelayTimeCenter);
  check_row(0x400153, 0x01, GsParam::kDelayTimeRatioLeft);
  check_row(0x400154, 0x01, GsParam::kDelayTimeRatioRight);
  check_row(0x400155, 0x7F, GsParam::kDelayLevelCenter);
  check_row(0x400156, 0x00, GsParam::kDelayLevelLeft);
  check_row(0x400157, 0x00, GsParam::kDelayLevelRight);
  check_row(0x400158, 0x40, GsParam::kDelayLevel);
  check_row(0x400159, 0x50, GsParam::kDelayFeedback);
  check_row(0x40015A, 0x00, GsParam::kDelaySendToReverb);
  check_row(0x40015B, 0x00, GsParam::kUndefined);
  check_row(0x40017F, 0x00, GsParam::kUndefined);

  // Master EQ (40 02 xx).
  check_row(0x400200, 0x01, GsParam::kEqLowFreq);
  check_row(0x400201, 0x40, GsParam::kEqLowGain);
  check_row(0x400202, 0x01, GsParam::kEqHighFreq);
  check_row(0x400203, 0x40, GsParam::kEqHighGain);
  check_row(0x400204, 0x00, GsParam::kUndefined);
  check_row(0x40027F, 0x00, GsParam::kUndefined);

  // EFX (40 03 xx).
  check_row(0x400300, 0x01, GsParam::kEfxType, 0);
  check_row(0x400301, 0x10, GsParam::kEfxType, 1);
  check_row(0x400302, 0x00, GsParam::kUndefined);
  check_row(0x400303, 0x40, GsParam::kEfxParameter, 0);
  check_row(0x400316, 0x40, GsParam::kEfxParameter, 19);
  check_row(0x400317, 0x28, GsParam::kEfxSendToReverb);
  check_row(0x400318, 0x00, GsParam::kEfxSendToChorus);
  check_row(0x400319, 0x00, GsParam::kEfxSendToDelay);
  check_row(0x40031A, 0x00, GsParam::kUndefined);
  check_row(0x40031B, 0x00, GsParam::kEfxControlSource1);
  check_row(0x40031C, 0x40, GsParam::kEfxControlDepth1);
  check_row(0x40031D, 0x00, GsParam::kEfxControlSource2);
  check_row(0x40031E, 0x40, GsParam::kEfxControlDepth2);
  check_row(0x40031F, 0x01, GsParam::kEfxSendEqSwitch);

  // Part parameters (40 1x / 40 4x): the block nibble resolves to a channel, so
  // block 0 comes back as part 10.
  check_row(0x401913, 0x00, GsParam::kPartMonoPoly, 0, 8);
  check_row(0x401514, 0x02, GsParam::kPartAssignMode, 0, 4);
  check_row(0x401015, 0x01, GsParam::kUseForRhythmPart, 0, 9);
  check_row(0x401215, 0x02, GsParam::kUseForRhythmPart, 0, 1);
  check_row(0x401A16, 0x4C, GsParam::kPartKeyShift, 0, 10);
  check_row(0x401119, 0x64, GsParam::kPartLevel, 0, 0);
  check_row(0x40101C, 0x40, GsParam::kPartPanpot, 0, 9);
  check_row(0x401F21, 0x00, GsParam::kPartChorusSend, 0, 15);
  check_row(0x401022, 0x28, GsParam::kPartReverbSend, 0, 9);
  check_row(0x401025, 0x00, GsParam::kUndefined);
  // PITCH FINE TUNE is one 14-bit word: the two bytes come back as one param
  // with the byte position as the index, like MASTER TUNE.
  check_row(0x40102A, 0x40, GsParam::kPartPitchFineTune, 0, 9);
  check_row(0x40102B, 0x00, GsParam::kPartPitchFineTune, 1, 9);
  check_row(0x40102C, 0x00, GsParam::kPartDelaySend, 0, 9);
  check_row(0x40102D, 0x00, GsParam::kUndefined);
  // TONE MODIFY 1-8 share one row, so the index is which of the eight.
  check_row(0x401030, 0x40, GsParam::kPartToneModify, 0, 9);
  check_row(0x401237, 0x40, GsParam::kPartToneModify, 7, 1);
  check_row(0x401038, 0x00, GsParam::kUndefined);
  check_row(0x40104C, 0x00, GsParam::kUndefined);
  // Controller destinations (40 2x xx). One decode per row, plus the last
  // byte of every row that carries more than one, spread over blocks so the
  // block-to-part mapping is exercised alongside the index.
  check_row(0x402100, 0x40, GsParam::kPartModDest, 0, 0);
  check_row(0x402201, 0x40, GsParam::kPartModDest, 0, 1);
  check_row(0x402303, 0x40, GsParam::kPartModDest, 2, 2);
  check_row(0x402404, 0x0A, GsParam::kPartModLfo1PitchDepth, 0, 3);
  check_row(0x402505, 0x00, GsParam::kPartModDest, 0, 4);
  check_row(0x402606, 0x00, GsParam::kPartModDest, 1, 5);
  check_row(0x402707, 0x40, GsParam::kPartModDest, 0, 6);
  check_row(0x402808, 0x00, GsParam::kPartModDest, 0, 7);
  check_row(0x40290A, 0x00, GsParam::kPartModDest, 2, 8);
  check_row(0x402A10, 0x42, GsParam::kPartBendPitchControl, 0, 10);
  check_row(0x402B11, 0x40, GsParam::kPartBendDest, 0, 11);
  check_row(0x402C13, 0x40, GsParam::kPartBendDest, 2, 12);
  check_row(0x402D14, 0x00, GsParam::kPartBendDest, 0, 13);
  check_row(0x402E16, 0x00, GsParam::kPartBendDest, 2, 14);
  check_row(0x402F17, 0x40, GsParam::kPartBendDest, 0, 15);
  check_row(0x402018, 0x00, GsParam::kPartBendDest, 0, 9);
  check_row(0x40211A, 0x00, GsParam::kPartBendDest, 2, 0);
  check_row(0x402220, 0x40, GsParam::kPartCafDest, 0, 1);
  check_row(0x402321, 0x40, GsParam::kPartCafDest, 0, 2);
  check_row(0x402423, 0x40, GsParam::kPartCafDest, 2, 3);
  check_row(0x402524, 0x00, GsParam::kPartCafDest, 0, 4);
  check_row(0x402626, 0x00, GsParam::kPartCafDest, 2, 5);
  check_row(0x402727, 0x40, GsParam::kPartCafDest, 0, 6);
  check_row(0x402828, 0x00, GsParam::kPartCafDest, 0, 7);
  check_row(0x40292A, 0x00, GsParam::kPartCafDest, 2, 8);
  check_row(0x402A30, 0x40, GsParam::kPartPafDest, 0, 10);
  check_row(0x402B31, 0x40, GsParam::kPartPafDest, 0, 11);
  check_row(0x402C33, 0x40, GsParam::kPartPafDest, 2, 12);
  check_row(0x402D34, 0x00, GsParam::kPartPafDest, 0, 13);
  check_row(0x402E36, 0x00, GsParam::kPartPafDest, 2, 14);
  check_row(0x402F37, 0x40, GsParam::kPartPafDest, 0, 15);
  check_row(0x402038, 0x00, GsParam::kPartPafDest, 0, 9);
  check_row(0x40213A, 0x00, GsParam::kPartPafDest, 2, 0);
  check_row(0x402240, 0x40, GsParam::kPartCc1Dest, 0, 1);
  check_row(0x402341, 0x40, GsParam::kPartCc1Dest, 0, 2);
  check_row(0x402443, 0x40, GsParam::kPartCc1Dest, 2, 3);
  check_row(0x402544, 0x00, GsParam::kPartCc1Dest, 0, 4);
  check_row(0x402646, 0x00, GsParam::kPartCc1Dest, 2, 5);
  check_row(0x402747, 0x40, GsParam::kPartCc1Dest, 0, 6);
  check_row(0x402848, 0x00, GsParam::kPartCc1Dest, 0, 7);
  check_row(0x40294A, 0x00, GsParam::kPartCc1Dest, 2, 8);
  check_row(0x402A50, 0x40, GsParam::kPartCc2Dest, 0, 10);
  check_row(0x402B51, 0x40, GsParam::kPartCc2Dest, 0, 11);
  check_row(0x402C53, 0x40, GsParam::kPartCc2Dest, 2, 12);
  check_row(0x402D54, 0x00, GsParam::kPartCc2Dest, 0, 13);
  check_row(0x402E56, 0x00, GsParam::kPartCc2Dest, 2, 14);
  check_row(0x402F57, 0x40, GsParam::kPartCc2Dest, 0, 15);
  check_row(0x402058, 0x00, GsParam::kPartCc2Dest, 0, 9);
  check_row(0x40215A, 0x00, GsParam::kPartCc2Dest, 2, 0);
  check_row(0x40200B, 0x00, GsParam::kUndefined);
  check_row(0x40211B, 0x00, GsParam::kUndefined);
  check_row(0x40222B, 0x00, GsParam::kUndefined);
  check_row(0x40233B, 0x00, GsParam::kUndefined);
  check_row(0x40244B, 0x00, GsParam::kUndefined);
  check_row(0x40255B, 0x00, GsParam::kUndefined);
  check_row(0x40267F, 0x00, GsParam::kUndefined);

  // Tone map (40 4x 00-01), and the three gaps the block leaves around it.
  check_row(0x404100, 0x00, GsParam::kPartToneMapNumber, 0, 0);
  check_row(0x404A00, 0x00, GsParam::kPartToneMapNumber, 0, 10);
  check_row(0x404001, 0x04, GsParam::kPartToneMap0Number, 0, 9);
  check_row(0x404F01, 0x04, GsParam::kPartToneMap0Number, 0, 15);
  check_row(0x404114, 0x00, GsParam::kUndefined);
  check_row(0x404221, 0x00, GsParam::kUndefined);
  check_row(0x404335, 0x00, GsParam::kUndefined);
  check_row(0x404020, 0x01, GsParam::kPartEqSwitch, 0, 9);
  check_row(0x404320, 0x00, GsParam::kPartEqSwitch, 0, 2);
  check_row(0x404022, 0x01, GsParam::kPartEfxAssign, 0, 9);
  check_row(0x404122, 0x10, GsParam::kPartEfxAssign, 0, 0);

  // Drum setup (41 mn rr): one row per parameter covers both maps and all 128
  // notes, so the map nibble comes back as the part and the note as the index.
  // The nibble is zero-based here, unlike 40 1x 15's value.
  check_row(0x410126, 0x24, GsParam::kDrumPlayNote, 0x26, 0);
  check_row(0x41117F, 0x00, GsParam::kDrumPlayNote, 0x7F, 1);
  check_row(0x410226, 0x00, GsParam::kDrumLevel, 0x26, 0);
  check_row(0x41032A, 0x7F, GsParam::kDrumAssignGroup, 0x2A, 0);
  check_row(0x411226, 0x7F, GsParam::kDrumLevel, 0x26, 1);
  check_row(0x410400, 0x7F, GsParam::kDrumPanpot, 0x00, 0);
  check_row(0x41057F, 0x00, GsParam::kDrumReverbSend, 0x7F, 0);
  check_row(0x41163C, 0x00, GsParam::kDrumChorusSend, 0x3C, 1);
  check_row(0x410926, 0x40, GsParam::kDrumDelaySend, 0x26, 0);

  // The opposite group (50 ** ** / 51 ** **): one row over the whole block, so
  // the mid byte comes back as the part and the low byte as the index, and an
  // address deep inside it still resolves.
  check_row(0x500000, 0x00, GsParam::kOppositeGroupBlock, 0x00, 0);
  check_row(0x501119, 0x64, GsParam::kOppositeGroupBlock, 0x19, 1);
  check_row(0x507F7F, 0x7F, GsParam::kOppositeGroupBlock, 0x7F, 0x0F);
  check_row(0x510226, 0x00, GsParam::kOppositeGroupBlock, 0x26, 2);

  std::string untested;
  for (size_t i = 0; i < kGsAddressTable.size(); ++i) {
    if (g_covered[i]) continue;
    untested +=
        " " + hex24(kGsAddressTable[i].addr) + " (" + gs_param_name(kGsAddressTable[i].param) + ")";
  }
  for (size_t i = 0; i < kGsUndefinedRanges.size(); ++i) {
    if (g_range_covered[i]) continue;
    untested += " " + hex24(kGsUndefinedRanges[i].lo_addr) + " (undefined range)";
  }
  INFO("rows with no decode test:" << untested);
  CHECK(untested.empty());
}

TEST_CASE("GS address table: exclusions carry a reason", "[midi][gs][address]") {
  for (const GsAddressEntry& entry : kGsAddressTable) {
    INFO("address " << hex24(entry.addr));
    const bool needs_why = entry.level == GsLevel::kIgnore || entry.level == GsLevel::kAccept;
    const bool has_why = entry.why != nullptr && entry.why[0] != '\0';
    CHECK(needs_why == has_why);
  }
  for (const GsAddressRange& range : kGsUndefinedRanges) {
    INFO("range " << hex24(range.lo_addr));
    CHECK(range.why != nullptr);
    CHECK(std::string(range.why != nullptr ? range.why : "").size() > 0);
    CHECK(range.lo_addr <= range.hi_addr);
  }
}

TEST_CASE("GS address table: no address is claimed twice", "[midi][gs][address]") {
  for (size_t i = 0; i < kGsAddressTable.size(); ++i) {
    if (i > 0) {
      INFO("row " << hex24(kGsAddressTable[i].addr) << " follows "
                  << hex24(kGsAddressTable[i - 1].addr));
      CHECK(kGsAddressTable[i - 1].addr < kGsAddressTable[i].addr);
    }
    for (size_t j = 0; j < i; ++j) {
      INFO(hex24(kGsAddressTable[j].addr) << " vs " << hex24(kGsAddressTable[i].addr));
      CHECK_FALSE(
          sonare::midi::synth::detail::gs_rows_overlap(kGsAddressTable[j], kGsAddressTable[i]));
    }
  }
  // An undefined range never shadows a defined address.
  for (const GsAddressRange& range : kGsUndefinedRanges) {
    for (uint32_t addr = range.lo_addr; addr <= range.hi_addr; ++addr) {
      INFO("range address " << hex24(addr));
      CHECK(gs_lookup_address(addr) == nullptr);
    }
  }
}

TEST_CASE("GS frame layer refuses what it cannot trust", "[midi][gs][address]") {
  const std::vector<uint8_t> good = roland_message(0x400004, {0x50});

  SECTION("framed and unframed parse the same") {
    const GsFrame framed = gs_sysex_frame(good.data(), good.size());
    REQUIRE(framed.valid);
    CHECK(framed.device == 0x10);
    CHECK(framed.model == kGsModelId);
    CHECK(framed.command == kGsCommandDt1);
    CHECK(framed.addr == 0x400004);
    REQUIRE(framed.len == 1);
    CHECK(framed.data[0] == 0x50);

    const std::vector<uint8_t> bare(good.begin() + 1, good.end() - 1);
    const GsFrame stripped = gs_sysex_frame(bare.data(), bare.size());
    REQUIRE(stripped.valid);
    CHECK(stripped.addr == framed.addr);
    CHECK(stripped.len == framed.len);
    CHECK(stripped.command == framed.command);
  }

  SECTION("a wrong checksum is refused") {
    std::vector<uint8_t> bad = good;
    bad[bad.size() - 2] ^= 0x01u;
    CHECK_FALSE(gs_sysex_frame(bad.data(), bad.size()).valid);
  }

  SECTION("a truncated message is refused") {
    for (size_t len = 0; len < good.size() - 1; ++len) {
      INFO("length " << len);
      CHECK_FALSE(gs_sysex_frame(good.data(), len).valid);
    }
    CHECK_FALSE(gs_sysex_frame(nullptr, 8).valid);
  }

  SECTION("another manufacturer is refused") {
    const std::vector<uint8_t> yamaha =
        roland_message(0x400004, {0x50}, kGsCommandDt1, kGsModelId, 0x10, 0x43);
    CHECK_FALSE(gs_sysex_frame(yamaha.data(), yamaha.size()).valid);
  }

  SECTION("a body byte with its high bit set is refused") {
    std::vector<uint8_t> bad = good;
    bad[7] = 0x90;  // a data byte no SysEx can carry
    CHECK_FALSE(gs_sysex_frame(bad.data(), bad.size()).valid);
  }

  SECTION("broadcast and matching device IDs both parse") {
    const std::vector<uint8_t> broadcast =
        roland_message(0x400004, {0x50}, kGsCommandDt1, kGsModelId, 0x7F);
    const GsFrame frame = gs_sysex_frame(broadcast.data(), broadcast.size());
    REQUIRE(frame.valid);
    CHECK(frame.device == 0x7F);
  }
}

TEST_CASE("GS decode: only a GS DT1 frame writes", "[midi][gs][address]") {
  SECTION("a request is a valid frame that writes nothing") {
    const std::vector<uint8_t> rq1 = roland_message(0x400004, {0x00, 0x00, 0x01}, kGsCommandRq1);
    const GsFrame frame = gs_sysex_frame(rq1.data(), rq1.size());
    REQUIRE(frame.valid);
    CHECK(frame.command == kGsCommandRq1);
    const Decoded decoded = decode(rq1);
    CHECK(decoded.total == 0);
    CHECK(decoded.unknown == 0);
  }

  SECTION("another Roland model is accepted without counting") {
    const std::vector<uint8_t> display = roland_message(0x100000, {0x20}, kGsCommandDt1, 0x45);
    const GsFrame frame = gs_sysex_frame(display.data(), display.size());
    REQUIRE(frame.valid);
    CHECK(frame.model == 0x45);
    const Decoded decoded = decode(display);
    CHECK(decoded.total == 0);
    CHECK(decoded.unknown == 0);
  }

  SECTION("a refused frame produces nothing") {
    std::vector<uint8_t> bad = roland_message(0x400004, {0x50});
    bad[bad.size() - 2] ^= 0x01u;
    const Decoded decoded = decode(bad);
    CHECK(decoded.total == 0);
    CHECK(decoded.unknown == 0);
  }
}

TEST_CASE("GS decode: an address no row claims is counted, not dropped", "[midi][gs][address]") {
  const Decoded decoded = decode(roland_message(0x400007, {0x11}));
  REQUIRE(decoded.writes.size() == 1);
  CHECK(decoded.writes[0].param == GsParam::kUnknown);
  CHECK(decoded.writes[0].addr == 0x400007);
  CHECK(decoded.writes[0].value == 0x11);
  CHECK(decoded.unknown == 1);
}

TEST_CASE("GS decode: a run walks consecutive addresses", "[midi][gs][address]") {
  SECTION("through the EFX block, across an undefined address") {
    const Decoded decoded = decode(roland_message(0x400300, {0x01, 0x10, 0x00, 0x40}));
    REQUIRE(decoded.writes.size() == 4);
    CHECK(decoded.writes[0].param == GsParam::kEfxType);
    CHECK(decoded.writes[0].index == 0);
    CHECK(decoded.writes[1].param == GsParam::kEfxType);
    CHECK(decoded.writes[1].index == 1);
    CHECK(decoded.writes[2].param == GsParam::kUndefined);
    CHECK(decoded.writes[3].param == GsParam::kEfxParameter);
    CHECK(decoded.writes[3].index == 0);
    CHECK(decoded.unknown == 0);
  }

  SECTION("carrying at 0x80 into the next address byte") {
    CHECK(gs_address_offset(0x40017E, 2) == 0x400200u);
    CHECK(gs_address_offset(0x40007F, 1) == 0x400100u);
    CHECK(gs_address_offset(0x407F7F, 1) == 0x410000u);
    CHECK(gs_address_offset(0x400004, 0) == 0x400004u);

    const Decoded decoded = decode(roland_message(0x40007E, {0x00, 0x00, 0x00}));
    REQUIRE(decoded.writes.size() == 3);
    CHECK(decoded.writes[0].addr == 0x40007Eu);
    CHECK(decoded.writes[1].addr == 0x40007Fu);
    CHECK(decoded.writes[1].param == GsParam::kModeSet);
    CHECK(decoded.writes[2].addr == 0x400100u);
    CHECK(decoded.writes[2].param == GsParam::kPatchName);
    // Only 40 00 7E is unclaimed: the carry lands on the first PATCH NAME byte.
    CHECK(decoded.unknown == 1);
  }

  SECTION("a full buffer reports the writes it could not store") {
    const std::vector<uint8_t> msg = roland_message(0x400300, {0x01, 0x10, 0x00, 0x40});
    const GsFrame frame = gs_sysex_frame(msg.data(), msg.size());
    std::array<GsWrite, 1> out{};
    uint32_t unknown = 0;
    CHECK(gs_decode_writes(frame, out.data(), out.size(), &unknown) == 4);
    CHECK(out[0].param == GsParam::kEfxType);
    CHECK(gs_decode_writes(frame, nullptr, 0, nullptr) == 4);
  }
}

TEST_CASE("GS address table: values outside a row's range are not accepted",
          "[midi][gs][address]") {
  const GsAddressEntry* pan = gs_lookup_address(0x400006);
  REQUIRE(pan != nullptr);
  CHECK_FALSE(gs_value_in_range(*pan, 0x00));  // no random pan on the master
  CHECK(gs_value_in_range(*pan, 0x01));
  CHECK(gs_value_in_range(*pan, 0x7F));

  const GsAddressEntry* mode = gs_lookup_address(0x00007F);
  REQUIRE(mode != nullptr);
  CHECK(gs_value_in_range(*mode, 0x00));
  CHECK_FALSE(gs_value_in_range(*mode, 0x01));  // the target device has no Mode-2

  const GsAddressEntry* shift = gs_lookup_address(0x400005);
  REQUIRE(shift != nullptr);
  CHECK_FALSE(gs_value_in_range(*shift, 0x27));
  CHECK(gs_value_in_range(*shift, 0x40));
  CHECK_FALSE(gs_value_in_range(*shift, 0x59));

  // PITCH KEY SHIFT bounds the same +/-24 semitones, one part at a time.
  const GsAddressEntry* part_shift = gs_lookup_address(0x401A16);
  REQUIRE(part_shift != nullptr);
  CHECK_FALSE(gs_value_in_range(*part_shift, 0x27));
  CHECK(gs_value_in_range(*part_shift, 0x28));
  CHECK(gs_value_in_range(*part_shift, 0x40));
  CHECK(gs_value_in_range(*part_shift, 0x58));
  CHECK_FALSE(gs_value_in_range(*part_shift, 0x59));

  // MONO/POLY MODE takes 00 Mono and 01 Poly and nothing above.
  const GsAddressEntry* mono_poly = gs_lookup_address(0x401913);
  REQUIRE(mono_poly != nullptr);
  CHECK(gs_value_in_range(*mono_poly, 0x00));
  CHECK(gs_value_in_range(*mono_poly, 0x01));
  CHECK_FALSE(gs_value_in_range(*mono_poly, 0x02));

  // ASSIGN MODE takes 00 SINGLE, 01 LIMITED-MULTI and 02 FULL-MULTI, and the
  // same row bounds every part block.
  const GsAddressEntry* assign_mode = gs_lookup_address(0x401514);
  REQUIRE(assign_mode != nullptr);
  CHECK(gs_value_in_range(*assign_mode, 0x00));
  CHECK(gs_value_in_range(*assign_mode, 0x01));
  CHECK(gs_value_in_range(*assign_mode, 0x02));
  CHECK_FALSE(gs_value_in_range(*assign_mode, 0x03));
}

TEST_CASE("GS address table: a variable nibble resolves to its entity", "[midi][gs][address]") {
  // Part blocks (40 1x / 40 2x / 40 4x): block 0 is part 10.
  CHECK(gs_address_block_index(0x401019, 0x000F00) == 9);
  CHECK(gs_address_block_index(0x401119, 0x000F00) == 0);
  CHECK(gs_address_block_index(0x401919, 0x000F00) == 8);
  CHECK(gs_address_block_index(0x401A19, 0x000F00) == 10);
  CHECK(gs_address_block_index(0x404F22, 0x000F00) == 15);
  // An EFX unit and a drum map are the raw nibble, in different positions.
  CHECK(gs_address_block_index(0x403500, 0x000F00) == 5);
  CHECK(gs_address_block_index(0x41123C, 0x00F07F) == 1);
  // No variable nibble.
  CHECK(gs_address_block_index(0x400004, 0x000000) == 0);
}

TEST_CASE("GS address table: a masked row claims exactly its own addresses",
          "[midi][gs][address]") {
  using sonare::midi::synth::detail::gs_row_claims;
  // The shape of a drum-setup row: a variable map nibble in the mid byte and a
  // variable note in the low byte. The note mask is 7 bits, not 8 — a GS address
  // byte carries no more, and a row is not allowed to reach past 7F.
  const GsAddressEntry* drum_level_row = gs_lookup_address(0x410200);
  REQUIRE(drum_level_row != nullptr);
  const GsAddressEntry& drum_level = *drum_level_row;
  CHECK(drum_level.mask == 0x00F07Fu);
  CHECK(gs_row_claims(drum_level, 0x410200));
  CHECK(gs_row_claims(drum_level, 0x41023C));
  CHECK(gs_row_claims(drum_level, 0x41027F));
  CHECK(gs_row_claims(drum_level, 0x41123C));        // map 2
  CHECK_FALSE(gs_row_claims(drum_level, 0x41013C));  // a neighbouring parameter group
  CHECK_FALSE(gs_row_claims(drum_level, 0x40023C));  // another block entirely

  // The shape of a whole-block row: every mid byte variable and the low byte
  // spanning its own 128, which is the widest a row can be and still stop at
  // the high byte it names.
  const GsAddressEntry* group_row = gs_lookup_address(0x500000);
  REQUIRE(group_row != nullptr);
  const GsAddressEntry& group = *group_row;
  CHECK(group.mask == 0x007F00u);
  CHECK(group.size == 128);
  CHECK(gs_row_claims(group, 0x500000));
  CHECK(gs_row_claims(group, 0x507F7F));
  CHECK_FALSE(gs_row_claims(group, 0x4F7F7F));  // the byte below
  CHECK_FALSE(gs_row_claims(group, 0x510000));  // the sibling row's block

  // The two CHANNEL MSG RX PORT rows split one nibble-masked span, and each
  // claims only its own half.
  const GsAddressEntry* low = gs_lookup_address(0x000105);
  const GsAddressEntry* high = gs_lookup_address(0x000115);
  REQUIRE(low != nullptr);
  REQUIRE(high != nullptr);
  CHECK(low != high);
  CHECK(low->def == 0x00);
  CHECK(high->def == 0x01);
  CHECK(gs_lookup_address(0x000120) == nullptr);
}

TEST_CASE("GS address table: the block mapping matches the SysEx layer", "[midi][gs][address]") {
  for (uint8_t block = 0; block < 16; ++block) {
    const uint32_t addr = 0x401015u | (static_cast<uint32_t>(block) << 8);
    const std::vector<uint8_t> msg = roland_message(addr, {0x01});
    const sonare::midi::synth::GsSysEx parsed = parse_gs_sysex(msg.data(), msg.size());
    INFO("block " << static_cast<int>(block));
    REQUIRE(parsed.kind == GsSysExKind::kUseForRhythm);
    CHECK(parsed.channel == gs_part_block_to_channel(block));
  }
}

TEST_CASE("GS param names come from the enum itself", "[midi][gs][address]") {
  CHECK(std::string(gs_param_name(GsParam::kUnknown)) == "kUnknown");
  CHECK(std::string(gs_param_name(GsParam::kEfxSendEqSwitch)) == "kEfxSendEqSwitch");
  for (const GsAddressEntry& entry : kGsAddressTable) {
    INFO("address " << hex24(entry.addr));
    CHECK(std::string(gs_param_name(entry.param)) != "?");
  }
}

namespace {

using sonare::midi::synth::apply_gs_efx_sysex;
using sonare::midi::synth::gs_decode_sysex;
using sonare::midi::synth::gs_lookup_range;
using sonare::midi::synth::GsEfx;
using sonare::midi::synth::GsSysEx;

std::string hex_bytes(const std::vector<uint8_t>& bytes) {
  std::string out;
  for (uint8_t byte : bytes) {
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%02X ", byte);
    out += buf;
  }
  return out;
}

/// One message and the decision parse_gs_sysex owes it.
struct ParseCase {
  std::vector<uint8_t> message;
  GsSysExKind kind;
  uint8_t channel;
  uint8_t value;
};

/// Messages whose validity is broken in each of the ways the frame layer must
/// refuse, crossed pairwise with the addresses and framings that would
/// otherwise be recognised.
const std::vector<ParseCase>& parse_frame_cases() {
  static const std::vector<ParseCase> cases = {
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x7F, 0xF7}, GsSysExKind::kNone, 0, 0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x01, 0x41}, GsSysExKind::kNone, 0, 0},
      {{0x41, 0x10, 0x45, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41}, GsSysExKind::kNone, 0, 0},
      {{0xF0, 0x41, 0x10, 0x42, 0x11, 0x40, 0x10, 0x15, 0x00, 0x1B, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0x43, 0x10, 0x42, 0x12, 0x40, 0x40, 0x22, 0x7F, 0x5F}, GsSysExKind::kNone, 0, 0},
      {{0xF0, 0x41, 0x10, 0x45, 0x12, 0x40, 0x40, 0x22, 0x01, 0x5D, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0xF0, 0x41, 0x10, 0x45, 0x12, 0x40, 0x10, 0x14, 0x7F, 0x1D, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x7F, 0x1D, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x7F, 0x42, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x01}, GsSysExKind::kNone, 0, 0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x40, 0x22, 0x80, 0x5E}, GsSysExKind::kNone, 0, 0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x40, 0x22, 0x01, 0x5D}, GsSysExKind::kEfxPartSwitch, 9, 1},
      {{0x41, 0x10, 0x42, 0x11, 0x40, 0x10, 0x14, 0x01, 0x1B}, GsSysExKind::kNone, 0, 0},
      {{0x41, 0x10, 0x45, 0x12, 0x40, 0x10, 0x15, 0x01, 0x1A}, GsSysExKind::kNone, 0, 0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x14, 0x80, 0x1C, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0xF0, 0x41, 0x10, 0x42, 0x11, 0x40, 0x40, 0x22, 0x7F, 0x5F, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x14, 0xFF, 0x1D}, GsSysExKind::kNone, 0, 0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x00, 0x1B}, GsSysExKind::kUseForRhythm, 9, 0},
      {{0xF0, 0x43, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x00, 0x1B, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x40, 0x22, 0x00, 0x5F, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x80, 0x1B, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0xF0, 0x43, 0x10, 0x42, 0x12, 0x40, 0x10, 0x14, 0x01, 0x1B, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x40, 0x22, 0x00, 0xF7}, GsSysExKind::kNone, 0, 0},
      {{0x41, 0x10, 0x42, 0x11, 0x40, 0x00, 0x7F, 0x7F, 0x42}, GsSysExKind::kNone, 0, 0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x81, 0x40, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x14, 0x7F}, GsSysExKind::kNone, 0, 0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x14, 0x00, 0x1C, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0xF0, 0x43, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x7F, 0x42, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x14, 0x00, 0x1D, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
  };
  return cases;
}

/// Every (address, value) pair on a well-formed frame, crossed pairwise with
/// the framing. This is where the value coercions live: a rhythm map above 2
/// reads as map 1, any non-zero EFX assignment reads as on, and MODE SET
/// resets only on 00.
const std::vector<ParseCase>& parse_value_cases() {
  static const std::vector<ParseCase> cases = {
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x11, 0x0A}, GsSysExKind::kUseForRhythm, 9, 1},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x14, 0x02, 0x1A}, GsSysExKind::kNone, 0, 0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x14, 0x01, 0x1B, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x1F, 0x15, 0x01, 0x0B}, GsSysExKind::kUseForRhythm, 15, 1},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x40, 0x22, 0x00, 0x5E, 0xF7},
       GsSysExKind::kEfxPartSwitch,
       9,
       0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x7F, 0x42}, GsSysExKind::kNone, 0, 0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x14, 0x03, 0x19, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x1F, 0x15, 0x11, 0x7B, 0xF7},
       GsSysExKind::kUseForRhythm,
       15,
       1},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41}, GsSysExKind::kGsReset, 0, 0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x10, 0x0B, 0xF7},
       GsSysExKind::kUseForRhythm,
       9,
       1},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x00, 0x00, 0x7F, 0x02, 0x7F, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x7F, 0x1C, 0xF7},
       GsSysExKind::kUseForRhythm,
       9,
       1},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x14, 0x10, 0x0C}, GsSysExKind::kNone, 0, 0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x1F, 0x15, 0x03, 0x09}, GsSysExKind::kUseForRhythm, 15, 1},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x10, 0x4D}, GsSysExKind::kEfxPartSwitch, 0, 1},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x02, 0x3F, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x12, 0x15, 0x00, 0x19}, GsSysExKind::kUseForRhythm, 1, 0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x03, 0x5A, 0xF7},
       GsSysExKind::kEfxPartSwitch,
       0,
       1},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x40, 0x22, 0x7F, 0x5F}, GsSysExKind::kEfxPartSwitch, 9, 1},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x12, 0x15, 0x02, 0x17, 0xF7},
       GsSysExKind::kUseForRhythm,
       1,
       2},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x11, 0x30, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x00, 0x1B, 0xF7},
       GsSysExKind::kUseForRhythm,
       9,
       0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x14, 0x7F, 0x1D}, GsSysExKind::kNone, 0, 0},
      {{0x41, 0x10, 0x42, 0x12, 0x00, 0x00, 0x7F, 0x01, 0x00}, GsSysExKind::kNone, 0, 0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x00, 0x00, 0x7F, 0x7F, 0x02, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x40, 0x22, 0x02, 0x5C}, GsSysExKind::kEfxPartSwitch, 9, 1},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x40, 0x22, 0x10, 0x4E, 0xF7},
       GsSysExKind::kEfxPartSwitch,
       9,
       1},
      {{0x41, 0x10, 0x42, 0x12, 0x00, 0x00, 0x7F, 0x11, 0x70}, GsSysExKind::kNone, 0, 0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x12, 0x15, 0x10, 0x09}, GsSysExKind::kUseForRhythm, 1, 1},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x01, 0x1A, 0xF7},
       GsSysExKind::kUseForRhythm,
       9,
       1},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x1F, 0x15, 0x10, 0x7C, 0xF7},
       GsSysExKind::kUseForRhythm,
       15,
       1},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x03, 0x3E, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x00, 0x5D, 0xF7},
       GsSysExKind::kEfxPartSwitch,
       0,
       0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x10, 0x31, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      // SYSTEM MODE SET with value 00. The target has no Mode-2, so it is a GS
      // Reset (docs/gs.md); every other value on this address stays kNone.
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x00, 0x00, 0x7F, 0x00, 0x01, 0xF7},
       GsSysExKind::kGsReset,
       0,
       0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x14, 0x11, 0x0B}, GsSysExKind::kNone, 0, 0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x03, 0x18}, GsSysExKind::kUseForRhythm, 9, 1},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x12, 0x15, 0x7F, 0x1A}, GsSysExKind::kUseForRhythm, 1, 1},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x14, 0x00, 0x1C, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x02, 0x19}, GsSysExKind::kUseForRhythm, 9, 2},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x00, 0x00, 0x7F, 0x10, 0x71, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x7F, 0x5E, 0xF7},
       GsSysExKind::kEfxPartSwitch,
       0,
       1},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x40, 0x22, 0x01, 0x5D}, GsSysExKind::kEfxPartSwitch, 9, 1},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x00, 0x00, 0x7F, 0x03, 0x7E, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x12, 0x15, 0x11, 0x08, 0xF7},
       GsSysExKind::kUseForRhythm,
       1,
       1},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x02, 0x5B}, GsSysExKind::kEfxPartSwitch, 0, 1},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x40, 0x22, 0x03, 0x5B, 0xF7},
       GsSysExKind::kEfxPartSwitch,
       9,
       1},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x01, 0x40, 0xF7},
       GsSysExKind::kNone,
       0,
       0},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x12, 0x15, 0x03, 0x16}, GsSysExKind::kUseForRhythm, 1, 1},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x1F, 0x15, 0x00, 0x0C}, GsSysExKind::kUseForRhythm, 15, 0},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x11, 0x4C, 0xF7},
       GsSysExKind::kEfxPartSwitch,
       0,
       1},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x12, 0x15, 0x01, 0x18, 0xF7},
       GsSysExKind::kUseForRhythm,
       1,
       1},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x40, 0x22, 0x11, 0x4D, 0xF7},
       GsSysExKind::kEfxPartSwitch,
       9,
       1},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x01, 0x5C, 0xF7},
       GsSysExKind::kEfxPartSwitch,
       0,
       1},
      {{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x1F, 0x15, 0x7F, 0x0D, 0xF7},
       GsSysExKind::kUseForRhythm,
       15,
       1},
      {{0x41, 0x10, 0x42, 0x12, 0x40, 0x1F, 0x15, 0x02, 0x0A}, GsSysExKind::kUseForRhythm, 15, 2},
  };
  return cases;
}

/// The Universal SysEx path, which is matched ahead of the address table.
const std::vector<ParseCase>& parse_universal_cases() {
  static const std::vector<ParseCase> cases = {
      {{0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}, GsSysExKind::kGmReset, 0, 0},
      {{0x7E, 0x7F, 0x09, 0x01}, GsSysExKind::kGmReset, 0, 0},
      {{0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7}, GsSysExKind::kGmReset, 0, 0},
      // GM System Off is not a reset.
      {{0xF0, 0x7E, 0x7F, 0x09, 0x02, 0xF7}, GsSysExKind::kNone, 0, 0},
      {{0xF0, 0x7E, 0x7F, 0x09}, GsSysExKind::kNone, 0, 0},
      {{0x7E, 0x7F, 0x09}, GsSysExKind::kNone, 0, 0},
  };
  return cases;
}

void check_parse_cases(const std::vector<ParseCase>& cases) {
  for (const ParseCase& test : cases) {
    INFO("message " << hex_bytes(test.message));
    const GsSysEx parsed = parse_gs_sysex(test.message.data(), test.message.size());
    CHECK(parsed.kind == test.kind);
    CHECK(parsed.channel == test.channel);
    CHECK(parsed.value == test.value);
  }
}

/// How a message is malformed. kHighBit is a 7-bit violation no device can send
/// — the frame layer refuses the whole message rather than masking the byte.
enum class Fault : uint8_t {
  kNone,
  kBadChecksum,
  kTruncated,
  kManufacturer,
  kModel,
  kRq1,
  kHighBit,
};

std::vector<uint8_t> faulted_message(uint32_t addr, std::vector<uint8_t> data, Fault fault,
                                     bool framed) {
  const uint8_t manufacturer = fault == Fault::kManufacturer ? 0x43 : kRolandManufacturerId;
  const uint8_t model = fault == Fault::kModel ? 0x45 : kGsModelId;
  const uint8_t command = fault == Fault::kRq1 ? kGsCommandRq1 : kGsCommandDt1;
  if (fault == Fault::kHighBit && !data.empty()) data[0] = static_cast<uint8_t>(data[0] | 0x80u);
  std::vector<uint8_t> msg = roland_message(addr, data, command, model, 0x10, manufacturer);
  if (fault == Fault::kBadChecksum) msg[msg.size() - 2] ^= 0x01u;
  if (fault == Fault::kTruncated) msg.erase(msg.end() - 2);
  if (!framed) msg = std::vector<uint8_t>(msg.begin() + 1, msg.end() - 1);
  return msg;
}

/// The GsEfx field the byte at a block offset lands in. Written as the block
/// layout rather than derived from the applier, so the two can disagree.
/// Offsets with no field — the reserved 02 and 1A, and the control-source,
/// control-depth and send-EQ addresses GsEfx does not model — are preserved.
enum class EfxField : uint8_t {
  kNone,
  kTypeMsb,
  kTypeLsb,
  kParameter,
  kSendReverb,
  kSendChorus,
  kSendDelay,
};

EfxField efx_field_at(unsigned offset) {
  switch (offset) {
    case 0x00:
      return EfxField::kTypeMsb;
    case 0x01:
      return EfxField::kTypeLsb;
    case 0x17:
      return EfxField::kSendReverb;
    case 0x18:
      return EfxField::kSendChorus;
    case 0x19:
      return EfxField::kSendDelay;
    default:
      return offset >= 0x03 && offset <= 0x16 ? EfxField::kParameter : EfxField::kNone;
  }
}

struct EfxOutcome {
  GsEfx efx;
  bool touched = false;
};

/// The state @p before must reach when a run of @p data lands from block offset
/// @p start_lo.
EfxOutcome expected_efx(const GsEfx& before, unsigned start_lo, const std::vector<uint8_t>& data) {
  EfxOutcome out;
  out.efx = before;
  uint8_t type_msb = static_cast<uint8_t>(before.type >> 8);
  uint8_t type_lsb = static_cast<uint8_t>(before.type & 0x7Fu);
  for (size_t i = 0; i < data.size(); ++i) {
    const unsigned offset = start_lo + static_cast<unsigned>(i);
    const uint8_t value = static_cast<uint8_t>(data[i] & 0x7Fu);
    const EfxField field = efx_field_at(offset);
    if (field == EfxField::kNone) continue;
    out.touched = true;
    switch (field) {
      case EfxField::kTypeMsb:
        type_msb = value;
        break;
      case EfxField::kTypeLsb:
        type_lsb = value;
        break;
      case EfxField::kParameter:
        out.efx.params[offset - 0x03] = value;
        break;
      case EfxField::kSendReverb:
        out.efx.send_reverb = value;
        break;
      case EfxField::kSendChorus:
        out.efx.send_chorus = value;
        break;
      case EfxField::kSendDelay:
        out.efx.send_delay = value;
        break;
      case EfxField::kNone:
        break;
    }
  }
  if (out.touched) {
    out.efx.type = static_cast<uint16_t>((static_cast<uint16_t>(type_msb) << 8) | type_lsb);
    out.efx.assigned = true;
  }
  return out;
}

bool same_efx(const GsEfx& a, const GsEfx& b) {
  return a.type == b.type && a.params == b.params && a.send_reverb == b.send_reverb &&
         a.send_chorus == b.send_chorus && a.send_delay == b.send_delay && a.assigned == b.assigned;
}

struct EfxCase {
  uint32_t start;
  uint8_t len;
  bool framed;
  Fault fault;
};

const std::vector<EfxCase>& efx_frame_cases() {
  static const std::vector<EfxCase> cases = {
      {0x400300, 40, true, Fault::kTruncated},     {0x400300, 4, false, Fault::kBadChecksum},
      {0x400300, 1, false, Fault::kModel},         {0x400303, 1, true, Fault::kRq1},
      {0x400317, 40, false, Fault::kManufacturer}, {0x400317, 4, true, Fault::kModel},
      {0x400400, 40, true, Fault::kModel},         {0x400303, 40, true, Fault::kBadChecksum},
      {0x400300, 40, true, Fault::kNone},          {0x400303, 4, false, Fault::kTruncated},
      {0x400317, 1, false, Fault::kHighBit},       {0x400317, 4, false, Fault::kNone},
      {0x400400, 4, false, Fault::kRq1},           {0x400303, 4, false, Fault::kModel},
      {0x400400, 1, true, Fault::kHighBit},        {0x400317, 40, true, Fault::kRq1},
      {0x400400, 40, false, Fault::kHighBit},      {0x400303, 1, false, Fault::kNone},
      {0x400303, 1, true, Fault::kManufacturer},   {0x400317, 1, true, Fault::kBadChecksum},
      {0x400303, 1, true, Fault::kHighBit},        {0x400400, 4, true, Fault::kManufacturer},
      {0x400317, 1, true, Fault::kTruncated},      {0x400300, 40, false, Fault::kRq1},
      {0x400300, 4, true, Fault::kHighBit},        {0x400400, 40, false, Fault::kTruncated},
      {0x400400, 1, true, Fault::kNone},           {0x400300, 40, true, Fault::kManufacturer},
      {0x400400, 1, true, Fault::kBadChecksum},
  };
  return cases;
}

const std::vector<EfxCase>& efx_run_cases() {
  static const std::vector<EfxCase> cases = {
      {0x40031F, 2, false, Fault::kNone},  {0x400302, 2, true, Fault::kNone},
      {0x40031B, 40, false, Fault::kNone}, {0x40031A, 32, false, Fault::kNone},
      {0x400316, 4, true, Fault::kNone},   {0x40031A, 2, true, Fault::kNone},
      {0x400303, 32, true, Fault::kNone},  {0x400300, 1, true, Fault::kNone},
      {0x400300, 32, false, Fault::kNone}, {0x400319, 40, true, Fault::kNone},
      {0x400301, 1, true, Fault::kNone},   {0x400320, 4, true, Fault::kNone},
      {0x400303, 2, false, Fault::kNone},  {0x400300, 4, false, Fault::kNone},
      {0x400318, 1, false, Fault::kNone},  {0x400316, 40, false, Fault::kNone},
      {0x400316, 1, false, Fault::kNone},  {0x400302, 32, false, Fault::kNone},
      {0x400317, 1, true, Fault::kNone},   {0x40031F, 1, true, Fault::kNone},
      {0x400318, 40, true, Fault::kNone},  {0x400302, 40, true, Fault::kNone},
      {0x40031B, 2, true, Fault::kNone},   {0x400320, 1, false, Fault::kNone},
      {0x400318, 32, false, Fault::kNone}, {0x40031F, 32, true, Fault::kNone},
      {0x400317, 2, false, Fault::kNone},  {0x400302, 1, true, Fault::kNone},
      {0x400317, 32, true, Fault::kNone},  {0x400319, 1, false, Fault::kNone},
      {0x400300, 2, true, Fault::kNone},   {0x400320, 2, true, Fault::kNone},
      {0x400303, 1, false, Fault::kNone},  {0x40031F, 4, true, Fault::kNone},
      {0x40031A, 4, false, Fault::kNone},  {0x40031B, 1, false, Fault::kNone},
      {0x400301, 40, false, Fault::kNone}, {0x400320, 40, false, Fault::kNone},
      {0x400319, 32, false, Fault::kNone}, {0x400319, 2, false, Fault::kNone},
      {0x40031F, 40, true, Fault::kNone},  {0x400318, 2, false, Fault::kNone},
      {0x400303, 40, true, Fault::kNone},  {0x40031A, 1, true, Fault::kNone},
      {0x40031B, 32, true, Fault::kNone},  {0x400318, 4, true, Fault::kNone},
      {0x400301, 32, false, Fault::kNone}, {0x400301, 2, false, Fault::kNone},
      {0x400303, 4, true, Fault::kNone},   {0x400316, 2, false, Fault::kNone},
      {0x40031B, 4, true, Fault::kNone},   {0x400302, 4, false, Fault::kNone},
      {0x40031A, 40, false, Fault::kNone}, {0x400317, 4, false, Fault::kNone},
      {0x400300, 40, false, Fault::kNone}, {0x400319, 4, false, Fault::kNone},
      {0x400317, 40, true, Fault::kNone},  {0x400320, 32, false, Fault::kNone},
      {0x400316, 32, false, Fault::kNone}, {0x400301, 4, false, Fault::kNone},
  };
  return cases;
}

void check_efx_cases(const std::vector<EfxCase>& cases) {
  // Two starting states, so "the write landed" and "the untouched field was
  // preserved" are both visible.
  GsEfx populated;
  populated.type = 0x0122;
  populated.params.fill(0x05);
  populated.send_reverb = 0x11;
  populated.send_chorus = 0x22;
  populated.send_delay = 0x33;
  populated.assigned = true;
  const std::array<GsEfx, 2> starts{GsEfx{}, populated};

  for (const EfxCase& test : cases) {
    std::vector<uint8_t> data(test.len);
    for (size_t i = 0; i < data.size(); ++i) {
      data[i] = static_cast<uint8_t>((0x11u + i) & 0x7Fu);
    }
    const std::vector<uint8_t> msg = faulted_message(test.start, data, test.fault, test.framed);
    const bool addresses_efx = (test.start & 0xFFFF00u) == 0x400300u;
    const bool reaches_block = test.fault == Fault::kNone && addresses_efx;

    for (const GsEfx& before : starts) {
      INFO("start " << hex24(test.start) << " len " << static_cast<int>(test.len) << " fault "
                    << static_cast<int>(test.fault) << (test.framed ? " framed" : " bare"));
      const EfxOutcome expected = reaches_block ? expected_efx(before, test.start & 0xFFu, data)
                                                : EfxOutcome{before, false};

      GsEfx efx = before;
      bool type_changed = true;  // the call must clear it, whatever the outcome
      // The return is "a byte reached a field", not "the address was in the
      // block": a run landing only on the block's IGNORE rows applies nothing.
      CHECK(apply_gs_efx_sysex(efx, msg.data(), msg.size(), &type_changed) == expected.touched);
      CHECK(same_efx(efx, expected.efx));
      CHECK(type_changed == (expected.touched && expected.efx.type != before.type));
    }
  }
}

}  // namespace

TEST_CASE("GS SysEx: a malformed frame is refused whatever it addresses", "[midi][gs][address]") {
  check_parse_cases(parse_frame_cases());
}

TEST_CASE("GS SysEx: the value each address answers with is pinned", "[midi][gs][address]") {
  check_parse_cases(parse_value_cases());
  check_parse_cases(parse_universal_cases());
}

TEST_CASE("GS SysEx: a 7-bit violation is refused, not masked", "[midi][gs][address]") {
  // 40 12 15 with data 0x81: a byte no SysEx can carry, with a checksum that
  // matches it. The frame layer refuses the message rather than masking the
  // byte down to a rhythm-part assignment.
  const std::vector<uint8_t> msg = faulted_message(0x401215, {0x01}, Fault::kHighBit, true);
  REQUIRE(msg[8] == 0x81);
  CHECK(parse_gs_sysex(msg.data(), msg.size()).kind == GsSysExKind::kNone);
  CHECK_FALSE(gs_sysex_frame(msg.data(), msg.size()).valid);

  GsEfx efx;
  const std::vector<uint8_t> efx_msg =
      faulted_message(0x400300, {0x01, 0x10}, Fault::kHighBit, true);
  CHECK_FALSE(apply_gs_efx_sysex(efx, efx_msg.data(), efx_msg.size()));
  CHECK_FALSE(efx.assigned);
}

TEST_CASE("GS EFX: a malformed frame leaves the unit untouched", "[midi][gs][address]") {
  check_efx_cases(efx_frame_cases());
}

TEST_CASE("GS EFX: every block offset lands where the layout says", "[midi][gs][address]") {
  check_efx_cases(efx_run_cases());

  SECTION("a run reaching past the block carries into the next address group") {
    // 40 03 7E + 4 bytes runs 7E, 7F, then carries to 40 04 00 and 40 04 01.
    // The message addressed the EFX block and no byte in it is a field offset,
    // so the unit is untouched and the call reports nothing applied.
    GsEfx efx;
    const std::vector<uint8_t> msg = roland_message(0x40037E, {0x01, 0x02, 0x03, 0x04});
    CHECK_FALSE(apply_gs_efx_sysex(efx, msg.data(), msg.size()));
    CHECK_FALSE(efx.assigned);
  }

  SECTION("a run longer than the block still applies its last in-block byte") {
    // EFX PARAMETER 20 sits at offset 0x16, the 23rd byte of a run from 0x00.
    GsEfx efx;
    std::vector<uint8_t> data(40);
    for (size_t i = 0; i < data.size(); ++i) {
      data[i] = static_cast<uint8_t>((0x11u + i) & 0x7Fu);
    }
    const std::vector<uint8_t> msg = roland_message(0x400300, data);
    CHECK(apply_gs_efx_sysex(efx, msg.data(), msg.size()));
    CHECK(efx.params[19] == data[0x16]);
    CHECK(efx.send_delay == data[0x19]);
  }
}

TEST_CASE("GS address table: the 40 4x 22 range is the extension's, not the manual's",
          "[midi][gs][address]") {
  const GsAddressEntry* assign = gs_lookup_address(0x404022);
  REQUIRE(assign != nullptr);
  // 02-10 select insertion units 1-15 (docs/gs.md). A failure here means the
  // range was narrowed back to the manual's 00/01 and the extension is
  // unreachable.
  CHECK(assign->hi == 0x10);
  CHECK(gs_value_in_range(*assign, 0x00));
  CHECK(gs_value_in_range(*assign, 0x01));
  CHECK(gs_value_in_range(*assign, 0x02));
  CHECK(gs_value_in_range(*assign, 0x10));
  CHECK_FALSE(gs_value_in_range(*assign, 0x11));

  const GsAddressEntry* rhythm = gs_lookup_address(0x401015);
  REQUIRE(rhythm != nullptr);
  CHECK(gs_value_in_range(*rhythm, 0x02));
  CHECK_FALSE(gs_value_in_range(*rhythm, 0x03));
}

TEST_CASE("GS decode: the unknown counter separates a gap from a claimed address",
          "[midi][gs][address]") {
  auto unknowns = [](uint32_t addr) {
    const std::vector<uint8_t> msg = roland_message(addr, {0x01});
    GsWrite write;
    uint32_t unknown = 0;
    const size_t total = gs_decode_sysex(msg.data(), msg.size(), &write, 1, &unknown);
    CHECK(total == 1);
    return unknown;
  };
  // The control: an address a row claims, and an address the range table
  // claims, are both silent.
  CHECK(unknowns(0x400004) == 0);
  CHECK(unknowns(0x401015) == 0);
  CHECK(unknowns(0x400302) == 0);
  REQUIRE(gs_lookup_range(0x400302) != nullptr);
  // The finding: 40 1x 23 RX BANK SELECT sits above REVERB SEND LEVEL and below
  // the undefined run at 40 1x 25 with no row and no range of its own, so it
  // counts. The manual defines it, so the absence is a gap awaiting a row rather
  // than a decision; this case wants any such address and no file writes this
  // one, which is the only reason it was picked over a busier gap.
  REQUIRE(gs_lookup_address(0x401023) == nullptr);
  REQUIRE(gs_lookup_range(0x401023) == nullptr);
  CHECK(unknowns(0x401023) == 1);
  // A second one from another family, so the counter is not being shown to work
  // in a single block. 41 mA rr is a drum-setup parameter nibble the manual does
  // not define — the block ends at m9 — so no file writes it and nothing is
  // planned for it, where the previous choice here was a nibble the manual DOES
  // define and stopped being a probe as soon as it was implemented. Both carry
  // the lookup guards above, because an address that quietly acquires a row
  // would keep the case passing for the wrong reason.
  REQUIRE(gs_lookup_address(0x410A00) == nullptr);
  REQUIRE(gs_lookup_range(0x410A00) == nullptr);
  CHECK(unknowns(0x410A00) == 1);

  // The combined entry point refuses what the frame layer refuses.
  uint32_t unknown = 0;
  CHECK(gs_decode_sysex(nullptr, 0, nullptr, 0, &unknown) == 0);
  const std::vector<uint8_t> junk{0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0x01, 0x02};
  CHECK(gs_decode_sysex(junk.data(), junk.size(), nullptr, 0, &unknown) == 0);
  CHECK(unknown == 0);
}

namespace {

using sonare::midi::synth::gs_delay_feedback_signed;
using sonare::midi::synth::GsSystemEffects;

/// One row as the Parameter Address Map gives it. Transcribed from the manual
/// rather than read back from the table, so a wrong number in the header is a
/// disagreement between two sources instead of the header matching itself.
///
/// The level is deliberately not among these fields: the manual has no such
/// column, so a copy of it here could only ever agree with whatever the table
/// last said. It is measured instead, in gs_audibility_test.cpp.
struct MapRow {
  uint32_t addr;
  const char* name;
  GsParam param;
  uint8_t lo, hi, def;
};

const std::vector<MapRow>& patch_common_map() {
  static const std::vector<MapRow> rows = {
      {0x400100, "PATCH NAME", GsParam::kPatchName, 0x20, 0x7F, 0x20},
      {0x400130, "REVERB MACRO", GsParam::kReverbMacro, 0, 7, 4},
      {0x400131, "REVERB CHARACTER", GsParam::kReverbCharacter, 0, 7, 4},
      {0x400132, "REVERB PRE-LPF", GsParam::kReverbPreLpf, 0, 7, 0},
      {0x400133, "REVERB LEVEL", GsParam::kReverbLevel, 0, 127, 64},
      {0x400134, "REVERB TIME", GsParam::kReverbTime, 0, 127, 64},
      {0x400135, "REVERB DELAY FEEDBACK", GsParam::kReverbDelayFeedback, 0, 127, 0},
      {0x400137, "REVERB PREDELAY TIME", GsParam::kReverbPredelay, 0, 127, 0},
      {0x400138, "CHORUS MACRO", GsParam::kChorusMacro, 0, 7, 2},
      {0x400139, "CHORUS PRE-LPF", GsParam::kChorusPreLpf, 0, 7, 0},
      {0x40013A, "CHORUS LEVEL", GsParam::kChorusLevel, 0, 127, 64},
      {0x40013B, "CHORUS FEEDBACK", GsParam::kChorusFeedback, 0, 127, 8},
      {0x40013C, "CHORUS DELAY", GsParam::kChorusDelay, 0, 127, 80},
      {0x40013D, "CHORUS RATE", GsParam::kChorusRate, 0, 127, 3},
      {0x40013E, "CHORUS DEPTH", GsParam::kChorusDepth, 0, 127, 19},
      {0x40013F, "CHORUS SEND TO REVERB", GsParam::kChorusSendToReverb, 0, 127, 0},
      {0x400140, "CHORUS SEND TO DELAY", GsParam::kChorusSendToDelay, 0, 127, 0},
      {0x400150, "DELAY MACRO", GsParam::kDelayMacro, 0x00, 0x09, 0x00},
      {0x400151, "DELAY PRE-LPF", GsParam::kDelayPreLpf, 0, 7, 0},
      {0x400152, "DELAY TIME CENTER", GsParam::kDelayTimeCenter, 0x01, 0x73, 0x61},
      {0x400153, "DELAY TIME RATIO LEFT", GsParam::kDelayTimeRatioLeft, 0x01, 0x78, 0x01},
      {0x400154, "DELAY TIME RATIO RIGHT", GsParam::kDelayTimeRatioRight, 0x01, 0x78, 0x01},
      {0x400155, "DELAY LEVEL CENTER", GsParam::kDelayLevelCenter, 0, 127, 0x7F},
      {0x400156, "DELAY LEVEL LEFT", GsParam::kDelayLevelLeft, 0, 127, 0x00},
      {0x400157, "DELAY LEVEL RIGHT", GsParam::kDelayLevelRight, 0, 127, 0x00},
      {0x400158, "DELAY LEVEL", GsParam::kDelayLevel, 0, 127, 0x40},
      {0x400159, "DELAY FEEDBACK", GsParam::kDelayFeedback, 0x00, 0x7F, 0x50},
      {0x40015A, "DELAY SEND TO REVERB", GsParam::kDelaySendToReverb, 0, 127, 0x00},
      {0x400200, "EQ LOW FREQ", GsParam::kEqLowFreq, 0, 1, 0},
      {0x400201, "EQ LOW GAIN", GsParam::kEqLowGain, 0x34, 0x4C, 0x40},
      {0x400202, "EQ HIGH FREQ", GsParam::kEqHighFreq, 0, 1, 0},
      {0x400203, "EQ HIGH GAIN", GsParam::kEqHighGain, 0x34, 0x4C, 0x40},
      {0x404020, "EQ ON/OFF", GsParam::kPartEqSwitch, 0, 1, 1},
  };
  return rows;
}

/// The address each system-effect field arrives on, so the table's def and the
/// value layer's power-on default are checked against each other by name.
struct SystemEffectField {
  uint32_t addr;
  const char* name;
  uint8_t GsSystemEffects::*field;
};

const std::vector<SystemEffectField>& system_effect_fields() {
  static const std::vector<SystemEffectField> fields = {
      {0x400130, "reverb_macro", &GsSystemEffects::reverb_macro},
      {0x400131, "reverb_character", &GsSystemEffects::reverb_character},
      {0x400132, "reverb_pre_lpf", &GsSystemEffects::reverb_pre_lpf},
      {0x400133, "reverb_level", &GsSystemEffects::reverb_level},
      {0x400134, "reverb_time", &GsSystemEffects::reverb_time},
      {0x400135, "reverb_delay_feedback", &GsSystemEffects::reverb_delay_feedback},
      {0x400137, "reverb_predelay", &GsSystemEffects::reverb_predelay},
      {0x400138, "chorus_macro", &GsSystemEffects::chorus_macro},
      {0x400139, "chorus_pre_lpf", &GsSystemEffects::chorus_pre_lpf},
      {0x40013A, "chorus_level", &GsSystemEffects::chorus_level},
      {0x40013B, "chorus_feedback", &GsSystemEffects::chorus_feedback},
      {0x40013C, "chorus_delay", &GsSystemEffects::chorus_delay},
      {0x40013D, "chorus_rate", &GsSystemEffects::chorus_rate},
      {0x40013E, "chorus_depth", &GsSystemEffects::chorus_depth},
      {0x40013F, "chorus_send_to_reverb", &GsSystemEffects::chorus_send_to_reverb},
      {0x400140, "chorus_send_to_delay", &GsSystemEffects::chorus_send_to_delay},
      {0x400150, "delay_macro", &GsSystemEffects::delay_macro},
      {0x400151, "delay_pre_lpf", &GsSystemEffects::delay_pre_lpf},
      {0x400152, "delay_time_center", &GsSystemEffects::delay_time_center},
      {0x400153, "delay_time_ratio_left", &GsSystemEffects::delay_time_ratio_left},
      {0x400154, "delay_time_ratio_right", &GsSystemEffects::delay_time_ratio_right},
      {0x400155, "delay_level_center", &GsSystemEffects::delay_level_center},
      {0x400156, "delay_level_left", &GsSystemEffects::delay_level_left},
      {0x400157, "delay_level_right", &GsSystemEffects::delay_level_right},
      {0x400158, "delay_level", &GsSystemEffects::delay_level},
      {0x400159, "delay_feedback", &GsSystemEffects::delay_feedback},
      {0x40015A, "delay_send_to_reverb", &GsSystemEffects::delay_send_to_reverb},
  };
  return fields;
}

}  // namespace

TEST_CASE("GS address table: the patch-common and EQ rows match the address map",
          "[midi][gs][address]") {
  for (const MapRow& row : patch_common_map()) {
    INFO(row.name << " at " << hex24(row.addr));
    const GsAddressEntry* entry = gs_lookup_address(row.addr);
    REQUIRE(entry != nullptr);
    CHECK(entry->param == row.param);
    CHECK(entry->lo == row.lo);
    CHECK(entry->hi == row.hi);
    CHECK(entry->def == row.def);
  }
}

TEST_CASE("GS address table: REVERB MACRO powers on at Hall 2, not at Room 1",
          "[midi][gs][address]") {
  const GsAddressEntry* macro = gs_lookup_address(0x400130);
  REQUIRE(macro != nullptr);
  // The trap: 0 is a valid macro (Room 1) and a plausible-looking default, but
  // the map's power-on value is 4.
  CHECK(macro->def == 4);
  CHECK(macro->def != 0);
  CHECK(gs_value_in_range(*macro, 0x07));
  CHECK_FALSE(gs_value_in_range(*macro, 0x08));
}

TEST_CASE("GS address table: DELAY FEEDBACK powers on at 50, which is not the centre",
          "[midi][gs][address]") {
  const GsAddressEntry* feedback = gs_lookup_address(0x400159);
  REQUIRE(feedback != nullptr);
  // The row bounds the raw byte; the -64..+63 reading belongs to the value
  // layer, and 50 is +16 there rather than the 40 a centred parameter takes.
  CHECK(feedback->lo == 0x00);
  CHECK(feedback->hi == 0x7F);
  CHECK(feedback->def == 0x50);
  CHECK(feedback->def != 0x40);
  CHECK(gs_delay_feedback_signed(feedback->def) == 16);
  CHECK(gs_delay_feedback_signed(0x40) == 0);
}

TEST_CASE("GS address table: the system-effect defaults agree with the value layer",
          "[midi][gs][address]") {
  const GsSystemEffects power_on;
  for (const SystemEffectField& field : system_effect_fields()) {
    INFO(field.name << " at " << hex24(field.addr));
    const GsAddressEntry* entry = gs_lookup_address(field.addr);
    REQUIRE(entry != nullptr);
    CHECK(entry->def == power_on.*field.field);
    CHECK(gs_value_in_range(*entry, power_on.*field.field));
  }
  // Every address the value layer models has a row, and vice versa: a field
  // added to one side without the other leaves the counts apart.
  CHECK(system_effect_fields().size() == sonare::midi::synth::kGsSystemEffectFieldCount);
}

TEST_CASE("GS decode: a run through the patch-common block", "[midi][gs][address]") {
  SECTION("from the reverb block into the undefined 36 and out the other side") {
    const Decoded decoded = decode(roland_message(0x400134, {0x40, 0x00, 0x00, 0x00, 0x02}));
    REQUIRE(decoded.writes.size() == 5);
    CHECK(decoded.writes[0].param == GsParam::kReverbTime);
    CHECK(decoded.writes[1].param == GsParam::kReverbDelayFeedback);
    CHECK(decoded.writes[2].param == GsParam::kUndefined);
    CHECK(decoded.writes[2].addr == 0x400136u);
    CHECK(decoded.writes[3].param == GsParam::kReverbPredelay);
    CHECK(decoded.writes[4].param == GsParam::kChorusMacro);
    CHECK(decoded.unknown == 0);
  }

  SECTION("from the chorus block into the undefined 41-4F and on to DELAY MACRO") {
    std::vector<uint8_t> data(18, 0x00);
    const Decoded decoded = decode(roland_message(0x40013F, data));
    REQUIRE(decoded.writes.size() == 18);
    CHECK(decoded.writes[0].param == GsParam::kChorusSendToReverb);
    CHECK(decoded.writes[1].param == GsParam::kChorusSendToDelay);
    for (size_t i = 2; i < 17; ++i) {
      INFO("byte " << i);
      CHECK(decoded.writes[i].param == GsParam::kUndefined);
    }
    CHECK(decoded.writes[17].param == GsParam::kDelayMacro);
    CHECK(decoded.writes[17].addr == 0x400150u);
    CHECK(decoded.unknown == 0);
  }

  SECTION("across the 7F carry from the delay tail into the master EQ") {
    // 40 01 7E is inside the undefined tail; the third byte carries to 40 02 00.
    const Decoded decoded = decode(roland_message(0x40017E, {0x00, 0x00, 0x01, 0x40}));
    REQUIRE(decoded.writes.size() == 4);
    CHECK(decoded.writes[0].param == GsParam::kUndefined);
    CHECK(decoded.writes[1].addr == 0x40017Fu);
    CHECK(decoded.writes[1].param == GsParam::kUndefined);
    CHECK(decoded.writes[2].addr == 0x400200u);
    CHECK(decoded.writes[2].param == GsParam::kEqLowFreq);
    CHECK(decoded.writes[3].addr == 0x400201u);
    CHECK(decoded.writes[3].param == GsParam::kEqLowGain);
    CHECK(decoded.unknown == 0);
  }

  SECTION("PATCH NAME takes 16 bytes as one parameter") {
    std::vector<uint8_t> data(16, 0x41);
    const Decoded decoded = decode(roland_message(0x400100, data));
    REQUIRE(decoded.writes.size() == 16);
    for (size_t i = 0; i < decoded.writes.size(); ++i) {
      INFO("character " << i);
      CHECK(decoded.writes[i].param == GsParam::kPatchName);
      CHECK(decoded.writes[i].index == i);
    }
    CHECK(decoded.unknown == 0);
  }
}

TEST_CASE("GS address table: the part EQ switch is per part and powers on ON",
          "[midi][gs][address]") {
  for (uint8_t block = 0; block < 16; ++block) {
    const uint32_t addr = 0x404020u | (static_cast<uint32_t>(block) << 8);
    INFO("block " << static_cast<int>(block));
    const GsAddressEntry* entry = gs_lookup_address(addr);
    REQUIRE(entry != nullptr);
    CHECK(entry->param == GsParam::kPartEqSwitch);
    CHECK(entry->def == 0x01);
    CHECK(gs_address_block_index(addr, entry->mask) == gs_part_block_to_channel(block));
    CHECK_FALSE(gs_value_in_range(*entry, 0x02));
  }
  // The switch claims only its own byte: its neighbours keep their own answers.
  CHECK(gs_lookup_address(0x40401F) == nullptr);
  CHECK(gs_lookup_address(0x404021) == nullptr);
  const GsAddressEntry* assign = gs_lookup_address(0x404022);
  REQUIRE(assign != nullptr);
  CHECK(assign->param == GsParam::kPartEfxAssign);
}
