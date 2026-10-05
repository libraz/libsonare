/// @file smf2_roundtrip_test.cpp
/// @brief MIDI core: MIDI 2.0 Clip File (SMF2) import / export round-trip,
///        lossless MIDI 2.0 channel-voice preservation, Flex Data meta, and
///        malformed-input safety.

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cfenv>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
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

void push_dcs(std::vector<uint8_t>* bytes, uint32_t ticks) {
  push_word(bytes, (0x4u << 20) | (ticks & 0xFFFFFu));
}

void push_stream(std::vector<uint8_t>* bytes, uint16_t status) {
  push_word(bytes, (0xFu << 28) | ((static_cast<uint32_t>(status) & 0x3FFu) << 16));
  push_word(bytes, 0);
  push_word(bytes, 0);
  push_word(bytes, 0);
}

void push_flex_packet(std::vector<uint8_t>* bytes, uint8_t bank, uint8_t status, uint8_t format,
                      uint8_t address, uint32_t word1) {
  const uint8_t byte1 = static_cast<uint8_t>(((format & 0x03u) << 6) | ((address & 0x0Fu) << 4));
  push_word(bytes, (0xDu << 28) | (static_cast<uint32_t>(byte1) << 16) |
                       (static_cast<uint32_t>(bank) << 8) | status);
  push_word(bytes, word1);
  push_word(bytes, 0);
  push_word(bytes, 0);
}

