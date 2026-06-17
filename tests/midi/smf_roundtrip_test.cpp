/// @file smf_roundtrip_test.cpp
/// @brief MIDI core: Standard MIDI File import / export round-trip,
///        running-status + meta-event coverage and malformed-input safety.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

#include "midi/midi_clip.h"
#include "midi/program_map.h"
#include "midi/smf.h"
#include "midi/ump.h"
#include "transport/tempo_map.h"

namespace {

using sonare::midi::export_smf;
using sonare::midi::import_smf;
using sonare::midi::MidiClip;
using sonare::midi::MidiClipEvent;
using sonare::midi::ProgramState;
using sonare::midi::SmfExportOptions;
using sonare::midi::SmfImportResult;
using sonare::midi::SmfStatus;
using sonare::midi::Ump;

// Appends a big-endian 32-bit value.
void push_u32(std::vector<uint8_t>* v, uint32_t x) {
  v->push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
  v->push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
  v->push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
  v->push_back(static_cast<uint8_t>(x & 0xFF));
}

void push_u16(std::vector<uint8_t>* v, uint16_t x) {
  v->push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
  v->push_back(static_cast<uint8_t>(x & 0xFF));
}

void push_tag(std::vector<uint8_t>* v, const char* tag) {
  for (int i = 0; i < 4; ++i) v->push_back(static_cast<uint8_t>(tag[i]));
}

void push_vlq(std::vector<uint8_t>* v, uint32_t value) {
  uint8_t buf[5]{};
  int n = 0;
  buf[n++] = static_cast<uint8_t>(value & 0x7Fu);
  while ((value >>= 7) != 0) {
    buf[n++] = static_cast<uint8_t>((value & 0x7Fu) | 0x80u);
  }
  for (int i = n - 1; i >= 0; --i) v->push_back(buf[i]);
}

// Builds a known-good format-0 SMF with 480 PPQN containing:
//   - set-tempo 500000 us/quarter (= 120 BPM)
//   - time-signature 3/4
//   - track name "lead"
//   - a marker "verse"
//   - note-on C4 (vel 100) at tick 0, note-off at tick 480 (= 1 quarter / 1 PPQ)
//   - a control change exercising running status
std::vector<uint8_t> make_known_smf() {
  std::vector<uint8_t> body;
  // delta 0, set-tempo 0xFF 0x51 0x03 07 A1 20 (500000).
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x51);
  body.push_back(0x03);
  body.push_back(0x07);
  body.push_back(0xA1);
  body.push_back(0x20);
  // delta 0, time-signature 3/4: numerator 3, denom-power 2, clocks 24, 32nds 8.
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x58);
  body.push_back(0x04);
  body.push_back(0x03);
  body.push_back(0x02);
  body.push_back(0x18);
  body.push_back(0x08);
  // delta 0, track name "lead".
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x03);
  body.push_back(0x04);
  body.push_back('l');
  body.push_back('e');
  body.push_back('a');
  body.push_back('d');
  // delta 0, marker "verse".
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x06);
  body.push_back(0x05);
  body.push_back('v');
  body.push_back('e');
  body.push_back('r');
  body.push_back('s');
  body.push_back('e');
  // delta 0, note-on ch0 note 60 vel 100 (0x90 0x3C 0x64).
  body.push_back(0x00);
  body.push_back(0x90);
  body.push_back(0x3C);
  body.push_back(0x64);
  // delta 0, control change ch0 cc7 val 90 (0xB0 0x07 0x5A).
  body.push_back(0x00);
  body.push_back(0xB0);
  body.push_back(0x07);
  body.push_back(0x5A);
  // delta 0, running-status CC cc10 val 64 (no status byte: 0x0A 0x40).
  body.push_back(0x00);
  body.push_back(0x0A);
  body.push_back(0x40);
  // delta 480 (VLQ 0x83 0x60), note-off ch0 note 60 vel 0 (0x80 0x3C 0x00).
  body.push_back(0x83);
  body.push_back(0x60);
  body.push_back(0x80);
  body.push_back(0x3C);
  body.push_back(0x00);
  // delta 0, end-of-track.
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x2F);
  body.push_back(0x00);

  std::vector<uint8_t> smf;
  push_tag(&smf, "MThd");
  push_u32(&smf, 6);
  push_u16(&smf, 0);    // Format 0.
  push_u16(&smf, 1);    // One track.
  push_u16(&smf, 480);  // 480 PPQN.
  push_tag(&smf, "MTrk");
  push_u32(&smf, static_cast<uint32_t>(body.size()));
  smf.insert(smf.end(), body.begin(), body.end());
  return smf;
}

