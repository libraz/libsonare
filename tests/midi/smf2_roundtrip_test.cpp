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
  sigs.push_back(seg);

  const auto exported = export_clip_file(clip, tempos, sigs, Smf2ExportOptions{});
  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());

  REQUIRE(imported.tempo_segments.size() >= 1);
  REQUIRE(imported.tempo_segments.front().bpm == Catch::Approx(140.0).margin(0.01));
  REQUIRE(imported.time_signatures.size() >= 1);
  REQUIRE(imported.time_signatures.front().time_sig.numerator == 6);
  REQUIRE(imported.time_signatures.front().time_sig.denominator == 8);
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
}