void push_name_packet(std::vector<uint8_t>* bytes, uint8_t group, uint8_t address, uint8_t channel,
                      uint8_t status, uint8_t format, const std::string& text) {
  REQUIRE(text.size() <= 12u);
  std::array<uint8_t, 12> chunk{};
  std::copy(text.begin(), text.end(), chunk.begin());
  const uint8_t byte1 =
      static_cast<uint8_t>(((format & 0x03u) << 6) | ((address & 0x0Fu) << 4) | (channel & 0x0Fu));
  push_word(bytes, (0xDu << 28) | (static_cast<uint32_t>(group & 0x0Fu) << 24) |
                       (static_cast<uint32_t>(byte1) << 16) | (0x01u << 8) | status);
  for (int w = 0; w < 3; ++w) {
    push_word(bytes, (static_cast<uint32_t>(chunk[static_cast<size_t>(w) * 4 + 0]) << 24) |
                         (static_cast<uint32_t>(chunk[static_cast<size_t>(w) * 4 + 1]) << 16) |
                         (static_cast<uint32_t>(chunk[static_cast<size_t>(w) * 4 + 2]) << 8) |
                         static_cast<uint32_t>(chunk[static_cast<size_t>(w) * 4 + 3]));
  }
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

uint32_t first_smf2_tempo_word(const std::vector<uint8_t>& bytes) {
  for (size_t offset = 8; offset + 8 <= bytes.size(); offset += 4) {
    const uint32_t word = read_word(bytes, offset);
    if (((word >> 28) & 0x0Fu) == 0xDu && ((word >> 8) & 0xFFu) == 0x00u &&
        (word & 0xFFu) == 0x00u) {
      return read_word(bytes, offset + 4);
    }
  }
  return 0;
}

bool contains_message_type(const std::vector<uint8_t>& bytes, uint32_t message_type) {
  for (size_t offset = 8; offset + 4 <= bytes.size(); offset += 4) {
    if (((read_word(bytes, offset) >> 28) & 0x0Fu) == message_type) return true;
  }
  return false;
}

std::vector<uint8_t> smf2_header_with_dctpq() {
  std::vector<uint8_t> bytes = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P'};
  push_dcs(&bytes, 0);
  push_word(&bytes, (0x0u << 28) | (0x3u << 20) | 480u);
  push_dcs(&bytes, 0);
  push_stream(&bytes, 0x20u);
  return bytes;
}

void finish_smf2_file(std::vector<uint8_t>* bytes) {
  push_dcs(bytes, 0);
  push_stream(bytes, 0x21u);
}

std::vector<uint8_t> smf2_structural_header() {
  std::vector<uint8_t> bytes = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P'};
  push_dcs(&bytes, 0);
  push_word(&bytes, (0x3u << 20) | 480u);
  return bytes;
}

std::vector<uint8_t> smf2_empty_valid_file() {
  std::vector<uint8_t> bytes = smf2_structural_header();
  push_dcs(&bytes, 0);
  push_stream(&bytes, 0x20u);
  push_dcs(&bytes, 0);
  push_stream(&bytes, 0x21u);
  return bytes;
}

void require_transactionally_empty(const Smf2ImportResult& result) {
  CHECK(result.ticks_per_quarter == 0);
  CHECK(result.clips.empty());
  CHECK(result.clip_names.empty());
  CHECK(result.clip_lengths_ppq.empty());
  CHECK(result.tempo_segments.empty());
  CHECK(result.time_signatures.empty());
  CHECK(result.sysex_store.size() == 0);
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

TEST_CASE("SMF2 import skips a tempo outside the public range", "[midi][smf2]") {
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 64)));
  clip.add_event(ev(4.0, sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  const std::vector<sonare::transport::TempoSegment> tempos = {{0.0, 140.0, 0.0},
                                                               {1.0, 150.0, 0.0}};
  const auto exported = export_clip_file(clip, tempos, {}, Smf2ExportOptions{});
  REQUIRE(exported.ok());
  const Smf2ImportResult clean = import_clip_file(exported.bytes);
  REQUIRE(clean.ok());
  REQUIRE(clean.tempo_segments.size() == 2);

  // Rewrite the 150 BPM tempo word (6e9 / 150 ten-nanosecond units) to 1, i.e. 6e9 BPM.
  std::vector<uint8_t> bytes = exported.bytes;
  const std::vector<uint8_t> word150{0x02, 0x62, 0x5A, 0x00};
  const auto at = std::search(bytes.begin(), bytes.end(), word150.begin(), word150.end());
  REQUIRE(at != bytes.end());
  const std::vector<uint8_t> word1{0x00, 0x00, 0x00, 0x01};
  std::copy(word1.begin(), word1.end(), at);

  const Smf2ImportResult imported = import_clip_file(bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.skipped_events == clean.skipped_events + 1);
  REQUIRE(imported.tempo_segments.size() == 1);
  REQUIRE(imported.tempo_segments.front().bpm == Catch::Approx(140.0).margin(0.01));
  for (const auto& segment : imported.tempo_segments) {
    REQUIRE(sonare::transport::valid_public_tempo_segment(segment));
  }
}

TEST_CASE("SMF2 import skips a zero tempo word", "[midi][smf2]") {
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 64)));
  clip.add_event(ev(4.0, sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  const std::vector<sonare::transport::TempoSegment> tempos = {{0.0, 140.0, 0.0},
                                                               {1.0, 150.0, 0.0}};
  const auto exported = export_clip_file(clip, tempos, {}, Smf2ExportOptions{});
  REQUIRE(exported.ok());
  const Smf2ImportResult clean = import_clip_file(exported.bytes);
  REQUIRE(clean.ok());

  // Rewrite the 150 BPM tempo word to 0, which names no tempo.
  std::vector<uint8_t> bytes = exported.bytes;
  const std::vector<uint8_t> word150{0x02, 0x62, 0x5A, 0x00};
  const auto at = std::search(bytes.begin(), bytes.end(), word150.begin(), word150.end());
  REQUIRE(at != bytes.end());
  std::fill(at, at + 4, uint8_t{0});

  const Smf2ImportResult imported = import_clip_file(bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.skipped_events == clean.skipped_events + 1);
  REQUIRE(imported.tempo_segments.size() == 1);
  REQUIRE(imported.tempo_segments.front().bpm == Catch::Approx(140.0).margin(0.01));
}

namespace {

/// A hand-built clip file holding one Set Time Signature packet at tick 0.
std::vector<uint8_t> clip_file_with_time_signature(uint8_t numerator, uint8_t exponent) {
  std::vector<uint8_t> bytes = smf2_header_with_dctpq();
  push_word(&bytes, (0xDu << 28) | (0x1u << 20) | (0x00u << 8) | 0x01u);
  push_word(&bytes, (static_cast<uint32_t>(numerator) << 24) |
                        (static_cast<uint32_t>(exponent) << 16) | (8u << 8));
  push_word(&bytes, 0);
  push_word(&bytes, 0);
  finish_smf2_file(&bytes);
  return bytes;
}

/// The denominator field of the first Set Time Signature packet in @p bytes.
int exported_denominator_field(const std::vector<uint8_t>& bytes) {
  for (size_t offset = 8; offset + 8 <= bytes.size(); offset += 4) {
    const uint32_t word = read_word(bytes, offset);
    if (((word >> 28) & 0x0Fu) == 0xDu && ((word >> 8) & 0xFFu) == 0x00u &&
        (word & 0xFFu) == 0x01u) {
      return static_cast<int>((read_word(bytes, offset + 4) >> 16) & 0xFFu);
    }
  }
  return -1;
}

}  // namespace

TEST_CASE("SMF2 decodes the time-signature denominator as a power-of-two exponent",
          "[midi][smf2]") {
  const std::pair<std::pair<uint8_t, uint8_t>, std::pair<int, int>> cases[] = {{{4, 2}, {4, 4}},
                                                                               {{6, 3}, {6, 8}}};
  for (const auto& [wire, meter] : cases) {
    INFO("numerator " << int{wire.first} << " exponent " << int{wire.second});
    const Smf2ImportResult imported =
        import_clip_file(clip_file_with_time_signature(wire.first, wire.second));
    REQUIRE(imported.ok());
    CHECK(imported.skipped_events == 0);
    REQUIRE(imported.time_signatures.size() == 1);
    CHECK(imported.time_signatures.front().start_ppq == 0.0);
    CHECK(imported.time_signatures.front().time_sig.numerator == meter.first);
    CHECK(imported.time_signatures.front().time_sig.denominator == meter.second);
    CHECK(imported.time_signatures.front().thirty_seconds_per_quarter == 8);
  }
}

TEST_CASE("SMF2 import skips a non-standard or out-of-range denominator exponent", "[midi][smf2]") {
  for (const uint8_t exponent : {uint8_t{0}, uint8_t{8}, uint8_t{0xFF}}) {
    INFO("exponent " << int{exponent});
    const Smf2ImportResult imported = import_clip_file(clip_file_with_time_signature(3, exponent));
    REQUIRE(imported.ok());
    CHECK(imported.skipped_events == 1);
    // Only the 4/4 the importer supplies at tick 0 when none was read.
    REQUIRE(imported.time_signatures.size() == 1);
    CHECK(imported.time_signatures.front().time_sig.numerator == 4);
    CHECK(imported.time_signatures.front().time_sig.denominator == 4);
  }
}

TEST_CASE("SMF2 export writes the denominator exponent and counts a lossy one", "[midi][smf2]") {
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 64)));
  clip.add_event(ev(4.0, sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  auto export_meter = [&clip](int numerator, int denominator) {
    sonare::transport::TimeSignatureSegment seg;
    seg.time_sig.numerator = numerator;
    seg.time_sig.denominator = denominator;
    return export_clip_file(clip, {}, {seg}, Smf2ExportOptions{});
  };

  const auto six_eight = export_meter(6, 8);
  REQUIRE(six_eight.ok());
  CHECK(six_eight.skipped_events == 0);
  CHECK(exported_denominator_field(six_eight.bytes) == 3);

  // A non-power-of-two denominator rounds up as SMF export rounds it, and counts.
  const auto six_six = export_meter(6, 6);
  REQUIRE(six_six.ok());
  CHECK(six_six.skipped_events == 1);
  CHECK(exported_denominator_field(six_six.bytes) == 3);

  // Exponent 0 is reserved for a non-standard denominator, so a whole note is not exact.
  const auto four_one = export_meter(4, 1);
  REQUIRE(four_one.ok());
  CHECK(four_one.skipped_events == 1);
  CHECK(exported_denominator_field(four_one.bytes) == 1);

  // Beyond the cap.
  const auto four_256 = export_meter(4, 256);
  REQUIRE(four_256.ok());
  CHECK(four_256.skipped_events == 1);
  CHECK(exported_denominator_field(four_256.bytes) == 7);
}

TEST_CASE("SMF2 export clamps and counts unstorable time-signature numerators", "[midi][smf2]") {
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 64)));

  struct Case {
    int input;
    uint32_t skipped;
    int stored;
  };
  const Case cases[] = {
      {0, 1, 1}, {256, 1, 255}, {260, 1, 255}, {1000, 1, 255}, {1, 0, 1}, {3, 0, 3}, {255, 0, 255},
  };
  for (const Case& c : cases) {
    CAPTURE(c.input, c.skipped, c.stored);
    sonare::transport::TimeSignatureSegment seg;
    seg.start_ppq = 0.0;
    seg.time_sig.numerator = c.input;
    seg.time_sig.denominator = 4;
    const auto exported = export_clip_file(clip, {}, {seg}, Smf2ExportOptions{});
    REQUIRE(exported.ok());
    REQUIRE(exported.skipped_events == c.skipped);

    const Smf2ImportResult imported = import_clip_file(exported.bytes);
    REQUIRE(imported.ok());
    REQUIRE(imported.time_signatures.size() == 1);
    CHECK(imported.time_signatures.front().time_sig.numerator == c.stored);
    CHECK(imported.time_signatures.front().time_sig.denominator == 4);
  }
}