std::vector<uint8_t> make_sysex_smf(const std::vector<uint8_t>& payload, uint32_t delta = 0) {
  std::vector<uint8_t> body;
  push_vlq(&body, delta);
  body.push_back(0xF0);
  push_vlq(&body, static_cast<uint32_t>(payload.size()));
  body.insert(body.end(), payload.begin(), payload.end());
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x2F);
  body.push_back(0x00);

  std::vector<uint8_t> smf;
  push_tag(&smf, "MThd");
  push_u32(&smf, 6);
  push_u16(&smf, 0);
  push_u16(&smf, 1);
  push_u16(&smf, 480);
  push_tag(&smf, "MTrk");
  push_u32(&smf, static_cast<uint32_t>(body.size()));
  smf.insert(smf.end(), body.begin(), body.end());
  return smf;
}

std::vector<uint8_t> wrap_format0_track(const std::vector<uint8_t>& body) {
  std::vector<uint8_t> smf;
  push_tag(&smf, "MThd");
  push_u32(&smf, 6);
  push_u16(&smf, 0);
  push_u16(&smf, 1);
  push_u16(&smf, 480);
  push_tag(&smf, "MTrk");
  push_u32(&smf, static_cast<uint32_t>(body.size()));
  smf.insert(smf.end(), body.begin(), body.end());
  return smf;
}

// Builds a format-1 SMF whose track 0 is a meta-only conductor track carrying a
// track-name meta (= the song / sequence title) and whose track 1 carries a
// single note. Mirrors what DAWs export.
std::vector<uint8_t> make_format1_with_conductor_name(const char* song_name) {
  std::vector<uint8_t> conductor;
  // delta 0, track name = song title.
  conductor.push_back(0x00);
  conductor.push_back(0xFF);
  conductor.push_back(0x03);
  const auto name_len = static_cast<uint8_t>(std::string(song_name).size());
  conductor.push_back(name_len);
  for (const char* p = song_name; *p != '\0'; ++p) conductor.push_back(static_cast<uint8_t>(*p));
  // delta 0, end-of-track.
  conductor.push_back(0x00);
  conductor.push_back(0xFF);
  conductor.push_back(0x2F);
  conductor.push_back(0x00);

  std::vector<uint8_t> note;
  // delta 0, note-on ch0 note 60 vel 100, then delta 480 note-off.
  note.push_back(0x00);
  note.push_back(0x90);
  note.push_back(0x3C);
  note.push_back(0x64);
  note.push_back(0x83);
  note.push_back(0x60);
  note.push_back(0x80);
  note.push_back(0x3C);
  note.push_back(0x00);
  note.push_back(0x00);
  note.push_back(0xFF);
  note.push_back(0x2F);
  note.push_back(0x00);

  std::vector<uint8_t> smf;
  push_tag(&smf, "MThd");
  push_u32(&smf, 6);
  push_u16(&smf, 1);    // Format 1.
  push_u16(&smf, 2);    // Two tracks.
  push_u16(&smf, 480);  // 480 PPQN.
  push_tag(&smf, "MTrk");
  push_u32(&smf, static_cast<uint32_t>(conductor.size()));
  smf.insert(smf.end(), conductor.begin(), conductor.end());
  push_tag(&smf, "MTrk");
  push_u32(&smf, static_cast<uint32_t>(note.size()));
  smf.insert(smf.end(), note.begin(), note.end());
  return smf;
}

// Appends a text-class meta event (0xFF type len payload).
void push_text_meta(std::vector<uint8_t>* body, uint8_t type, const std::string& text) {
  push_vlq(body, 0);
  body->push_back(0xFF);
  body->push_back(type);
  push_vlq(body, static_cast<uint32_t>(text.size()));
  for (char c : text) body->push_back(static_cast<uint8_t>(c));
}

