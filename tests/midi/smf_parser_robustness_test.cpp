/// @file smf_parser_robustness_test.cpp
/// @brief Regression coverage for SMF / SMF2 parser + serializer hardening:
///        per-track bounds clamping on import (a corrupt track no longer fails
///        the whole file), 4-byte VLQ clamping on export, split-SysEx
///        invalidation by an intervening event, and clip-length preservation
///        through the MIDI Clip File End-of-Clip marker.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <vector>

#include "midi/midi_clip.h"
#include "midi/smf.h"
#include "midi/smf2.h"
#include "midi/ump.h"
#include "transport/tempo_map.h"
#include "util/resource_limits.h"

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
using sonare::midi::Ump;

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

std::vector<uint8_t> make_running_status_smf(std::size_t event_count) {
  std::vector<uint8_t> body;
  body.reserve(event_count * 3u + 8u);
  for (std::size_t i = 0; i < event_count; ++i) {
    body.push_back(i == 0 ? 0x00u : 0x01u);
    if (i == 0) body.push_back(0x90u);
    body.push_back(static_cast<uint8_t>(60u + (i % 12u)));
    body.push_back(static_cast<uint8_t>(64u + (i % 64u)));
  }
  body.insert(body.end(), {0x00u, 0xFFu, 0x2Fu, 0x00u});
  return wrap_format0_track(body);
}

std::vector<uint8_t> make_sysex_budget_smf(std::size_t payload_size) {
  const std::size_t original_payload_size = payload_size;
  std::vector<uint8_t> body;
  body.reserve(payload_size + 16u);
  body.insert(body.end(), {0x00u, 0xF0u});

  uint8_t encoded[4]{};
  std::size_t encoded_count = 0;
  encoded[encoded_count++] = static_cast<uint8_t>(payload_size & 0x7Fu);
  for (payload_size >>= 7u; payload_size != 0u; payload_size >>= 7u) {
    encoded[encoded_count++] = static_cast<uint8_t>(payload_size & 0x7Fu);
  }
  while (encoded_count > 0u) {
    const std::size_t index = --encoded_count;
    body.push_back(static_cast<uint8_t>(encoded[index] | (index == 0u ? 0u : 0x80u)));
  }
  for (std::size_t i = 0; i < original_payload_size; ++i) {
    body.push_back(i + 1u == original_payload_size ? 0xF7u : static_cast<uint8_t>(i % 0x7Fu));
  }
  body.insert(body.end(), {0x00u, 0xFFu, 0x2Fu, 0x00u});
  return wrap_format0_track(body);
}

MidiClipEvent ev(double ppq, const Ump& ump) {
  MidiClipEvent e;
  e.ppq = ppq;
  e.ump = ump;
  return e;
}

}  // namespace