TEST_CASE("SMF2 round-trips every representable time-signature denominator", "[midi][smf2]") {
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 64)));
  clip.add_event(ev(8.0, sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  std::vector<sonare::transport::TimeSignatureSegment> sigs;
  for (int exponent = 1; exponent <= 7; ++exponent) {
    sonare::transport::TimeSignatureSegment seg;
    seg.start_ppq = static_cast<double>(exponent - 1);
    seg.time_sig.numerator = exponent + 1;
    seg.time_sig.denominator = 1 << exponent;
    sigs.push_back(seg);
  }
  const auto exported = export_clip_file(clip, {}, sigs, Smf2ExportOptions{});
  REQUIRE(exported.ok());
  CHECK(exported.skipped_events == 0);
  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  CHECK(imported.skipped_events == 0);
  REQUIRE(imported.time_signatures.size() == sigs.size());
  for (size_t i = 0; i < sigs.size(); ++i) {
    INFO("segment " << i);
    CHECK(imported.time_signatures[i].start_ppq == Catch::Approx(sigs[i].start_ppq));
    CHECK(imported.time_signatures[i].time_sig.numerator == sigs[i].time_sig.numerator);
    CHECK(imported.time_signatures[i].time_sig.denominator == sigs[i].time_sig.denominator);
  }
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

TEST_CASE("SMF2 export clamps a tiny positive BPM before converting to the wire field",
          "[midi][smf2]") {
  for (const double bpm : {std::numeric_limits<double>::min(), 1.0e-20}) {
    CAPTURE(bpm);
    const std::vector<sonare::transport::TempoSegment> tempos = {{0.0, bpm, 0.0}};
    std::feclearexcept(FE_ALL_EXCEPT);
    const auto exported = export_clip_file(MidiClip{}, tempos, {}, Smf2ExportOptions{});
    // Both inputs produce a quotient larger than the integer conversion range;
    // clamping the floating quotient first avoids llround's invalid operation.
    CHECK((std::fetestexcept(FE_INVALID) & FE_INVALID) == 0);
    REQUIRE(exported.ok());
    REQUIRE(first_smf2_tempo_word(exported.bytes) == std::numeric_limits<uint32_t>::max());
  }

  const auto normal = export_clip_file(MidiClip{}, {{0.0, 120.0, 0.0}}, {}, Smf2ExportOptions{});
  REQUIRE(normal.ok());
  CHECK(first_smf2_tempo_word(normal.bytes) == 50'000'000u);
}

TEST_CASE("SMF2 export round-trips the minimum representable tempo", "[midi][smf2]") {
  const double minimum_bpm = 6.0e9 / static_cast<double>(std::numeric_limits<uint32_t>::max());
  const auto exported =
      export_clip_file(MidiClip{}, {{0.0, minimum_bpm, 0.0}}, {}, Smf2ExportOptions{});
  REQUIRE(exported.ok());
  REQUIRE(first_smf2_tempo_word(exported.bytes) == std::numeric_limits<uint32_t>::max());

  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.tempo_segments.size() == 1);
  CHECK(imported.tempo_segments.front().bpm == Catch::Approx(minimum_bpm).epsilon(1e-12));
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

  // 2,400,000 ticks = 2 * 0xFFFFF + 302,850. Each full-span DCS must be
  // followed by a Null utility word so the following DCS starts a new delta.
  const uint32_t max_dcs = (0x4u << 20) | 0xFFFFFu;
  const uint32_t remainder_dcs = (0x4u << 20) | 302'850u;
  const uint32_t note_off_word = sonare::midi::make_midi1_note_off(0, 0, 60, 0).words[0];
  size_t note_off_offset = exported.bytes.size();
  for (size_t offset = 8; offset + 4 <= exported.bytes.size(); offset += 4) {
    if (read_word(exported.bytes, offset) == note_off_word) {
      note_off_offset = offset;
      break;
    }
  }
  REQUIRE(note_off_offset >= 5u * 4u);
  CHECK(read_word(exported.bytes, note_off_offset - 5u * 4u) == max_dcs);
  CHECK(read_word(exported.bytes, note_off_offset - 4u * 4u) == 0u);
  CHECK(read_word(exported.bytes, note_off_offset - 3u * 4u) == max_dcs);
  CHECK(read_word(exported.bytes, note_off_offset - 2u * 4u) == 0u);
  CHECK(read_word(exported.bytes, note_off_offset - 1u * 4u) == remainder_dcs);

  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.skipped_events == 0);
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
  finish_smf2_file(&bytes);

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
  finish_smf2_file(&bytes);

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
  finish_smf2_file(&bytes);

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
  finish_smf2_file(&bytes);

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
    finish_smf2_file(&bytes);
    const Smf2ImportResult imported = import_clip_file(bytes);
    REQUIRE(imported.ok());
    REQUIRE(imported.skipped_events == 1);
    REQUIRE(imported.clips.empty());
  }

  SECTION("orphan end is skipped") {
    std::vector<uint8_t> bytes = smf2_header_with_dctpq();
    push_word(&bytes, (0x3u << 28) | (0x3u << 20) | (0x2u << 16) | (0x11u << 8) | 0x22u);
    push_word(&bytes, 0);
    finish_smf2_file(&bytes);
    const Smf2ImportResult imported = import_clip_file(bytes);
    REQUIRE(imported.ok());
    REQUIRE(imported.skipped_events == 1);
    REQUIRE(imported.clips.empty());
  }

  SECTION("unterminated start is skipped") {
    std::vector<uint8_t> bytes = smf2_header_with_dctpq();
    push_word(&bytes, (0x3u << 28) | (0x1u << 20) | (0x2u << 16) | (0x11u << 8) | 0x22u);
    push_word(&bytes, 0);
    finish_smf2_file(&bytes);
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

TEST_CASE("SMF2 exports ClipName with the canonical metadata status", "[midi][smf2]") {
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  Smf2ExportOptions options;
  options.name = "Lead";
  const auto exported = export_clip_file(clip, {}, {}, options);
  REQUIRE(exported.ok());

  bool found_canonical_name = false;
  for (size_t offset = 8; offset + 4 <= exported.bytes.size(); offset += 4) {
    const uint32_t word = read_word(exported.bytes, offset);
    if (((word >> 28) & 0x0Fu) == 0xDu && ((word >> 8) & 0xFFu) == 0x01u &&
        (word & 0xFFu) == 0x03u) {
      found_canonical_name = true;
      break;
    }
  }
  CHECK(found_canonical_name);
}

TEST_CASE("SMF2 imports canonical and legacy ClipName metadata", "[midi][smf2]") {
  for (const auto [status, expected] : {std::pair<uint8_t, const char*>{0x03u, "Canonical"},
                                        std::pair<uint8_t, const char*>{0x02u, "Legacy"}}) {
    CAPTURE(status, expected);
    std::vector<uint8_t> bytes = smf2_structural_header();
    push_dcs(&bytes, 0);
    push_name_packet(&bytes, 0, 1, 0, status, 0, expected);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_word(&bytes, sonare::midi::make_midi1_note_on(0, 0, 60, 100).words[0]);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);

    const Smf2ImportResult imported = import_clip_file(bytes);
    REQUIRE(imported.ok());
    REQUIRE(imported.clip_names.size() == 1);
    CHECK(imported.clip_names.front() == expected);
  }
}