// Builds a format-0 SMF carrying one of each recognized text-class meta event
// (text / lyric / cue point / marker / key signature) plus a single note so the
// track yields a clip. The key signature is sf=1, mi=1 (E minor).
std::vector<uint8_t> make_meta_classes_smf() {
  std::vector<uint8_t> body;
  push_text_meta(&body, 0x01, "memo");   // Text.
  push_text_meta(&body, 0x05, "la");     // Lyric.
  push_text_meta(&body, 0x07, "cue1");   // Cue point.
  push_text_meta(&body, 0x06, "verse");  // Marker.
  // delta 0, key signature 0xFF 0x59 0x02 sf=1 mi=1 (E minor).
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x59);
  body.push_back(0x02);
  body.push_back(0x01);
  body.push_back(0x01);
  // delta 0, note-on then delta 480 note-off so a clip exists.
  body.push_back(0x00);
  body.push_back(0x90);
  body.push_back(0x3C);
  body.push_back(0x64);
  body.push_back(0x83);
  body.push_back(0x60);
  body.push_back(0x80);
  body.push_back(0x3C);
  body.push_back(0x00);
  // delta 0, end-of-track.
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x2F);
  body.push_back(0x00);
  return wrap_format0_track(body);
}

}  // namespace

TEST_CASE("SMF format-1 import preserves the conductor track sequence name", "[midi]") {
  const std::vector<uint8_t> smf = make_format1_with_conductor_name("My Song");
  const SmfImportResult r = import_smf(smf);
  REQUIRE(r.ok());
  REQUIRE(r.format == 1);
  // The meta-only conductor track produces no clip, but its name is kept as the
  // sequence/song title rather than discarded.
  REQUIRE(r.sequence_name == "My Song");
  REQUIRE(r.clips.size() == 1);  // Only the note track yields a clip.
}

TEST_CASE("SMF import parses tempo, time-signature, names, markers and notes", "[midi]") {
  const std::vector<uint8_t> smf = make_known_smf();
  const SmfImportResult r = import_smf(smf);

  REQUIRE(r.ok());
  REQUIRE(r.format == 0);
  REQUIRE(r.ticks_per_quarter == 480);

  // Tempo: 500000 us/quarter -> 120 BPM.
  REQUIRE(r.tempo_segments.size() == 1);
  REQUIRE(r.tempo_segments[0].start_ppq == 0.0);
  REQUIRE(r.tempo_segments[0].bpm > 119.9);
  REQUIRE(r.tempo_segments[0].bpm < 120.1);

  // Time signature 3/4.
  REQUIRE(r.time_signatures.size() == 1);
  REQUIRE(r.time_signatures[0].time_sig.numerator == 3);
  REQUIRE(r.time_signatures[0].time_sig.denominator == 4);
  REQUIRE(r.time_signatures[0].clocks_per_metronome_click == 24);
  REQUIRE(r.time_signatures[0].thirty_seconds_per_quarter == 8);
  REQUIRE(r.clip_lengths_ppq.size() == 1);
  REQUIRE(r.clip_lengths_ppq[0] == 1.0);

  // One clip with a track name and a marker.
  REQUIRE(r.clips.size() == 1);
  REQUIRE(r.clip_names.size() == 1);
  REQUIRE(r.clip_names[0] == "lead");
  REQUIRE(r.markers.size() == 1);
  REQUIRE(r.markers[0].text == "verse");
  REQUIRE(r.markers[0].ppq == 0.0);

  // Events: note-on, 2 CC (one via running status), note-off = 4 channel events.
  const auto& events = r.clips[0].events();
  REQUIRE(events.size() == 4);

  // The note-on sits at ppq 0 (alongside the two control changes, which
  // MidiClip::sort_stable orders before the note-on at the same timestamp).
  bool found_note_on_at_0 = false;
  for (const auto& e : events) {
    if (e.ump.is_note_on() && e.ump.note_number() == 60) {
      REQUIRE(e.ppq == 0.0);
      found_note_on_at_0 = true;
    }
  }
  REQUIRE(found_note_on_at_0);

  // Note-off appears at ppq 1.0 (480 ticks / 480 PPQN).
  bool found_note_off_at_1 = false;
  for (const auto& e : events) {
    if (e.ump.is_note_off() && e.ump.note_number() == 60) {
      REQUIRE(e.ppq == 1.0);
      found_note_off_at_1 = true;
    }
  }
  REQUIRE(found_note_off_at_1);

  // Running status produced a second control-change event.
  int cc_count = 0;
  for (const auto& e : events) {
    if (e.ump.status_nibble() == static_cast<uint8_t>(sonare::midi::UmpStatus::kControlChange)) {
      ++cc_count;
    }
  }
  REQUIRE(cc_count == 2);
}