// A track with a corrupt in-track event length is skipped/repaired, and the
// following track still imports (instead of the whole file failing).
TEST_CASE("SMF import bounds a corrupt in-track meta length and keeps later tracks", "[midi]") {
  // Track 0: a valid note-on, then a text meta declaring 127 payload bytes while
  // only a handful remain before the (correctly framed) track boundary. The old
  // parser read that payload into the next track and failed the whole import.
  std::vector<uint8_t> track0;
  track0.push_back(0x00);
  track0.push_back(0x90);
  track0.push_back(0x3C);  // note 60
  track0.push_back(0x64);
  track0.push_back(0x00);
  track0.push_back(0xFF);
  track0.push_back(0x01);  // text meta
  track0.push_back(0x7F);  // declared length 127 (corrupt: overruns the track)
  track0.push_back('A');
  track0.push_back('B');
  track0.push_back(0x00);
  track0.push_back(0xFF);
  track0.push_back(0x2F);
  track0.push_back(0x00);

  // Track 1: a clean note (note 62) that must survive the corrupt track 0.
  std::vector<uint8_t> track1;
  track1.push_back(0x00);
  track1.push_back(0x90);
  track1.push_back(0x3E);  // note 62
  track1.push_back(0x64);
  track1.push_back(0x83);
  track1.push_back(0x60);  // delta 480
  track1.push_back(0x80);
  track1.push_back(0x3E);
  track1.push_back(0x00);
  track1.push_back(0x00);
  track1.push_back(0xFF);
  track1.push_back(0x2F);
  track1.push_back(0x00);

  std::vector<uint8_t> smf;
  push_tag(&smf, "MThd");
  push_u32(&smf, 6);
  push_u16(&smf, 1);  // format 1
  push_u16(&smf, 2);  // two tracks
  push_u16(&smf, 480);
  push_tag(&smf, "MTrk");
  push_u32(&smf, static_cast<uint32_t>(track0.size()));
  smf.insert(smf.end(), track0.begin(), track0.end());
  push_tag(&smf, "MTrk");
  push_u32(&smf, static_cast<uint32_t>(track1.size()));
  smf.insert(smf.end(), track1.begin(), track1.end());

  const SmfImportResult r = import_smf(smf);
  REQUIRE_FALSE(r.ok());
  REQUIRE(r.status == SmfStatus::kTruncated);
  REQUIRE(r.diagnostic.find("truncated") != std::string::npos);
  REQUIRE(r.skipped_events >= 1);
  // Both tracks yield a clip; the corrupt track keeps only its pre-corruption
  // note-on, and the second track is fully intact for diagnostics/recovery, but
  // callers cannot mistake the partial result for a lossless success.
  REQUIRE(r.clips.size() == 2);

  bool found_60 = false;
  bool found_62 = false;
  for (const auto& clip : r.clips) {
    for (const auto& e : clip.events()) {
      if (e.ump.is_note_on() && e.ump.note_number() == 60) found_60 = true;
      if (e.ump.note_number() == 62) found_62 = true;
    }
  }
  REQUIRE(found_60);
  REQUIRE(found_62);
}

// A track whose trailing delta-time VLQ starts inside the declared MTrk
// length but decodes past it (the VLQ continuation bit chases into the next
// chunk's bytes) still resynchronizes at the correct next-track boundary,
// recovering both the corrupt track's valid prefix and the following track,
// instead of desyncing and failing the whole import.
TEST_CASE("SMF import resyncs after a trailing VLQ overruns a non-final track boundary", "[midi]") {
  // Track 0: a valid note-on/note-off pair, then a single trailing byte with
  // the VLQ continuation bit set (0x81) as the last byte before the declared
  // track boundary. Decoding that delta consumes one more byte past the
  // boundary (the first byte of track 1's "MTrk" tag) before the per-read
  // overrun check catches it.
  std::vector<uint8_t> track0;
  track0.push_back(0x00);
  track0.push_back(0x90);
  track0.push_back(0x3C);  // note 60
  track0.push_back(0x64);
  track0.push_back(0x00);
  track0.push_back(0x80);
  track0.push_back(0x3C);  // note 60 off
  track0.push_back(0x00);
  track0.push_back(0x81);  // dangling VLQ continuation byte, no terminator in track0

  // Track 1: a clean note (note 62) that must survive the corrupt track 0.
  std::vector<uint8_t> track1;
  track1.push_back(0x00);
  track1.push_back(0x90);
  track1.push_back(0x3E);  // note 62
  track1.push_back(0x64);
  track1.push_back(0x83);
  track1.push_back(0x60);  // delta 480
  track1.push_back(0x80);
  track1.push_back(0x3E);
  track1.push_back(0x00);
  track1.push_back(0x00);
  track1.push_back(0xFF);
  track1.push_back(0x2F);
  track1.push_back(0x00);

  std::vector<uint8_t> smf;
  push_tag(&smf, "MThd");
  push_u32(&smf, 6);
  push_u16(&smf, 1);  // format 1
  push_u16(&smf, 2);  // two tracks
  push_u16(&smf, 480);
  push_tag(&smf, "MTrk");
  push_u32(&smf, static_cast<uint32_t>(track0.size()));
  smf.insert(smf.end(), track0.begin(), track0.end());
  push_tag(&smf, "MTrk");
  push_u32(&smf, static_cast<uint32_t>(track1.size()));
  smf.insert(smf.end(), track1.begin(), track1.end());

  const SmfImportResult r = import_smf(smf);
  REQUIRE_FALSE(r.ok());
  REQUIRE(r.status == SmfStatus::kTruncated);
  REQUIRE(r.diagnostic.find("truncated") != std::string::npos);
  REQUIRE(r.skipped_events >= 1);
  REQUIRE(r.clips.size() == 2);

  bool found_60 = false;
  bool found_62 = false;
  for (const auto& clip : r.clips) {
    for (const auto& e : clip.events()) {
      if (e.ump.note_number() == 60) found_60 = true;
      if (e.ump.note_number() == 62) found_62 = true;
    }
  }
  REQUIRE(found_60);
  REQUIRE(found_62);
}