TEST_CASE("SMF2 prefers canonical ClipName over legacy metadata", "[midi][smf2]") {
  std::vector<uint8_t> bytes = smf2_structural_header();
  push_dcs(&bytes, 0);
  push_name_packet(&bytes, 0, 1, 0, 0x02u, 0, "Legacy");
  push_dcs(&bytes, 0);
  push_name_packet(&bytes, 0, 1, 0, 0x03u, 0, "Canonical");
  push_dcs(&bytes, 0);
  push_stream(&bytes, 0x20u);
  push_word(&bytes, sonare::midi::make_midi1_note_on(0, 0, 60, 100).words[0]);
  push_dcs(&bytes, 0);
  push_stream(&bytes, 0x21u);

  const Smf2ImportResult imported = import_clip_file(bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.clip_names.size() == 1);
  CHECK(imported.clip_names.front() == "Canonical");
  CHECK(imported.skipped_events >= 1);
}

TEST_CASE("SMF2 keeps complete ClipName candidates independent", "[midi][smf2]") {
  for (const uint8_t status : {uint8_t{0x02u}, uint8_t{0x03u}}) {
    CAPTURE(status);
    std::vector<uint8_t> bytes = smf2_structural_header();
    push_dcs(&bytes, 0);
    push_name_packet(&bytes, 0, 1, 0, status, 0, "A");
    push_dcs(&bytes, 0);
    push_name_packet(&bytes, 0, 1, 0, status, 0, "B");
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_word(&bytes, sonare::midi::make_midi1_note_on(0, 0, 60, 100).words[0]);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);

    const Smf2ImportResult imported = import_clip_file(bytes);
    REQUIRE(imported.ok());
    REQUIRE(imported.clip_names.size() == 1);
    CHECK(imported.clip_names.front() == "A");
    CHECK(imported.skipped_events >= 1);
  }
}

