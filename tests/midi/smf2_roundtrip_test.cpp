/// @file smf2_roundtrip_test.cpp
/// @brief MIDI core: MIDI 2.0 Clip File (SMF2) import / export round-trip,
///        lossless MIDI 2.0 channel-voice preservation, Flex Data meta, and
///        malformed-input safety.

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

#include "midi/midi_clip.h"
#include "midi/smf2.h"
#include "midi/ump.h"
#include "transport/tempo_map.h"

TEST_CASE("SMF2 export rejects inconsistent UMP word counts", "[midi][smf2]") {
  for (const uint8_t count : {uint8_t{1}, uint8_t{5}}) {
    sonare::midi::MidiClip clip;
    sonare::midi::MidiClipEvent event;
    event.ump = sonare::midi::make_midi2_note_on(0, 0, 60, 50000);
    event.ump.word_count = count;
    clip.add_event(event);
    const auto result = sonare::midi::export_clip_file(clip, {}, {}, {});
    CHECK(result.status == sonare::midi::Smf2Status::kInvalidArgument);
    CHECK(result.bytes.empty());
  }
}

TEST_CASE("SMF2 started clips require an End of Clip marker", "[midi][smf2]") {
  sonare::midi::MidiClip clip;
  sonare::midi::MidiClipEvent event;
  event.ump = sonare::midi::make_midi1_note_on(0, 0, 60, 100);
  clip.add_event(event);
  auto complete = sonare::midi::export_clip_file(clip, {}, {}, {});
  REQUIRE(complete.ok());
  REQUIRE(complete.bytes.size() >= 24);
  // The final stream message is the four-word End of Clip.
  complete.bytes.resize(complete.bytes.size() - 16);
  const auto result = sonare::midi::import_clip_file(complete.bytes.data(), complete.bytes.size());
  CHECK(result.status == sonare::midi::Smf2Status::kTruncated);
  CHECK(result.clips.empty());
}

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

void push_sysex7_packet(std::vector<uint8_t>* bytes, uint8_t group, uint8_t status,
                        std::initializer_list<uint8_t> payload) {
  std::array<uint8_t, 6> data{};
  size_t count = 0;
  for (const uint8_t value : payload) {
    REQUIRE(count < data.size());
    data[count++] = value;
  }
  push_word(bytes, (0x3u << 28) | (static_cast<uint32_t>(group) << 24) |
                       (static_cast<uint32_t>(status) << 20) |
                       (static_cast<uint32_t>(count) << 16) |
                       (static_cast<uint32_t>(data[0]) << 8) | data[1]);
  push_word(bytes, (static_cast<uint32_t>(data[2]) << 24) | (static_cast<uint32_t>(data[3]) << 16) |
                       (static_cast<uint32_t>(data[4]) << 8) | data[5]);
}