// A delta beyond the 4-byte VLQ range is clamped on export so it stays readable
// (the reader rejects a 5-byte VLQ).
TEST_CASE("SMF export clamps a delta beyond the 4-byte VLQ range so it re-imports", "[midi]") {
  // At 480 PPQN, ppq 600000 -> 288,000,000 ticks, which needs 29 bits (a 5-byte
  // VLQ). Unclamped, that byte stream is unreadable by this library.
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  clip.add_event(ev(600000.0, sonare::midi::make_midi1_note_off(0, 0, 60, 0)));

  SmfExportOptions opts;
  opts.ticks_per_quarter = 480;
  const auto exported = export_smf({clip}, {}, {}, {}, opts);
  REQUIRE(exported.ok());

  const SmfImportResult round = import_smf(exported.bytes);
  REQUIRE(round.ok());  // Would fail before the clamp (overflowing 5-byte VLQ).
  REQUIRE(round.clips.size() == 1);
  REQUIRE(round.clips[0].events().size() == 2);

  // The note-off lands at the clamped maximum tick (0x0FFFFFFF / 480 PPQN),
  // shortened from the original 600000 but readable and finite.
  const double clamped_ppq = static_cast<double>(0x0FFFFFFF) / 480.0;
  double note_off_ppq = -1.0;
  for (const auto& e : round.clips[0].events()) {
    if (e.ump.is_note_off() && e.ump.note_number() == 60) note_off_ppq = e.ppq;
  }
  REQUIRE(note_off_ppq == Catch::Approx(clamped_ppq));
  REQUIRE(note_off_ppq < 600000.0);
}

TEST_CASE("SMF import keeps cumulative ticks beyond uint32 without wrapping", "[midi][smf_tick]") {
  std::vector<uint8_t> body;
  constexpr int kEvents = 17;
  for (int i = 0; i < kEvents; ++i) {
    body.insert(body.end(), {0xFF, 0xFF, 0xFF, 0x7F});  // max 28-bit delta
    body.insert(body.end(), {0x90, static_cast<uint8_t>(60 + (i % 4)), 0x40});
  }
  body.insert(body.end(), {0x00, 0xFF, 0x2F, 0x00});

  const SmfImportResult imported = import_smf(wrap_format0_track(body));
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.size() == 1);
  REQUIRE(imported.clips[0].events().size() == kEvents);
  const double expected_ticks = static_cast<double>(kEvents) * static_cast<double>(0x0FFFFFFF);
  REQUIRE(imported.clips[0].events().back().ppq == Catch::Approx(expected_ticks / 480.0));
  REQUIRE(imported.clips[0].events().back().ppq >
          static_cast<double>(std::numeric_limits<uint32_t>::max()) / 480.0);
}