TEST_CASE("SMF2 keys multipart ClipName candidates by group/address/channel/status",
          "[midi][smf2]") {
  for (const uint8_t status : {uint8_t{0x02u}, uint8_t{0x03u}}) {
    CAPTURE(status);
    std::vector<uint8_t> bytes = smf2_structural_header();
    const uint8_t other_status = status == 0x02u ? 0x03u : 0x02u;
    push_dcs(&bytes, 0);
    // A is a valid group-addressed name (address=1, channel=0).
    push_name_packet(&bytes, 0, 1, 0, status, 1, "A-start");
    push_dcs(&bytes, 0);
    // B has the same group/address/channel but the opposite metadata status.
    push_name_packet(&bytes, 0, 1, 0, other_status, 1, "B-start");
    push_dcs(&bytes, 0);
    // C exercises address separation while retaining channel zero.
    push_name_packet(&bytes, 0, 0, 0, status, 1, "C-start");
    push_dcs(&bytes, 0);
    // D exercises channel separation independently of address.
    push_name_packet(&bytes, 0, 0, 1, status, 1, "D-start");
    push_dcs(&bytes, 0);
    // E exercises group separation while retaining the valid group address.
    push_name_packet(&bytes, 1, 1, 0, status, 1, "E-start");
    push_dcs(&bytes, 0);
    push_name_packet(&bytes, 0, 1, 0, status, 3, "A-end");
    push_dcs(&bytes, 0);
    push_name_packet(&bytes, 0, 1, 0, other_status, 3, "B-end");
    push_dcs(&bytes, 0);
    push_name_packet(&bytes, 0, 0, 0, status, 3, "C-end");
    push_dcs(&bytes, 0);
    push_name_packet(&bytes, 0, 0, 1, status, 3, "D-end");
    push_dcs(&bytes, 0);
    push_name_packet(&bytes, 1, 1, 0, status, 3, "E-end");
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_word(&bytes, sonare::midi::make_midi1_note_on(0, 0, 60, 100).words[0]);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);

    const Smf2ImportResult imported = import_clip_file(bytes);
    REQUIRE(imported.ok());
    REQUIRE(imported.clip_names.size() == 1);
    const std::string expected = status == 0x02u ? "B-startB-end" : "A-startA-end";
    CHECK(imported.clip_names.front() == expected);
    CHECK(imported.skipped_events >= 4);
  }
}

TEST_CASE("SMF2 skips orphan multipart ClipName packets", "[midi][smf2]") {
  for (const uint8_t status : {uint8_t{0x02u}, uint8_t{0x03u}}) {
    CAPTURE(status);
    std::vector<uint8_t> bytes = smf2_structural_header();
    push_dcs(&bytes, 0);
    push_name_packet(&bytes, 0, 1, 0, status, 2, "orphan-c");
    push_dcs(&bytes, 0);
    push_name_packet(&bytes, 0, 1, 0, status, 3, "orphan-e");
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_word(&bytes, sonare::midi::make_midi1_note_on(0, 0, 60, 100).words[0]);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);

    const Smf2ImportResult imported = import_clip_file(bytes);
    REQUIRE(imported.ok());
    REQUIRE(imported.clip_names.size() == 1);
    CHECK(imported.clip_names.front().empty());
    CHECK(imported.skipped_events >= 2);
  }
}

TEST_CASE("SMF2 rejects an overlong logical ClipName without failing the file", "[midi][smf2]") {
  for (const uint8_t status : {uint8_t{0x02u}, uint8_t{0x03u}}) {
    CAPTURE(status);
    std::vector<uint8_t> bytes = smf2_structural_header();
    for (int packet = 0; packet < 33; ++packet) {
      push_dcs(&bytes, 0);
      const uint8_t format = packet == 0 ? 1u : (packet == 32 ? 3u : 2u);
      push_name_packet(&bytes, 0, 1, 0, status, format, std::string(12, 'X'));
    }
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_word(&bytes, sonare::midi::make_midi1_note_on(0, 0, 60, 100).words[0]);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);

    const Smf2ImportResult imported = import_clip_file(bytes);
    REQUIRE(imported.ok());
    REQUIRE(imported.clip_names.size() == 1);
    CHECK(imported.clip_names.front().empty());
    CHECK(imported.skipped_events >= 1);
  }
}

TEST_CASE("SMF2 bounds exported ClipName metadata at 384 bytes", "[midi][smf2]") {
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));

  Smf2ExportOptions too_long;
  too_long.name = std::string(385, 'X');
  const auto rejected = export_clip_file(clip, {}, {}, too_long);
  REQUIRE(rejected.ok());
  CHECK(rejected.skipped_events == 1);
  const Smf2ImportResult rejected_import = import_clip_file(rejected.bytes);
  REQUIRE(rejected_import.ok());
  REQUIRE(rejected_import.clip_names.size() == 1);
  CHECK(rejected_import.clip_names.front().empty());

  Smf2ExportOptions maximum;
  maximum.name = std::string(384, 'Y');
  const auto accepted = export_clip_file(clip, {}, {}, maximum);
  REQUIRE(accepted.ok());
  CHECK(accepted.skipped_events == 0);
  const Smf2ImportResult imported = import_clip_file(accepted.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.clip_names.size() == 1);
  CHECK(imported.clip_names.front() == maximum.name);
}