TEST_CASE("SMF export then re-import round-trips events and tempo", "[midi]") {
  const std::vector<uint8_t> smf = make_known_smf();
  const SmfImportResult imported = import_smf(smf);
  REQUIRE(imported.ok());

  // Export back to bytes (480 PPQN preserves the 480-tick note length exactly).
  SmfExportOptions opts;
  opts.ticks_per_quarter = 480;
  opts.markers = imported.markers;
  const auto exported = export_smf(imported.clips, imported.tempo_segments,
                                   imported.time_signatures, imported.clip_names, opts);
  REQUIRE(exported.ok());
  REQUIRE(exported.skipped_events == 0);
  REQUIRE_FALSE(exported.bytes.empty());

  // Re-import the exported bytes.
  const SmfImportResult round = import_smf(exported.bytes);
  REQUIRE(round.ok());
  REQUIRE(round.format == 1);  // Export always writes format 1.
  REQUIRE(round.ticks_per_quarter == 480);

  // Tempo / time-signature survive.
  REQUIRE(round.tempo_segments.size() == 1);
  REQUIRE(round.tempo_segments[0].bpm > 119.9);
  REQUIRE(round.tempo_segments[0].bpm < 120.1);
  REQUIRE(round.time_signatures[0].time_sig.numerator == 3);
  REQUIRE(round.time_signatures[0].time_sig.denominator == 4);
  REQUIRE(round.time_signatures[0].clocks_per_metronome_click == 24);
  REQUIRE(round.time_signatures[0].thirty_seconds_per_quarter == 8);
  REQUIRE(round.markers.size() == 1);
  REQUIRE(round.markers[0].text == "verse");
  REQUIRE(round.markers[0].ppq == 0.0);

  // Clip events match the original import exactly (UMP equality + ppq).
  REQUIRE(round.clips.size() == imported.clips.size());
  REQUIRE(round.clip_names.size() == imported.clip_names.size());
  REQUIRE(round.clip_names[0] == "lead");
  REQUIRE(round.clip_lengths_ppq.size() == 1);
  REQUIRE(round.clip_lengths_ppq[0] == 1.0);

  const auto& a = imported.clips[0].events();
  const auto& b = round.clips[0].events();
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    REQUIRE(a[i].ppq == b[i].ppq);
    REQUIRE(a[i].ump == b[i].ump);
  }
}

TEST_CASE("SMF import tags text / lyric / cue / marker / key-signature meta with kinds", "[midi]") {
  using sonare::midi::SmfMarkerKind;
  const SmfImportResult r = import_smf(make_meta_classes_smf());
  REQUIRE(r.ok());
  // None of the text-class meta events are dropped lossily.
  REQUIRE(r.skipped_events == 0);
  REQUIRE(r.markers.size() == 5);

  auto by_kind = [&](SmfMarkerKind kind) -> const sonare::midi::SmfMarker* {
    for (const auto& m : r.markers) {
      if (m.kind == kind) return &m;
    }
    return nullptr;
  };

  REQUIRE(by_kind(SmfMarkerKind::kText) != nullptr);
  REQUIRE(by_kind(SmfMarkerKind::kText)->text == "memo");
  REQUIRE(by_kind(SmfMarkerKind::kLyric) != nullptr);
  REQUIRE(by_kind(SmfMarkerKind::kLyric)->text == "la");
  REQUIRE(by_kind(SmfMarkerKind::kCuePoint) != nullptr);
  REQUIRE(by_kind(SmfMarkerKind::kCuePoint)->text == "cue1");
  REQUIRE(by_kind(SmfMarkerKind::kMarker) != nullptr);
  REQUIRE(by_kind(SmfMarkerKind::kMarker)->text == "verse");

  const sonare::midi::SmfMarker* key = by_kind(SmfMarkerKind::kKeySignature);
  REQUIRE(key != nullptr);
  REQUIRE(key->key_fifths == 1);
  REQUIRE(key->key_minor == true);
  REQUIRE(key->text == "E minor");
}

