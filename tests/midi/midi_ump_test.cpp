/// @file midi_ump_test.cpp
/// @brief MIDI core: UMP encode/decode round-trips, MIDI 1.0 byte-stream
///        adapter round-trip, and MIDI 1.0 <-> 2.0 conversion with the
///        documented lossy velocity/CC scaling pinned to exact values.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <ios>
#include <vector>

#include "midi/ump.h"

TEST_CASE("Default MIDI translation isolates UMP groups", "[midi]") {
  using namespace sonare::midi;
  Midi1ToMidi2Translator translator;
  translator.translate(make_midi1_control_change(1, 3, 0, 12));
  translator.translate(make_midi1_control_change(1, 3, 32, 34));
  const auto other = translator.translate(make_midi1_program_change(2, 3, 7));
  REQUIRE(other.count == 1);
  CHECK((other.messages[0].words[0] & 1u) == 0);
  const auto original = translator.translate(make_midi1_program_change(1, 3, 7));
  REQUIRE(original.count == 1);
  CHECK((original.messages[0].words[0] & 1u) == 1);
  translator.translate(make_midi1_control_change(1, 3, 101, 0));
  translator.translate(make_midi1_control_change(1, 3, 100, 1));
  translator.translate(make_midi1_control_change(2, 3, 6, 50));
  CHECK(translator.translate(make_midi1_control_change(2, 3, 38, 20)).count == 0);
  translator.translate(make_midi1_control_change(1, 3, 6, 50));
  CHECK(translator.translate(make_midi1_control_change(1, 3, 38, 20)).count == 1);
}

TEST_CASE("SysEx store rejects an oversized payload without eviction", "[midi]") {
  using namespace sonare::midi;
  SysExStore store;
  store.set_retention_budget(8, 4);
  const std::vector<uint8_t> small{1, 2, 3};
  const std::vector<uint8_t> large(16, 42);
  const auto handle = store.add(small);
  REQUIRE(handle != 0);
  CHECK(store.add(large) == 0);
  CHECK_FALSE(store.add_with_handle(handle, large.data(), large.size()));
  CHECK_FALSE(store.add_with_handle(99, large.data(), large.size()));
  REQUIRE(store.lookup(handle) != nullptr);
  CHECK(*store.lookup(handle) == small);
  CHECK(store.lookup(99) == nullptr);
  CHECK(store.retained_bytes() == small.size());
  CHECK(store.evicted_count() == 0);
}

namespace {

using sonare::midi::Ump;
using sonare::midi::UmpMessageType;
using sonare::midi::UmpStatus;

uint8_t status_of(const Ump& u) { return u.status_nibble(); }

}  // namespace

TEST_CASE("UMP MIDI 1.0 channel-voice encode exposes the expected fields", "[midi]") {
  const Ump note_on = sonare::midi::make_midi1_note_on(/*group=*/2, /*channel=*/5,
                                                       /*note=*/60, /*velocity7=*/100);
  REQUIRE(note_on.message_type() == UmpMessageType::kMidi1ChannelVoice);
  REQUIRE(status_of(note_on) == static_cast<uint8_t>(UmpStatus::kNoteOn));
  REQUIRE(note_on.channel() == 5);
  REQUIRE(note_on.group == 2);
  REQUIRE(note_on.note_number() == 60);
  REQUIRE(note_on.is_note_on());
  REQUIRE_FALSE(note_on.is_note_off());
  REQUIRE(note_on.word_count == 1);

  const Ump note_off = sonare::midi::make_midi1_note_off(2, 5, 60, 0);
  REQUIRE(note_off.is_note_off());
  REQUIRE_FALSE(note_off.is_note_on());
  REQUIRE(note_off.note_number() == 60);

  const Ump cc = sonare::midi::make_midi1_control_change(1, 3, /*controller=*/7, /*value7=*/64);
  REQUIRE(status_of(cc) == static_cast<uint8_t>(UmpStatus::kControlChange));
  REQUIRE(cc.channel() == 3);
  REQUIRE(((cc.words[0] >> 8) & 0x7Fu) == 7u);
  REQUIRE((cc.words[0] & 0x7Fu) == 64u);

  const Ump poly = sonare::midi::make_midi1_poly_pressure(1, 3, /*note=*/61, /*pressure7=*/70);
  REQUIRE(status_of(poly) == static_cast<uint8_t>(UmpStatus::kPolyPressure));
  REQUIRE(poly.note_number() == 61);
  REQUIRE((poly.words[0] & 0x7Fu) == 70u);

  const Ump pc = sonare::midi::make_midi1_program_change(0, 9, /*program=*/42);
  REQUIRE(status_of(pc) == static_cast<uint8_t>(UmpStatus::kProgramChange));
  REQUIRE(pc.channel() == 9);
  REQUIRE(((pc.words[0] >> 8) & 0x7Fu) == 42u);

  const Ump pressure = sonare::midi::make_midi1_channel_pressure(0, 9, /*pressure7=*/88);
  REQUIRE(status_of(pressure) == static_cast<uint8_t>(UmpStatus::kChannelPressure));
  REQUIRE(((pressure.words[0] >> 8) & 0x7Fu) == 88u);

  const Ump bend = sonare::midi::make_midi1_pitch_bend(0, 9, /*bend14=*/0x1234u);
  REQUIRE(status_of(bend) == static_cast<uint8_t>(UmpStatus::kPitchBend));
  REQUIRE(((bend.words[0] >> 8) & 0x7Fu) == 0x34u);
  REQUIRE((bend.words[0] & 0x7Fu) == 0x24u);
}

TEST_CASE("documented sonare_c_project_midi data0 packing yields a note-on", "[midi]") {
  // The public C-ABI header documents how a caller hand-packs a MIDI 1.0 note-on
  // into data0. A word built by that exact formula must be recognized as a
  // note-on and expose the packed fields, and must match make_midi1_note_on.
  const uint32_t group = 2;
  const uint32_t status = static_cast<uint32_t>(UmpStatus::kNoteOn);  // 0x9
  const uint32_t channel = 5;
  const uint32_t note = 60;
  const uint32_t velocity = 100;
  const uint32_t data0 =
      (0x2u << 28) | (group << 24) | (status << 20) | (channel << 16) | (note << 8) | velocity;

  Ump packed;
  packed.words[0] = data0;
  packed.word_count = 1;
  packed.group = static_cast<uint8_t>(group);

  REQUIRE(packed.is_note_on());
  REQUIRE(packed.channel() == channel);
  REQUIRE(packed.note_number() == note);
  REQUIRE(packed.words[0] == sonare::midi::make_midi1_note_on(
                                 static_cast<uint8_t>(group), static_cast<uint8_t>(channel),
                                 static_cast<uint8_t>(note), static_cast<uint8_t>(velocity))
                                 .words[0]);
}

