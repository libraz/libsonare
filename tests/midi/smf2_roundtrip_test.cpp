/// @file smf2_roundtrip_test.cpp
/// @brief MIDI core: MIDI 2.0 Clip File (SMF2) import / export round-trip,
///        lossless MIDI 2.0 channel-voice preservation, Flex Data meta, and
///        malformed-input safety.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

#include "midi/midi_clip.h"
#include "midi/smf2.h"
#include "midi/ump.h"
#include "transport/tempo_map.h"

namespace {

using sonare::midi::export_clip_file;
using sonare::midi::import_clip_file;
using sonare::midi::MidiClip;
using sonare::midi::MidiClipEvent;
using sonare::midi::Smf2ExportOptions;
using sonare::midi::Smf2ImportResult;
using sonare::midi::Smf2Status;
using sonare::midi::SysExStore;
using sonare::midi::Ump;

MidiClipEvent ev(double ppq, const Ump& ump) {
  MidiClipEvent e;
  e.ppq = ppq;
  e.ump = ump;
  return e;
}

void push_word(std::vector<uint8_t>* bytes, uint32_t w) {
  bytes->push_back(static_cast<uint8_t>((w >> 24) & 0xFFu));
  bytes->push_back(static_cast<uint8_t>((w >> 16) & 0xFFu));
  bytes->push_back(static_cast<uint8_t>((w >> 8) & 0xFFu));
  bytes->push_back(static_cast<uint8_t>(w & 0xFFu));
}

uint32_t read_word(const std::vector<uint8_t>& bytes, size_t offset) {
  REQUIRE(offset + 4 <= bytes.size());
  return (static_cast<uint32_t>(bytes[offset]) << 24) |
         (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 8) | static_cast<uint32_t>(bytes[offset + 3]);
}

bool contains_message_type(const std::vector<uint8_t>& bytes, uint32_t message_type) {
  for (size_t offset = 8; offset + 4 <= bytes.size(); offset += 4) {
    if (((read_word(bytes, offset) >> 28) & 0x0Fu) == message_type) return true;
  }
  return false;
}

std::vector<uint8_t> smf2_header_with_dctpq() {
  std::vector<uint8_t> bytes = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P'};
  push_word(&bytes, (0x0u << 28) | (0x3u << 20) | 480u);
  return bytes;
}

}  // namespace

TEST_CASE("SMF2 export writes the SMF2CLIP file header", "[midi][smf2]") {
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  const auto result = export_clip_file(clip, {}, {}, Smf2ExportOptions{});
  REQUIRE(result.ok());
  REQUIRE(result.bytes.size() >= 8);
  const std::string header(result.bytes.begin(), result.bytes.begin() + 8);
  REQUIRE(header == "SMF2CLIP");
  // All content after the header must be a whole number of 32-bit UMP words.
  REQUIRE((result.bytes.size() - 8) % 4 == 0);
}

TEST_CASE("SMF2 round-trips a MIDI 2.0 note losslessly", "[midi][smf2]") {
  // A 16-bit velocity whose low bits would be lost through MIDI 1.0.
  const uint16_t velocity16 = 0xABCD;
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi2_note_on(0, 3, 64, velocity16)));
  clip.add_event(ev(1.0, sonare::midi::make_midi2_note_off(0, 3, 64, 0)));

  Smf2ExportOptions options;
  options.ticks_per_quarter = 480;
  const auto exported = export_clip_file(clip, {}, {}, options);
  REQUIRE(exported.ok());

  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.ticks_per_quarter == 480);
  REQUIRE(imported.clips.size() == 1);

  const auto& events = imported.clips[0].events();
  REQUIRE(events.size() == 2);

  // The note-on survives bit-for-bit (MT=0x4, two words, full 16-bit velocity).
  const Ump& on = events[0].ump.is_note_on() ? events[0].ump : events[1].ump;
  REQUIRE(on.message_type() == sonare::midi::UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(on.word_count == 2);
  REQUIRE(static_cast<uint16_t>(on.words[1] >> 16) == velocity16);
  REQUIRE(on.note_number() == 64);
  REQUIRE(on.channel() == 3);
}

TEST_CASE("SMF2 positions events by Delta Clockstamp ticks", "[midi][smf2]") {
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  clip.add_event(ev(2.5, sonare::midi::make_midi1_note_off(0, 0, 60, 0)));

  Smf2ExportOptions options;
  options.ticks_per_quarter = 960;
  const auto exported = export_clip_file(clip, {}, {}, options);
  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.size() == 1);
  const auto& events = imported.clips[0].events();
  REQUIRE(events.size() == 2);
  REQUIRE(events[0].ppq == Catch::Approx(0.0));
  REQUIRE(events[1].ppq == Catch::Approx(2.5));
  REQUIRE(imported.clip_lengths_ppq.front() == Catch::Approx(2.5));
}