TEST_CASE("SMF round-trips text-class meta kinds and key signatures", "[midi]") {
  using sonare::midi::SmfMarkerKind;
  const SmfImportResult imported = import_smf(make_meta_classes_smf());
  REQUIRE(imported.ok());

  SmfExportOptions opts;
  opts.ticks_per_quarter = 480;
  opts.markers = imported.markers;
  const auto exported = export_smf(imported.clips, imported.tempo_segments,
                                   imported.time_signatures, imported.clip_names, opts);
  REQUIRE(exported.ok());
  REQUIRE(exported.skipped_events == 0);

  const SmfImportResult round = import_smf(exported.bytes);
  REQUIRE(round.ok());
  REQUIRE(round.skipped_events == 0);
  REQUIRE(round.markers.size() == imported.markers.size());

  auto count_kind = [](const SmfImportResult& r, SmfMarkerKind kind) {
    int n = 0;
    for (const auto& m : r.markers) {
      if (m.kind == kind) ++n;
    }
    return n;
  };
  for (SmfMarkerKind kind : {SmfMarkerKind::kText, SmfMarkerKind::kLyric, SmfMarkerKind::kCuePoint,
                             SmfMarkerKind::kMarker, SmfMarkerKind::kKeySignature}) {
    REQUIRE(count_kind(round, kind) == count_kind(imported, kind));
  }

  for (const auto& m : round.markers) {
    if (m.kind == SmfMarkerKind::kKeySignature) {
      REQUIRE(m.key_fifths == 1);
      REQUIRE(m.key_minor == true);
    }
  }
}

TEST_CASE("SMF key-signature C major round-trips with sf=0 mi=0", "[midi]") {
  std::vector<uint8_t> body;
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x59);
  body.push_back(0x02);
  body.push_back(0x00);  // sf = 0.
  body.push_back(0x00);  // mi = 0 (major).
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x2F);
  body.push_back(0x00);

  const SmfImportResult imported = import_smf(wrap_format0_track(body));
  REQUIRE(imported.ok());
  REQUIRE(imported.markers.size() == 1);
  REQUIRE(imported.markers[0].kind == sonare::midi::SmfMarkerKind::kKeySignature);
  REQUIRE(imported.markers[0].key_fifths == 0);
  REQUIRE(imported.markers[0].key_minor == false);
  REQUIRE(imported.markers[0].text == "C major");
}

TEST_CASE("SMF time-signature metronome bytes round-trip", "[midi]") {
  std::vector<uint8_t> body;
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x58);
  body.push_back(0x04);
  body.push_back(0x07);  // 7/8.
  body.push_back(0x03);
  body.push_back(0x24);  // 36 MIDI clocks per metronome click.
  body.push_back(0x06);  // 6 notated 32nds per quarter.
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x2F);
  body.push_back(0x00);

  const SmfImportResult imported = import_smf(wrap_format0_track(body));
  REQUIRE(imported.ok());
  REQUIRE(imported.time_signatures.size() == 1);
  REQUIRE(imported.time_signatures[0].time_sig.numerator == 7);
  REQUIRE(imported.time_signatures[0].time_sig.denominator == 8);
  REQUIRE(imported.time_signatures[0].clocks_per_metronome_click == 36);
  REQUIRE(imported.time_signatures[0].thirty_seconds_per_quarter == 6);

  SmfExportOptions opts;
  opts.ticks_per_quarter = 480;
  const auto exported = export_smf({}, imported.tempo_segments, imported.time_signatures, {}, opts);
  REQUIRE(exported.ok());
  REQUIRE(exported.skipped_events == 0);

  const SmfImportResult round = import_smf(exported.bytes);
  REQUIRE(round.ok());
  REQUIRE(round.time_signatures.size() == 1);
  REQUIRE(round.time_signatures[0].time_sig.numerator == 7);
  REQUIRE(round.time_signatures[0].time_sig.denominator == 8);
  REQUIRE(round.time_signatures[0].clocks_per_metronome_click == 36);
  REQUIRE(round.time_signatures[0].thirty_seconds_per_quarter == 6);
}

TEST_CASE("SMF import skips a time-signature denominator the exporter cannot reproduce", "[midi]") {
  std::vector<uint8_t> body;
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x58);
  body.push_back(0x04);
  body.push_back(0x04);  // numerator 4.
  body.push_back(0x08);  // exponent 8 (denominator 256) — beyond the exporter's cap of 7.
  body.push_back(0x18);
  body.push_back(0x08);
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x2F);
  body.push_back(0x00);

  const SmfImportResult imported = import_smf(wrap_format0_track(body));
  REQUIRE(imported.ok());
  // The oversized denominator is dropped (counted as a skipped event), not
  // silently kept and re-quantized to 128 on a later export. The importer then
  // substitutes its default 4/4 segment since none was parsed.
  REQUIRE(imported.skipped_events >= 1);
  for (const auto& seg : imported.time_signatures) {
    REQUIRE(seg.time_sig.denominator != 256);
  }
}