TEST_CASE("MIDI 1.0 note-on velocity zero is treated as note-off", "[midi]") {
  const Ump zero_vel = sonare::midi::make_midi1_note_on(0, 0, 60, 0);
  REQUIRE(zero_vel.message_type() == UmpMessageType::kMidi1ChannelVoice);
  REQUIRE(status_of(zero_vel) == static_cast<uint8_t>(UmpStatus::kNoteOn));
  REQUIRE_FALSE(zero_vel.is_note_on());
  REQUIRE(zero_vel.is_note_off());

  const Ump midi2_zero_vel = sonare::midi::make_midi2_note_on(0, 0, 60, 0);
  REQUIRE(midi2_zero_vel.message_type() == UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(midi2_zero_vel.is_note_on());
  REQUIRE_FALSE(midi2_zero_vel.is_note_off());
}

TEST_CASE("UMP note predicates ignore non-channel message types", "[midi]") {
  for (uint8_t message_type = 0; message_type < 16; ++message_type) {
    if (message_type == static_cast<uint8_t>(UmpMessageType::kMidi1ChannelVoice) ||
        message_type == static_cast<uint8_t>(UmpMessageType::kMidi2ChannelVoice)) {
      continue;
    }
    for (const uint8_t status :
         {static_cast<uint8_t>(UmpStatus::kNoteOff), static_cast<uint8_t>(UmpStatus::kNoteOn)}) {
      Ump packet;
      packet.words[0] = (static_cast<uint32_t>(message_type) << 28) |
                        (static_cast<uint32_t>(status) << 20) | (60u << 8) | 100u;
      packet.word_count = sonare::midi::ump_word_count_for_word0(packet.words[0]);

      CHECK_FALSE(packet.is_note_on());
      CHECK_FALSE(packet.is_note_off());
    }
  }
}

TEST_CASE("MIDI 1.0 -> 2.0 lowers a velocity-zero note-on to a note-off", "[midi]") {
  const Ump m1_zero = sonare::midi::make_midi1_note_on(2, 5, /*note=*/60, /*velocity7=*/0);
  const Ump m2 = sonare::midi::midi1_to_midi2(m1_zero);
  REQUIRE(m2.message_type() == UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(m2.is_note_off());
  REQUIRE_FALSE(m2.is_note_on());

  const Ump m1_hit = sonare::midi::make_midi1_note_on(2, 5, 60, /*velocity7=*/64);
  const Ump m2_hit = sonare::midi::midi1_to_midi2(m1_hit);
  REQUIRE(m2_hit.is_note_on());
  REQUIRE_FALSE(m2_hit.is_note_off());
}

TEST_CASE("UMP MIDI 2.0 channel-voice encode exposes the expected fields", "[midi]") {
  const Ump note_on = sonare::midi::make_midi2_note_on(/*group=*/1, /*channel=*/4,
                                                       /*note=*/64, /*velocity16=*/0x8000u);
  REQUIRE(note_on.message_type() == UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(note_on.is_note_on());
  REQUIRE(note_on.channel() == 4);
  REQUIRE(note_on.note_number() == 64);
  REQUIRE(note_on.word_count == 2);
  REQUIRE(static_cast<uint16_t>(note_on.words[1] >> 16) == 0x8000u);

  const Ump note_off = sonare::midi::make_midi2_note_off(1, 4, 64, /*velocity16=*/0x4000u);
  REQUIRE(note_off.is_note_off());
  REQUIRE(static_cast<uint16_t>(note_off.words[1] >> 16) == 0x4000u);

  const Ump cc =
      sonare::midi::make_midi2_control_change(0, 2, /*controller=*/10, /*value32=*/0xDEADBEEFu);
  REQUIRE(status_of(cc) == static_cast<uint8_t>(UmpStatus::kControlChange));
  REQUIRE(cc.words[1] == 0xDEADBEEFu);

  const Ump poly = sonare::midi::make_midi2_poly_pressure(0, 2, /*note=*/62,
                                                          /*pressure32=*/0xCAFEBABEu);
  REQUIRE(status_of(poly) == static_cast<uint8_t>(UmpStatus::kPolyPressure));
  REQUIRE(poly.note_number() == 62);
  REQUIRE(poly.words[1] == 0xCAFEBABEu);

  const Ump pc = sonare::midi::make_midi2_program_change(0, 6, /*program=*/12, /*bank_msb=*/1,
                                                         /*bank_lsb=*/2, /*bank_valid=*/true);
  REQUIRE(status_of(pc) == static_cast<uint8_t>(UmpStatus::kProgramChange));
  REQUIRE(((pc.words[1] >> 24) & 0x7Fu) == 12u);
  REQUIRE(((pc.words[1] >> 8) & 0x7Fu) == 1u);
  REQUIRE((pc.words[1] & 0x7Fu) == 2u);
  REQUIRE((pc.words[0] & 0x01u) == 0x01u);  // bank-valid flag in byte3.

  const Ump pressure = sonare::midi::make_midi2_channel_pressure(0, 6, 0x80000000u);
  REQUIRE(status_of(pressure) == static_cast<uint8_t>(UmpStatus::kChannelPressure));
  REQUIRE(pressure.words[1] == 0x80000000u);

  const Ump bend = sonare::midi::make_midi2_pitch_bend(0, 6, 0x80000000u);
  REQUIRE(status_of(bend) == static_cast<uint8_t>(UmpStatus::kPitchBend));
  REQUIRE(bend.words[1] == 0x80000000u);

  const Ump pnc = sonare::midi::make_midi2_per_note_controller(0, 0, /*note=*/72, /*index=*/3,
                                                               /*value32=*/0x12345678u);
  REQUIRE(pnc.message_type() == UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(status_of(pnc) == static_cast<uint8_t>(UmpStatus::kRegisteredPerNoteController));
  REQUIRE(pnc.note_number() == 72);
  REQUIRE(((pnc.words[0]) & 0xFFu) == 3u);  // controller index in byte3.
  REQUIRE(pnc.words[1] == 0x12345678u);

  const Ump apnc =
      sonare::midi::make_midi2_assignable_per_note_controller(0, 0, 73, 4, 0x23456789u);
  REQUIRE(status_of(apnc) == static_cast<uint8_t>(UmpStatus::kAssignablePerNoteController));
  REQUIRE(apnc.note_number() == 73);
  REQUIRE((apnc.words[0] & 0xFFu) == 4u);
  REQUIRE(apnc.words[1] == 0x23456789u);

  const Ump rc = sonare::midi::make_midi2_registered_controller(0, 1, 2, 3, 0x3456789Au);
  REQUIRE(status_of(rc) == static_cast<uint8_t>(UmpStatus::kRegisteredController));
  REQUIRE(((rc.words[0] >> 8) & 0xFFu) == 2u);
  REQUIRE((rc.words[0] & 0xFFu) == 3u);
  REQUIRE(rc.words[1] == 0x3456789Au);

  const Ump ac = sonare::midi::make_midi2_assignable_controller(0, 1, 4, 5, 0x456789ABu);
  REQUIRE(status_of(ac) == static_cast<uint8_t>(UmpStatus::kAssignableController));
  REQUIRE(((ac.words[0] >> 8) & 0xFFu) == 4u);
  REQUIRE((ac.words[0] & 0xFFu) == 5u);
  REQUIRE(ac.words[1] == 0x456789ABu);
}

TEST_CASE("UMP SysEx handle carries the handle id without inline payload", "[midi]") {
  const Ump sx = sonare::midi::make_sysex_handle(/*group=*/3, /*handle=*/0xABCDu);
  REQUIRE(sx.message_type() == UmpMessageType::kData64);
  REQUIRE(sx.group == 3);
  REQUIRE(sx.sysex_handle == 0xABCDu);
}

TEST_CASE("SysExStore owns variable-length payloads behind UMP handles", "[midi]") {
  sonare::midi::SysExStore store;
  const std::vector<uint8_t> payload = {0xF0u, 0x7Eu, 0x7Fu, 0x09u, 0x01u, 0xF7u};

  const sonare::midi::SysExHandle handle = store.add(payload);
  REQUIRE(handle != 0);
  REQUIRE(store.size() == 1);
  REQUIRE(store.contains(handle));

  const std::vector<uint8_t>* stored = store.lookup(handle);
  REQUIRE(stored != nullptr);
  REQUIRE(*stored == payload);

  const Ump sx = sonare::midi::make_sysex_handle(/*group=*/2, handle);
  REQUIRE(sx.sysex_handle == handle);
  REQUIRE(sx.group == 2);

  REQUIRE(store.lookup(0) == nullptr);
  REQUIRE(store.add(nullptr, payload.size()) == 0);
  REQUIRE(store.add(payload.data(), 0) == 0);

  REQUIRE(store.remove(handle));
  REQUIRE_FALSE(store.contains(handle));
  REQUIRE_FALSE(store.remove(handle));

  const sonare::midi::SysExHandle second = store.add(payload);
  REQUIRE(second != 0);
  store.clear();
  REQUIRE(store.size() == 0);
  REQUIRE(store.lookup(second) == nullptr);
}

// A store fed by a live input accumulates for as long as the device is open:
// consumers copy the bytes out of a drain and never tell the store they are
// done, so without a budget the footprint is everything the device has ever
// sent. An import store has the opposite lifetime and must keep everything,
// which is why the budget is opt-in rather than a default.
TEST_CASE("SysExStore retention budget bounds a live stream", "[midi]") {
  sonare::midi::SysExStore store;
  const std::vector<uint8_t> chunk(1024u, 0x7Fu);

  SECTION("unbounded by default, so an import store keeps every entry") {
    for (int i = 0; i < 256; ++i) {
      REQUIRE(store.add(chunk) != 0);
    }
    REQUIRE(store.size() == 256);
    REQUIRE(store.retained_bytes() == 256u * 1024u);
    REQUIRE(store.evicted_count() == 0);
  }

  SECTION("a budget caps the footprint and drops the oldest first") {
    store.set_retention_budget(8u * 1024u, 1024u);
    std::vector<sonare::midi::SysExHandle> handles;
    for (int i = 0; i < 64; ++i) {
      const sonare::midi::SysExHandle h = store.add(chunk);
      REQUIRE(h != 0);
      handles.push_back(h);
    }
    // Steady state, not growth: 8 KiB of payload however much was pushed.
    REQUIRE(store.retained_bytes() <= 8u * 1024u);
    REQUIRE(store.size() == 8);
    REQUIRE(store.evicted_count() == 56);
    // The survivors are the newest, and the oldest are the ones gone.
    for (size_t i = 0; i < handles.size() - 8u; ++i) {
      REQUIRE_FALSE(store.contains(handles[i]));
    }
    for (size_t i = handles.size() - 8u; i < handles.size(); ++i) {
      REQUIRE(store.contains(handles[i]));
    }
  }

  SECTION("lowering the budget evicts immediately") {
    for (int i = 0; i < 16; ++i) REQUIRE(store.add(chunk) != 0);
    REQUIRE(store.retained_bytes() == 16u * 1024u);
    store.set_retention_budget(4u * 1024u, 1024u);
    REQUIRE(store.retained_bytes() <= 4u * 1024u);
    REQUIRE(store.evicted_count() == 12);
  }

  SECTION("a consumer that reclaims keeps the store empty and evicts nothing") {
    // The reclaim path: remove() after copying the bytes out. Nothing is ever
    // lost to eviction, and the bookkeeping the eviction order needs does not
    // itself become the unbounded thing.
    store.set_retention_budget(8u * 1024u, 1024u);
    for (int i = 0; i < 4096; ++i) {
      const sonare::midi::SysExHandle h = store.add(chunk);
      REQUIRE(h != 0);
      REQUIRE(store.remove(h));
    }
    REQUIRE(store.size() == 0);
    REQUIRE(store.retained_bytes() == 0);
    REQUIRE(store.evicted_count() == 0);
  }

  SECTION("the entry cap bounds memory that the byte cap alone does not") {
    // 48 MiB of one-byte payloads is 48 million map nodes: the byte budget is
    // satisfied while the per-node overhead is orders of magnitude larger than
    // everything it accounts for. The entry cap is what actually bounds memory.
    sonare::midi::SysExStore tiny;
    tiny.set_retention_budget(1024u * 1024u, 32u);
    const std::vector<uint8_t> one_byte(1u, 0x01u);
    for (int i = 0; i < 4096; ++i) {
      REQUIRE(tiny.add(one_byte) != 0);
    }
    REQUIRE(tiny.size() == 32);
    REQUIRE(tiny.retained_bytes() == 32u);
    REQUIRE(tiny.evicted_count() == 4064);
  }

  SECTION("re-committing a handle replaces its bytes without double-counting") {
    store.set_retention_budget(8u * 1024u, 1024u);
    const sonare::midi::SysExHandle handle = 42;
    REQUIRE(store.add_with_handle(handle, chunk.data(), chunk.size()));
    REQUIRE(store.retained_bytes() == 1024u);
    const std::vector<uint8_t> shorter(16u, 0x01u);
    REQUIRE(store.add_with_handle(handle, shorter.data(), shorter.size()));
    REQUIRE(store.size() == 1);
    REQUIRE(store.retained_bytes() == 16u);
    REQUIRE(store.evicted_count() == 0);
  }
}

namespace {
// Decode one SysEx7 (MT=0x3) UMP data message back to its payload bytes, the
// inverse of sysex7_payload_to_umps. Mirrors the SMF2 importer's reader.
uint8_t sysex7_status(const Ump& u) { return static_cast<uint8_t>((u.words[0] >> 20) & 0x0Fu); }
void append_sysex7(const Ump& u, std::vector<uint8_t>* out) {
  const uint8_t num = static_cast<uint8_t>((u.words[0] >> 16) & 0x0Fu);
  const uint8_t bytes[6] = {static_cast<uint8_t>((u.words[0] >> 8) & 0xFFu),
                            static_cast<uint8_t>(u.words[0] & 0xFFu),
                            static_cast<uint8_t>((u.words[1] >> 24) & 0xFFu),
                            static_cast<uint8_t>((u.words[1] >> 16) & 0xFFu),
                            static_cast<uint8_t>((u.words[1] >> 8) & 0xFFu),
                            static_cast<uint8_t>(u.words[1] & 0xFFu)};
  for (uint8_t i = 0; i < num && i < 6; ++i) out->push_back(bytes[i]);
}
}  // namespace

TEST_CASE("SysEx7 payload packetizes into UMP data messages and round-trips", "[midi]") {
  // Short payload (<= 6 data bytes) => a single Complete packet.
  {
    const std::vector<uint8_t> payload = {0x7Eu, 0x7Fu, 0x09u, 0x01u};  // 4 bytes, all 7-bit
    std::array<Ump, 8> out{};
    const size_t count = sonare::midi::sysex7_payload_to_umps(payload.data(), payload.size(),
                                                              /*group=*/5, out.data(), out.size());
    REQUIRE(count == 1);
    REQUIRE(out[0].message_type() == UmpMessageType::kData64);
    REQUIRE(out[0].group == 5);
    REQUIRE(sysex7_status(out[0]) == 0x0u);  // Complete
    std::vector<uint8_t> decoded;
    append_sysex7(out[0], &decoded);
    REQUIRE(decoded == payload);
  }

  // Multi-packet payload (13 bytes => 6 + 6 + 1) => Start / Continue / End.
  {
    std::vector<uint8_t> payload;
    for (uint8_t i = 0; i < 13; ++i) payload.push_back(static_cast<uint8_t>(i + 1));  // 7-bit
    std::array<Ump, 8> out{};
    const size_t count = sonare::midi::sysex7_payload_to_umps(payload.data(), payload.size(),
                                                              /*group=*/0, out.data(), out.size());
    REQUIRE(count == 3);
    REQUIRE(sysex7_status(out[0]) == 0x1u);  // Start
    REQUIRE(sysex7_status(out[1]) == 0x2u);  // Continue
    REQUIRE(sysex7_status(out[2]) == 0x3u);  // End
    std::vector<uint8_t> decoded;
    for (size_t k = 0; k < count; ++k) append_sysex7(out[k], &decoded);
    REQUIRE(decoded == payload);
  }

  // A leading 0xF0 / trailing 0xF7 frame is stripped before packetizing.
  {
    const std::vector<uint8_t> framed = {0xF0u, 0x41u, 0x10u, 0x42u, 0xF7u};
    const std::vector<uint8_t> inner = {0x41u, 0x10u, 0x42u};
    std::array<Ump, 8> out{};
    const size_t count = sonare::midi::sysex7_payload_to_umps(framed.data(), framed.size(),
                                                              /*group=*/1, out.data(), out.size());
    REQUIRE(count == 1);
    std::vector<uint8_t> decoded;
    append_sysex7(out[0], &decoded);
    REQUIRE(decoded == inner);
  }
}

TEST_CASE("SysEx7 packetizer rejects empty, 8-bit, and over-capacity payloads", "[midi]") {
  std::array<Ump, 8> out{};
  // Empty payload (also empty after stripping F0/F7) => 0.
  REQUIRE(sonare::midi::sysex7_payload_to_umps(nullptr, 0, 0, out.data(), out.size()) == 0);
  const std::vector<uint8_t> framed_only = {0xF0u, 0xF7u};
  REQUIRE(sonare::midi::sysex7_payload_to_umps(framed_only.data(), framed_only.size(), 0,
                                               out.data(), out.size()) == 0);
  // An 8-bit data byte is not valid SysEx7 (needs SysEx8) => 0.
  const std::vector<uint8_t> eight_bit = {0x41u, 0x80u, 0x42u};
  REQUIRE(sonare::midi::sysex7_payload_to_umps(eight_bit.data(), eight_bit.size(), 0, out.data(),
                                               out.size()) == 0);
  // Buffer too small for the required packet count => 0 (no partial SysEx).
  std::vector<uint8_t> big(13, 0x01u);
  std::array<Ump, 2> tiny{};  // needs 3 packets, only 2 fit
  REQUIRE(sonare::midi::sysex7_payload_to_umps(big.data(), big.size(), 0, tiny.data(),
                                               tiny.size()) == 0);
}

TEST_CASE("stateful SysEx7 packetizer streams beyond the 1536-byte backend boundary", "[midi]") {
  std::array<Ump, 256> scratch{};

  std::vector<uint8_t> boundary(1536);
  for (size_t i = 0; i < boundary.size(); ++i) boundary[i] = static_cast<uint8_t>(i % 0x80u);
  REQUIRE(sonare::midi::sysex7_payload_to_umps(boundary.data(), boundary.size(), 2, scratch.data(),
                                               scratch.size()) == 256);

  std::vector<uint8_t> payload(4097);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>((i + 1u) % 0x80u);
  REQUIRE(sonare::midi::sysex7_payload_to_umps(payload.data(), payload.size(), 2, scratch.data(),
                                               scratch.size()) == 0);

  sonare::midi::SysEx7Packetizer packetizer(payload.data(), payload.size(), /*group=*/2);
  REQUIRE(packetizer.valid());
  REQUIRE(packetizer.packet_count() == 683);
  std::vector<uint8_t> decoded;
  size_t batches = 0;
  size_t last_batch_count = 0;
  while (packetizer.remaining_packets() > 0) {
    const size_t count = packetizer.read(scratch.data(), scratch.size());
    REQUIRE(count > 0);
    for (size_t i = 0; i < count; ++i) append_sysex7(scratch[i], &decoded);
    last_batch_count = count;
    ++batches;
  }
  REQUIRE(batches == 3);
  REQUIRE(decoded == payload);
  REQUIRE(last_batch_count > 0);
  REQUIRE(sysex7_status(scratch[last_batch_count - 1u]) == 0x3u);

  REQUIRE(packetizer.seek_packet(256));
  REQUIRE(packetizer.packet_position() == 256);
  REQUIRE(packetizer.read(scratch.data(), 1) == 1);
  REQUIRE(sysex7_status(scratch[0]) == 0x2u);
  REQUIRE_FALSE(packetizer.seek_packet(packetizer.packet_count() + 1u));
}

TEST_CASE("MIDI 1.0 byte-stream <-> UMP adapter round-trips channel-voice", "[midi]") {
  uint8_t running = 0;
  // Note-on, channel 5, note 60, velocity 100.
  const uint8_t bytes[] = {0x95u, 60u, 100u};
  Ump out;
  const size_t consumed =
      sonare::midi::midi1_bytes_to_ump(bytes, sizeof(bytes), /*group=*/0, &running, &out);
  REQUIRE(consumed == 3);
  REQUIRE(out.message_type() == UmpMessageType::kMidi1ChannelVoice);
  REQUIRE(out.is_note_on());
  REQUIRE(out.channel() == 5);
  REQUIRE(out.note_number() == 60);
  REQUIRE((out.words[0] & 0x7Fu) == 100u);
  REQUIRE(running == 0x95u);

  uint8_t serialized[3] = {0, 0, 0};
  const size_t written = sonare::midi::ump_to_midi1_bytes(out, serialized, sizeof(serialized));
  REQUIRE(written == 3);
  REQUIRE(serialized[0] == bytes[0]);
  REQUIRE(serialized[1] == bytes[1]);
  REQUIRE(serialized[2] == bytes[2]);

  // Running status: a second note-on with no leading status byte reuses 0x95.
  const uint8_t running_bytes[] = {62u, 90u};
  Ump out2;
  const size_t consumed2 =
      sonare::midi::midi1_bytes_to_ump(running_bytes, sizeof(running_bytes), 0, &running, &out2);
  REQUIRE(consumed2 == 2);
  REQUIRE(out2.is_note_on());
  REQUIRE(out2.channel() == 5);
  REQUIRE(out2.note_number() == 62);

  // Program change is a one-data-byte message.
  uint8_t pc_running = 0;
  const uint8_t pc_bytes[] = {0xC3u, 42u};
  Ump pc_out;
  REQUIRE(sonare::midi::midi1_bytes_to_ump(pc_bytes, sizeof(pc_bytes), 0, &pc_running, &pc_out) ==
          2);
  REQUIRE(pc_out.status_nibble() == static_cast<uint8_t>(UmpStatus::kProgramChange));
  uint8_t pc_serialized[3] = {0, 0, 0};
  REQUIRE(sonare::midi::ump_to_midi1_bytes(pc_out, pc_serialized, sizeof(pc_serialized)) == 2);
  REQUIRE(pc_serialized[0] == 0xC3u);
  REQUIRE(pc_serialized[1] == 42u);

  // Channel pressure is also a one-data-byte message.
  uint8_t cp_running = 0;
  const uint8_t cp_bytes[] = {0xD2u, 99u};
  Ump cp_out;
  REQUIRE(sonare::midi::midi1_bytes_to_ump(cp_bytes, sizeof(cp_bytes), 0, &cp_running, &cp_out) ==
          2);
  REQUIRE(cp_out.status_nibble() == static_cast<uint8_t>(UmpStatus::kChannelPressure));
  uint8_t cp_serialized[3] = {0, 0, 0};
  REQUIRE(sonare::midi::ump_to_midi1_bytes(cp_out, cp_serialized, sizeof(cp_serialized)) == 2);
  REQUIRE(cp_serialized[0] == 0xD2u);
  REQUIRE(cp_serialized[1] == 99u);

  // Pitch bend preserves 14-bit LSB/MSB ordering.
  uint8_t bend_running = 0;
  const uint8_t bend_bytes[] = {0xE1u, 0x34u, 0x12u};
  Ump bend_out;
  REQUIRE(sonare::midi::midi1_bytes_to_ump(bend_bytes, sizeof(bend_bytes), 0, &bend_running,
                                           &bend_out) == 3);
  REQUIRE(bend_out.status_nibble() == static_cast<uint8_t>(UmpStatus::kPitchBend));
  uint8_t bend_serialized[3] = {0, 0, 0};
  REQUIRE(sonare::midi::ump_to_midi1_bytes(bend_out, bend_serialized, sizeof(bend_serialized)) ==
          3);
  REQUIRE(bend_serialized[0] == 0xE1u);
  REQUIRE(bend_serialized[1] == 0x34u);
  REQUIRE(bend_serialized[2] == 0x12u);
}

TEST_CASE("MIDI 1.0 byte adapter rejects system messages and incomplete buffers", "[midi]") {
  uint8_t running = 0;
  Ump out;
  const uint8_t sysex_start[] = {0xF0u, 0x7Eu};
  REQUIRE(sonare::midi::midi1_bytes_to_ump(sysex_start, sizeof(sysex_start), 0, &running, &out) ==
          0);
  // Incomplete note-on (missing velocity byte).
  const uint8_t partial[] = {0x90u, 60u};
  REQUIRE(sonare::midi::midi1_bytes_to_ump(partial, sizeof(partial), 0, &running, &out) == 0);
}

TEST_CASE("MIDI 1.0 -> 2.0 -> 1.0 channel-voice round-trips losslessly (top bits)", "[midi]") {
  const Ump m1 = sonare::midi::make_midi1_note_on(0, 0, 60, /*velocity7=*/100);
  const Ump m2 = sonare::midi::midi1_to_midi2(m1);
  REQUIRE(m2.message_type() == UmpMessageType::kMidi2ChannelVoice);
  const Ump back = sonare::midi::midi2_to_midi1(m2);
  REQUIRE(back.message_type() == UmpMessageType::kMidi1ChannelVoice);
  REQUIRE(back.is_note_on());
  REQUIRE(back.channel() == 0);
  REQUIRE(back.note_number() == 60);
  REQUIRE((back.words[0] & 0x7Fu) == 100u);  // velocity survives the round-trip.

  // Control change round-trip (top 7 CC bits survive).
  const Ump cc1 = sonare::midi::make_midi1_control_change(1, 3, 7, /*value7=*/64);
  const Ump cc_back = sonare::midi::midi2_to_midi1(sonare::midi::midi1_to_midi2(cc1));
  REQUIRE(cc_back.channel() == 3);
  REQUIRE(((cc_back.words[0] >> 8) & 0x7Fu) == 7u);
  REQUIRE((cc_back.words[0] & 0x7Fu) == 64u);

  // Program change round-trip.
  const Ump pc1 = sonare::midi::make_midi1_program_change(0, 9, 42);
  const Ump pc_back = sonare::midi::midi2_to_midi1(sonare::midi::midi1_to_midi2(pc1));
  REQUIRE(pc_back.status_nibble() == static_cast<uint8_t>(UmpStatus::kProgramChange));
  REQUIRE(pc_back.channel() == 9);
  REQUIRE(((pc_back.words[0] >> 8) & 0x7Fu) == 42u);

  const Ump poly1 = sonare::midi::make_midi1_poly_pressure(0, 4, 61, 77);
  const Ump poly_back = sonare::midi::midi2_to_midi1(sonare::midi::midi1_to_midi2(poly1));
  REQUIRE(poly_back.status_nibble() == static_cast<uint8_t>(UmpStatus::kPolyPressure));
  REQUIRE(poly_back.channel() == 4);
  REQUIRE(poly_back.note_number() == 61);
  REQUIRE((poly_back.words[0] & 0x7Fu) == 77u);

  const Ump pressure1 = sonare::midi::make_midi1_channel_pressure(0, 4, 88);
  const Ump pressure_back = sonare::midi::midi2_to_midi1(sonare::midi::midi1_to_midi2(pressure1));
  REQUIRE(pressure_back.status_nibble() == static_cast<uint8_t>(UmpStatus::kChannelPressure));
  REQUIRE(((pressure_back.words[0] >> 8) & 0x7Fu) == 88u);

  const Ump bend1 = sonare::midi::make_midi1_pitch_bend(0, 4, 0x1234u);
  const Ump bend_back = sonare::midi::midi2_to_midi1(sonare::midi::midi1_to_midi2(bend1));
  REQUIRE(bend_back.status_nibble() == static_cast<uint8_t>(UmpStatus::kPitchBend));
  REQUIRE(((bend_back.words[0] >> 8) & 0x7Fu) == 0x34u);
  REQUIRE((bend_back.words[0] & 0x7Fu) == 0x24u);
}

TEST_CASE("MIDI 1.0 -> 2.0 velocity/CC up-scale is min-center-max", "[midi]") {
  // M2-115-U 3.3: shift up to the center, bit-repeat above it.
  REQUIRE(sonare::midi::scale_velocity_7_to_16(0) == 0u);
  REQUIRE(sonare::midi::scale_velocity_7_to_16(1) == 0x0200u);
  REQUIRE(sonare::midi::scale_velocity_7_to_16(10) == 0x1400u);
  REQUIRE(sonare::midi::scale_velocity_7_to_16(64) == 0x8000u);
  REQUIRE(sonare::midi::scale_velocity_7_to_16(87) == 0xAEBAu);
  REQUIRE(sonare::midi::scale_velocity_7_to_16(127) == 0xFFFFu);

  REQUIRE(sonare::midi::scale_cc_7_to_32(0) == 0u);
  REQUIRE(sonare::midi::scale_cc_7_to_32(64) == 0x80000000u);
  REQUIRE(sonare::midi::scale_cc_7_to_32(127) == 0xFFFFFFFFu);

  // The note-on path embeds the up-scaled 16-bit velocity in word[1].
  const Ump m2 = sonare::midi::midi1_to_midi2(sonare::midi::make_midi1_note_on(0, 0, 60, 127));
  REQUIRE(static_cast<uint16_t>(m2.words[1] >> 16) == 65535u);
  const Ump cc2 =
      sonare::midi::midi1_to_midi2(sonare::midi::make_midi1_control_change(0, 0, 7, 127));
  REQUIRE(cc2.words[1] == 0xFFFFFFFFu);
  const Ump cc64 =
      sonare::midi::midi1_to_midi2(sonare::midi::make_midi1_control_change(0, 0, 7, 64));
  REQUIRE(cc64.words[1] == 0x80000000u);
}

TEST_CASE("min-center-max up-scale then truncating down-scale returns every source value",
          "[midi]") {
  for (uint32_t v = 0; v < 128u; ++v) {
    INFO("7-bit value " << v);
    const uint8_t v7 = static_cast<uint8_t>(v);
    REQUIRE(sonare::midi::scale_velocity_16_to_7(sonare::midi::scale_velocity_7_to_16(v7)) == v7);
    REQUIRE(sonare::midi::scale_cc_32_to_7(sonare::midi::scale_cc_7_to_32(v7)) == v7);
  }
  for (uint32_t v = 0; v < 0x4000u; ++v) {
    INFO("14-bit value " << v);
    const uint16_t v14 = static_cast<uint16_t>(v);
    const uint32_t up = sonare::midi::scale_cc_14_to_32(v14);
    REQUIRE(sonare::midi::scale_cc_32_to_14(up) == v14);
    // Same family as pitch bend.
    REQUIRE(up == sonare::midi::scale_bend_14_to_32(v14));
  }
  REQUIRE(sonare::midi::scale_cc_14_to_32(0x2000u) == 0x80000000u);
  REQUIRE(sonare::midi::scale_cc_14_to_32(0x3FFFu) == 0xFFFFFFFFu);
  REQUIRE(sonare::midi::scale_cc_14_to_32(0u) == 0u);
}

TEST_CASE("MIDI 1.0 note-on velocity zero translates to note-off velocity 0x8000", "[midi]") {
  const Ump off = sonare::midi::midi1_to_midi2(sonare::midi::make_midi1_note_on(3, 7, 61, 0));
  REQUIRE(off.message_type() == UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(off.status_nibble() == static_cast<uint8_t>(UmpStatus::kNoteOff));
  REQUIRE(off.group == 3);
  REQUIRE(off.channel() == 7);
  REQUIRE(off.note_number() == 61);
  REQUIRE(off.words[0] == sonare::midi::make_midi2_note_off(3, 7, 61, 0x8000u).words[0]);
  REQUIRE(off.words[1] == 0x80000000u);   // velocity 0x8000, attribute data 0.
  REQUIRE((off.words[0] & 0xFFu) == 0u);  // attribute type 0.

  // Velocity 1 stays a note-on at 0x0200 with no attribute.
  const Ump on = sonare::midi::midi1_to_midi2(sonare::midi::make_midi1_note_on(0, 0, 60, 1));
  REQUIRE(on.status_nibble() == static_cast<uint8_t>(UmpStatus::kNoteOn));
  REQUIRE(on.words[1] == 0x02000000u);
  REQUIRE((on.words[0] & 0xFFu) == 0u);

  // A real MIDI 1.0 note-off scales its release velocity like any velocity.
  const Ump real_off =
      sonare::midi::midi1_to_midi2(sonare::midi::make_midi1_note_off(0, 0, 60, 64));
  REQUIRE(real_off.status_nibble() == static_cast<uint8_t>(UmpStatus::kNoteOff));
  REQUIRE(real_off.words[1] == 0x80000000u);
}

TEST_CASE("MIDI 2.0 registered controller lowers by truncating to 14 bits", "[midi]") {
  // 32-bit 0x80000000 is the center: MSB 0x40, LSB 0x00.
  const auto center = sonare::midi::midi2_to_midi1_messages(
      sonare::midi::make_midi2_registered_controller(0, 0, 0, 0, 0x80000000u));
  REQUIRE(center.count == 4);
  REQUIRE(center.messages[2].data2_7bit() == 0x40u);
  REQUIRE(center.messages[3].data2_7bit() == 0x00u);
  // Just under the next 14-bit step: truncation, not rounding, so it stays put.
  const auto low = sonare::midi::midi2_to_midi1_messages(
      sonare::midi::make_midi2_registered_controller(0, 0, 0, 0, 0x8003FFFFu));
  REQUIRE(low.messages[2].data2_7bit() == 0x40u);
  REQUIRE(low.messages[3].data2_7bit() == 0x00u);
}

TEST_CASE("MIDI 2.0 -> 1.0 velocity/CC down-scale is the top-7-bit truncation", "[midi]") {
  // Down-scale takes the top 7 bits (>> 9 for velocity, >> 25 for CC). LOSSY in
  // the low bits: a 2.0 velocity whose low 9 bits are non-zero loses them.
  REQUIRE(sonare::midi::scale_velocity_16_to_7(0x8000u) == 64u);
  REQUIRE(sonare::midi::scale_velocity_16_to_7(65535u) == 127u);
  REQUIRE(sonare::midi::scale_velocity_16_to_7(0x81FFu) == 64u);  // low 9 bits dropped.

  const Ump quiet_on = sonare::midi::midi2_to_midi1(sonare::midi::make_midi2_note_on(0, 0, 60, 1));
  REQUIRE(quiet_on.is_note_on());
  REQUIRE((quiet_on.words[0] & 0x7Fu) == 1u);
  // D.2.1 reserves MIDI 1.0 velocity zero for note-off, so every MIDI 2.0
  // note-on whose down-scaled velocity is zero clamps to the quietest on value.
  for (const uint16_t velocity16 : {0u, 1u, 0x01FFu, 0x0200u}) {
    INFO("note-on velocity16 " << velocity16);
    REQUIRE(sonare::midi::scale_note_on_velocity_16_to_7(velocity16) == 1u);
    const Ump lowered =
        sonare::midi::midi2_to_midi1(sonare::midi::make_midi2_note_on(0, 0, 60, velocity16));
    REQUIRE(status_of(lowered) == static_cast<uint8_t>(UmpStatus::kNoteOn));
    REQUIRE(lowered.data2_7bit() == 1u);
    REQUIRE(lowered.is_note_on());
  }
  const Ump quiet_off =
      sonare::midi::midi2_to_midi1(sonare::midi::make_midi2_note_off(0, 0, 60, 1));
  REQUIRE(quiet_off.is_note_off());
  REQUIRE((quiet_off.words[0] & 0x7Fu) == 0u);
  const Ump silent_off =
      sonare::midi::midi2_to_midi1(sonare::midi::make_midi2_note_off(0, 0, 60, 0));
  REQUIRE(silent_off.is_note_off());
  REQUIRE(silent_off.data2_7bit() == 0u);

  REQUIRE(sonare::midi::scale_cc_32_to_7(0x80000000u) == 64u);
  REQUIRE(sonare::midi::scale_cc_32_to_7(0xFFFFFFFFu) == 127u);
  REQUIRE(sonare::midi::scale_cc_32_to_7(0x81FFFFFFu) == 64u);  // low 25 bits dropped.

  REQUIRE(sonare::midi::scale_bend_14_to_32(0) == 0u);
  REQUIRE(sonare::midi::scale_bend_14_to_32(8192) == 0x80000000u);
  REQUIRE(sonare::midi::scale_bend_32_to_14(0x80000000u) == 8192u);
  REQUIRE(sonare::midi::scale_bend_32_to_14(0xFFFFFFFFu) == 16383u);
}

TEST_CASE("MIDI 2.0 per-note controller down-converts to a dropped (empty) UMP", "[midi]") {
  const Ump pnc = sonare::midi::make_midi2_per_note_controller(0, 0, 60, 1, 0x1234u);
  const Ump dropped = sonare::midi::midi2_to_midi1(pnc);
  REQUIRE(dropped.word_count == 0);  // no MIDI 1.0 equivalent: caller drops it.

  REQUIRE(sonare::midi::midi2_to_midi1(
              sonare::midi::make_midi2_assignable_per_note_controller(0, 0, 60, 1, 0x1234u))
              .word_count == 0);
  REQUIRE(sonare::midi::midi2_to_midi1(
              sonare::midi::make_midi2_registered_controller(0, 0, 1, 2, 0x1234u))
              .word_count == 0);
  REQUIRE(sonare::midi::midi2_to_midi1(
              sonare::midi::make_midi2_assignable_controller(0, 0, 1, 2, 0x1234u))
              .word_count == 0);
}

TEST_CASE("MIDI 2.0 banked program change lowers to MIDI 1.0 bank select and program", "[midi]") {
  const Ump pc = sonare::midi::make_midi2_program_change(2, 5, /*program=*/42, /*bank_msb=*/0x79,
                                                         /*bank_lsb=*/3, /*bank_valid=*/true);
  const auto lowered = sonare::midi::midi2_to_midi1_messages(pc);

  REQUIRE(lowered.count == 3);
  REQUIRE(lowered.messages[0].status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange));
  REQUIRE(lowered.messages[0].channel() == 5);
  REQUIRE(((lowered.messages[0].words[0] >> 8) & 0x7Fu) == 0u);
  REQUIRE((lowered.messages[0].words[0] & 0x7Fu) == 0x79u);

  REQUIRE(lowered.messages[1].status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange));
  REQUIRE(((lowered.messages[1].words[0] >> 8) & 0x7Fu) == 32u);
  REQUIRE((lowered.messages[1].words[0] & 0x7Fu) == 3u);

  REQUIRE(lowered.messages[2].status_nibble() == static_cast<uint8_t>(UmpStatus::kProgramChange));
  REQUIRE(((lowered.messages[2].words[0] >> 8) & 0x7Fu) == 42u);
}

TEST_CASE("UMP word-count table covers every message-type nibble", "[midi]") {
  // The whole 16-entry mapping, spelled out independently of the implementation
  // so a hand-edit of the table has to disagree with this list to land. Reading
  // and writing a UMP stream both size messages from this one function, so a
  // wrong entry silently desynchronizes a writer from its own reader.
  constexpr uint8_t kExpected[16] = {
      1,  // 0x0 Utility
      1,  // 0x1 System real time / common
      1,  // 0x2 MIDI 1.0 channel voice
      2,  // 0x3 Data (SysEx7)
      2,  // 0x4 MIDI 2.0 channel voice
      4,  // 0x5 Data 128 (SysEx8 / Mixed Data Set)
      1,  // 0x6 reserved 32-bit
      1,  // 0x7 reserved 32-bit
      2,  // 0x8 reserved 64-bit
      2,  // 0x9 reserved 64-bit
      2,  // 0xA reserved 64-bit
      3,  // 0xB reserved 96-bit
      3,  // 0xC reserved 96-bit
      4,  // 0xD Flex Data
      4,  // 0xE reserved 128-bit
      4,  // 0xF UMP Stream
  };
  for (uint8_t mt = 0; mt < 16; ++mt) {
    INFO("message type 0x" << std::hex << static_cast<unsigned>(mt));
    REQUIRE(sonare::midi::ump_word_count_for_message_type(mt) == kExpected[mt]);
    // Only the low nibble is read, so a caller that forgets to mask gets the
    // same answer rather than falling into the 128-bit default.
    REQUIRE(sonare::midi::ump_word_count_for_message_type(static_cast<uint8_t>(mt | 0xF0u)) ==
            kExpected[mt]);
    const uint32_t word0 = static_cast<uint32_t>(mt) << 28;
    REQUIRE(sonare::midi::ump_word_count_for_word0(word0) == kExpected[mt]);
  }
}

TEST_CASE("UMP constructors agree with the shared word-count table", "[midi]") {
  // Every Ump the core mints must already satisfy the invariant that consumers
  // check, otherwise a producer-side word_count guard would reject our own
  // messages. SysEx handles are the case worth pinning: they are message type
  // 0x3, which is a 64-bit form even though no payload word is carried inline.
  const std::array<Ump, 6> minted = {
      sonare::midi::make_midi1_note_on(1, 2, 60, 100),
      sonare::midi::make_midi1_control_change(1, 2, 7, 64),
      sonare::midi::make_midi2_note_on(1, 2, 60, 30000),
      sonare::midi::make_midi2_program_change(1, 2, 42, 0, 3, true),
      sonare::midi::make_midi2_registered_controller(1, 2, 0, 5, 12345),
      sonare::midi::make_sysex_handle(1, 77),
  };
  for (const Ump& ump : minted) {
    INFO("word0 0x" << std::hex << ump.words[0]);
    REQUIRE(ump.word_count == sonare::midi::ump_word_count_for_word0(ump.words[0]));
  }
  REQUIRE(minted[5].message_type() == UmpMessageType::kData64);
  REQUIRE(minted[5].word_count == 2);
}

TEST_CASE("UMP group derivation returns 0 for the message types that have no group", "[midi]") {
  // Bits 24..27 are the group for every message type except two, and for those
  // two they are something else that is routinely non-zero. Deriving a group
  // from them would mint a group for a message that addresses the endpoint.
  //
  // Utility (0x0): the nibble is Reserved. A sender that leaves it dirty, or a
  // caller that packs a group into a Utility word0 believing it lands somewhere,
  // must not produce a grouped message.
  for (uint8_t nibble = 0; nibble <= 0x0F; ++nibble) {
    const uint32_t utility = (uint32_t{0x0} << 28) | (uint32_t{nibble} << 24) | 0x0000'1234u;
    INFO("utility word0 nibble 0x" << std::hex << static_cast<int>(nibble));
    REQUIRE(sonare::midi::ump_group_from_word0(utility) == 0);
  }

  // UMP Stream (0xF): bits 26..27 are `form` and bits 24..25 the top of the
  // 10-bit `status`, so the nibble is structural and set on ordinary traffic.
  // A Start packet (form 0b01) alone reads as group 4 without this rule.
  const uint32_t stream_start = (uint32_t{0xF} << 28) | (uint32_t{0b01} << 26);
  REQUIRE(((stream_start >> 24) & 0x0Fu) == 4u);
  REQUIRE(sonare::midi::ump_group_from_word0(stream_start) == 0);
  const uint32_t stream_end = (uint32_t{0xF} << 28) | (uint32_t{0b11} << 26);
  REQUIRE(((stream_end >> 24) & 0x0Fu) == 12u);
  REQUIRE(sonare::midi::ump_group_from_word0(stream_end) == 0);

  // Every other type still reads its group straight out of the nibble, so the
  // exemption cannot be mistaken for "the derivation stopped working".
  for (uint8_t mt = 0; mt <= 0x0F; ++mt) {
    if (mt == 0x0 || mt == 0x0F) continue;
    const uint32_t word0 = (uint32_t{mt} << 28) | (uint32_t{9} << 24);
    INFO("message type 0x" << std::hex << static_cast<int>(mt));
    REQUIRE(sonare::midi::ump_message_type_has_group(mt));
    REQUIRE(sonare::midi::ump_group_from_word0(word0) == 9);
  }
  REQUIRE_FALSE(sonare::midi::ump_message_type_has_group(0x0));
  REQUIRE_FALSE(sonare::midi::ump_message_type_has_group(0x0F));
}

TEST_CASE("MIDI 2.0 registered and assignable controllers lower to RPN / NRPN data entry",
          "[midi]") {
  // Data MSB is bits 31..25 and LSB bits 24..18 of the 32-bit value.
  const uint32_t value = (uint32_t{0x45} << 25) | (uint32_t{0x12} << 18) | 0x3FFFFu;
  const auto check = [&](const Ump& ump, uint8_t msb_cc, uint8_t lsb_cc) {
    const auto lowered = sonare::midi::midi2_to_midi1_messages(ump);
    REQUIRE(lowered.count == 4);
    const uint8_t expected[4][2] = {{msb_cc, 9}, {lsb_cc, 17}, {6, 0x45}, {38, 0x12}};
    for (uint8_t i = 0; i < 4; ++i) {
      INFO("message " << int{i});
      const Ump& m = lowered.messages[i];
      REQUIRE(m.message_type() == UmpMessageType::kMidi1ChannelVoice);
      REQUIRE(m.status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange));
      REQUIRE(m.group == 2);
      REQUIRE(m.channel() == 5);
      REQUIRE(m.note_number() == expected[i][0]);
      REQUIRE(m.data2_7bit() == expected[i][1]);
    }
  };
  check(sonare::midi::make_midi2_registered_controller(2, 5, 9, 17, value), 101, 100);
  check(sonare::midi::make_midi2_assignable_controller(2, 5, 9, 17, value), 99, 98);
  // Per-note controllers have no MIDI 1.0 form and still lower to nothing.
  REQUIRE(sonare::midi::midi2_to_midi1_messages(
              sonare::midi::make_midi2_per_note_controller(2, 5, 60, 1, value))
              .count == 0);
}

TEST_CASE("UMP message type and status enums cover the MIDI 2.0 additions", "[midi]") {
  REQUIRE(static_cast<uint8_t>(UmpMessageType::kFlexData) == 0xD);
  REQUIRE(static_cast<uint8_t>(UmpMessageType::kStream) == 0xF);
  REQUIRE(static_cast<uint8_t>(UmpStatus::kRelativeRegisteredController) == 0x4);
  REQUIRE(static_cast<uint8_t>(UmpStatus::kRelativeAssignableController) == 0x5);
  REQUIRE(static_cast<uint8_t>(UmpStatus::kPerNotePitchBend) == 0x6);
  REQUIRE(static_cast<uint8_t>(UmpStatus::kPerNoteManagement) == 0xF);
}

TEST_CASE("MIDI 2.0 relative controller, per-note bend and management builders lay out fields",
          "[midi]") {
  // Relative controllers carry a 32-bit two's-complement delta.
  const Ump rrc = sonare::midi::make_midi2_relative_registered_controller(
      1, 2, /*bank=*/0, /*index=*/3, static_cast<uint32_t>(int32_t{-5}));
  REQUIRE(rrc.words[0] == 0x41420003u);
  REQUIRE(rrc.words[1] == 0xFFFFFFFBu);
  REQUIRE(rrc.word_count == 2);
  REQUIRE(rrc.group == 1);
  const Ump rac =
      sonare::midi::make_midi2_relative_assignable_controller(1, 2, /*bank=*/9, /*index=*/17, 7u);
  REQUIRE(rac.words[0] == 0x41520911u);
  REQUIRE(rac.words[1] == 7u);

  const Ump pnb = sonare::midi::make_midi2_per_note_pitch_bend(3, 4, 60, 0x80000000u);
  REQUIRE(pnb.words[0] == 0x4364'3C00u);
  REQUIRE(pnb.words[1] == 0x80000000u);
  REQUIRE(pnb.message_type() == UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(pnb.status_nibble() == static_cast<uint8_t>(UmpStatus::kPerNotePitchBend));

  // Per-Note Management: D is bit 1 and S is bit 0 of the flags byte.
  REQUIRE(sonare::midi::make_midi2_per_note_management(0, 5, 61, false, false).words[0] ==
          0x40F53D00u);
  REQUIRE(sonare::midi::make_midi2_per_note_management(0, 5, 61, false, true).words[0] ==
          0x40F53D01u);
  REQUIRE(sonare::midi::make_midi2_per_note_management(0, 5, 61, true, false).words[0] ==
          0x40F53D02u);
  const Ump pnm = sonare::midi::make_midi2_per_note_management(0, 5, 61, true, true);
  REQUIRE(pnm.words[0] == 0x40F53D03u);
  REQUIRE(pnm.words[1] == 0u);
  REQUIRE(pnm.word_count == 2);
}

TEST_CASE("MIDI 2.0 controller builders mask bank and index fields to 7 bits", "[midi]") {
  const std::array<Ump, 4> controllers = {
      sonare::midi::make_midi2_registered_controller(0, 0, 0xFF, 0xFF, 0),
      sonare::midi::make_midi2_assignable_controller(0, 0, 0xFF, 0xFF, 0),
      sonare::midi::make_midi2_relative_registered_controller(0, 0, 0xFF, 0xFF, 0),
      sonare::midi::make_midi2_relative_assignable_controller(0, 0, 0xFF, 0xFF, 0),
  };
  for (const Ump& controller : controllers) {
    CAPTURE(controller.words[0]);
    CHECK(((controller.words[0] >> 8u) & 0x7Fu) == 0x7Fu);
    CHECK((controller.words[0] & 0x7Fu) == 0x7Fu);
    CHECK((controller.words[0] & 0x8000u) == 0u);
    CHECK((controller.words[0] & 0x80u) == 0u);
  }
}

TEST_CASE("MIDI 2.0 note-on exposes its attribute type and data", "[midi]") {
  const Ump on = sonare::midi::make_midi2_note_on(0, 1, 60, 0x1234u, /*type=*/3, /*data=*/0xBEEFu);
  REQUIRE(sonare::midi::note_attribute_type(on) == 3u);
  REQUIRE(sonare::midi::note_attribute_data(on) == 0xBEEFu);
  REQUIRE(sonare::midi::note_attribute_type(sonare::midi::make_midi2_note_on(0, 1, 60, 1)) == 0u);
}

TEST_CASE("zero-extension is a plain left shift, selected by registered index 0..31", "[midi]") {
  // M2-115-U 4.3.2: 7-bit 127 -> 0xFE00, so a 14-bit RPN value shifts by 18.
  REQUIRE(sonare::midi::scale_rpn_14_to_32_zero_extend(0x2000u) == 0x80000000u);
  REQUIRE(sonare::midi::scale_rpn_14_to_32_zero_extend(0x3FFFu) == 0xFFFC0000u);
  REQUIRE(sonare::midi::scale_rpn_14_to_32_zero_extend(0u) == 0u);
  for (uint32_t v = 0; v < 0x4000u; ++v) {
    const uint32_t up = sonare::midi::scale_rpn_14_to_32_zero_extend(static_cast<uint16_t>(v));
    REQUIRE(up == (v << 18u));
    REQUIRE(sonare::midi::scale_cc_32_to_14(up) == v);
  }

  for (uint8_t index = 0; index < 32; ++index) {
    REQUIRE(sonare::midi::scale_data_entry_14_to_32(true, index, 0x3FFFu) == 0xFFFC0000u);
  }
  REQUIRE(sonare::midi::scale_data_entry_14_to_32(true, 32, 0x3FFFu) == 0xFFFFFFFFu);
  REQUIRE(sonare::midi::scale_data_entry_14_to_32(true, 127, 0x2001u) ==
          sonare::midi::scale_cc_14_to_32(0x2001u));
  // Assignable controllers always take min-center-max, whatever the index.
  REQUIRE(sonare::midi::scale_data_entry_14_to_32(false, 0, 0x3FFFu) == 0xFFFFFFFFu);
}

TEST_CASE("MIDI 2.0 messages with no MIDI 1.0 form are dropped on down-conversion", "[midi]") {
  const Ump dropped[] = {
      sonare::midi::make_midi2_relative_registered_controller(0, 0, 0, 0, 1u),
      sonare::midi::make_midi2_relative_assignable_controller(0, 0, 0, 0, 1u),
      sonare::midi::make_midi2_per_note_controller(0, 0, 60, 1, 1u),
      sonare::midi::make_midi2_assignable_per_note_controller(0, 0, 60, 1, 1u),
      sonare::midi::make_midi2_per_note_pitch_bend(0, 0, 60, 0x80000000u),
      sonare::midi::make_midi2_per_note_management(0, 0, 60, true, true),
  };
  for (const Ump& ump : dropped) {
    INFO("status " << int{ump.status_nibble()});
    REQUIRE(sonare::midi::midi2_to_midi1(ump).word_count == 0);
    REQUIRE(sonare::midi::midi2_to_midi1_messages(ump).count == 0);
  }
  // Registered controller 0/7 (per-note bend sensitivity) has no MIDI 1.0
  // function of its own but is still an RPN, so it is lowered like any other.
  REQUIRE(sonare::midi::midi2_to_midi1_messages(
              sonare::midi::make_midi2_registered_controller(0, 0, 0, 7, 0x10000000u))
              .count == 4);
}

namespace {

using sonare::midi::Midi1ToMidi2Translator;

std::vector<Ump> translate(Midi1ToMidi2Translator& t, const Ump& in) {
  const auto out = t.translate(in);
  return std::vector<Ump>(out.messages.begin(), out.messages.begin() + out.count);
}

std::vector<Ump> feed_cc(Midi1ToMidi2Translator& t, uint8_t channel, uint8_t cc, uint8_t value) {
  return translate(t, sonare::midi::make_midi1_control_change(0, channel, cc, value));
}

}  // namespace

TEST_CASE("translator latches bank select into the next program change", "[midi]") {
  Midi1ToMidi2Translator t;
  // A program change with no bank information keeps bank-valid clear.
  auto out = translate(t, sonare::midi::make_midi1_program_change(0, 3, 9));
  REQUIRE(out.size() == 1);
  REQUIRE(out[0] == sonare::midi::make_midi2_program_change(0, 3, 9, 0, 0, false));

  // Bank select CCs are consumed and only surface inside the program change.
  REQUIRE(feed_cc(t, 3, 0, 0x78).empty());
  REQUIRE(feed_cc(t, 3, 32, 2).empty());
  out = translate(t, sonare::midi::make_midi1_program_change(0, 3, 9));
  REQUIRE(out.size() == 1);
  REQUIRE(out[0] == sonare::midi::make_midi2_program_change(0, 3, 9, 0x78, 2, true));

  // Other channels are unaffected.
  out = translate(t, sonare::midi::make_midi1_program_change(0, 4, 9));
  REQUIRE(out[0] == sonare::midi::make_midi2_program_change(0, 4, 9, 0, 0, false));

  // The bank stays current for later program changes on the channel.
  out = translate(t, sonare::midi::make_midi1_program_change(0, 3, 10));
  REQUIRE(out[0] == sonare::midi::make_midi2_program_change(0, 3, 10, 0x78, 2, true));
}

TEST_CASE("translator assembles RPN data entry with zero-extension for indices 0..31", "[midi]") {
  Midi1ToMidi2Translator t;
  REQUIRE(feed_cc(t, 1, 101, 0).empty());
  REQUIRE(feed_cc(t, 1, 100, 0).empty());
  REQUIRE(feed_cc(t, 1, 6, 0x40).empty());
  auto out = feed_cc(t, 1, 38, 0);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0] == sonare::midi::make_midi2_registered_controller(0, 1, 0, 0, 0x80000000u));

  // Index 0..31 shifts (0x3FFF -> 0xFFFFC000); index 32.. is min-center-max.
  REQUIRE(feed_cc(t, 1, 100, 6).empty());
  REQUIRE(feed_cc(t, 1, 6, 0x7F).empty());
  out = feed_cc(t, 1, 38, 0x7F);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0] == sonare::midi::make_midi2_registered_controller(0, 1, 0, 6, 0xFFFC0000u));

  REQUIRE(feed_cc(t, 1, 100, 32).empty());
  REQUIRE(feed_cc(t, 1, 6, 0x7F).empty());
  out = feed_cc(t, 1, 38, 0x7F);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0] == sonare::midi::make_midi2_registered_controller(0, 1, 0, 32, 0xFFFFFFFFu));
}