void push_sysex8_packet(std::vector<uint8_t>* bytes, uint8_t group, uint8_t stream_id,
                        uint8_t status, std::initializer_list<uint8_t> payload) {
  std::array<uint8_t, 13> data{};
  size_t count = 0;
  for (const uint8_t value : payload) {
    REQUIRE(count < data.size());
    data[count++] = value;
  }
  REQUIRE(count + 1u <= 0x0Fu);
  push_word(bytes, (0x5u << 28) | (static_cast<uint32_t>(group) << 24) |
                       (static_cast<uint32_t>(status) << 20) |
                       (static_cast<uint32_t>(count + 1u) << 16) |
                       (static_cast<uint32_t>(stream_id) << 8) | data[0]);
  push_word(bytes, (static_cast<uint32_t>(data[1]) << 24) | (static_cast<uint32_t>(data[2]) << 16) |
                       (static_cast<uint32_t>(data[3]) << 8) | data[4]);
  push_word(bytes, (static_cast<uint32_t>(data[5]) << 24) | (static_cast<uint32_t>(data[6]) << 16) |
                       (static_cast<uint32_t>(data[7]) << 8) | data[8]);
  push_word(bytes, (static_cast<uint32_t>(data[9]) << 24) |
                       (static_cast<uint32_t>(data[10]) << 16) |
                       (static_cast<uint32_t>(data[11]) << 8) | data[12]);
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

TEST_CASE("SMF2 preserves the position of a non-zero first tempo/time-sig", "[midi][smf2]") {
  // A clip whose first tempo and time-signature changes do NOT start at tick 0
  // (e.g. they land at bar 2). The config header only holds the tick-0 initial
  // state, so these must round-trip at their real positions instead of being
  // collapsed to ppq 0.
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 64)));
  clip.add_event(ev(8.0, sonare::midi::make_midi1_note_off(0, 0, 60, 0)));

  std::vector<sonare::transport::TempoSegment> tempos = {{4.0, 150.0, 0.0}};
  sonare::transport::TimeSignatureSegment sig;
  sig.start_ppq = 4.0;
  sig.time_sig.numerator = 7;
  sig.time_sig.denominator = 8;
  sig.thirty_seconds_per_quarter = 0;
  std::vector<sonare::transport::TimeSignatureSegment> sigs = {sig};

  const auto exported = export_clip_file(clip, tempos, sigs, Smf2ExportOptions{});
  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());

  // Ahead of them the map carries the 120 BPM / 4/4 the file plays before its
  // first tempo and meter messages.
  REQUIRE(imported.tempo_segments.size() == 2);
  REQUIRE(imported.tempo_segments.front().start_ppq == 0.0);
  REQUIRE(imported.tempo_segments.front().bpm == Catch::Approx(120.0));
  REQUIRE(imported.tempo_segments[1].start_ppq == Catch::Approx(4.0).margin(0.01));
  REQUIRE(imported.tempo_segments[1].bpm == Catch::Approx(150.0).margin(0.01));
  REQUIRE(imported.time_signatures.size() == 2);
  REQUIRE(imported.time_signatures.front().start_ppq == 0.0);
  REQUIRE(imported.time_signatures.front().time_sig.numerator == 4);
  REQUIRE(imported.time_signatures[1].start_ppq == Catch::Approx(4.0).margin(0.01));
  REQUIRE(imported.time_signatures[1].time_sig.numerator == 7);
  REQUIRE(imported.time_signatures[1].time_sig.denominator == 8);
}

TEST_CASE("SMF2 export clamps very low BPM tempo instead of wrapping", "[midi][smf2]") {
  MidiClip clip;
  std::vector<sonare::transport::TempoSegment> tempos = {{0.0, 1.0, 0.0}, {1.0, 0.5, 0.0}};

  const auto exported = export_clip_file(clip, tempos, {}, Smf2ExportOptions{});
  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.tempo_segments.size() >= 2);

  const double min_representable_bpm =
      6.0e9 / static_cast<double>(std::numeric_limits<uint32_t>::max());
  REQUIRE(imported.tempo_segments[0].bpm == Catch::Approx(min_representable_bpm).epsilon(1e-9));
  REQUIRE(imported.tempo_segments[1].bpm == Catch::Approx(min_representable_bpm).epsilon(1e-9));
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

TEST_CASE("SMF2 round-trips a clip name longer than one Flex packet", "[midi][smf2]") {
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));

  // 20 characters spans two 12-byte Flex Data packets; a single-packet writer
  // would silently truncate to "Acoustic Gra".
  const std::string long_name = "Acoustic Grand Piano";
  Smf2ExportOptions options;
  options.name = long_name;
  const auto exported = export_clip_file(clip, {}, {}, options);
  REQUIRE(exported.skipped_events == 0);
  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.clip_names.size() == 1);
  REQUIRE(imported.clip_names.front() == long_name);
}

TEST_CASE("SMF2 round-trips a clip name that is an exact Flex-packet multiple", "[midi][smf2]") {
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));

  // Exactly 24 bytes: two full packets with no trailing NUL in the final one.
  const std::string name = "TwelveCharsABTwelveCharsA";  // 25 chars
  const std::string name24 = name.substr(0, 24);
  Smf2ExportOptions options;
  options.name = name24;
  const auto exported = export_clip_file(clip, {}, {}, options);
  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.clip_names.size() == 1);
  REQUIRE(imported.clip_names.front() == name24);
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