TEST_CASE("SMF2 round-trips tempo and time signature via Flex Data", "[midi][smf2]") {
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 64)));
  clip.add_event(ev(4.0, sonare::midi::make_midi1_note_off(0, 0, 60, 0)));

  std::vector<sonare::transport::TempoSegment> tempos = {{0.0, 140.0, 0.0}};
  std::vector<sonare::transport::TimeSignatureSegment> sigs;
  sonare::transport::TimeSignatureSegment seg;
  seg.start_ppq = 0.0;
  seg.time_sig.numerator = 6;
  seg.time_sig.denominator = 8;
  seg.thirty_seconds_per_quarter = 0;
  sigs.push_back(seg);

  const auto exported = export_clip_file(clip, tempos, sigs, Smf2ExportOptions{});
  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());

  REQUIRE(imported.tempo_segments.size() >= 1);
  REQUIRE(imported.tempo_segments.front().bpm == Catch::Approx(140.0).margin(0.01));
  REQUIRE(imported.time_signatures.size() >= 1);
  REQUIRE(imported.time_signatures.front().time_sig.numerator == 6);
  REQUIRE(imported.time_signatures.front().time_sig.denominator == 8);
  REQUIRE(imported.time_signatures.front().thirty_seconds_per_quarter == 0);
}

TEST_CASE("SMF2 round-trips the clip name via Flex Data metadata", "[midi][smf2]") {
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));

  Smf2ExportOptions options;
  options.name = "Lead";
  const auto exported = export_clip_file(clip, {}, {}, options);
  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.clip_names.size() == 1);
  REQUIRE(imported.clip_names.front() == "Lead");
}

TEST_CASE("SMF2 round-trips a SysEx payload", "[midi][smf2]") {
  SysExStore store;
  const std::vector<uint8_t> payload = {0x7E, 0x00, 0x09, 0x01};
  const auto handle = store.add(payload);
  REQUIRE(handle != 0);

  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_sysex_handle(0, handle)));

  Smf2ExportOptions options;
  options.sysex_store = &store;
  const auto exported = export_clip_file(clip, {}, {}, options);
  REQUIRE(exported.skipped_events == 0);

  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.size() == 1);
  const auto& events = imported.clips[0].events();
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].ump.sysex_handle != 0);
  const std::vector<uint8_t>* recovered = imported.sysex_store.lookup(events[0].ump.sysex_handle);
  REQUIRE(recovered != nullptr);
  REQUIRE(*recovered == payload);
}

TEST_CASE("SMF2 chains Delta Clockstamps for events beyond the 20-bit tick span", "[midi][smf2]") {
  // At dctpq=480, 5000 quarter notes -> tick 2,400,000, which exceeds two full
  // 20-bit DCS spans (2 * 0xFFFFF = 2,097,150). A single DCS word would clamp /
  // collapse the event; chained DCS words must preserve the position.
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  clip.add_event(ev(5000.0, sonare::midi::make_midi1_note_off(0, 0, 60, 0)));

  Smf2ExportOptions options;
  options.ticks_per_quarter = 480;
  const auto exported = export_clip_file(clip, {}, {}, options);
  REQUIRE(exported.ok());

  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.size() == 1);
  const auto& events = imported.clips[0].events();
  REQUIRE(events.size() == 2);
  REQUIRE(events[0].ppq == Catch::Approx(0.0));
  REQUIRE(events[1].ppq == Catch::Approx(5000.0));
  REQUIRE(imported.clip_lengths_ppq.front() == Catch::Approx(5000.0));
}

TEST_CASE("SMF2 imports a SysEx8 data message payload", "[midi][smf2]") {
  // Hand-built SMF2CLIP: header + DCTPQ + a complete SysEx8 packet (MT=0x5).
  std::vector<uint8_t> bytes = smf2_header_with_dctpq();
  // SysEx8 complete (status 0x0), numBytes=4 (streamID + 3 data), streamID=0x00,
  // payload bytes {0x11, 0x22, 0x33}. word0 byte3 = data[0]=0x11.
  push_word(&bytes, (0x5u << 28) | (0x4u << 16) | (0x00u << 8) | 0x11u);
  push_word(&bytes, (0x22u << 24) | (0x33u << 16));
  push_word(&bytes, 0);
  push_word(&bytes, 0);

  const Smf2ImportResult imported = import_clip_file(bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.size() == 1);
  const auto& events = imported.clips[0].events();
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].ump.sysex_handle != 0);
  const std::vector<uint8_t>* recovered = imported.sysex_store.lookup(events[0].ump.sysex_handle);
  REQUIRE(recovered != nullptr);
  REQUIRE(*recovered == std::vector<uint8_t>{0x11, 0x22, 0x33});
}

TEST_CASE("SMF2 exports high-bit SysEx payloads as SysEx8", "[midi][smf2]") {
  SysExStore store;
  const std::vector<uint8_t> payload = {0x11, 0x80, 0x22, 0xFF, 0x33};
  const auto handle = store.add(payload);
  REQUIRE(handle != 0);

  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_sysex_handle(3, handle)));

  Smf2ExportOptions options;
  options.sysex_store = &store;
  const auto exported = export_clip_file(clip, {}, {}, options);
  REQUIRE(exported.skipped_events == 0);
  REQUIRE(contains_message_type(exported.bytes, 0x5u));

  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.size() == 1);
  const auto& events = imported.clips[0].events();
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].ump.group == 3);
  const std::vector<uint8_t>* recovered = imported.sysex_store.lookup(events[0].ump.sysex_handle);
  REQUIRE(recovered != nullptr);
  REQUIRE(*recovered == payload);
}