TEST_CASE("SMF import derives clip length from end-of-track tick", "[midi]") {
  std::vector<uint8_t> body;
  body.push_back(0x00);
  body.push_back(0x90);
  body.push_back(60);
  body.push_back(100);
  body.push_back(0x87);
  body.push_back(0x40);  // delta 960 ticks = 2 PPQ at 480 PPQN.
  body.push_back(0xFF);
  body.push_back(0x2F);
  body.push_back(0x00);

  const SmfImportResult imported = import_smf(wrap_format0_track(body));
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.size() == 1);
  REQUIRE(imported.clips[0].events().size() == 1);
  REQUIRE(imported.clips[0].events()[0].ppq == 0.0);
  REQUIRE(imported.clip_lengths_ppq.size() == 1);
  REQUIRE(imported.clip_lengths_ppq[0] == 2.0);
}

TEST_CASE("SMF import and export preserve SysEx payloads via handles", "[midi]") {
  const std::vector<uint8_t> payload = {0x7E, 0x7F, 0x09, 0x01, 0xF7};
  const SmfImportResult imported = import_smf(make_sysex_smf(payload, /*delta=*/240));

  REQUIRE(imported.ok());
  REQUIRE(imported.skipped_events == 0);
  REQUIRE(imported.clips.size() == 1);
  REQUIRE(imported.clips[0].events().size() == 1);
  REQUIRE(imported.clips[0].events()[0].ppq == 0.5);

  const Ump& imported_ump = imported.clips[0].events()[0].ump;
  REQUIRE(imported_ump.sysex_handle != 0);
  REQUIRE(imported.sysex_store.size() == 1);
  const std::vector<uint8_t>* imported_payload =
      imported.sysex_store.lookup(imported_ump.sysex_handle);
  REQUIRE(imported_payload != nullptr);
  REQUIRE(*imported_payload == payload);

  SmfExportOptions opts;
  opts.ticks_per_quarter = 480;
  opts.sysex_store = &imported.sysex_store;
  const auto exported = export_smf(imported.clips, imported.tempo_segments,
                                   imported.time_signatures, imported.clip_names, opts);
  REQUIRE(exported.ok());
  REQUIRE(exported.skipped_events == 0);

  const SmfImportResult round = import_smf(exported.bytes);
  REQUIRE(round.ok());
  REQUIRE(round.skipped_events == 0);
  REQUIRE(round.clips.size() == 1);
  REQUIRE(round.clips[0].events().size() == 1);
  REQUIRE(round.clips[0].events()[0].ppq == 0.5);

  const Ump& round_ump = round.clips[0].events()[0].ump;
  REQUIRE(round_ump.sysex_handle != 0);
  const std::vector<uint8_t>* round_payload = round.sysex_store.lookup(round_ump.sysex_handle);
  REQUIRE(round_payload != nullptr);
  REQUIRE(*round_payload == payload);
}

TEST_CASE("SMF import skips invalid time signature denominator exponents", "[midi]") {
  std::vector<uint8_t> body;
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x58);
  body.push_back(0x04);
  body.push_back(0x04);
  body.push_back(0x1F);
  body.push_back(0x18);
  body.push_back(0x08);
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x2F);
  body.push_back(0x00);

  const SmfImportResult imported = import_smf(wrap_format0_track(body));

  REQUIRE(imported.ok());
  REQUIRE(imported.skipped_events == 1);
  REQUIRE(imported.time_signatures.size() == 1);
  REQUIRE(imported.time_signatures[0].time_sig.denominator == 4);
}

