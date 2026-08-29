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
    if (addr >= kGsUndefinedRanges[i].lo_addr && addr <= kGsUndefinedRanges[i].hi_addr) {
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
    CHECK(decoded.unknown == 2);
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
  CHECK(gs_address_block_index(0x41123C, 0x00F0FF) == 1);
  // No variable nibble.
  CHECK(gs_address_block_index(0x400004, 0x000000) == 0);
}

TEST_CASE("GS address table: a masked row claims exactly its own addresses",
          "[midi][gs][address]") {
  using sonare::midi::synth::detail::gs_row_claims;
  // The shape a drum-setup row will take: a variable map nibble in the mid byte
  // and a variable note in the low byte.
  const GsAddressEntry drum_level{
      0x410200, 0x00F0FF, GsParam::kUnknown, GsLevel::kAudible, 1, 0x00, 0x7F, 0x7F, nullptr};
  CHECK(gs_row_claims(drum_level, 0x410200));
  CHECK(gs_row_claims(drum_level, 0x41023C));
  CHECK(gs_row_claims(drum_level, 0x41123C));        // map 2
  CHECK_FALSE(gs_row_claims(drum_level, 0x41013C));  // a neighbouring parameter group
  CHECK_FALSE(gs_row_claims(drum_level, 0x40023C));  // another block entirely

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