TEST_CASE("SMF2 export drops a SysEx whose payload it cannot resolve", "[midi][smf2]") {
  // Message type 0x3 is a 64-bit form, so writing one word for it would leave
  // the reader -- which sizes messages from the same shared table -- consuming
  // the following word as the missing half and losing its bearings for the rest
  // of the stream. The writer never gets the chance: a SysEx whose payload it
  // cannot resolve is counted and dropped before any word is emitted, so the
  // event's own word_count is never consulted. That guard is what keeps a
  // stale word_count on a SysEx record from corrupting a file, and it is worth
  // holding in place.
  MidiClip clip;
  Ump orphan = sonare::midi::make_sysex_handle(0, /*handle=*/7);
  orphan.word_count = 1;  // a short count, as an unmigrated rebuild would leave
  clip.add_event(ev(0.0, orphan));
  clip.add_event(ev(1.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  clip.add_event(ev(2.0, sonare::midi::make_midi1_note_off(0, 0, 60, 0)));

  Smf2ExportOptions options;  // no sysex_store: the handle stays unresolvable
  const auto exported = export_clip_file(clip, {}, {}, options);
  REQUIRE(exported.skipped_events == 1);

  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.size() == 1);
  const auto& events = imported.clips[0].events();
  // The note pair survives at its own timing, which a desynchronized reader
  // would have destroyed.
  REQUIRE(events.size() == 2);
  REQUIRE(events[0].ump.is_note_on());
  REQUIRE(events[0].ump.note_number() == 60);
  REQUIRE(events[1].ump.is_note_off());
  REQUIRE(events[1].ppq == Catch::Approx(2.0));
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

TEST_CASE("SMF2 keeps interleaved SysEx groups, types, and streams independent", "[midi][smf2]") {
  std::vector<uint8_t> bytes = smf2_header_with_dctpq();
  // A SysEx7 message on group 0 is interleaved with two SysEx8 streams on
  // group 1. The packet status alone is insufficient to identify a fragment:
  // type, group, and SysEx8 stream ID all belong to the pending-message key.
  push_sysex7_packet(&bytes, 0, 0x1, {0x10});
  push_sysex8_packet(&bytes, 1, 0x11, 0x1, {0xA0});
  push_sysex8_packet(&bytes, 1, 0x22, 0x1, {0xB0});
  push_sysex7_packet(&bytes, 0, 0x3, {0x11});
  push_sysex8_packet(&bytes, 1, 0x22, 0x3, {0xB1});
  push_sysex8_packet(&bytes, 1, 0x11, 0x3, {0xA1});

  const Smf2ImportResult imported = import_clip_file(bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.skipped_events == 0);
  REQUIRE(imported.clips.size() == 1);
  const auto& events = imported.clips[0].events();
  REQUIRE(events.size() == 3);

  const auto* first = imported.sysex_store.lookup(events[0].ump.sysex_handle);
  const auto* second = imported.sysex_store.lookup(events[1].ump.sysex_handle);
  const auto* third = imported.sysex_store.lookup(events[2].ump.sysex_handle);
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  REQUIRE(third != nullptr);
  CHECK(events[0].ump.group == 0);
  CHECK(*first == std::vector<uint8_t>{0x10, 0x11});
  CHECK(events[1].ump.group == 1);
  CHECK(*second == std::vector<uint8_t>{0xB0, 0xB1});
  CHECK(events[2].ump.group == 1);
  CHECK(*third == std::vector<uint8_t>{0xA0, 0xA1});
}

TEST_CASE("SMF2 counts unfinished interleaved SysEx fragments without corrupting valid data",
          "[midi][smf2]") {
  std::vector<uint8_t> bytes = smf2_header_with_dctpq();
  push_sysex7_packet(&bytes, 0, 0x1, {0x10});
  push_sysex8_packet(&bytes, 1, 0x33, 0x0, {0xA0});

  const Smf2ImportResult imported = import_clip_file(bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.skipped_events == 1);
  REQUIRE(imported.clips.size() == 1);
  const auto& events = imported.clips[0].events();
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].ump.group == 1);
  const auto* payload = imported.sysex_store.lookup(events[0].ump.sysex_handle);
  REQUIRE(payload != nullptr);
  CHECK(*payload == std::vector<uint8_t>{0xA0});
}