TEST_CASE("SMF import enforces track event metadata and SysEx budgets before expansion",
          "[midi][resource_limit]") {
  auto limits = sonare::resource::kDefaultMidiImportResourceLimits;

  std::vector<uint8_t> two_events = {0x00, 0x90, 0x3C, 0x40, 0x00, 0x80,
                                     0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00};
  limits.max_events = 1;
  auto imported = import_smf(wrap_format0_track(two_events), limits);
  REQUIRE_FALSE(imported.ok());
  REQUIRE(imported.status == sonare::midi::SmfStatus::kInvalidArgument);
  REQUIRE(imported.diagnostic.find("resource limit") != std::string::npos);

  limits = sonare::resource::kDefaultMidiImportResourceLimits;
  limits.max_tracks = 0;
  imported = import_smf(wrap_format0_track({0x00, 0xFF, 0x2F, 0x00}), limits);
  REQUIRE_FALSE(imported.ok());
  REQUIRE(imported.status == sonare::midi::SmfStatus::kInvalidArgument);

  limits = sonare::resource::kDefaultMidiImportResourceLimits;
  limits.max_metadata_bytes = 1;
  imported = import_smf(
      wrap_format0_track({0x00, 0xFF, 0x03, 0x02, 'a', 'b', 0x00, 0xFF, 0x2F, 0x00}), limits);
  REQUIRE_FALSE(imported.ok());
  REQUIRE(imported.status == sonare::midi::SmfStatus::kInvalidArgument);

  limits = sonare::resource::kDefaultMidiImportResourceLimits;
  limits.max_sysex_bytes = 2;
  imported = import_smf(
      wrap_format0_track({0x00, 0xF0, 0x03, 0x01, 0x02, 0xF7, 0x00, 0xFF, 0x2F, 0x00}), limits);
  REQUIRE_FALSE(imported.ok());
  REQUIRE(imported.status == sonare::midi::SmfStatus::kInvalidArgument);
}

TEST_CASE("SMF import accepts exactly its raw event budget", "[.][slow][midi][resource_limit]") {
  const std::size_t cap = sonare::resource::kDefaultMidiImportResourceLimits.max_events;
  const auto at_cap = import_smf(make_running_status_smf(cap));
  REQUIRE(at_cap.ok());
  REQUIRE(at_cap.clips.size() == 1);
  REQUIRE(at_cap.clips.front().events().size() == cap);

  const auto over_cap = import_smf(make_running_status_smf(cap + 1u));
  REQUIRE_FALSE(over_cap.ok());
  REQUIRE(over_cap.status == sonare::midi::SmfStatus::kInvalidArgument);
  REQUIRE(over_cap.diagnostic.find("resource limit") != std::string::npos);
}

TEST_CASE("SMF import accepts its raw SysEx byte budget", "[.][slow][midi][resource_limit]") {
  constexpr std::size_t kPayloadBytes = 32u * 1024u * 1024u;
  const auto at_cap = import_smf(make_sysex_budget_smf(kPayloadBytes));
  REQUIRE(at_cap.ok());
  REQUIRE(at_cap.clips.size() == 1);
  REQUIRE(at_cap.clips.front().events().size() == 1);
  const auto* payload =
      at_cap.sysex_store.lookup(at_cap.clips.front().events().front().ump.sysex_handle);
  REQUIRE(payload != nullptr);
  REQUIRE(payload->size() == kPayloadBytes);
}