TEST_CASE("SMF2 accepts a valid ClipName after an overlong candidate", "[midi][smf2]") {
  for (const uint8_t status : {uint8_t{0x02u}, uint8_t{0x03u}}) {
    CAPTURE(status);
    std::vector<uint8_t> bytes = smf2_structural_header();
    for (int packet = 0; packet < 33; ++packet) {
      const uint8_t format = packet == 0 ? 1u : (packet == 32 ? 3u : 2u);
      push_name_packet(&bytes, 0, 1, 0, status, format, std::string(12, 'X'));
    }
    push_name_packet(&bytes, 0, 1, 0, status, 0, "Recovered");
    push_stream(&bytes, 0x20u);
    push_word(&bytes, sonare::midi::make_midi1_note_on(0, 0, 60, 100).words[0]);
    push_stream(&bytes, 0x21u);

    const auto imported = import_clip_file(bytes);
    REQUIRE(imported.ok());
    REQUIRE(imported.clip_names.size() == 1);
    CHECK(imported.clip_names.front() == "Recovered");
    CHECK(imported.skipped_events >= 1);
  }
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
  SECTION("header only is missing the required DCTPQ") {
    const std::vector<uint8_t> bytes = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P'};
    const Smf2ImportResult r = import_clip_file(bytes);
    REQUIRE(r.status == Smf2Status::kMissingDctpq);
    require_transactionally_empty(r);
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

TEST_CASE("SMF2 requires the structural DCS/DCTPQ and stream markers", "[midi][smf2]") {
  SECTION("a complete empty file is accepted") {
    const Smf2ImportResult imported = import_clip_file(smf2_empty_valid_file());
    REQUIRE(imported.ok());
    CHECK(imported.ticks_per_quarter == 480);
    CHECK(imported.skipped_events == 0);
    CHECK(imported.clips.empty());
  }

  SECTION("a naked DCTPQ without the leading DCS is rejected") {
    std::vector<uint8_t> bytes = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P'};
    push_word(&bytes, (0x0u << 28) | (0x3u << 20) | 480u);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);
    const Smf2ImportResult imported = import_clip_file(bytes);
    CHECK(imported.status == Smf2Status::kMalformed);
    require_transactionally_empty(imported);
  }

  SECTION("a non-zero DCS before DCTPQ is rejected") {
    std::vector<uint8_t> bytes = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P'};
    push_dcs(&bytes, 1);
    push_word(&bytes, (0x3u << 20) | 480u);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);
    const Smf2ImportResult imported = import_clip_file(bytes);
    CHECK(imported.status == Smf2Status::kMalformed);
    require_transactionally_empty(imported);
  }

  SECTION("a duplicate DCTPQ is rejected") {
    std::vector<uint8_t> bytes = smf2_structural_header();
    push_dcs(&bytes, 0);
    push_word(&bytes, (0x3u << 20) | 480u);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);
    const Smf2ImportResult imported = import_clip_file(bytes);
    CHECK(imported.status == Smf2Status::kMalformed);
    require_transactionally_empty(imported);
  }

  SECTION("a missing DCTPQ has its dedicated status") {
    std::vector<uint8_t> bytes = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P'};
    push_dcs(&bytes, 0);
    const Smf2ImportResult imported = import_clip_file(bytes);
    CHECK(imported.status == Smf2Status::kMissingDctpq);
    require_transactionally_empty(imported);
  }

  SECTION("a profile SysEx before DCTPQ is skipped as opaque") {
    std::vector<uint8_t> bytes = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P'};
    push_sysex7_packet(&bytes, 0, 0x0u, {0x7Eu, 0x7Fu, 0x0Du});
    const std::vector<uint8_t> prefix = smf2_structural_header();
    bytes.insert(bytes.end(), prefix.begin() + 8, prefix.end());
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);

    const Smf2ImportResult imported = import_clip_file(bytes);
    REQUIRE(imported.ok());
    CHECK(imported.skipped_events == 1);
    CHECK(imported.clips.empty());
    CHECK(imported.sysex_store.size() == 0);
  }
}

TEST_CASE("SMF2 requires exactly one Start and End of Clip in order", "[midi][smf2]") {
  SECTION("missing Start of Clip is truncated") {
    std::vector<uint8_t> bytes = smf2_structural_header();
    const Smf2ImportResult imported = import_clip_file(bytes);
    CHECK(imported.status == Smf2Status::kTruncated);
    require_transactionally_empty(imported);
  }

  SECTION("missing End of Clip is truncated") {
    std::vector<uint8_t> bytes = smf2_structural_header();
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    const Smf2ImportResult imported = import_clip_file(bytes);
    CHECK(imported.status == Smf2Status::kTruncated);
    require_transactionally_empty(imported);
  }

  SECTION("End of Clip before Start of Clip is malformed") {
    std::vector<uint8_t> bytes = smf2_structural_header();
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);
    const Smf2ImportResult imported = import_clip_file(bytes);
    CHECK(imported.status == Smf2Status::kMalformed);
    require_transactionally_empty(imported);
  }

  SECTION("duplicate Start of Clip is rejected") {
    std::vector<uint8_t> bytes = smf2_structural_header();
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);
    const Smf2ImportResult imported = import_clip_file(bytes);
    CHECK(imported.status == Smf2Status::kMalformed);
    require_transactionally_empty(imported);
  }

  SECTION("duplicate End of Clip is rejected") {
    std::vector<uint8_t> bytes = smf2_empty_valid_file();
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);
    const Smf2ImportResult imported = import_clip_file(bytes);
    CHECK(imported.status == Smf2Status::kMalformed);
    require_transactionally_empty(imported);
  }

  SECTION("trailing data after End of Clip is rejected") {
    std::vector<uint8_t> bytes = smf2_empty_valid_file();
    push_dcs(&bytes, 0);
    push_word(&bytes, sonare::midi::make_midi1_note_on(0, 0, 60, 100).words[0]);
    const Smf2ImportResult imported = import_clip_file(bytes);
    CHECK(imported.status == Smf2Status::kMalformed);
    require_transactionally_empty(imported);
  }

  SECTION("a high Stream status does not alias Start or End of Clip") {
    std::vector<uint8_t> alias_start = smf2_structural_header();
    push_dcs(&alias_start, 0);
    push_stream(&alias_start, 0x120u);
    push_dcs(&alias_start, 0);
    push_stream(&alias_start, 0x21u);
    const Smf2ImportResult start_result = import_clip_file(alias_start);
    CHECK(start_result.status == Smf2Status::kMalformed);
    require_transactionally_empty(start_result);

    std::vector<uint8_t> alias_end = smf2_structural_header();
    push_dcs(&alias_end, 0);
    push_stream(&alias_end, 0x20u);
    push_dcs(&alias_end, 0);
    push_stream(&alias_end, 0x121u);
    const Smf2ImportResult end_result = import_clip_file(alias_end);
    CHECK(end_result.status == Smf2Status::kTruncated);
    require_transactionally_empty(end_result);

    std::vector<uint8_t> non_complete = smf2_structural_header();
    push_dcs(&non_complete, 0);
    push_word(&non_complete, (0xFu << 28) | (1u << 26) | (0x20u << 16));
    push_word(&non_complete, 0);
    push_word(&non_complete, 0);
    push_word(&non_complete, 0);
    push_dcs(&non_complete, 0);
    push_stream(&non_complete, 0x21u);
    const Smf2ImportResult form_result = import_clip_file(non_complete);
    CHECK(form_result.status == Smf2Status::kMalformed);
    require_transactionally_empty(form_result);
  }
}