TEST_CASE("SMF2 keeps the same SysEx8 stream ID separate across groups", "[midi][smf2]") {
  std::vector<uint8_t> bytes = smf2_header_with_dctpq();
  push_sysex8_packet(&bytes, 0, 0x44, 0x1, {0x10});
  push_sysex8_packet(&bytes, 1, 0x44, 0x1, {0x20});
  push_sysex8_packet(&bytes, 0, 0x44, 0x3, {0x11});
  push_sysex8_packet(&bytes, 1, 0x44, 0x3, {0x21});

  const Smf2ImportResult imported = import_clip_file(bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.skipped_events == 0);
  REQUIRE(imported.clips.size() == 1);
  const auto& events = imported.clips[0].events();
  REQUIRE(events.size() == 2);
  REQUIRE(events[0].ump.group == 0);
  REQUIRE(events[1].ump.group == 1);
  const auto* group0 = imported.sysex_store.lookup(events[0].ump.sysex_handle);
  const auto* group1 = imported.sysex_store.lookup(events[1].ump.sysex_handle);
  REQUIRE(group0 != nullptr);
  REQUIRE(group1 != nullptr);
  CHECK(*group0 == std::vector<uint8_t>{0x10, 0x11});
  CHECK(*group1 == std::vector<uint8_t>{0x20, 0x21});
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
  SECTION("Flex Data before DCTPQ is rejected instead of timestamped at zero") {
    std::vector<uint8_t> bytes = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P'};
    push_word(&bytes, (0xDu << 28) | (0x1u << 16) | (0x01u << 8) | 0x00u);
    push_word(&bytes, 50000000u);
    push_word(&bytes, 0);
    push_word(&bytes, 0);
    const Smf2ImportResult r = import_clip_file(bytes);
    REQUIRE(r.status == Smf2Status::kMissingDctpq);
    REQUIRE(r.clips.empty());
  }
  SECTION("SysEx before DCTPQ is rejected instead of timestamped at zero") {
    std::vector<uint8_t> bytes = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P'};
    push_word(&bytes, (0x3u << 28) | (0x0u << 20) | (0x03u << 16) | 0xF0u);
    push_word(&bytes, 0x0102F700u);
    const Smf2ImportResult r = import_clip_file(bytes);
    REQUIRE(r.status == Smf2Status::kMissingDctpq);
    REQUIRE(r.clips.empty());
  }
}

// A Delta Clockstamp carries a 20-bit delta, so a larger gap becomes a chain of
// max-valued words. The gaps across a clip sum to its span, so the chain cost of
// a whole export is the SPAN divided by 0xFFFFF whatever the event count -- and
// a single event at the legal PPQ ceiling (1e12) asks for roughly 460 million
// words, about 1.8 GB, before anything else has been written.
TEST_CASE("export_clip_file bounds its output by a fixed tick span", "[midi]") {
  const auto note = sonare::midi::make_midi1_note_on(0, 0, 60, 100);

  SECTION("an event past the span limit is refused instead of serialized") {
    MidiClip clip;
    // transport::kMaxPublicPpq itself: legal everywhere else in the system. At
    // the default 480 DCTPQ that is ~4.8e14 ticks, whose DCS chain is ~4.6e8
    // words -- about 1.8 GB, from this one event.
    clip.add_event(ev(1.0e12, note));
    Smf2ExportOptions options;
    const auto result = export_clip_file(clip, {}, {}, options);
    REQUIRE(result.status == Smf2Status::kInvalidArgument);
    REQUIRE(result.bytes.empty());
    REQUIRE_FALSE(result.diagnostic.empty());
  }

  SECTION("a declared length past the limit is refused too") {
    MidiClip clip;
    clip.add_event(ev(0.0, note));
    Smf2ExportOptions options;
    options.length_ppq = 1.0e12;
    const auto result = export_clip_file(clip, {}, {}, options);
    REQUIRE(result.status == Smf2Status::kInvalidArgument);
    REQUIRE(result.bytes.empty());
  }

  SECTION("a long but representable clip still exports, and cheaply") {
    // Just inside the limit: at 480 DCTPQ this is ~8.9 million quarter notes,
    // and the whole chain for it is a few thousand words.
    MidiClip clip;
    const double last_ppq = static_cast<double>(sonare::midi::kMaxExportTick) / 480.0 - 1.0;
    clip.add_event(ev(0.0, note));
    clip.add_event(ev(last_ppq, note));
    Smf2ExportOptions options;
    const auto result = export_clip_file(clip, {}, {}, options);
    REQUIRE(result.status == Smf2Status::kOk);
    REQUIRE_FALSE(result.bytes.empty());
    // Two events spanning the entire legal range: the DCS chain is the span over
    // 0xFFFFF, so the file stays in the tens of kilobytes rather than gigabytes.
    REQUIRE(result.bytes.size() < 64u * 1024u);
  }
}