TEST_CASE("SMF export terminates SysEx payloads with F7", "[midi]") {
  sonare::midi::SysExStore store;
  const sonare::midi::SysExHandle handle = store.add(std::vector<uint8_t>{0xF0, 0x7D, 0x01});
  REQUIRE(handle != 0);

  MidiClip clip;
  clip.add_event(MidiClipEvent{0.0, sonare::midi::make_sysex_handle(0, handle)});

  SmfExportOptions opts;
  opts.sysex_store = &store;
  const auto exported = export_smf({clip}, {}, {}, {}, opts);
  REQUIRE(exported.ok());

  const SmfImportResult imported = import_smf(exported.bytes);

  REQUIRE(imported.ok());
  REQUIRE(imported.skipped_events == 0);
  REQUIRE(imported.clips.size() == 1);
  REQUIRE(imported.clips[0].events().size() == 1);
  const std::vector<uint8_t>* payload =
      imported.sysex_store.lookup(imported.clips[0].events()[0].ump.sysex_handle);
  REQUIRE(payload != nullptr);
  REQUIRE(*payload == std::vector<uint8_t>{0xF0, 0x7D, 0x01, 0xF7});
}

TEST_CASE("SMF import joins multi-packet SysEx continuations", "[midi]") {
  std::vector<uint8_t> body;
  body.push_back(0x00);
  body.push_back(0xF0);
  body.push_back(0x03);
  body.push_back(0x7E);
  body.push_back(0x7F);
  body.push_back(0x09);
  body.push_back(0x83);
  body.push_back(0x60);  // delta 480 ticks; continuation must keep original ppq.
  body.push_back(0xF7);
  body.push_back(0x02);
  body.push_back(0x01);
  body.push_back(0xF7);
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x2F);
  body.push_back(0x00);

  const SmfImportResult imported = import_smf(wrap_format0_track(body));
  REQUIRE(imported.ok());
  REQUIRE(imported.skipped_events == 0);
  REQUIRE(imported.clips.size() == 1);
  REQUIRE(imported.clips[0].events().size() == 1);
  REQUIRE(imported.clips[0].events()[0].ppq == 0.0);

  const Ump& ump = imported.clips[0].events()[0].ump;
  const std::vector<uint8_t>* payload = imported.sysex_store.lookup(ump.sysex_handle);
  REQUIRE(payload != nullptr);
  REQUIRE(*payload == std::vector<uint8_t>{0x7E, 0x7F, 0x09, 0x01, 0xF7});
}

TEST_CASE("SMF import preserves independent F7 escape SysEx payloads", "[midi]") {
  std::vector<uint8_t> body;
  body.push_back(0x00);
  body.push_back(0xF7);
  body.push_back(0x03);
  body.push_back(0x43);
  body.push_back(0x12);
  body.push_back(0x00);
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x2F);
  body.push_back(0x00);

  const SmfImportResult imported = import_smf(wrap_format0_track(body));
  REQUIRE(imported.ok());
  REQUIRE(imported.skipped_events == 0);
  REQUIRE(imported.clips.size() == 1);
  REQUIRE(imported.clips[0].events().size() == 1);

  const Ump& ump = imported.clips[0].events()[0].ump;
  const std::vector<uint8_t>* payload = imported.sysex_store.lookup(ump.sysex_handle);
  REQUIRE(payload != nullptr);
  REQUIRE(*payload == std::vector<uint8_t>{0x43, 0x12, 0x00});
}

TEST_CASE("SMF import skips system real-time bytes without clearing running status", "[midi]") {
  std::vector<uint8_t> body;
  body.push_back(0x00);
  body.push_back(0x90);
  body.push_back(60);
  body.push_back(100);
  body.push_back(0x00);
  body.push_back(0xF8);  // Timing clock: skipped, running status remains valid.
  body.push_back(0x00);
  body.push_back(62);
  body.push_back(100);
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x2F);
  body.push_back(0x00);

  const SmfImportResult r = import_smf(wrap_format0_track(body));
  REQUIRE(r.ok());
  REQUIRE(r.skipped_events == 1);
  REQUIRE(r.clips.size() == 1);
  REQUIRE(r.clips[0].events().size() == 2);
  REQUIRE(r.clips[0].events()[0].ump.note_number() == 60);
  REQUIRE(r.clips[0].events()[1].ump.note_number() == 62);
}

TEST_CASE("SMF import clears running status after meta events", "[midi]") {
  std::vector<uint8_t> body;
  body.push_back(0x00);
  body.push_back(0x90);
  body.push_back(60);
  body.push_back(100);
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x06);
  body.push_back(0x01);
  body.push_back('A');
  body.push_back(0x00);
  body.push_back(62);  // Invalid: running status must not survive the marker.
  body.push_back(100);

  const SmfImportResult r = import_smf(wrap_format0_track(body));
  REQUIRE_FALSE(r.ok());
  REQUIRE(r.status == SmfStatus::kTruncated);
}