TEST_CASE("SMF2 accepts timed packets sharing one DCS", "[midi][smf2]") {
  std::vector<uint8_t> bytes = smf2_structural_header();
  push_dcs(&bytes, 0);
  push_stream(&bytes, 0x20u);
  push_dcs(&bytes, 480);
  push_word(&bytes, sonare::midi::make_midi1_note_on(0, 0, 60, 100).words[0]);
  push_sysex7_packet(&bytes, 0, 0x0u, {0x7Eu, 0x7Fu, 0x09u});
  push_flex_packet(&bytes, 0, 0x00u, 0, 1, 50'000'000u);
  push_flex_packet(&bytes, 0, 0x01u, 0, 1, (3u << 24) | (2u << 16) | (8u << 8));
  push_dcs(&bytes, 0);
  push_stream(&bytes, 0x21u);

  const Smf2ImportResult imported = import_clip_file(bytes);
  REQUIRE(imported.ok());
  CHECK(imported.skipped_events == 0);
  REQUIRE(imported.clips.size() == 1);
  REQUIRE(imported.clips.front().events().size() == 2);
  CHECK(imported.clips.front().events()[0].ppq == Catch::Approx(1.0));
  CHECK(imported.clips.front().events()[1].ppq == Catch::Approx(1.0));
  REQUIRE(imported.tempo_segments.size() == 2);
  CHECK(imported.tempo_segments.back().start_ppq == Catch::Approx(1.0));
  REQUIRE(imported.time_signatures.size() == 2);
  CHECK(imported.time_signatures.back().start_ppq == Catch::Approx(1.0));
}

TEST_CASE("SMF2 skips unsupported Stream messages in configuration and sequence", "[midi][smf2]") {
  for (const bool configuration : {false, true}) {
    for (const bool multipart : {false, true}) {
      CAPTURE(configuration, multipart);
      std::vector<uint8_t> bytes = smf2_structural_header();
      if (!configuration) push_stream(&bytes, 0x20u);
      const auto endpoint_name = [&](uint8_t form, uint16_t text) {
        push_word(&bytes,
                  (0xFu << 28) | (static_cast<uint32_t>(form) << 26) | (0x12u << 16) | text);
        push_word(&bytes, 0);
        push_word(&bytes, 0);
        push_word(&bytes, 0);
      };
      endpoint_name(multipart ? 1u : 0u, 0x4100u);
      if (multipart) endpoint_name(3u, 0x4200u);
      if (configuration) push_stream(&bytes, 0x20u);
      push_word(&bytes, sonare::midi::make_midi1_note_on(0, 0, 60, 100).words[0]);
      push_stream(&bytes, 0x21u);

      const auto imported = import_clip_file(bytes);
      CHECK(imported.ok());
      CHECK(imported.skipped_events == (multipart ? 2u : 1u));
      CHECK(imported.clips.size() == 1);
      if (imported.clips.size() == 1) CHECK(imported.clips.front().events().size() == 1);
    }
  }
}

