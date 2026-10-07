/// @file sysex_framing_test.cpp
/// @brief SysEx framing shared by SMF and MIDI Clip File import / export: the stored
///        form's framing rule, SMF escape and continuation handling, and boundary
///        F0 / F7 data bytes in SysEx8.

#include "midi/sysex_framing.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "midi/midi_clip.h"
#include "midi/smf.h"
#include "midi/smf2.h"
#include "midi/ump.h"

namespace {

using sonare::midi::export_clip_file;
using sonare::midi::export_smf;
using sonare::midi::import_clip_file;
using sonare::midi::import_smf;
using sonare::midi::MidiClip;
using sonare::midi::MidiClipEvent;
using sonare::midi::Smf2ExportOptions;
using sonare::midi::Smf2ImportResult;
using sonare::midi::SmfExportOptions;
using sonare::midi::SmfImportResult;
using sonare::midi::SmfStatus;
using sonare::midi::SysExBody;
using sonare::midi::SysExStore;

using Bytes = std::vector<uint8_t>;

const Bytes kGmReset = {0x7E, 0x7F, 0x09, 0x01, 0xF7};

Bytes body_bytes(const SysExBody& body) { return Bytes(body.data, body.data + body.size); }

// ---- SMF builders -------------------------------------------------------------

void push_be(Bytes* out, uint32_t value, int bytes) {
  for (int i = bytes - 1; i >= 0; --i) out->push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void push_vlq(Bytes* out, uint32_t value) {
  uint8_t groups[5];
  int n = 0;
  do {
    groups[n++] = static_cast<uint8_t>(value & 0x7Fu);
    value >>= 7;
  } while (value != 0);
  while (n > 1) out->push_back(static_cast<uint8_t>(groups[--n] | 0x80u));
  out->push_back(groups[0]);
}

void push_event(Bytes* track, uint32_t delta, std::initializer_list<uint8_t> bytes) {
  push_vlq(track, delta);
  track->insert(track->end(), bytes.begin(), bytes.end());
}

void push_sysex_event(Bytes* track, uint32_t delta, uint8_t status, const Bytes& data) {
  push_vlq(track, delta);
  track->push_back(status);
  push_vlq(track, static_cast<uint32_t>(data.size()));
  track->insert(track->end(), data.begin(), data.end());
}

Bytes format0(Bytes track, bool end_of_track = true) {
  if (end_of_track) push_event(&track, 0, {0xFF, 0x2F, 0x00});
  Bytes smf = {'M', 'T', 'h', 'd'};
  push_be(&smf, 6, 4);
  push_be(&smf, 0, 2);
  push_be(&smf, 1, 2);
  push_be(&smf, 480, 2);
  smf.insert(smf.end(), {'M', 'T', 'r', 'k'});
  push_be(&smf, static_cast<uint32_t>(track.size()), 4);
  smf.insert(smf.end(), track.begin(), track.end());
  return smf;
}

struct ImportedSysEx {
  double ppq = -1.0;
  Bytes payload;
};

std::vector<ImportedSysEx> sysex_events(const MidiClip& clip, const SysExStore& store) {
  std::vector<ImportedSysEx> out;
  for (const MidiClipEvent& e : clip.events()) {
    if (e.ump.sysex_handle == 0) continue;
    const Bytes* payload = store.lookup(e.ump.sysex_handle);
    REQUIRE(payload != nullptr);
    out.push_back({e.ppq, *payload});
  }
  return out;
}

std::vector<ImportedSysEx> smf_sysex(const SmfImportResult& r) {
  REQUIRE(r.clips.size() == 1);
  return sysex_events(r.clips[0], r.sysex_store);
}

SmfImportResult smf_roundtrip(const SmfImportResult& imported) {
  SmfExportOptions opts;
  opts.sysex_store = &imported.sysex_store;
  const auto exported = export_smf(imported.clips, imported.tempo_segments,
                                   imported.time_signatures, imported.clip_names, opts);
  REQUIRE(exported.status == SmfStatus::kOk);
  REQUIRE(exported.skipped_events == 0);
  SmfImportResult round = import_smf(exported.bytes);
  REQUIRE(round.ok());
  return round;
}

// ---- MIDI Clip File builders --------------------------------------------------

void push_word(Bytes* out, uint32_t w) { push_be(out, w, 4); }

Bytes clip_file_header() {
  Bytes bytes = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P'};
  push_word(&bytes, 0x4u << 20);                    // DCS 0
  push_word(&bytes, (0x3u << 20) | 480u);           // DCTPQ 480
  push_word(&bytes, 0x4u << 20);                    // DCS 0
  push_word(&bytes, (0xFu << 28) | (0x20u << 16));  // Start of Clip
  for (int i = 0; i < 3; ++i) push_word(&bytes, 0);
  return bytes;
}

void finish_clip_file(Bytes* bytes) {
  push_word(bytes, 0x4u << 20);
  push_word(bytes, (0xFu << 28) | (0x21u << 16));  // End of Clip
  for (int i = 0; i < 3; ++i) push_word(bytes, 0);
}

// Packs @p data as group-0, stream-0 SysEx8 packets of up to 13 bytes each.
void push_sysex8(Bytes* bytes, const Bytes& data) {
  const size_t packets = (data.size() + 12u) / 13u;
  for (size_t p = 0; p < packets; ++p) {
    const size_t offset = p * 13u;
    const size_t chunk = std::min<size_t>(13u, data.size() - offset);
    const uint32_t status = packets == 1u ? 0x0u : p == 0u ? 0x1u : p + 1u == packets ? 0x3u : 0x2u;
    uint8_t d[13] = {};
    for (size_t i = 0; i < chunk; ++i) d[i] = data[offset + i];
    push_word(bytes,
              (0x5u << 28) | (status << 20) | (static_cast<uint32_t>(chunk + 1u) << 16) | d[0]);
    for (int w = 0; w < 3; ++w) {
      push_word(bytes, (static_cast<uint32_t>(d[1 + 4 * w]) << 24) |
                           (static_cast<uint32_t>(d[2 + 4 * w]) << 16) |
                           (static_cast<uint32_t>(d[3 + 4 * w]) << 8) | d[4 + 4 * w]);
    }
  }
}

}  // namespace

TEST_CASE("SysEx stored form returns every body unchanged", "[midi][smf]") {
  const std::vector<Bytes> bodies = {
      {0x7E, 0x7F, 0x09, 0x01},
      {0xF0},
      {0xF7},
      {0xF0, 0xF7},
      {0xF7, 0xF0},
      {0xF0, 0x11, 0x22},
      {0x11, 0x22, 0xF7},
      {0xF0, 0x80, 0xF7},
      {0xF0, 0xF0, 0xF7, 0xF7},
      {0x11, 0x80, 0x22},
  };
  for (const Bytes& body : bodies) {
    for (const bool terminated : {false, true}) {
      const Bytes payload =
          sonare::midi::sysex_payload_from_body(body.data(), body.size(), terminated);
      INFO("body size " << body.size() << " terminated " << terminated);
      CHECK(body_bytes(sonare::midi::sysex_body(payload)) == body);
    }
  }
  // A 7-bit body gains no start byte, and the terminated form is the SMF F0 event's data.
  const Bytes reset_body = {0x7E, 0x7F, 0x09, 0x01};
  CHECK(sonare::midi::sysex_payload_from_body(reset_body.data(), reset_body.size(), true) ==
        kGmReset);
  CHECK(sonare::midi::sysex_payload_from_body(reset_body.data(), reset_body.size(), false) ==
        reset_body);
}

TEST_CASE("SMF empty F7 escape keeps the next SysEx at its own time", "[midi][smf]") {
  Bytes full_f0 = {0xF0};
  full_f0.insert(full_f0.end(), kGmReset.begin(), kGmReset.end());

  for (const Bytes& escape : {kGmReset, full_f0}) {
    INFO("escape form size " << escape.size());
    Bytes control;
    push_sysex_event(&control, 480, 0xF7, escape);
    Bytes with_empty;
    push_sysex_event(&with_empty, 0, 0xF7, {});
    push_sysex_event(&with_empty, 480, 0xF7, escape);

    const SmfImportResult expected = import_smf(format0(control));
    const SmfImportResult actual = import_smf(format0(with_empty));
    REQUIRE(expected.ok());
    REQUIRE(actual.ok());
    CHECK(actual.skipped_events == 0);
    const auto want = smf_sysex(expected);
    const auto got = smf_sysex(actual);
    REQUIRE(want.size() == 1);
    REQUIRE(got.size() == 1);
    CHECK(got[0].ppq == 1.0);
    CHECK(got[0].payload == kGmReset);
    CHECK(got[0].ppq == want[0].ppq);
    CHECK(got[0].payload == want[0].payload);

    const auto round = smf_sysex(smf_roundtrip(actual));
    REQUIRE(round.size() == 1);
    CHECK(round[0].ppq == 1.0);
    CHECK(round[0].payload == kGmReset);
  }
}

TEST_CASE("SMF empty F7 continuation does not finish a pending message", "[midi][smf]") {
  Bytes track;
  push_sysex_event(&track, 0, 0xF0, {0x7E, 0x7F});
  push_sysex_event(&track, 240, 0xF7, {});
  push_sysex_event(&track, 240, 0xF7, {0x09, 0x01, 0xF7});
  const SmfImportResult r = import_smf(format0(track));
  REQUIRE(r.ok());
  CHECK(r.skipped_events == 0);
  const auto events = smf_sysex(r);
  REQUIRE(events.size() == 1);
  CHECK(events[0].ppq == 0.0);
  CHECK(events[0].payload == kGmReset);
}

TEST_CASE("SMF split SysEx survives intervening meta events", "[midi][smf]") {
  struct Case {
    const char* name;
    std::initializer_list<uint8_t> meta;
    uint32_t skipped;
  };
  const Case cases[] = {
      {"marker", {0xFF, 0x06, 0x01, 0x58}, 0},
      {"set tempo", {0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}, 0},
      {"unknown meta", {0xFF, 0x7F, 0x01, 0x7D}, 1},
  };
  for (const Case& c : cases) {
    INFO(c.name);
    Bytes track;
    push_sysex_event(&track, 0, 0xF0, {0x7E, 0x7F, 0x09});
    push_event(&track, 240, c.meta);
    push_sysex_event(&track, 240, 0xF7, {0x01, 0xF7});
    const SmfImportResult r = import_smf(format0(track));
    REQUIRE(r.ok());
    CHECK(r.skipped_events == c.skipped);
    const auto events = smf_sysex(r);
    REQUIRE(events.size() == 1);
    CHECK(events[0].ppq == 0.0);
    CHECK(events[0].payload == kGmReset);
  }
}

TEST_CASE("SMF split SysEx is abandoned by a channel message or the track end", "[midi][smf]") {
  SECTION("channel message") {
    Bytes track;
    push_sysex_event(&track, 0, 0xF0, {0x7E, 0x7F, 0x09});
    push_event(&track, 240, {0x90, 0x3C, 0x64});
    push_sysex_event(&track, 240, 0xF7, {0x01, 0xF7});
    const SmfImportResult r = import_smf(format0(track));
    REQUIRE(r.ok());
    CHECK(r.skipped_events == 1);
    const auto events = smf_sysex(r);
    REQUIRE(events.size() == 1);
    CHECK(events[0].ppq == 1.0);
    CHECK(events[0].payload == Bytes{0x01, 0xF7});
  }
  SECTION("End-of-Track") {
    Bytes track;
    push_sysex_event(&track, 0, 0xF0, {0x7E, 0x7F, 0x09});
    const SmfImportResult r = import_smf(format0(track));
    REQUIRE(r.ok());
    CHECK(r.skipped_events == 1);
    CHECK(r.sysex_store.size() == 0);
  }
  SECTION("track ends without End-of-Track") {
    Bytes track;
    push_sysex_event(&track, 0, 0xF0, {0x7E, 0x7F, 0x09});
    const SmfImportResult r = import_smf(format0(track, /*end_of_track=*/false));
    CHECK(r.status == SmfStatus::kTruncated);
    CHECK(r.skipped_events == 1);
    CHECK(r.sysex_store.size() == 0);
  }
}

TEST_CASE("MIDI Clip File keeps boundary F0 and F7 data bytes in SysEx8", "[midi][smf2]") {
  Bytes long_body = {0xF0};
  for (uint8_t i = 0; i < 18; ++i) long_body.push_back(static_cast<uint8_t>(0x10u + i));
  long_body.push_back(0xF7);
  const std::vector<Bytes> bodies = {
      {0x11, 0x80, 0x22}, {0xF0, 0x80, 0x22}, {0x11, 0x80, 0xF7}, {0xF0, 0x80, 0xF7},
      {0xF0, 0x11, 0x22}, {0x11, 0x22, 0xF7}, long_body,
  };

  Bytes file = clip_file_header();
  for (const Bytes& body : bodies) push_sysex8(&file, body);
  finish_clip_file(&file);
  const Smf2ImportResult imported = import_clip_file(file);
  REQUIRE(imported.ok());
  CHECK(imported.skipped_events == 0);
  const auto first = sysex_events(imported.clips.at(0), imported.sysex_store);
  REQUIRE(first.size() == bodies.size());
  for (size_t i = 0; i < bodies.size(); ++i) {
    INFO("body " << i);
    CHECK(body_bytes(sonare::midi::sysex_body(first[i].payload)) == bodies[i]);
  }

  Smf2ExportOptions options;
  options.sysex_store = &imported.sysex_store;
  const auto exported = export_clip_file(imported.clips.at(0), {}, {}, options);
  REQUIRE(exported.ok());
  CHECK(exported.skipped_events == 0);
  const Smf2ImportResult round = import_clip_file(exported.bytes);
  REQUIRE(round.ok());
  CHECK(round.skipped_events == 0);
  const auto second = sysex_events(round.clips.at(0), round.sysex_store);
  REQUIRE(second.size() == bodies.size());
  for (size_t i = 0; i < bodies.size(); ++i) {
    INFO("body " << i);
    CHECK(second[i].payload == first[i].payload);
  }
}

TEST_CASE("SMF export counts a SysEx8 body with boundary F0 or F7 as skipped", "[midi][smf]") {
  Bytes file = clip_file_header();
  push_sysex8(&file, {0xF0, 0x11, 0x22});
  push_sysex8(&file, {0x11, 0x22, 0xF7});
  push_sysex8(&file, {0x7E, 0x7F, 0x09, 0x01});
  finish_clip_file(&file);
  const Smf2ImportResult imported = import_clip_file(file);
  REQUIRE(imported.ok());

  SmfExportOptions opts;
  opts.sysex_store = &imported.sysex_store;
  const auto exported = export_smf(imported.clips, imported.tempo_segments,
                                   imported.time_signatures, imported.clip_names, opts);
  REQUIRE(exported.status == SmfStatus::kOk);
  CHECK(exported.skipped_events == 2);
  const SmfImportResult round = import_smf(exported.bytes);
  REQUIRE(round.ok());
  const auto events = smf_sysex(round);
  REQUIRE(events.size() == 1);
  CHECK(events[0].payload == kGmReset);
}

TEST_CASE("MIDI Clip File exports a framed 7-bit SysEx as SysEx7", "[midi][smf2]") {
  SysExStore store;
  const auto handle = store.add(Bytes{0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7});
  MidiClip clip;
  MidiClipEvent event;
  event.ump = sonare::midi::make_sysex_handle(0, handle);
  clip.add_event(event);
  Smf2ExportOptions options;
  options.sysex_store = &store;
  const auto exported = export_clip_file(clip, {}, {}, options);
  REQUIRE(exported.ok());
  CHECK(exported.skipped_events == 0);
  const Smf2ImportResult round = import_clip_file(exported.bytes);
  REQUIRE(round.ok());
  const auto events = sysex_events(round.clips.at(0), round.sysex_store);
  REQUIRE(events.size() == 1);
  CHECK(events[0].payload == Bytes{0x7E, 0x7F, 0x09, 0x01});
}