TEST_CASE("SMF export counts skipped unresolved SysEx and MIDI 2.0-only events", "[midi]") {
  MidiClip clip;
  clip.add_event(MidiClipEvent{0.0, sonare::midi::make_sysex_handle(0, 999)});
  clip.add_event(
      MidiClipEvent{1.0, sonare::midi::make_midi2_per_note_controller(0, 0, 60, 1, 0x12345678u)});

  SmfExportOptions opts;
  opts.ticks_per_quarter = 480;
  const auto exported = export_smf({clip}, {}, {}, {}, opts);
  REQUIRE(exported.ok());
  REQUIRE(exported.skipped_events == 2);

  const SmfImportResult round = import_smf(exported.bytes);
  REQUIRE(round.ok());
  REQUIRE(round.clips.empty());
}

TEST_CASE("SMF export preserves MIDI 2.0 banked program changes as MIDI 1.0 bank select",
          "[midi]") {
  MidiClip clip;
  clip.add_event(
      MidiClipEvent{0.0, sonare::midi::make_midi2_program_change(/*group=*/0, /*channel=*/4,
                                                                 /*program=*/42, /*bank_msb=*/0x79,
                                                                 /*bank_lsb=*/3,
                                                                 /*bank_valid=*/true)});

  SmfExportOptions opts;
  opts.ticks_per_quarter = 480;
  const auto exported = export_smf({clip}, {}, {}, {"banked"}, opts);
  REQUIRE(exported.ok());
  REQUIRE(exported.skipped_events == 0);

  const SmfImportResult round = import_smf(exported.bytes);
  REQUIRE(round.ok());
  REQUIRE(round.clips.size() == 1);
  REQUIRE(round.clips[0].events().size() == 3);

  ProgramState state;
  for (const auto& ev : round.clips[0].events()) {
    REQUIRE(ev.ppq == 0.0);
    state.observe(ev.ump);
  }
  REQUIRE(state.program_seen);
  REQUIRE(state.current.bank_msb == 0x79u);
  REQUIRE(state.current.bank_lsb == 3u);
  REQUIRE(state.current.program == 42u);
}

TEST_CASE("SMF import rejects malformed and truncated input without crashing", "[midi]") {
  SECTION("empty input") {
    const SmfImportResult r = import_smf(nullptr, 0);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == SmfStatus::kInvalidArgument);
  }

  SECTION("bad magic") {
    std::vector<uint8_t> junk = {'X', 'X', 'X', 'X', 0, 0, 0, 6};
    const SmfImportResult r = import_smf(junk);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == SmfStatus::kBadHeader);
  }

  SECTION("format 2 unsupported") {
    std::vector<uint8_t> smf;
    push_tag(&smf, "MThd");
    push_u32(&smf, 6);
    push_u16(&smf, 2);  // Format 2.
    push_u16(&smf, 1);
    push_u16(&smf, 480);
    const SmfImportResult r = import_smf(smf);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == SmfStatus::kUnsupportedFormat);
  }

  SECTION("truncated header fields") {
    std::vector<uint8_t> smf;
    push_tag(&smf, "MThd");
    push_u32(&smf, 6);
    smf.push_back(0x00);  // Only one byte of the format word.
    const SmfImportResult r = import_smf(smf);
    REQUIRE_FALSE(r.ok());
    // Either truncated or bad-header is acceptable; must not crash.
    REQUIRE_FALSE(r.ok());
  }

  SECTION("truncated track body") {
    std::vector<uint8_t> smf;
    push_tag(&smf, "MThd");
    push_u32(&smf, 6);
    push_u16(&smf, 0);
    push_u16(&smf, 1);
    push_u16(&smf, 480);
    push_tag(&smf, "MTrk");
    push_u32(&smf, 100);  // Claims 100 bytes but provides none.
    const SmfImportResult r = import_smf(smf);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == SmfStatus::kTruncated);
  }

  SECTION("fuzzy prefix bytes never crash") {
    // Walk a few truncations of the known-good buffer; none may crash / OOB.
    const std::vector<uint8_t> good = make_known_smf();
    for (size_t cut = 0; cut < good.size(); cut += 3) {
      std::vector<uint8_t> partial(good.begin(), good.begin() + cut);
      const SmfImportResult r = import_smf(partial);
      // No assertion on status — only that the call returns (no crash / OOB).
      (void)r;
    }
    SUCCEED();
  }
}