TEST_CASE("SMF2 import enforces file and event budgets before normalized allocation",
          "[midi][smf2][resource_limit]") {
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  clip.add_event(ev(1.0, sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  const auto exported = export_clip_file(clip, {}, {}, {});
  REQUIRE(exported.ok());

  auto limits = sonare::resource::kDefaultMidiImportResourceLimits;
  limits.max_file_bytes = exported.bytes.size() - 1u;
  auto imported = import_clip_file(exported.bytes, limits);
  REQUIRE_FALSE(imported.ok());
  REQUIRE(imported.status == sonare::midi::Smf2Status::kInvalidArgument);

  limits = sonare::resource::kDefaultMidiImportResourceLimits;
  limits.max_events = 1;
  imported = import_clip_file(exported.bytes, limits);
  REQUIRE_FALSE(imported.ok());
  REQUIRE(imported.status == sonare::midi::Smf2Status::kInvalidArgument);
  REQUIRE(imported.diagnostic.find("resource limit") != std::string::npos);
}

TEST_CASE("SMF export enforces the 16-bit track-count boundary", "[midi][smf_track_count][slow]") {
  constexpr size_t kMaxDataTracks = static_cast<size_t>(std::numeric_limits<uint16_t>::max()) - 1;
  std::vector<MidiClip> clips(kMaxDataTracks);
  const auto boundary = export_smf(clips, {}, {});
  REQUIRE(boundary.ok());
  REQUIRE(boundary.bytes.size() >= 12);
  REQUIRE(boundary.bytes[10] == 0xFF);
  REQUIRE(boundary.bytes[11] == 0xFF);

  clips.emplace_back();
  const auto overflow = export_smf(clips, {}, {});
  REQUIRE_FALSE(overflow.ok());
  REQUIRE(overflow.bytes.empty());
  REQUIRE(overflow.diagnostic.find("65534") != std::string::npos);
}

// An intervening event discards a pending split SysEx so a later F7 packet
// cannot concatenate onto the stale bytes.
TEST_CASE("SMF import discards a pending split SysEx when an event interrupts it", "[midi]") {
  std::vector<uint8_t> body;
  // F0 start with two bytes and NO terminal F7 -> a split dump stays pending.
  body.push_back(0x00);
  body.push_back(0xF0);
  body.push_back(0x02);
  body.push_back(0x7E);
  body.push_back(0x7F);
  // Intervening note-on: the pending split dump must be discarded here.
  body.push_back(0x00);
  body.push_back(0x90);
  body.push_back(0x3C);
  body.push_back(0x64);
  // F7 escape with its own complete payload. With the pending dump discarded it
  // is an independent event; it must NOT append onto the stale {7E,7F}.
  body.push_back(0x00);
  body.push_back(0xF7);
  body.push_back(0x02);
  body.push_back(0x01);
  body.push_back(0xF7);
  body.push_back(0x00);
  body.push_back(0xFF);
  body.push_back(0x2F);
  body.push_back(0x00);

  const SmfImportResult r = import_smf(wrap_format0_track(body));
  REQUIRE(r.ok());
  // The abandoned split dump is counted as one skipped event.
  REQUIRE(r.skipped_events == 1);
  REQUIRE(r.clips.size() == 1);

  const auto& events = r.clips[0].events();
  REQUIRE(events.size() == 2);  // note-on + the independent F7 event.

  const Ump* sysex = nullptr;
  for (const auto& e : events) {
    if (e.ump.sysex_handle != 0) sysex = &e.ump;
  }
  REQUIRE(sysex != nullptr);
  const std::vector<uint8_t>* payload = r.sysex_store.lookup(sysex->sysex_handle);
  REQUIRE(payload != nullptr);
  // Only the F7 escape bytes, with no stale {7E,7F} prefix concatenated on.
  REQUIRE(*payload == std::vector<uint8_t>{0x01, 0xF7});
}

// A MIDI Clip File keeps the clip length past the last event, so trailing
// silence / sustained tails survive the round trip.
TEST_CASE("SMF2 export preserves clip length beyond the final event", "[midi][smf2]") {
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  clip.add_event(ev(2.0, sonare::midi::make_midi1_note_off(0, 0, 60, 0)));

  Smf2ExportOptions options;
  options.ticks_per_quarter = 480;
  options.length_ppq = 8.0;  // Clip extends to ppq 8 (six beats of trailing tail).

  const auto exported = export_clip_file(clip, {}, {}, options);
  REQUIRE(exported.ok());

  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.size() == 1);
  REQUIRE(imported.clips[0].events().size() == 2);
  REQUIRE(imported.clip_lengths_ppq.size() == 1);
  // Without threading the length through, End-of-Clip would sit at ppq 2.0.
  REQUIRE(imported.clip_lengths_ppq.front() == Catch::Approx(8.0));
}