TEST_CASE("translator assembles NRPN data entry with min-center-max", "[midi]") {
  Midi1ToMidi2Translator t;
  REQUIRE(feed_cc(t, 0, 99, 5).empty());
  REQUIRE(feed_cc(t, 0, 98, 9).empty());
  REQUIRE(feed_cc(t, 0, 6, 0x7F).empty());
  const auto out = feed_cc(t, 0, 38, 0x7F);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0] == sonare::midi::make_midi2_assignable_controller(0, 0, 5, 9, 0xFFFFFFFFu));
}

TEST_CASE("translator emits pending data entry when the next trigger arrives", "[midi]") {
  Midi1ToMidi2Translator t;
  REQUIRE(feed_cc(t, 0, 101, 0).empty());
  REQUIRE(feed_cc(t, 0, 100, 2).empty());
  REQUIRE(feed_cc(t, 0, 6, 0x10).empty());
  // A second CC 6 ends the first message (LSB not sent -> 0) and starts a new one.
  auto out = feed_cc(t, 0, 6, 0x20);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0] == sonare::midi::make_midi2_registered_controller(0, 0, 0, 2, 0x10u << 25));
  out = feed_cc(t, 0, 38, 0x01);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0] ==
          sonare::midi::make_midi2_registered_controller(
              0, 0, 0, 2, sonare::midi::scale_rpn_14_to_32_zero_extend((0x20u << 7) | 1u)));

  // A new selector ends a data-entry MSB that never got its LSB.
  REQUIRE(feed_cc(t, 0, 6, 0x30).empty());
  out = feed_cc(t, 0, 99, 1);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0] == sonare::midi::make_midi2_registered_controller(0, 0, 0, 2, 0x30u << 25));
  // The data entry does not leak into the new NRPN selection.
  REQUIRE(feed_cc(t, 0, 98, 1).empty());
  REQUIRE(feed_cc(t, 0, 38, 5).empty());
}