TEST_CASE("SMF2 limits configuration tempo and meter but not sequence changes", "[midi][smf2]") {
  SECTION("an invalid first value cannot hide a duplicate configuration message") {
    for (const uint8_t status : {uint8_t{0}, uint8_t{1}}) {
      CAPTURE(status);
      std::vector<uint8_t> bytes = smf2_structural_header();
      push_dcs(&bytes, 0);
      push_flex_packet(&bytes, 0, status, 0, 1, 0u);
      push_dcs(&bytes, 0);
      const uint32_t valid_value = status == 0u ? 50'000'000u : (4u << 24) | (2u << 16) | (8u << 8);
      push_flex_packet(&bytes, 0, status, 0, 1, valid_value);
      push_dcs(&bytes, 0);
      push_stream(&bytes, 0x20u);
      push_dcs(&bytes, 0);
      push_stream(&bytes, 0x21u);
      const Smf2ImportResult imported = import_clip_file(bytes);
      CHECK(imported.status == Smf2Status::kMalformed);
      require_transactionally_empty(imported);
    }
  }

  SECTION("duplicate configuration tempo is rejected") {
    std::vector<uint8_t> bytes = smf2_structural_header();
    push_dcs(&bytes, 0);
    push_flex_packet(&bytes, 0, 0x00u, 0, 1, 50'000'000u);
    push_dcs(&bytes, 0);
    push_flex_packet(&bytes, 0, 0x00u, 0, 1, 60'000'000u);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);
    const Smf2ImportResult imported = import_clip_file(bytes);
    CHECK(imported.status == Smf2Status::kMalformed);
    require_transactionally_empty(imported);
  }

  SECTION("duplicate configuration meter is rejected") {
    std::vector<uint8_t> bytes = smf2_structural_header();
    push_dcs(&bytes, 0);
    push_flex_packet(&bytes, 0, 0x01u, 0, 1, (4u << 24) | (2u << 16) | (8u << 8));
    push_dcs(&bytes, 0);
    push_flex_packet(&bytes, 0, 0x01u, 0, 1, (3u << 24) | (2u << 16) | (8u << 8));
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);
    const Smf2ImportResult imported = import_clip_file(bytes);
    CHECK(imported.status == Smf2Status::kMalformed);
    require_transactionally_empty(imported);
  }

  SECTION("sequence tempo and meter changes have no cardinality cap") {
    std::vector<uint8_t> bytes = smf2_structural_header();
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_dcs(&bytes, 480);
    push_flex_packet(&bytes, 0, 0x00u, 0, 1, 50'000'000u);
    push_flex_packet(&bytes, 0, 0x00u, 0, 1, 60'000'000u);
    push_flex_packet(&bytes, 0, 0x01u, 0, 1, (3u << 24) | (2u << 16) | (8u << 8));
    push_flex_packet(&bytes, 0, 0x01u, 0, 1, (5u << 24) | (2u << 16) | (8u << 8));
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);
    const Smf2ImportResult imported = import_clip_file(bytes);
    REQUIRE(imported.ok());
    REQUIRE(imported.tempo_segments.size() == 3);
    CHECK(imported.tempo_segments[1].start_ppq == Catch::Approx(1.0));
    CHECK(imported.tempo_segments[2].start_ppq == Catch::Approx(1.0));
    REQUIRE(imported.time_signatures.size() == 3);
    CHECK(imported.time_signatures[1].start_ppq == Catch::Approx(1.0));
    CHECK(imported.time_signatures[2].start_ppq == Catch::Approx(1.0));
  }
}

TEST_CASE("SMF2 skips non-complete or non-group Flex tempo and meter packets", "[midi][smf2]") {
  const std::pair<uint8_t, uint8_t> invalid_shapes[] = {{1, 1}, {0, 0}};
  for (const auto [format, address] : invalid_shapes) {
    CAPTURE(format, address);
    std::vector<uint8_t> bytes = smf2_structural_header();
    push_dcs(&bytes, 0);
    push_flex_packet(&bytes, 0, 0x00u, format, address, 50'000'000u);
    push_dcs(&bytes, 0);
    push_flex_packet(&bytes, 0, 0x01u, format, address, (3u << 24) | (2u << 16) | (8u << 8));
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);
    const Smf2ImportResult imported = import_clip_file(bytes);
    REQUIRE(imported.ok());
    CHECK(imported.skipped_events == 2);
    REQUIRE(imported.tempo_segments.size() == 1);
    REQUIRE(imported.time_signatures.size() == 1);
  }
}

TEST_CASE("SMF2 clears recovered state when a later structural error occurs", "[midi][smf2]") {
  std::vector<uint8_t> bytes = smf2_structural_header();
  push_dcs(&bytes, 0);
  push_flex_packet(&bytes, 0, 0x00u, 0, 1, 50'000'000u);
  push_dcs(&bytes, 0);
  push_flex_packet(&bytes, 0, 0x01u, 0, 1, (3u << 24) | (2u << 16) | (8u << 8));
  push_dcs(&bytes, 0);
  push_stream(&bytes, 0x20u);
  push_dcs(&bytes, 480);
  push_sysex7_packet(&bytes, 0, 0x0u, {0x7Eu, 0x7Fu, 0x09u});
  push_dcs(&bytes, 0);
  push_stream(&bytes, 0x21u);
  push_dcs(&bytes, 0);
  push_word(&bytes, sonare::midi::make_midi1_note_on(0, 0, 60, 100).words[0]);

  const Smf2ImportResult imported = import_clip_file(bytes);
  CHECK(imported.status == Smf2Status::kMalformed);
  CHECK(imported.skipped_events == 0);
  require_transactionally_empty(imported);
}

TEST_CASE("SMF2 import reads Null utility words only as DCS chain separators", "[midi][smf2]") {
  const auto chained_file = [](bool with_null) {
    std::vector<uint8_t> bytes = smf2_structural_header();
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_dcs(&bytes, 0xFFFFFu);
    if (with_null) push_word(&bytes, 0u);
    push_dcs(&bytes, 1);
    push_word(&bytes, sonare::midi::make_midi1_note_on(0, 0, 60, 100).words[0]);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);
    return bytes;
  };
  const double expected_ppq = static_cast<double>(0xFFFFFu + 1u) / 480.0;

  for (const bool with_null : {true, false}) {
    CAPTURE(with_null);
    const Smf2ImportResult imported = import_clip_file(chained_file(with_null));
    REQUIRE(imported.ok());
    CHECK(imported.skipped_events == 0);
    REQUIRE(imported.clips.size() == 1);
    REQUIRE(imported.clips[0].events().size() == 1);
    CHECK(imported.clips[0].events()[0].ppq == Catch::Approx(expected_ppq));
  }

  SECTION("a Null before DCTPQ is malformed") {
    std::vector<uint8_t> bytes = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P'};
    push_dcs(&bytes, 0);
    push_word(&bytes, 0u);
    push_word(&bytes, (0x3u << 20) | 480u);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x20u);
    push_dcs(&bytes, 0);
    push_stream(&bytes, 0x21u);
    const Smf2ImportResult imported = import_clip_file(bytes);
    CHECK(imported.status == Smf2Status::kMalformed);
    require_transactionally_empty(imported);
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