TEST_CASE("SMF2 import validates SysEx packet ordering", "[midi][smf2]") {
  SECTION("orphan continue is skipped") {
    std::vector<uint8_t> bytes = smf2_header_with_dctpq();
    push_word(&bytes, (0x3u << 28) | (0x2u << 20) | (0x2u << 16) | (0x11u << 8) | 0x22u);
    push_word(&bytes, 0);
    const Smf2ImportResult imported = import_clip_file(bytes);
    REQUIRE(imported.ok());
    REQUIRE(imported.skipped_events == 1);
    REQUIRE(imported.clips.empty());
  }

  SECTION("orphan end is skipped") {
    std::vector<uint8_t> bytes = smf2_header_with_dctpq();
    push_word(&bytes, (0x3u << 28) | (0x3u << 20) | (0x2u << 16) | (0x11u << 8) | 0x22u);
    push_word(&bytes, 0);
    const Smf2ImportResult imported = import_clip_file(bytes);
    REQUIRE(imported.ok());
    REQUIRE(imported.skipped_events == 1);
    REQUIRE(imported.clips.empty());
  }

  SECTION("unterminated start is skipped") {
    std::vector<uint8_t> bytes = smf2_header_with_dctpq();
    push_word(&bytes, (0x3u << 28) | (0x1u << 20) | (0x2u << 16) | (0x11u << 8) | 0x22u);
    push_word(&bytes, 0);
    const Smf2ImportResult imported = import_clip_file(bytes);
    REQUIRE(imported.ok());
    REQUIRE(imported.skipped_events == 1);
    REQUIRE(imported.clips.empty());
  }
}

TEST_CASE("SMF2 SysEx import and export preserve group", "[midi][smf2]") {
  SysExStore store;
  const auto handle = store.add(std::vector<uint8_t>{0x7E, 0x7F, 0x09, 0x01});
  REQUIRE(handle != 0);

  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_sysex_handle(5, handle)));
  Smf2ExportOptions options;
  options.sysex_store = &store;
  const auto exported = export_clip_file(clip, {}, {}, options);
  REQUIRE(exported.ok());
  REQUIRE(exported.skipped_events == 0);

  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.size() == 1);
  REQUIRE(imported.clips[0].events().size() == 1);
  REQUIRE(imported.clips[0].events()[0].ump.group == 5);
}

TEST_CASE("SMF2 export skips empty SysEx payloads instead of writing a dropped packet",
          "[midi][smf2]") {
  SysExStore store;
  const auto handle = store.add(std::vector<uint8_t>{0xF0, 0xF7});
  REQUIRE(handle != 0);

  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_sysex_handle(0, handle)));
  Smf2ExportOptions options;
  options.sysex_store = &store;
  const auto exported = export_clip_file(clip, {}, {}, options);
  REQUIRE(exported.ok());
  REQUIRE(exported.skipped_events == 1);

  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.empty());
}

TEST_CASE("SMF2 import rejects malformed input without reading out of bounds", "[midi][smf2]") {
  SECTION("empty buffer") {
    const Smf2ImportResult r = import_clip_file(nullptr, 0);
    REQUIRE(r.status == Smf2Status::kInvalidArgument);
  }
  SECTION("bad header") {
    const std::vector<uint8_t> bytes = {'N', 'O', 'T', 'S', 'M', 'F', '2', '!'};
    const Smf2ImportResult r = import_clip_file(bytes);
    REQUIRE(r.status == Smf2Status::kBadHeader);
  }
  SECTION("header only is a valid empty clip file") {
    const std::vector<uint8_t> bytes = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P'};
    const Smf2ImportResult r = import_clip_file(bytes);
    REQUIRE(r.ok());
    REQUIRE(r.clips.empty());
  }
  SECTION("truncated mid-word after header") {
    std::vector<uint8_t> bytes = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P', 0x00, 0x40};
    const Smf2ImportResult r = import_clip_file(bytes);
    REQUIRE(r.status == Smf2Status::kTruncated);
  }
  SECTION("truncated multi-word message") {
    // Header + a Flex Data message type (MT=0xD needs 4 words) with only 1 word.
    std::vector<uint8_t> bytes = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P', 0xD0, 0x10, 0x00, 0x00};
    const Smf2ImportResult r = import_clip_file(bytes);
    REQUIRE(r.status == Smf2Status::kTruncated);
  }
  SECTION("channel voice before DCTPQ is rejected transactionally") {
    std::vector<uint8_t> bytes = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P'};
    push_word(&bytes, sonare::midi::make_midi1_note_on(0, 0, 60, 100).words[0]);
    const Smf2ImportResult r = import_clip_file(bytes);
    REQUIRE(r.status == Smf2Status::kMissingDctpq);
    REQUIRE(r.clips.empty());
  }
}