TEST_CASE("translator ignores data entry with no selector and the null function", "[midi]") {
  Midi1ToMidi2Translator t;
  REQUIRE(feed_cc(t, 0, 6, 0x40).empty());
  REQUIRE(feed_cc(t, 0, 38, 0x00).empty());

  REQUIRE(feed_cc(t, 0, 101, 0x7F).empty());
  REQUIRE(feed_cc(t, 0, 100, 0x7F).empty());
  REQUIRE(feed_cc(t, 0, 6, 0x40).empty());
  REQUIRE(feed_cc(t, 0, 38, 0x00).empty());
  // Selecting a real parameter again resumes emission.
  REQUIRE(feed_cc(t, 0, 100, 0).empty());
  REQUIRE(feed_cc(t, 0, 101, 0).empty());
  REQUIRE(feed_cc(t, 0, 6, 0x40).empty());
  REQUIRE(feed_cc(t, 0, 38, 0x00).size() == 1);
}

TEST_CASE("translator keeps CC 96 and 97 as control changes and applies stateless rules",
          "[midi]") {
  Midi1ToMidi2Translator t;
  for (uint8_t cc : {uint8_t{96}, uint8_t{97}}) {
    const auto out = feed_cc(t, 2, cc, 1);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0] ==
            sonare::midi::make_midi2_control_change(0, 2, cc, sonare::midi::scale_cc_7_to_32(1)));
  }
  auto out = feed_cc(t, 2, 64, 64);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].words[1] == 0x80000000u);

  out = translate(t, sonare::midi::make_midi1_note_on(0, 2, 60, 0));
  REQUIRE(out.size() == 1);
  REQUIRE(out[0] == sonare::midi::make_midi2_note_off(0, 2, 60, 0x8000u));

  // Messages that are not MIDI 1.0 channel voice pass through untouched.
  const Ump m2 = sonare::midi::make_midi2_note_on(0, 2, 60, 0x1234u);
  out = translate(t, m2);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0] == m2);
}
