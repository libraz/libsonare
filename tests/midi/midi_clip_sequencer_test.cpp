/// @file midi_clip_sequencer_test.cpp
/// @brief MIDI core: MidiClip ordering / note-pair validation /
///        PPQ->frame rendering, and the RT MidiSequencer dispatch +
///        hang-note safety + overflow + boundary collection.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "midi/midi_clip.h"
#include "midi/midi_clip_envelope.h"
#include "midi/midi_event.h"
#include "midi/prepared_sysex.h"
#include "midi/sequencer.h"
#include "midi/ump.h"
#include "transport/tempo_map.h"
#include "util/exception.h"

namespace {

using sonare::midi::DeviceFrame;
using sonare::midi::MidiClip;
using sonare::midi::MidiClipEvent;
using sonare::midi::MidiClipSchedule;
using sonare::midi::MidiEvent;
using sonare::midi::MidiEventSink;
using sonare::midi::MidiFxChain;
using sonare::midi::MidiSequencer;
using sonare::midi::MidiSysExPayloadBank;
using sonare::midi::PreparedMidiSysEx;
using sonare::midi::SequencerClock;
using sonare::midi::Ump;

struct TestPreparedMidiSysEx final : PreparedMidiSysEx {
  explicit TestPreparedMidiSysEx(int value) : value(value) {}
  int value = 0;
};

MidiClipEvent ev(double ppq, const Ump& ump) {
  MidiClipEvent e;
  e.ppq = ppq;
  e.ump = ump;
  return e;
}

Ump raw_ump(uint8_t message_type, uint8_t status, uint8_t note = 60, uint8_t data2 = 100) {
  Ump packet;
  packet.words[0] = (static_cast<uint32_t>(message_type) << 28) |
                    (static_cast<uint32_t>(status) << 20) | (static_cast<uint32_t>(note) << 8) |
                    data2;
  packet.word_count = sonare::midi::ump_word_count_for_word0(packet.words[0]);
  return packet;
}

// A capturing test sink that records dispatched events into a vector. Used only
// for dispatch-correctness tests (NOT the no-alloc test, which uses a fixed
// counter sink).
class CapturingSink final : public MidiEventSink {
 public:
  struct Captured {
    uint32_t destination;
    MidiEvent event;
  };
  void on_event(uint32_t destination, const MidiEvent& event) noexcept override {
    events.push_back({destination, event});
  }
  std::vector<Captured> events;
};

void init_tempo_map(sonare::transport::TempoMap* map, double bpm = 120.0,
                    double sample_rate = 48000.0) {
  map->prepare(sample_rate);
  sonare::transport::TempoSegment seg;
  seg.start_ppq = 0.0;
  seg.bpm = bpm;
  seg.start_sample = 0.0;
  map->set_segments({seg});
}

// Offsets in [0, num_frames) from @p start at which the sequencer holds an
// event, found by walking frames_until_next_event from the frame before.
std::vector<int> event_offsets(const MidiSequencer& seq, int64_t start, int num_frames) {
  std::vector<int> offsets;
  int64_t frame = start - 1;
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  const int64_t end = start > kMax - num_frames ? kMax : start + num_frames;
  while (frame < end) {
    frame +=
        seq.frames_until_next_event(SequencerClock::aligned(frame), static_cast<int>(end - frame));
    if (frame < end) offsets.push_back(static_cast<int>(frame - start));
  }
  return offsets;
}

}  // namespace

static_assert(std::is_trivially_copyable_v<MidiEvent>);
static_assert(sizeof(MidiSequencer) < 64u * 1024u,
              "MidiSequencer must keep its large MIDI-FX work buffers off the stack");

TEST_CASE("MidiClip SysEx preparation survives source bank destruction", "[midi][sysex]") {
  const std::vector<uint8_t> expected{0xF0, 0x7D, 0x41, 0x42, 0xF7};
  std::weak_ptr<const TestPreparedMidiSysEx> weak_prepared;
  std::vector<MidiClipSchedule> schedules(1);
  {
    auto source_bank = std::make_shared<MidiSysExPayloadBank>();
    source_bank->payloads.push_back(expected);
    auto prepared = std::make_shared<TestPreparedMidiSysEx>(17);
    weak_prepared = prepared;
    source_bank->prepared_operations.push_back(prepared);
    MidiEvent event;
    event.render_frame = 10;
    event.ump = sonare::midi::make_sysex_handle(0, 1);
    event.sysex_payload = source_bank->payloads.front().data();
    event.sysex_payload_size = expected.size();
    event.prepared_sysex = prepared.get();
    schedules.front().events = {event};
    schedules.front().sysex_payload_bank = source_bank;
    REQUIRE(sonare::midi::own_sysex_payloads(schedules));
  }

  REQUIRE_FALSE(weak_prepared.expired());
  REQUIRE(schedules.front().events.front().prepared_sysex != nullptr);
  REQUIRE(static_cast<const TestPreparedMidiSysEx*>(schedules.front().events.front().prepared_sysex)
              ->value == 17);
  REQUIRE(std::vector<uint8_t>(schedules.front().events.front().sysex_payload,
                               schedules.front().events.front().sysex_payload + expected.size()) ==
          expected);
}

TEST_CASE("MidiClip SysEx preparation preserves a token borrowed by a later schedule",
          "[midi][sysex]") {
  const std::vector<uint8_t> expected{0xF0, 0x7D, 0x31, 0x32, 0xF7};
  std::vector<MidiClipSchedule> schedules(1);
  auto source_bank = std::make_shared<MidiSysExPayloadBank>();
  source_bank->payloads.push_back(expected);
  auto prepared = std::make_shared<TestPreparedMidiSysEx>(31);
  source_bank->prepared_operations.push_back(prepared);

  MidiEvent first;
  first.render_frame = 10;
  first.ump = sonare::midi::make_sysex_handle(0, 2);
  first.sysex_payload = source_bank->payloads.front().data();
  first.sysex_payload_size = expected.size();
  first.prepared_sysex = prepared.get();
  schedules.front().events = {first};
  schedules.front().sysex_payload_bank = source_bank;

  MidiClipSchedule second;
  MidiEvent borrowed = first;
  borrowed.render_frame = 20;
  borrowed.ump = sonare::midi::make_sysex_handle(0, 3);
  second.events = {borrowed};
  schedules.push_back(std::move(second));

  REQUIRE(sonare::midi::own_sysex_payloads(schedules));
  for (const MidiClipSchedule& schedule : schedules) {
    REQUIRE(schedule.events.front().prepared_sysex != nullptr);
    REQUIRE(
        static_cast<const TestPreparedMidiSysEx*>(schedule.events.front().prepared_sysex)->value ==
        31);
    REQUIRE(std::vector<uint8_t>(schedule.events.front().sysex_payload,
                                 schedule.events.front().sysex_payload + expected.size()) ==
            expected);
  }
}

TEST_CASE("MidiClip SysEx preparation failure leaves schedules untouched", "[midi][sysex]") {
  const std::vector<uint8_t> source{0xF0, 0x7D, 0x55, 0xF7};
  MidiClipSchedule schedule;
  MidiEvent event;
  event.ump = sonare::midi::make_sysex_handle(0, 4);
  event.sysex_payload = source.data();
  event.sysex_payload_size = source.size();
  schedule.events = {event};
  std::vector<MidiClipSchedule> schedules{schedule};
  const uint8_t* original_pointer = schedules.front().events.front().sysex_payload;
  sonare::midi::MidiSysExPayloadError error = sonare::midi::MidiSysExPayloadError::kNone;
  REQUIRE_FALSE(sonare::midi::own_sysex_payloads(
      schedules, &error,
      [](uint32_t, const uint8_t*, size_t, std::shared_ptr<const PreparedMidiSysEx>&) {
        return false;
      }));
  REQUIRE(error == sonare::midi::MidiSysExPayloadError::kPreparationFailed);
  REQUIRE(schedules.front().events.front().sysex_payload == original_pointer);
  REQUIRE(schedules.front().events.front().sysex_payload_size == source.size());
  REQUIRE(schedules.front().events.front().prepared_sysex == nullptr);
}

TEST_CASE("MidiClip SysEx allocation failure is reported separately", "[midi][sysex]") {
  const std::vector<uint8_t> source{0xF0, 0x7D, 0x56, 0xF7};
  MidiClipSchedule schedule;
  MidiEvent event;
  event.ump = sonare::midi::make_sysex_handle(0, 5);
  event.sysex_payload = source.data();
  event.sysex_payload_size = source.size();
  schedule.events = {event};
  std::vector<MidiClipSchedule> schedules{schedule};
  const uint8_t* original_pointer = schedules.front().events.front().sysex_payload;
  sonare::midi::MidiSysExPayloadError error = sonare::midi::MidiSysExPayloadError::kNone;

  REQUIRE_FALSE(sonare::midi::own_sysex_payloads(
      schedules, &error,
      [](uint32_t, const uint8_t*, size_t, std::shared_ptr<const PreparedMidiSysEx>&) -> bool {
        throw std::bad_alloc();
      }));
  REQUIRE(error == sonare::midi::MidiSysExPayloadError::kOutOfMemory);
  REQUIRE(schedules.front().events.front().sysex_payload == original_pointer);
  REQUIRE(schedules.front().events.front().sysex_payload_size == source.size());
  REQUIRE(schedules.front().events.front().prepared_sysex == nullptr);
}

TEST_CASE("MidiSequencer bypasses MIDI FX timing for SysEx", "[midi][sysex]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 100;
  quantize.strength = 1.0f;
  fx.set_quantize(quantize);
  REQUIRE(seq.set_midi_fx(9, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  const std::vector<uint8_t> payload{0xF0, 0x7D, 0x66, 0xF7};
  MidiClipSchedule clip;
  clip.destination_id = 9;
  MidiEvent sysex;
  sysex.render_frame = 60;
  sysex.ump = sonare::midi::make_sysex_handle(0, 5);
  sysex.sysex_payload = payload.data();
  sysex.sysex_payload_size = payload.size();
  MidiEvent note;
  note.render_frame = 60;
  note.ump = sonare::midi::make_midi1_note_on(0, 0, 60, 100);
  clip.events = {sysex, note};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 128);

  REQUIRE(sink.events.size() == 2);
  REQUIRE(sink.events[0].event.ump.message_type() == sonare::midi::UmpMessageType::kData64);
  REQUIRE(sink.events[0].event.render_frame == 60);
  REQUIRE(sink.events[1].event.ump.is_note_on());
  REQUIRE(sink.events[1].event.render_frame == 100);
}

TEST_CASE("MidiFxChain keeps SysEx opaque and synchronous", "[midi][sysex]") {
  MidiFxChain fx;
  sonare::midi::QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 100;
  quantize.strength = 1.0f;
  fx.set_quantize(quantize);

  const std::vector<uint8_t> payload{0xF0, 0x7D, 0x67, 0xF7};
  TestPreparedMidiSysEx prepared(23);
  MidiEvent input;
  input.render_frame = 60;
  input.ump = sonare::midi::make_sysex_handle(0, 6);
  input.sysex_payload = payload.data();
  input.sysex_payload_size = payload.size();
  input.prepared_sysex = &prepared;

  sonare::midi::MidiFxBuffer output;
  fx.process(&input, 1, &output);

  REQUIRE(output.size == 1);
  REQUIRE(output.events[0].render_frame == 60);
  REQUIRE(output.events[0].sysex_payload == payload.data());
  REQUIRE(output.events[0].sysex_payload_size == payload.size());
  REQUIRE(output.events[0].prepared_sysex == &prepared);
}

TEST_CASE("MidiSequencer reports every event frame of a dense block", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);
  MidiClipSchedule clip;
  for (int frame = 0; frame < 80; ++frame) {
    clip.events.push_back({frame, sonare::midi::make_midi1_control_change(
                                      0, 0, static_cast<uint8_t>(frame & 0x7F), 1)});
  }
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();

  const std::vector<int> offsets = event_offsets(seq, 0, 80);
  REQUIRE(offsets.size() == 80);
  for (size_t i = 0; i < offsets.size(); ++i) REQUIRE(offsets[i] == static_cast<int>(i));
  const std::vector<int> shifted = event_offsets(seq, 40, 40);
  REQUIRE(shifted.size() == 40);
  for (size_t i = 0; i < shifted.size(); ++i) REQUIRE(shifted[i] == static_cast<int>(i));
}

TEST_CASE("MidiClip sort_stable orders by ppq, note-off before note-on, stable tiebreak",
          "[midi]") {
  MidiClip clip;
  // Insert out of order; same ppq=1.0 carries a note-off and a note-on which
  // must end up note-off first.
  clip.add_event(ev(2.0, sonare::midi::make_midi1_note_off(0, 0, 64, 0)));
  clip.add_event(ev(1.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  clip.add_event(ev(1.0, sonare::midi::make_midi1_note_off(0, 0, 62, 0)));
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 67, 80)));
  // Two note-ons at the same ppq with different note numbers: stable tiebreak
  // orders by note number ascending.
  clip.add_event(ev(1.0, sonare::midi::make_midi1_note_on(0, 0, 72, 90)));
  clip.add_event(ev(1.0, sonare::midi::make_midi1_note_on(0, 0, 65, 90)));

  clip.sort_stable();
  const auto& e = clip.events();
  REQUIRE(e.size() == 6);
  REQUIRE(e[0].ppq == 0.0);
  REQUIRE(e[0].ump.note_number() == 67);
  // ppq == 1.0 group: note-off (rank 0) precedes note-ons (rank 2).
  REQUIRE(e[1].ppq == 1.0);
  REQUIRE(e[1].ump.is_note_off());
  REQUIRE(e[1].ump.note_number() == 62);
  // Then the note-ons in ascending note order: 65, 72.
  REQUIRE(e[2].ump.is_note_on());
  REQUIRE(e[2].ump.note_number() == 60);
  REQUIRE(e[3].ump.note_number() == 65);
  REQUIRE(e[4].ump.note_number() == 72);
  REQUIRE(e[5].ppq == 2.0);

  // Idempotent: re-sorting does not change order.
  const auto before = clip.events();
  clip.sort_stable();
  REQUIRE(clip.events() == before);
}

TEST_CASE("same-time ranking treats only channel voice notes as notes", "[midi]") {
  using sonare::midi::kGeneralRank;
  using sonare::midi::same_time_rank;

  const Ump midi1_zero_velocity = sonare::midi::make_midi1_note_on(0, 0, 60, 0);
  const Ump midi2_zero_velocity = sonare::midi::make_midi2_note_on(0, 0, 60, 0);
  REQUIRE(same_time_rank(midi1_zero_velocity) == 0);
  REQUIRE(same_time_rank(midi2_zero_velocity) == 5);
  REQUIRE(same_time_rank(sonare::midi::make_midi1_note_off(0, 0, 60, 0)) == 0);
  REQUIRE(same_time_rank(sonare::midi::make_midi2_note_off(0, 0, 60, 0)) == 0);

  for (uint8_t message_type = 0; message_type < 16; ++message_type) {
    if (message_type == static_cast<uint8_t>(sonare::midi::UmpMessageType::kMidi1ChannelVoice) ||
        message_type == static_cast<uint8_t>(sonare::midi::UmpMessageType::kMidi2ChannelVoice)) {
      continue;
    }
    CHECK(same_time_rank(
              raw_ump(message_type, static_cast<uint8_t>(sonare::midi::UmpStatus::kNoteOff))) ==
          kGeneralRank);
    CHECK(same_time_rank(
              raw_ump(message_type, static_cast<uint8_t>(sonare::midi::UmpStatus::kNoteOn))) ==
          kGeneralRank);
  }
}

TEST_CASE("sort_render_events_stable orders same-frame events note-off before note-on", "[midi]") {
  // The live/realtime clip paths (C-ABI and WASM setMidiClips) feed absolute
  // render-frame events through this shared sort. A same-frame re-trigger must
  // release before re-attacking, matching the offline MidiClip path — otherwise
  // the new note-on can be dropped/blipped.
  auto re = [](int64_t frame, const Ump& ump) {
    MidiEvent e;
    e.render_frame = frame;
    e.ump = ump;
    return e;
  };
  std::vector<MidiEvent> events;
  // Note-on inserted BEFORE the note-off at the SAME frame (480).
  events.push_back(re(480, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  events.push_back(re(480, sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  // Two note-ons at the same frame: deterministic ascending-note tiebreak.
  events.push_back(re(480, sonare::midi::make_midi1_note_on(0, 0, 72, 90)));
  events.push_back(re(480, sonare::midi::make_midi1_note_on(0, 0, 64, 90)));
  events.push_back(re(0, sonare::midi::make_midi1_note_on(0, 0, 48, 80)));

  sonare::midi::sort_render_events_stable(events);
  REQUIRE(events.size() == 5);
  REQUIRE(events[0].render_frame == 0);
  REQUIRE(events[0].ump.note_number() == 48);
  // Frame 480 group: note-off (rank 0) precedes the note-ons.
  REQUIRE(events[1].render_frame == 480);
  REQUIRE(events[1].ump.is_note_off());
  REQUIRE(events[1].ump.note_number() == 60);
  // Then note-ons in ascending note order: 60, 64, 72.
  REQUIRE(events[2].ump.is_note_on());
  REQUIRE(events[2].ump.note_number() == 60);
  REQUIRE(events[3].ump.note_number() == 64);
  REQUIRE(events[4].ump.note_number() == 72);

  // Idempotent.
  const auto before = events;
  sonare::midi::sort_render_events_stable(events);
  REQUIRE(events == before);
}

TEST_CASE("a same-timestamp controller gesture keeps the order it was written in", "[midi]") {
  // An RPN is CC101, CC100 then Data Entry, and a file writes all three at one
  // tick. Ordering same-timestamp events by controller number delivers them as
  // 6, 100, 101 -- the value ahead of the selector that gives it meaning -- and
  // a receiver that discards unselected data entry then drops the whole edit,
  // which is what happened to every RPN and NRPN reaching the synth.
  const auto rpn_at = [](double ppq) {
    return std::vector<MidiClipEvent>{
        ev(ppq, sonare::midi::make_midi1_control_change(0, 0, 101, 0)),
        ev(ppq, sonare::midi::make_midi1_control_change(0, 0, 100, 0)),
        ev(ppq, sonare::midi::make_midi1_control_change(0, 0, 6, 12)),
        ev(ppq, sonare::midi::make_midi1_control_change(0, 0, 38, 0)),
    };
  };
  const std::vector<uint8_t> written{101, 100, 6, 38};

  MidiClip clip;
  for (const MidiClipEvent& e : rpn_at(1.0)) clip.add_event(e);
  // A note-on at the same tick, added first, still has to end up last: the rank
  // ordering is what the gesture rides on and it is not being given up here.
  clip.add_event(ev(1.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  clip.sort_stable();

  REQUIRE(clip.events().size() == 5);
  for (size_t i = 0; i < written.size(); ++i) {
    REQUIRE(clip.events()[i].ump.note_number() == written[i]);
  }
  REQUIRE(clip.events()[4].ump.is_note_on());

  // Idempotent: a second sort must not start reordering what the first kept.
  const auto before = clip.events();
  clip.sort_stable();
  REQUIRE(clip.events() == before);
}

TEST_CASE("sort_render_events_stable keeps a same-frame controller gesture in order", "[midi]") {
  // The live and baked paths sort render-frame events with the same comparator,
  // so the gesture has to survive there too.
  auto re = [](int64_t frame, const Ump& ump) {
    MidiEvent e;
    e.render_frame = frame;
    e.ump = ump;
    return e;
  };
  const std::vector<uint8_t> written{99, 98, 6};
  std::vector<MidiEvent> events{
      re(480, sonare::midi::make_midi1_note_on(0, 0, 60, 100)),
      re(480, sonare::midi::make_midi1_control_change(0, 0, 99, 0x01)),
      re(480, sonare::midi::make_midi1_control_change(0, 0, 98, 0x20)),
      re(480, sonare::midi::make_midi1_control_change(0, 0, 6, 104)),
  };
  sonare::midi::sort_render_events_stable(events);

  REQUIRE(events.size() == 4);
  for (size_t i = 0; i < written.size(); ++i) {
    REQUIRE(events[i].ump.note_number() == written[i]);
  }
  REQUIRE(events[3].ump.is_note_on());

  const auto before = events;
  sonare::midi::sort_render_events_stable(events);
  REQUIRE(events == before);
}

TEST_CASE("MidiClip validate_note_pairs reports matched and unmatched notes", "[midi]") {
  {
    MidiClip clip;
    clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    clip.add_event(ev(1.0, sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
    clip.add_event(ev(0.5, sonare::midi::make_midi1_note_on(0, 1, 64, 90)));
    clip.add_event(ev(1.5, sonare::midi::make_midi1_note_off(0, 1, 64, 0)));
    const auto report = clip.validate_note_pairs();
    REQUIRE(report.ok);
    REQUIRE(report.unmatched_note_ons == 0);
    REQUIRE(report.unmatched_note_offs == 0);
  }
  {
    MidiClip clip;
    clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));  // never released.
    clip.add_event(ev(1.0, sonare::midi::make_midi1_note_off(0, 0, 62, 0)));   // no preceding on.
    const auto report = clip.validate_note_pairs();
    REQUIRE_FALSE(report.ok);
    REQUIRE(report.unmatched_note_ons == 1);
    REQUIRE(report.unmatched_note_offs == 1);
  }
  {
    MidiClip clip;
    clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    clip.add_event(ev(1.0, sonare::midi::make_midi1_note_off(1, 0, 60, 0)));
    const auto report = clip.validate_note_pairs();
    REQUIRE_FALSE(report.ok);
    REQUIRE(report.unmatched_note_ons == 1);
    REQUIRE(report.unmatched_note_offs == 1);
  }
}

TEST_CASE("MidiClip note validation ignores non-channel note-shaped packets", "[midi]") {
  MidiClip clip;
  // These packets carry note-looking status/data fields, but their message
  // types are not channel voice and must not create an unmatched pair.
  clip.add_event(ev(0.0, raw_ump(0x3, static_cast<uint8_t>(sonare::midi::UmpStatus::kNoteOn),
                                 /*note=*/60, /*velocity=*/100)));
  clip.add_event(ev(1.0, raw_ump(0x5, static_cast<uint8_t>(sonare::midi::UmpStatus::kNoteOff),
                                 /*note=*/61, /*velocity=*/0)));

  // MIDI 1.0 velocity-zero note-on is a note-off, while MIDI 2.0 retains a
  // note-on even when its velocity is zero.
  clip.add_event(ev(2.0, sonare::midi::make_midi1_note_on(0, 0, 62, 100)));
  clip.add_event(ev(3.0, sonare::midi::make_midi1_note_on(0, 0, 62, 0)));
  clip.add_event(ev(4.0, sonare::midi::make_midi2_note_on(0, 0, 63, 0)));
  clip.add_event(ev(5.0, sonare::midi::make_midi2_note_off(0, 0, 63, 0)));

  const auto report = clip.validate_note_pairs();
  REQUIRE(report.ok);
  REQUIRE(report.unmatched_note_ons == 0);
  REQUIRE(report.unmatched_note_offs == 0);
}

TEST_CASE("MidiClip sort_stable keeps bank select before program change at same ppq", "[midi]") {
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_program_change(0, 0, 5)));
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  clip.add_event(ev(0.0, sonare::midi::make_midi1_control_change(0, 0, 32, 7)));
  clip.add_event(ev(0.0, sonare::midi::make_midi1_control_change(0, 0, 0, 0x79)));
  clip.add_event(ev(0.0, sonare::midi::make_midi1_control_change(0, 0, 11, 64)));

  clip.sort_stable();
  const auto& e = clip.events();
  REQUIRE(e.size() == 5);
  REQUIRE(e[0].ump.status_nibble() ==
          static_cast<uint8_t>(sonare::midi::UmpStatus::kControlChange));
  REQUIRE(e[0].ump.note_number() == 0);
  REQUIRE(e[1].ump.status_nibble() ==
          static_cast<uint8_t>(sonare::midi::UmpStatus::kControlChange));
  REQUIRE(e[1].ump.note_number() == 32);
  REQUIRE(e[2].ump.status_nibble() ==
          static_cast<uint8_t>(sonare::midi::UmpStatus::kProgramChange));
  REQUIRE(e[3].ump.status_nibble() ==
          static_cast<uint8_t>(sonare::midi::UmpStatus::kControlChange));
  REQUIRE(e[3].ump.note_number() == 11);
  REQUIRE(e[4].ump.is_note_on());
}

TEST_CASE("MidiClip sort_stable orders a MIDI 2.0 banked program change before note-on", "[midi]") {
  // MIDI 2.0 carries bank select inside the program-change UMP, so it must rank
  // ahead of a same-ppq note-on (so the note uses the new program/bank), just
  // like a MIDI 1.0 program change.
  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi2_note_on(0, 0, 60, 0x4000, 0, 0)));
  clip.add_event(ev(0.0, sonare::midi::make_midi2_program_change(0, 0, 5, 1, 2, true)));

  clip.sort_stable();
  const auto& e = clip.events();
  REQUIRE(e.size() == 2);
  REQUIRE(e[0].ump.status_nibble() ==
          static_cast<uint8_t>(sonare::midi::UmpStatus::kProgramChange));
  REQUIRE(e[1].ump.is_note_on());
}

TEST_CASE("MidiClip to_render_events converts PPQ to render frames via the tempo map", "[midi]") {
  // 120 BPM, 48000 Hz: one quarter note = 0.5 s = 24000 samples.
  sonare::transport::TempoMap map;
  init_tempo_map(&map, 120.0, 48000.0);

  MidiClip clip;
  clip.add_event(ev(0.0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
  clip.add_event(ev(1.0, sonare::midi::make_midi1_note_off(0, 0, 60, 0)));
  clip.sort_stable();

  std::vector<MidiEvent> out;
  clip.to_render_events(map, /*clip_start_ppq=*/2.0, &out);
  REQUIRE(out.size() == 2);
  // clip_start_ppq=2.0 -> 48000 samples; +1 quarter -> 72000.
  REQUIRE(out[0].render_frame == map.ppq_to_sample(2.0));
  REQUIRE(out[1].render_frame == map.ppq_to_sample(3.0));
  REQUIRE(out[0].render_frame == 48000);
  REQUIRE(out[1].render_frame == 72000);
  REQUIRE(out[0].ump.is_note_on());
  REQUIRE(out[1].ump.is_note_off());
}

TEST_CASE("MidiSequencer releases notes from clips dropped by a republished set", "[midi]") {
  // A live mute (or clip delete) recompiles the arrangement and republishes the
  // MIDI clip set WITHOUT the affected clip. Any note still sounding from that
  // clip must be released so it does not hang -- mirroring the audio path's
  // "scheduled but silent" model rather than being cut off with no note-off.
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule clip;
  clip.id = 42;
  clip.destination_id = 5;
  clip.start_sample = 0;
  clip.length_samples = 0;  // open-ended: the note-on has no matching note-off
  clip.events = {
      {0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
  };
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();

  seq.process_block(SequencerClock::aligned(0), 256);
  REQUIRE(seq.active_note_count() == 1);
  const size_t dispatched_before = sink.events.size();

  // Republish WITHOUT the clip (mute / delete) and render the next block.
  seq.set_midi_clips({});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(256), 256);

  // The hung note was released: a note-off for note 60 on destination 5.
  REQUIRE(seq.active_note_count() == 0);
  bool found_off = false;
  for (size_t i = dispatched_before; i < sink.events.size(); ++i) {
    const auto& c = sink.events[i];
    if (c.destination == 5 && c.event.ump.is_note_off() && c.event.ump.note_number() == 60) {
      found_off = true;
    }
  }
  REQUIRE(found_off);
}

TEST_CASE("MidiSequencer keeps notes sounding when a republished set still contains the clip",
          "[midi]") {
  // Editing an unrelated property republishes the clip set with the same clip
  // id still present; a note sounding from it must NOT be spuriously released.
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule clip;
  clip.id = 7;
  clip.destination_id = 2;
  clip.length_samples = 0;
  clip.events = {
      {0, sonare::midi::make_midi1_note_on(0, 0, 64, 90)},
  };
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 256);
  REQUIRE(seq.active_note_count() == 1);

  // Republish the same clip (id unchanged) and render on.
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(256), 256);
  REQUIRE(seq.active_note_count() == 1);  // still sounding, not released
}

TEST_CASE(
    "MidiSequencer refresh releases a clip whose destination changed and drops its pending FX",
    "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::ArpeggiatorConfig arp;
  arp.enabled = true;
  arp.steps = 2;
  arp.intervals[0] = 0;
  arp.intervals[1] = 12;
  arp.step_frames = 40;
  arp.gate_frames = 40;
  fx.set_arpeggiator(arp);
  REQUIRE(seq.set_midi_fx(5, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  MidiClipSchedule original;
  original.id = 43;
  original.destination_id = 5;
  original.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  seq.set_midi_clips({original});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 32);

  REQUIRE(sink.events.size() == 1);
  REQUIRE(sink.events[0].destination == 5);
  REQUIRE(sink.events[0].event.ump.is_note_on());
  REQUIRE(sink.events[0].event.ump.note_number() == 60);
  REQUIRE(seq.active_note_count() == 1);

  // A refreshed schedule may reuse the clip id while routing the clip to a new
  // instrument. The old note and generated events belong to destination 5 and
  // must not survive the refresh under the id-only identity check.
  MidiClipSchedule replacement = original;
  replacement.destination_id = 6;
  replacement.events.clear();
  seq.set_midi_clips({replacement});
  seq.acquire_midi_clips();
  sink.events.clear();
  seq.process_block(SequencerClock::aligned(32), 32);

  REQUIRE(seq.active_note_count() == 0);
  REQUIRE(sink.events.size() == 1);
  REQUIRE(sink.events[0].destination == 5);
  REQUIRE(sink.events[0].event.render_frame == 32);
  REQUIRE(sink.events[0].event.ump.is_note_off());
  REQUIRE(sink.events[0].event.ump.note_number() == 60);
}

TEST_CASE("MidiSequencer refresh releases a clip whose source track changed", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::ArpeggiatorConfig arp;
  arp.enabled = true;
  arp.steps = 2;
  arp.intervals[0] = 0;
  arp.intervals[1] = 12;
  arp.step_frames = 40;
  arp.gate_frames = 40;
  fx.set_arpeggiator(arp);
  REQUIRE(seq.set_midi_fx(5, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  MidiClipSchedule original;
  original.id = 44;
  original.track_id = 10;
  original.destination_id = 5;
  original.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  seq.set_midi_clips({original});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 32);

  REQUIRE(seq.active_note_count() == 1);
  MidiClipSchedule replacement = original;
  replacement.track_id = 20;
  replacement.events.clear();
  seq.set_midi_clips({replacement});
  seq.acquire_midi_clips();
  sink.events.clear();
  seq.process_block(SequencerClock::aligned(32), 32);

  REQUIRE(seq.active_note_count() == 0);
  REQUIRE(sink.events.size() == 1);
  REQUIRE(sink.events[0].destination == 5);
  REQUIRE(sink.events[0].event.render_frame == 32);
  REQUIRE(sink.events[0].event.ump.is_note_off());
  REQUIRE(sink.events[0].event.ump.note_number() == 60);
  REQUIRE(sink.events[0].event.source_track_id == 10);
}

TEST_CASE("MidiSequencer note-off fallback keeps a different source track active", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule track_a;
  track_a.id = 101;
  track_a.track_id = 10;
  track_a.destination_id = 5;
  track_a.events = {
      {0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
      {10, sonare::midi::make_midi1_note_off(0, 0, 60, 0)},
      {20, sonare::midi::make_midi1_note_off(0, 0, 60, 0)},
  };
  MidiClipSchedule track_b;
  track_b.id = 102;
  track_b.track_id = 20;
  track_b.destination_id = 5;
  track_b.events = {{5, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  seq.set_midi_clips({track_a, track_b});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 32);

  // The duplicate A note-off must not consume B's same-pitch note.
  REQUIRE(seq.active_note_count() == 1);

  sink.events.clear();
  seq.set_midi_clips({track_a});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(32), 32);

  REQUIRE(seq.active_note_count() == 0);
  REQUIRE(sink.events.size() == 1);
  REQUIRE(sink.events[0].destination == 5);
  REQUIRE(sink.events[0].event.ump.is_note_off());
  REQUIRE(sink.events[0].event.ump.note_number() == 60);
  REQUIRE(sink.events[0].event.source_track_id == 20);
}

TEST_CASE("MidiSequencer retains sustain state for global and destination stop resets", "[midi]") {
  constexpr uint32_t kDestination = 11;
  const auto seed_sustain_state = [&](MidiSequencer& seq) {
    seq.inject_event(kDestination, DeviceFrame{0},
                     sonare::midi::make_midi1_control_change(0, 0, 64, 127));
    seq.inject_event(kDestination, DeviceFrame{1}, sonare::midi::make_midi1_note_on(0, 0, 60, 100));
    seq.inject_event(kDestination, DeviceFrame{2}, sonare::midi::make_midi1_note_off(0, 0, 60, 0));
  };
  const auto require_reset = [&](const CapturingSink& sink, int64_t render_frame) {
    REQUIRE(sink.events.size() == 4);
    for (const auto& captured : sink.events) {
      REQUIRE(captured.destination == kDestination);
      REQUIRE(captured.event.render_frame == render_frame);
    }
    REQUIRE(sink.events[0].event.ump == sonare::midi::make_midi1_control_change(0, 0, 64, 0));
    REQUIRE(sink.events[1].event.ump == sonare::midi::make_midi1_control_change(0, 0, 121, 0));
    REQUIRE(sink.events[2].event.ump == sonare::midi::make_midi1_control_change(0, 0, 123, 0));
    REQUIRE(sink.events[3].event.ump == sonare::midi::make_midi1_pitch_bend(0, 0, 8192));
  };

  SECTION("global stop") {
    MidiSequencer seq;
    CapturingSink sink;
    seq.prepare(48000.0);
    seq.set_sink(&sink);
    seed_sustain_state(seq);
    REQUIRE(seq.active_note_count() == 0);
    sink.events.clear();

    seq.all_notes_off(DeviceFrame{/*render_frame=*/3});
    require_reset(sink, 3);
  }

  SECTION("destination stop") {
    MidiSequencer seq;
    CapturingSink sink;
    seq.prepare(48000.0);
    seq.set_sink(&sink);
    seed_sustain_state(seq);
    REQUIRE(seq.active_note_count() == 0);
    sink.events.clear();

    seq.all_notes_off_for_destination(kDestination, /*render_frame=*/DeviceFrame{4});
    require_reset(sink, 4);
  }
}

TEST_CASE("MidiSequencer does not stop-track non-channel note-shaped packets", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  const Ump nonchannel_note_on =
      raw_ump(0x3, static_cast<uint8_t>(sonare::midi::UmpStatus::kNoteOn));
  seq.inject_event(/*destination=*/5, /*render_frame=*/DeviceFrame{0}, nonchannel_note_on);

  REQUIRE(sink.events.size() == 1);
  REQUIRE(sink.events.front().event.ump == nonchannel_note_on);
  REQUIRE(seq.active_note_count() == 0);

  sink.events.clear();
  seq.all_notes_off(DeviceFrame{/*render_frame=*/1});
  REQUIRE(sink.events.empty());
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer resets all retained channel triples at ledger capacity", "[midi]") {
  constexpr uint32_t kDestination = 12;
  constexpr size_t kChannelTriples = 16u * 16u;
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  for (uint8_t group = 0; group < 16; ++group) {
    for (uint8_t channel = 0; channel < 16; ++channel) {
      seq.inject_event(kDestination, DeviceFrame{0},
                       sonare::midi::make_midi1_control_change(group, channel, 64, 127));
    }
  }
  sink.events.clear();
  seq.all_notes_off_for_destination(kDestination, /*render_frame=*/DeviceFrame{256});

  REQUIRE(sink.events.size() == kChannelTriples * 4);
  std::array<std::array<std::array<int, 4>, 16>, 16> reset_counts{};
  for (const auto& captured : sink.events) {
    REQUIRE(captured.destination == kDestination);
    REQUIRE(captured.event.render_frame == 256);
    const uint8_t group = captured.event.ump.group;
    const uint8_t channel = captured.event.ump.channel();
    REQUIRE(group < 16);
    REQUIRE(channel < 16);
    if (captured.event.ump.status_nibble() ==
        static_cast<uint8_t>(sonare::midi::UmpStatus::kControlChange)) {
      const uint8_t controller = captured.event.ump.note_number();
      if (controller == 64) {
        ++reset_counts[group][channel][0];
      } else if (controller == 121) {
        ++reset_counts[group][channel][1];
      } else if (controller == 123) {
        ++reset_counts[group][channel][2];
      } else {
        FAIL("unexpected controller in retained-channel reset");
      }
    } else if (captured.event.ump.status_nibble() ==
               static_cast<uint8_t>(sonare::midi::UmpStatus::kPitchBend)) {
      ++reset_counts[group][channel][3];
    } else {
      FAIL("unexpected message in retained-channel reset");
    }
  }
  for (const auto& by_channel : reset_counts) {
    for (const auto& counts : by_channel) {
      REQUIRE(counts[0] == 1);
      REQUIRE(counts[1] == 1);
      REQUIRE(counts[2] == 1);
      REQUIRE(counts[3] == 1);
    }
  }
}

TEST_CASE("MidiSequencer drops new stateful events when the ledger is full but forwards note-offs",
          "[midi]") {
  constexpr uint32_t kFullDestination = 13;
  constexpr uint32_t kOverflowDestination = 14;
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  for (uint8_t group = 0; group < 16; ++group) {
    for (uint8_t channel = 0; channel < 16; ++channel) {
      seq.inject_event(kFullDestination, DeviceFrame{0},
                       sonare::midi::make_midi1_control_change(group, channel, 1, 127));
    }
  }
  sink.events.clear();

  seq.inject_event(kOverflowDestination, DeviceFrame{1},
                   sonare::midi::make_midi1_control_change(0, 0, 64, 127));
  REQUIRE(sink.events.empty());
  REQUIRE(seq.retained_channel_overflow_count() == 1);

  seq.inject_event(kOverflowDestination, DeviceFrame{2},
                   sonare::midi::make_midi1_note_off(0, 0, 60, 0));
  REQUIRE(sink.events.size() == 1);
  REQUIRE(sink.events[0].destination == kOverflowDestination);
  REQUIRE(sink.events[0].event.render_frame == 2);
  REQUIRE(sink.events[0].event.ump.is_note_off());
}

TEST_CASE("MidiSequencer dispatches in-block events in order and frame", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 7;
  clip.events = {
      {100, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
      {150, sonare::midi::make_midi1_note_off(0, 0, 60, 0)},
      {300, sonare::midi::make_midi1_note_on(0, 0, 64, 90)},
      {600, sonare::midi::make_midi1_note_off(0, 0, 64, 0)},
  };
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();

  // Block 0: [0,256) captures the first two events.
  seq.process_block(SequencerClock::aligned(0), 256);
  REQUIRE(sink.events.size() == 2);
  REQUIRE(sink.events[0].destination == 7);
  REQUIRE(sink.events[0].event.render_frame == 100);
  REQUIRE(sink.events[0].event.ump.is_note_on());
  REQUIRE(sink.events[1].event.render_frame == 150);
  REQUIRE(sink.events[1].event.ump.is_note_off());

  // Block 1: [256,512) captures the third event only.
  seq.process_block(SequencerClock::aligned(256), 256);
  REQUIRE(sink.events.size() == 3);
  REQUIRE(sink.events[2].event.render_frame == 300);

  // Block 2: [512,768) captures the fourth.
  seq.process_block(SequencerClock::aligned(512), 256);
  REQUIRE(sink.events.size() == 4);
  REQUIRE(sink.events[3].event.render_frame == 600);
  REQUIRE(seq.dispatched_event_count() == 4);
  REQUIRE(seq.active_note_count() == 0);  // every note-on was released.
}

TEST_CASE("MidiSequencer preserves source track through MIDI FX and a synthetic clip-end note-off",
          "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::TransposeConfig transpose;
  transpose.enabled = true;
  transpose.semitones = 12;
  fx.set_transpose(transpose);
  REQUIRE(seq.set_midi_fx(7, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  MidiClipSchedule clip;
  clip.id = 99;
  clip.track_id = 4242;
  clip.destination_id = 7;
  clip.start_sample = 0;
  clip.length_samples = 80;
  clip.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 128);

  REQUIRE(sink.events.size() == 2);
  REQUIRE(sink.events[0].event.ump.is_note_on());
  REQUIRE(sink.events[0].event.ump.note_number() == 72);
  REQUIRE(sink.events[0].event.source_track_id == clip.track_id);
  REQUIRE(sink.events[1].event.ump.is_note_off());
  REQUIRE(sink.events[1].event.source_track_id == clip.track_id);
}

TEST_CASE("MidiSequencer dispatches pre-resolved SysEx payload views", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  const std::vector<uint8_t> expected = {0xF0, 0x7D, 0x20, 0x21, 0xF7};
  std::vector<uint8_t> payload = expected;
  MidiEvent sysex;
  sysex.render_frame = 120;
  sysex.ump = sonare::midi::make_sysex_handle(/*group=*/0, /*handle=*/77);
  sysex.sysex_payload = payload.data();
  sysex.sysex_payload_size = payload.size();

  MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 7;
  clip.events = {sysex};
  seq.set_midi_clips({clip});
  // The control setter must snapshot caller-owned bytes. Mutating the source
  // after publication must not alter the event adopted by the audio thread.
  payload[2] = 0x55;
  seq.acquire_midi_clips();

  seq.process_block(SequencerClock::aligned(0), 256);
  REQUIRE(sink.events.size() == 1);
  REQUIRE(sink.events.front().destination == 7);
  REQUIRE(sink.events.front().event.render_frame == 120);
  REQUIRE(sink.events.front().event.ump.sysex_handle == 77);
  REQUIRE(seq.current_clips() != nullptr);
  REQUIRE(seq.current_clips()->front().sysex_payload_bank != nullptr);
  REQUIRE(sink.events.front().event.sysex_payload_size == expected.size());
  REQUIRE(std::vector<uint8_t>(sink.events.front().event.sysex_payload,
                               sink.events.front().event.sysex_payload +
                                   sink.events.front().event.sysex_payload_size) == expected);
}

TEST_CASE("MidiSequencer rejects null SysEx payload without replacing its snapshot", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  const std::vector<uint8_t> good_payload = {0xF0, 0x7D, 0x01, 0xF7};
  MidiEvent good_event;
  good_event.ump = sonare::midi::make_sysex_handle(0, 1);
  good_event.sysex_payload = good_payload.data();
  good_event.sysex_payload_size = good_payload.size();
  MidiClipSchedule good_clip;
  good_clip.id = 7;
  good_clip.events = {good_event};
  seq.set_midi_clips({good_clip});
  seq.acquire_midi_clips();

  SECTION("null pointer with a nonzero size") {
    MidiEvent invalid = good_event;
    invalid.sysex_payload = nullptr;
    invalid.sysex_payload_size = 1;
    MidiClipSchedule invalid_clip = good_clip;
    invalid_clip.events = {invalid};
    REQUIRE_THROWS_AS(seq.set_midi_clips({invalid_clip}), sonare::SonareException);
  }

  REQUIRE(seq.current_clips() != nullptr);
  REQUIRE(seq.current_clips()->size() == 1);
  const MidiEvent& retained = seq.current_clips()->front().events.front();
  REQUIRE(retained.sysex_payload_size == good_payload.size());
  REQUIRE(std::vector<uint8_t>(retained.sysex_payload,
                               retained.sysex_payload + retained.sysex_payload_size) ==
          good_payload);
}

TEST_CASE("MidiSequencer owns an arbitrary-size scheduled SysEx payload", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  std::vector<uint8_t> payload(513);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>(i);
  }
  const std::vector<uint8_t> expected = payload;
  MidiEvent event;
  event.render_frame = 120;
  event.ump = sonare::midi::make_sysex_handle(0, 2);
  event.sysex_payload = payload.data();
  event.sysex_payload_size = payload.size();
  MidiClipSchedule clip;
  clip.events = {event};
  seq.set_midi_clips({clip});
  payload[37] ^= 0xFFu;
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 256);

  REQUIRE(sink.events.size() == 1);
  REQUIRE(sink.events.front().event.sysex_payload_size == expected.size());
  REQUIRE(std::vector<uint8_t>(sink.events.front().event.sysex_payload,
                               sink.events.front().event.sysex_payload + expected.size()) ==
          expected);
}

TEST_CASE("MidiSequencer preserves payloads borrowed by later schedules", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  const std::vector<uint8_t> expected = {0xF0, 0x7D, 0x31, 0x32, 0xF7};
  std::weak_ptr<const TestPreparedMidiSysEx> weak_prepared;
  std::vector<MidiClipSchedule> schedules(1);
  {
    auto source_bank = std::make_shared<MidiSysExPayloadBank>();
    source_bank->payloads.push_back(expected);
    auto prepared = std::make_shared<TestPreparedMidiSysEx>(37);
    weak_prepared = prepared;
    source_bank->prepared_operations.push_back(prepared);
    MidiEvent first_event;
    first_event.render_frame = 100;
    first_event.ump = sonare::midi::make_sysex_handle(0, 3);
    first_event.sysex_payload = source_bank->payloads.front().data();
    first_event.sysex_payload_size = expected.size();
    first_event.prepared_sysex = prepared.get();
    schedules.front().events = {first_event};
    schedules.front().sysex_payload_bank = source_bank;
  }

  // The second schedule borrows the first schedule's bank. This is valid while
  // the vector is handed to set_midi_clips; publication must not release the
  // first owner before copying the later event.
  MidiClipSchedule second;
  MidiEvent second_event = schedules.front().events.front();
  second_event.render_frame = 140;
  second_event.ump = sonare::midi::make_sysex_handle(0, 4);
  second_event.sysex_payload = schedules.front().events.front().sysex_payload;
  second_event.sysex_payload_size = schedules.front().events.front().sysex_payload_size;
  second.events = {second_event};
  schedules.push_back(std::move(second));

  seq.set_midi_clips(std::move(schedules));
  REQUIRE_FALSE(weak_prepared.expired());
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 256);

  REQUIRE(sink.events.size() == 2);
  for (const auto& captured : sink.events) {
    REQUIRE(captured.event.sysex_payload_size == expected.size());
    REQUIRE(std::vector<uint8_t>(captured.event.sysex_payload,
                                 captured.event.sysex_payload + expected.size()) == expected);
    REQUIRE(captured.event.prepared_sysex != nullptr);
    REQUIRE(static_cast<const TestPreparedMidiSysEx*>(captured.event.prepared_sysex)->value == 37);
  }
}

TEST_CASE("MidiSequencer applies live MIDI FX per destination before dispatch", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::TransposeConfig transpose;
  transpose.enabled = true;
  transpose.semitones = 12;
  fx.set_transpose(transpose);
  sonare::midi::ChordConfig chord;
  chord.enabled = true;
  chord.count = 2;
  chord.intervals[0] = 0;
  chord.intervals[1] = 7;
  fx.set_chord(chord);
  REQUIRE(seq.set_midi_fx(7, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  MidiClipSchedule processed;
  processed.id = 1;
  processed.destination_id = 7;
  processed.events = {{10, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                      {20, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  MidiClipSchedule bypassed = processed;
  bypassed.id = 2;
  bypassed.destination_id = 8;
  seq.set_midi_clips({processed, bypassed});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 64);

  REQUIRE(sink.events.size() == 6);
  REQUIRE(sink.events[0].destination == 7);
  REQUIRE(sink.events[0].event.ump.note_number() == 72);
  REQUIRE(sink.events[1].destination == 7);
  REQUIRE(sink.events[1].event.ump.note_number() == 79);
  REQUIRE(sink.events[2].destination == 8);
  REQUIRE(sink.events[2].event.ump.note_number() == 60);
  REQUIRE(sink.events[3].destination == 7);
  REQUIRE(sink.events[3].event.ump.is_note_off());
  REQUIRE(sink.events[3].event.ump.note_number() == 72);
  REQUIRE(sink.events[4].destination == 7);
  REQUIRE(sink.events[4].event.ump.note_number() == 79);
  REQUIRE(sink.events[5].destination == 8);
  REQUIRE(sink.events[5].event.ump.note_number() == 60);
  for (size_t i = 1; i < sink.events.size(); ++i) {
    REQUIRE(sink.events[i - 1].event.render_frame <= sink.events[i].event.render_frame);
  }
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer preserves same-frame controller stream order after MIDI FX", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 100;
  quantize.strength = 1.0f;
  fx.set_quantize(quantize);
  REQUIRE(seq.set_midi_fx(9, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  // A same-timestamp RPN gesture must retain its written order after all four
  // controllers are delayed into the pending-FX queue. The selector bytes
  // (101, 100) give the following Data Entry pair its meaning.
  MidiClipSchedule clip;
  clip.id = 1201;
  clip.destination_id = 9;
  clip.events = {
      {60, sonare::midi::make_midi1_control_change(0, 0, 101, 0)},
      {60, sonare::midi::make_midi1_control_change(0, 0, 100, 0)},
      {60, sonare::midi::make_midi1_control_change(0, 0, 6, 12)},
      {60, sonare::midi::make_midi1_control_change(0, 0, 38, 0)},
  };
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 128);

  REQUIRE(sink.events.size() == 4);
  constexpr std::array<uint8_t, 4> kWritten{101, 100, 6, 38};
  for (size_t i = 0; i < kWritten.size(); ++i) {
    REQUIRE(sink.events[i].destination == 9);
    REQUIRE(sink.events[i].event.render_frame == 100);
    REQUIRE(sink.events[i].event.ump.note_number() == kWritten[i]);
  }
}

TEST_CASE("MidiSequencer releases a note now when the MIDI FX pending list is full", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 1000;
  quantize.strength = 1.0f;
  fx.set_quantize(quantize);
  REQUIRE(seq.set_midi_fx(9, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  // The note sounds at 1000; controllers then fill the pending list at 2000, so the
  // note-off, shifted with its note-on to 2001, has no slot.
  MidiClipSchedule clip;
  clip.id = 1208;
  clip.destination_id = 9;
  clip.events.push_back({600, sonare::midi::make_midi1_note_on(0, 0, 60, 100)});
  for (size_t i = 0; i < MidiSequencer::kMaxPendingFxEvents; ++i) {
    clip.events.push_back({1600, sonare::midi::make_midi1_control_change(0, 0, 1, 0)});
  }
  clip.events.push_back({1601, sonare::midi::make_midi1_note_off(0, 0, 60, 0)});
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 3000);

  const auto note_off =
      std::find_if(sink.events.begin(), sink.events.end(),
                   [](const CapturingSink::Captured& c) { return c.event.ump.is_note_off(); });
  REQUIRE(note_off != sink.events.end());
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer ranks same-frame note-off before note-on across clips", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule incoming;
  incoming.id = 1206;
  incoming.track_id = 91;
  incoming.destination_id = 9;
  incoming.events = {{100, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};

  MidiClipSchedule old;
  old.id = 1207;
  old.track_id = 91;
  old.destination_id = 9;
  old.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                {100, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};

  // The incoming clip is deliberately first in the published vector. Its
  // note-on must still follow the old clip's same-frame note-off after the
  // sequencer merges events from all clips.
  seq.set_midi_clips({incoming, old});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 128);

  REQUIRE(sink.events.size() == 3);
  REQUIRE(sink.events[0].event.render_frame == 0);
  REQUIRE(sink.events[0].event.ump.is_note_on());
  REQUIRE(sink.events[1].event.render_frame == 100);
  REQUIRE(sink.events[1].event.ump.is_note_off());
  REQUIRE(sink.events[2].event.render_frame == 100);
  REQUIRE(sink.events[2].event.ump.is_note_on());
  REQUIRE(seq.active_note_count() == 1);
}

TEST_CASE("MidiSequencer globally ranks a scheduled note-off before a pending note-on", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 100;
  quantize.strength = 1.0f;
  fx.set_quantize(quantize);
  REQUIRE(seq.set_midi_fx(9, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  // The note-on is moved from 60 to 100 and therefore waits in the FX queue.
  // The other clip's note-off is already scheduled at 100. Both clips share a
  // track, destination, group, channel and key, so the off-before-on ordering
  // is observable in both the sink stream and the active-note ledger.
  MidiClipSchedule pending_on;
  pending_on.id = 1202;
  pending_on.track_id = 77;
  pending_on.destination_id = 9;
  pending_on.events = {{60, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  MidiClipSchedule scheduled_off;
  scheduled_off.id = 1203;
  scheduled_off.track_id = 77;
  scheduled_off.destination_id = 9;
  scheduled_off.events = {{100, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};

  seq.set_midi_clips({pending_on, scheduled_off});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 128);

  REQUIRE(sink.events.size() == 2);
  REQUIRE(sink.events[0].event.render_frame == 100);
  REQUIRE(sink.events[0].event.ump.is_note_off());
  REQUIRE(sink.events[1].event.render_frame == 100);
  REQUIRE(sink.events[1].event.ump.is_note_on());
  REQUIRE(seq.active_note_count() == 1);
}

TEST_CASE("MidiSequencer rescans pending events after a channel reset removes a note", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 100;
  quantize.strength = 1.0f;
  fx.set_quantize(quantize);
  REQUIRE(seq.set_midi_fx(9, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  MidiClipSchedule clip;
  clip.id = 1208;
  clip.track_id = 92;
  clip.destination_id = 9;
  // The three source frames quantize to 100 in this order, producing a
  // pending queue of note-on(rank 5), reset(rank 4), CC(rank 4). Dispatching
  // the reset removes the pending note-on, so the iterator must rescan the
  // shifted slot and still dispatch the CC at frame 100.
  clip.events = {
      {60, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
      {61, sonare::midi::make_midi1_control_change(0, 0, 120, 0)},
      {62, sonare::midi::make_midi1_control_change(0, 0, 1, 64)},
  };
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 128);

  REQUIRE(sink.events.size() == 2);
  REQUIRE(sink.events[0].event.render_frame == 100);
  REQUIRE(sink.events[0].event.ump.note_number() == 120);
  REQUIRE(sink.events[1].event.render_frame == 100);
  REQUIRE(sink.events[1].event.ump.note_number() == 1);
  REQUIRE(seq.active_note_count() == 0);

  // A skipped CC must not be carried into the following block.
  seq.process_block(SequencerClock::aligned(128), 128);
  REQUIRE(sink.events.size() == 2);
}

TEST_CASE("MidiSequencer ranks a pending note-on after a one-shot clip-end release", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 100;
  quantize.strength = 1.0f;
  fx.set_quantize(quantize);
  REQUIRE(seq.set_midi_fx(9, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  MidiClipSchedule ending;
  ending.id = 1204;
  ending.track_id = 88;
  ending.destination_id = 9;
  ending.loop_mode = sonare::midi::MidiLoopMode::kOneShot;
  ending.length_samples = 100;
  ending.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};

  // This note is transformed at frame 60 and becomes a pending note-on at the
  // exact exclusive end of the first clip. The synthetic clip-end release for
  // the first clip must still be emitted before that note-on.
  MidiClipSchedule incoming;
  incoming.id = 1205;
  incoming.track_id = 88;
  incoming.destination_id = 9;
  incoming.events = {{60, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};

  seq.set_midi_clips({incoming, ending});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 128);

  REQUIRE(sink.events.size() == 3);
  REQUIRE(sink.events[0].event.render_frame == 0);
  REQUIRE(sink.events[0].event.ump.is_note_on());
  REQUIRE(sink.events[1].event.render_frame == 100);
  REQUIRE(sink.events[1].event.ump.is_note_off());
  REQUIRE(sink.events[2].event.render_frame == 100);
  REQUIRE(sink.events[2].event.ump.is_note_on());
  REQUIRE(seq.active_note_count() == 1);
}

TEST_CASE("MidiSequencer humanize advances ordinals like offline MIDI FX", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain config;
  sonare::midi::HumanizeConfig humanize;
  humanize.enabled = true;
  humanize.seed = 0x12345678u;
  humanize.timing_frames = 20;
  config.set_humanize(humanize);
  REQUIRE(seq.set_midi_fx(7, config));
  seq.acquire_midi_fx(DeviceFrame{0});

  MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 7;
  std::array<MidiEvent, 8> input{};
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = {100 + static_cast<int64_t>(i) * 100,
                sonare::midi::make_midi1_note_on(0, 0, static_cast<uint8_t>(60 + i), 100)};
    clip.events.push_back(input[i]);
  }
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 1024);

  MidiFxChain offline;
  offline.set_humanize(humanize);
  offline.prepare();
  sonare::midi::MidiFxBuffer expected;
  offline.process(input.data(), input.size(), &expected);

  REQUIRE(sink.events.size() == input.size());
  REQUIRE(expected.size == input.size());
  bool varied = false;
  const int64_t first_jitter = expected.events[0].render_frame - input[0].render_frame;
  for (size_t i = 0; i < input.size(); ++i) {
    REQUIRE(sink.events[i].event == expected.events[i]);
    varied = varied || expected.events[i].render_frame - input[i].render_frame != first_jitter;
  }
  REQUIRE(varied);
}

TEST_CASE("MidiSequencer live MIDI FX keeps future arpeggiator events pending", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::ArpeggiatorConfig arp;
  arp.enabled = true;
  arp.steps = 2;
  arp.intervals[0] = 0;
  arp.intervals[1] = 12;
  arp.step_frames = 40;
  arp.gate_frames = 10;
  fx.set_arpeggiator(arp);
  REQUIRE(seq.set_midi_fx(9, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 9;
  clip.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                 {120, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();

  seq.process_block(SequencerClock::aligned(0), 32);
  REQUIRE(sink.events.size() == 2);
  REQUIRE(sink.events[0].event.render_frame == 0);
  REQUIRE(sink.events[0].event.ump.is_note_on());
  REQUIRE(sink.events[1].event.render_frame == 10);
  REQUIRE(sink.events[1].event.ump.is_note_off());
  REQUIRE(seq.active_note_count() == 0);

  seq.process_block(SequencerClock::aligned(32), 32);
  REQUIRE(sink.events.size() == 4);
  REQUIRE(sink.events[2].event.render_frame == 40);
  REQUIRE(sink.events[2].event.ump.is_note_on());
  REQUIRE(sink.events[2].event.ump.note_number() == 72);
  REQUIRE(sink.events[3].event.render_frame == 50);
  REQUIRE(sink.events[3].event.ump.is_note_off());
  REQUIRE(seq.midi_fx_pending_overflow_count() == 0);
}

TEST_CASE("MidiSequencer clamps overdue pending MIDI FX events to block start", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::ArpeggiatorConfig arp;
  arp.enabled = true;
  arp.steps = 2;
  arp.intervals[0] = 0;
  arp.intervals[1] = 12;
  arp.step_frames = 40;
  arp.gate_frames = 10;
  fx.set_arpeggiator(arp);
  REQUIRE(seq.set_midi_fx(9, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  MidiClipSchedule clip;
  clip.id = 121;
  clip.destination_id = 9;
  clip.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();

  seq.process_block(SequencerClock::aligned(0), 32);
  REQUIRE(sink.events.size() == 2);

  sink.events.clear();
  seq.process_block(SequencerClock::aligned(64), 32);
  // Overdue events keep their chronological order, so the late note cannot hang.
  REQUIRE(sink.events.size() == 2);
  REQUIRE(sink.events[0].event.render_frame == 64);
  REQUIRE(sink.events[0].event.ump.is_note_on());
  REQUIRE(sink.events[0].event.ump.note_number() == 72);
  REQUIRE(sink.events[1].event.render_frame == 64);
  REQUIRE(sink.events[1].event.ump.is_note_off());
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer keeps arpeggiator pending events across loop wrap", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::ArpeggiatorConfig arp;
  arp.enabled = true;
  arp.steps = 2;
  arp.intervals[0] = 0;
  arp.intervals[1] = 12;
  arp.step_frames = 30;
  arp.gate_frames = 5;
  fx.set_arpeggiator(arp);
  REQUIRE(seq.set_midi_fx(9, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  MidiClipSchedule clip;
  clip.id = 101;
  clip.start_sample = 0;
  clip.length_samples = 60;
  clip.loop_mode = sonare::midi::MidiLoopMode::kLoop;
  clip.loop_length_samples = 20;
  clip.destination_id = 9;
  clip.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();

  seq.process_block(SequencerClock::aligned(0), 20);
  REQUIRE(sink.events.size() == 2);
  REQUIRE(sink.events[0].event.render_frame == 0);
  REQUIRE(sink.events[1].event.render_frame == 5);

  sink.events.clear();
  seq.process_block(SequencerClock::aligned(20), 20);

  bool saw_carried_arp_note = false;
  for (const auto& cap : sink.events) {
    if (cap.event.render_frame == 30 && cap.event.ump.is_note_on() &&
        cap.event.ump.note_number() == 72) {
      saw_carried_arp_note = true;
    }
  }
  REQUIRE(saw_carried_arp_note);
  REQUIRE(seq.midi_fx_pending_overflow_count() == 0);
}

TEST_CASE("MidiSequencer MIDI FX hot-swap releases transformed active notes", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain transpose_up;
  sonare::midi::TransposeConfig up;
  up.enabled = true;
  up.semitones = 12;
  transpose_up.set_transpose(up);
  REQUIRE(seq.set_midi_fx(7, transpose_up));
  seq.acquire_midi_fx(DeviceFrame{0});

  MidiClipSchedule clip;
  clip.id = 111;
  clip.destination_id = 7;
  clip.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                 {100, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 64);

  REQUIRE(seq.active_note_count() == 1);
  REQUIRE(sink.events.size() == 1);
  REQUIRE(sink.events[0].event.ump.note_number() == 72);

  MidiFxChain bypass;
  sink.events.clear();
  REQUIRE(seq.set_midi_fx(7, bypass));
  seq.acquire_midi_fx(DeviceFrame{64});

  REQUIRE(seq.active_note_count() == 0);
  REQUIRE_FALSE(sink.events.empty());
  bool saw_old_pitch_release = false;
  for (const auto& cap : sink.events) {
    if (cap.event.render_frame == 64 && cap.event.ump.is_note_off() &&
        cap.event.ump.note_number() == 72) {
      saw_old_pitch_release = true;
    }
  }
  REQUIRE(saw_old_pitch_release);
}

TEST_CASE("MidiSequencer MIDI FX clear releases generated notes at the audio boundary", "[midi]") {
  auto exercise_clear = [](const MidiFxChain& fx, const std::vector<uint8_t>& generated_notes) {
    MidiSequencer seq;
    CapturingSink sink;
    seq.prepare(48000.0);
    seq.set_sink(&sink);
    REQUIRE(seq.set_midi_fx(7, fx));
    seq.acquire_midi_fx(DeviceFrame{0});

    seq.inject_event(7, DeviceFrame{0}, sonare::midi::make_midi1_note_on(0, 0, 60, 100));
    REQUIRE(seq.active_note_count() == generated_notes.size());
    sink.events.clear();

    // The control call only publishes. Note flush and chain reset happen when
    // the audio thread adopts the snapshot at the next block boundary.
    seq.clear_midi_fx(7);
    REQUIRE(seq.active_note_count() == generated_notes.size());
    REQUIRE(sink.events.empty());
    seq.acquire_midi_fx(DeviceFrame{64});
    REQUIRE(seq.active_note_count() == 0);

    for (uint8_t note : generated_notes) {
      bool released = false;
      for (const auto& captured : sink.events) {
        if (captured.destination == 7 && captured.event.render_frame == 64 &&
            captured.event.ump.is_note_off() && captured.event.ump.note_number() == note) {
          released = true;
        }
      }
      REQUIRE(released);
    }

    // Pending arpeggiator/chord output was discarded with the old chain. The
    // source note-off now passes through unchanged and cannot resurrect state.
    sink.events.clear();
    seq.process_block(SequencerClock::aligned(64), 64);
    REQUIRE(sink.events.empty());
    seq.inject_event(7, DeviceFrame{128}, sonare::midi::make_midi1_note_off(0, 0, 60, 0));
    REQUIRE(seq.active_note_count() == 0);
    REQUIRE(sink.events.size() == 1);
    REQUIRE(sink.events[0].event.ump.note_number() == 60);
  };

  SECTION("transpose") {
    MidiFxChain fx;
    sonare::midi::TransposeConfig transpose;
    transpose.enabled = true;
    transpose.semitones = 12;
    fx.set_transpose(transpose);
    exercise_clear(fx, {72});
  }

  SECTION("chord") {
    MidiFxChain fx;
    sonare::midi::ChordConfig chord;
    chord.enabled = true;
    chord.count = 3;
    chord.intervals[0] = 0;
    chord.intervals[1] = 4;
    chord.intervals[2] = 7;
    fx.set_chord(chord);
    exercise_clear(fx, {60, 64, 67});
  }

  SECTION("arpeggiator") {
    MidiFxChain fx;
    sonare::midi::ArpeggiatorConfig arp;
    arp.enabled = true;
    arp.steps = 3;
    arp.intervals[0] = 0;
    arp.intervals[1] = 4;
    arp.intervals[2] = 7;
    arp.step_frames = 32;
    arp.gate_frames = 16;
    fx.set_arpeggiator(arp);
    exercise_clear(fx, {60});
  }
}

TEST_CASE("MidiSequencer clears MIDI FX timing state on stop", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 100;
  quantize.strength = 1.0f;
  fx.set_quantize(quantize);
  REQUIRE(seq.set_midi_fx(7, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  // The first note stores a +40-frame quantize shift (60 -> 100) in the live
  // MIDI FX chain. all_notes_off() must retire that pairing state as well as
  // the sequencer's sounding-note table.
  MidiClipSchedule clip;
  clip.id = 501;
  clip.destination_id = 7;
  clip.events = {{60, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 128);
  REQUIRE(sink.events.size() == 1);
  REQUIRE(sink.events[0].event.ump.is_note_on());
  REQUIRE(sink.events[0].event.render_frame == 100);
  REQUIRE(seq.active_note_count() == 1);

  seq.all_notes_off(DeviceFrame{128});
  REQUIRE(seq.active_note_count() == 0);
  sink.events.clear();

  // A new stream starts at the origin. Its short gate must retain its own
  // zero shift and close at frame 20, rather than inheriting the old shaped
  // onset at 100 and being forced to frame 101.
  seq.inject_event(7, DeviceFrame{0}, sonare::midi::make_midi1_note_on(0, 0, 60, 100));
  seq.inject_event(7, DeviceFrame{20}, sonare::midi::make_midi1_note_off(0, 0, 60, 0));

  REQUIRE(sink.events.size() == 2);
  REQUIRE(sink.events[0].event.ump.is_note_on());
  REQUIRE(sink.events[0].event.render_frame == 0);
  REQUIRE(sink.events[1].event.ump.is_note_off());
  REQUIRE(sink.events[1].event.render_frame == 20);
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer keeps unchanged destination MIDI FX timing across updates", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain destination_a;
  sonare::midi::QuantizeConfig quantize_a;
  quantize_a.enabled = true;
  quantize_a.grid_frames = 100;
  quantize_a.strength = 1.0f;
  destination_a.set_quantize(quantize_a);
  REQUIRE(seq.set_midi_fx(7, destination_a));
  seq.acquire_midi_fx(DeviceFrame{0});

  MidiClipSchedule clip;
  clip.id = 502;
  clip.destination_id = 7;
  clip.events = {{60, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                 {80, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();

  // Leave the transformed note-on pending at frame 100 while the source
  // note-off remains in the next block. This preserves the A-chain timing
  // ledger across the unrelated B update below.
  seq.process_block(SequencerClock::aligned(0), 70);
  REQUIRE(sink.events.empty());

  MidiFxChain destination_b;
  sonare::midi::QuantizeConfig quantize_b;
  quantize_b.enabled = true;
  quantize_b.grid_frames = 80;
  quantize_b.strength = 1.0f;
  destination_b.set_quantize(quantize_b);
  REQUIRE(seq.set_midi_fx(8, destination_b));
  seq.acquire_midi_fx(DeviceFrame{70});

  seq.process_block(SequencerClock::aligned(70), 100);

  REQUIRE(sink.events.size() == 2);
  REQUIRE(sink.events[0].destination == 7);
  REQUIRE(sink.events[0].event.ump.is_note_on());
  REQUIRE(sink.events[0].event.render_frame == 100);
  REQUIRE(sink.events[1].destination == 7);
  REQUIRE(sink.events[1].event.ump.is_note_off());
  REQUIRE(sink.events[1].event.render_frame == 120);
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer releases an active clip note after its published end is shortened",
          "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule original;
  original.id = 503;
  original.track_id = 17;
  original.destination_id = 7;
  original.length_samples = 1000;
  original.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  seq.set_midi_clips({original});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 256);
  REQUIRE(seq.active_note_count() == 1);

  // The clip keeps the same identity, but its new exclusive end (128) is
  // already behind the playhead when the replacement is adopted at 256.
  MidiClipSchedule shortened = original;
  shortened.length_samples = 128;
  seq.set_midi_clips({shortened});
  seq.acquire_midi_clips();
  sink.events.clear();
  seq.process_block(SequencerClock::aligned(256), 64);

  REQUIRE(seq.active_note_count() == 0);
  REQUIRE(sink.events.size() == 1);
  REQUIRE(sink.events[0].destination == 7);
  REQUIRE(sink.events[0].event.ump.is_note_off());
  REQUIRE(sink.events[0].event.ump.note_number() == 60);
  REQUIRE(sink.events[0].event.render_frame == 256);
  REQUIRE(sink.events[0].event.source_track_id == original.track_id);
}

TEST_CASE("MidiSequencer clears only a removed clip's MIDI FX timing state", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 100;
  quantize.strength = 1.0f;
  fx.set_quantize(quantize);
  REQUIRE(seq.set_midi_fx(7, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  MidiClipSchedule removed;
  removed.id = 504;
  removed.track_id = 10;
  removed.destination_id = 7;
  removed.events = {{60, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  MidiClipSchedule retained;
  retained.id = 505;
  retained.track_id = 20;
  retained.destination_id = 7;
  retained.events = {{70, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  seq.set_midi_clips({removed, retained});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 128);
  REQUIRE(seq.active_note_count() == 2);

  // Remove only track 10 and publish a replacement clip on that same track.
  // Its new onset at 170 quantizes to 200 (+30 shift), so a stale +40 timing
  // entry from the removed onset would move its off to frame 230 instead of
  // the correct frame 220.
  MidiClipSchedule replacement;
  replacement.id = 506;
  replacement.track_id = 10;
  replacement.destination_id = 7;
  replacement.events = {{170, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                        {190, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  seq.set_midi_clips({replacement, retained});
  seq.acquire_midi_clips();
  sink.events.clear();
  seq.process_block(SequencerClock::aligned(128), 128);

  bool saw_removed_off = false;
  bool saw_replacement_off = false;
  for (const auto& captured : sink.events) {
    if (captured.destination == 7 && captured.event.source_track_id == 10 &&
        captured.event.ump.is_note_off() && captured.event.ump.note_number() == 60) {
      if (captured.event.render_frame == 128) {
        saw_removed_off = true;
      } else if (captured.event.render_frame == 220) {
        saw_replacement_off = true;
      } else {
        FAIL("unexpected source-track note-off frame");
      }
    }
  }
  REQUIRE(saw_removed_off);
  REQUIRE(saw_replacement_off);
  REQUIRE(seq.active_note_count() == 1);  // retained track 20 is still held.
}

TEST_CASE("MidiSequencer clears matching MIDI FX timing after a channel-mode reset", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 100;
  quantize.strength = 1.0f;
  fx.set_quantize(quantize);
  REQUIRE(seq.set_midi_fx(7, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  MidiClipSchedule clip;
  clip.id = 507;
  clip.track_id = 10;
  clip.destination_id = 7;
  clip.events = {{60, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 128);
  REQUIRE(seq.active_note_count() == 1);

  seq.inject_event(7, DeviceFrame{128}, sonare::midi::make_midi1_control_change(0, 0, 120, 0));
  REQUIRE(seq.active_note_count() == 0);

  // Keep the clip identity so only the channel-mode reset can retire the old
  // timing entry. The new in-block gate quantizes 170 -> 200 (+30), and its
  // off must land at 220 rather than inherit the old +40 shift and land at
  // 230.
  clip.events = {{60, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                 {170, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                 {190, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  sink.events.clear();
  seq.process_block(SequencerClock::aligned(128), 128);

  bool saw_restarted_off = false;
  for (const auto& captured : sink.events) {
    if (captured.destination == 7 && captured.event.source_track_id == 10 &&
        captured.event.ump.is_note_off() && captured.event.ump.note_number() == 60) {
      REQUIRE(captured.event.render_frame == 220);
      saw_restarted_off = true;
    }
  }
  REQUIRE(saw_restarted_off);
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer channel-mode resets retire the active-note ledger", "[midi]") {
  constexpr std::array<uint8_t, 6> kResetControllers = {120, 123, 124, 125, 126, 127};
  for (const uint8_t controller : kResetControllers) {
    INFO("controller=" << static_cast<int>(controller));
    MidiSequencer seq;
    CapturingSink sink;
    seq.prepare(48000.0);
    seq.set_sink(&sink);

    constexpr uint32_t kDestination = 31;
    for (size_t i = 0; i < MidiSequencer::kMaxActiveNotes; ++i) {
      const uint8_t channel = static_cast<uint8_t>((i / 128u) & 0x0Fu);
      const uint8_t note = static_cast<uint8_t>(i % 128u);
      seq.inject_event(kDestination, DeviceFrame{static_cast<int64_t>(i)},
                       sonare::midi::make_midi1_note_on(0, channel, note, 100));
    }
    REQUIRE(seq.active_note_count() == MidiSequencer::kMaxActiveNotes);
    REQUIRE(seq.active_note_overflow_count() == 0);

    seq.inject_event(kDestination, DeviceFrame{1000},
                     sonare::midi::make_midi1_control_change(0, 0, controller, 0));
    // The reset is channel-scoped: the 128 notes on channel 1 remain tracked
    // while all 128 notes on channel 0 are retired.
    REQUIRE(seq.active_note_count() == 128);
    REQUIRE(seq.active_note_overflow_count() == 0);

    const size_t dispatched_after_reset = sink.events.size();
    seq.inject_event(kDestination, DeviceFrame{1001},
                     sonare::midi::make_midi1_note_on(0, 0, 60, 100));
    REQUIRE(seq.active_note_count() == 129);
    REQUIRE(seq.active_note_overflow_count() == 0);
    REQUIRE(sink.events.size() > dispatched_after_reset);
    REQUIRE(seq.retained_channel_overflow_count() == 0);
  }
}

TEST_CASE("MidiSequencer channel-mode reset cancels only matching pending MIDI FX notes",
          "[midi]") {
  const auto configure_arpeggiator = [](MidiFxChain& fx) {
    sonare::midi::ArpeggiatorConfig arp;
    arp.enabled = true;
    arp.steps = 2;
    arp.intervals[0] = 0;
    arp.intervals[1] = 12;
    arp.step_frames = 40;
    arp.gate_frames = 10;
    fx.set_arpeggiator(arp);
  };

  SECTION("destination isolation") {
    MidiSequencer seq;
    CapturingSink sink;
    seq.prepare(48000.0);
    seq.set_sink(&sink);
    MidiFxChain destination_a;
    MidiFxChain destination_b;
    configure_arpeggiator(destination_a);
    configure_arpeggiator(destination_b);
    REQUIRE(seq.set_midi_fx(9, destination_a));
    REQUIRE(seq.set_midi_fx(10, destination_b));
    seq.acquire_midi_fx(DeviceFrame{0});

    seq.inject_event(9, DeviceFrame{0}, sonare::midi::make_midi1_note_on(0, 0, 60, 100));
    seq.inject_event(10, DeviceFrame{0}, sonare::midi::make_midi1_note_on(0, 0, 60, 100));
    sink.events.clear();
    seq.inject_event(9, DeviceFrame{20}, sonare::midi::make_midi1_control_change(0, 0, 120, 0));
    seq.process_block(SequencerClock::aligned(20), 64);

    size_t surviving_destination_b = 0;
    for (const auto& captured : sink.events) {
      if (captured.destination == 9 && captured.event.render_frame >= 40) {
        FAIL("channel-mode reset leaked a pending note on its own destination");
      }
      if (captured.destination == 10 && captured.event.render_frame >= 40) {
        ++surviving_destination_b;
      }
    }
    REQUIRE(surviving_destination_b == 2);
  }

  SECTION("group and channel isolation") {
    MidiSequencer seq;
    CapturingSink sink;
    seq.prepare(48000.0);
    seq.set_sink(&sink);
    MidiFxChain destination;
    configure_arpeggiator(destination);
    REQUIRE(seq.set_midi_fx(9, destination));
    seq.acquire_midi_fx(DeviceFrame{0});

    seq.inject_event(9, DeviceFrame{0}, sonare::midi::make_midi1_note_on(0, 0, 60, 100));
    seq.inject_event(9, DeviceFrame{0}, sonare::midi::make_midi1_note_on(0, 1, 60, 100));
    seq.inject_event(9, DeviceFrame{0}, sonare::midi::make_midi1_note_on(1, 0, 60, 100));
    seq.inject_event(9, DeviceFrame{0}, sonare::midi::make_midi1_note_on(1, 1, 60, 100));
    sink.events.clear();
    seq.inject_event(9, DeviceFrame{20}, sonare::midi::make_midi1_control_change(0, 0, 123, 0));
    seq.process_block(SequencerClock::aligned(20), 64);

    size_t surviving_other_lanes = 0;
    for (const auto& captured : sink.events) {
      if (captured.event.render_frame < 40) continue;
      const uint8_t group = captured.event.ump.group;
      const uint8_t channel = captured.event.ump.channel();
      if (group == 0 && channel == 0) {
        FAIL("channel-mode reset leaked a pending note on its own channel");
      }
      if ((group == 0 && channel == 1) || (group == 1 && channel == 0) ||
          (group == 1 && channel == 1)) {
        ++surviving_other_lanes;
      }
    }
    REQUIRE(surviving_other_lanes == 6);
  }
}

TEST_CASE("MidiSequencer removes queued notes before dispatching a queued channel reset",
          "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::ArpeggiatorConfig arp;
  arp.enabled = true;
  arp.steps = 2;
  arp.intervals[0] = 0;
  arp.intervals[1] = 12;
  arp.step_frames = 200;
  arp.gate_frames = 50;
  fx.set_arpeggiator(arp);
  sonare::midi::QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 100;
  quantize.strength = 1.0f;
  fx.set_quantize(quantize);
  REQUIRE(seq.set_midi_fx(9, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  // The second arpeggiator gate is pending in the future. The controller reset
  // is also pending (60 -> 100), so its cleanup must remove the selected slot
  // safely before it erases the remaining future note slots.
  seq.inject_event(9, DeviceFrame{0}, sonare::midi::make_midi1_note_on(0, 0, 60, 100));
  seq.inject_event(9, DeviceFrame{60}, sonare::midi::make_midi1_control_change(0, 0, 120, 0));
  sink.events.clear();
  seq.process_block(SequencerClock::aligned(61), 300);

  bool saw_queued_reset = false;
  for (const auto& captured : sink.events) {
    if (captured.destination != 9) continue;
    if (captured.event.render_frame == 100 &&
        captured.event.ump.status_nibble() ==
            static_cast<uint8_t>(sonare::midi::UmpStatus::kControlChange)) {
      saw_queued_reset = true;
    }
    REQUIRE((captured.event.render_frame < 200 || !captured.event.ump.is_note_on()));
  }
  REQUIRE(saw_queued_reset);
  REQUIRE(seq.midi_fx_pending_overflow_count() == 0);
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer all_notes_off releases sounding notes (hang-note safety)", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 3;
  clip.events = {
      {10, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
      {20, sonare::midi::make_midi1_note_on(0, 1, 64, 90)},
      {30, sonare::midi::make_midi1_note_on(0, 2, 67, 80)},
  };
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 128);
  REQUIRE(seq.active_note_count() == 3);

  const uint32_t before = seq.dispatched_event_count();
  sink.events.clear();
  seq.all_notes_off(DeviceFrame{/*render_frame=*/128});
  REQUIRE(seq.active_note_count() == 0);
  // 3 note-offs plus the 4-message controller-reset sequence (damper / reset-all
  // / all-notes-off / pitch-bend) on each of the 3 distinct channels.
  size_t note_offs = 0;
  for (const auto& cap : sink.events) {
    REQUIRE(cap.destination == 3);
    REQUIRE(cap.event.render_frame == 128);
    if (cap.event.ump.is_note_off()) ++note_offs;
  }
  REQUIRE(note_offs == 3);
  REQUIRE(sink.events.size() == 3 + 3 * 4);
  REQUIRE(seq.dispatched_event_count() == before + 3 + 3 * 4);

  // A second all_notes_off is a no-op (nothing sounding).
  sink.events.clear();
  seq.all_notes_off(DeviceFrame{256});
  REQUIRE(sink.events.empty());
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer one-shot clip end releases only that clip's sounding notes", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule ending;
  ending.id = 11;
  ending.start_sample = 0;
  ending.length_samples = 50;
  ending.destination_id = 7;
  ending.events = {{10, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                   {70, sonare::midi::make_midi1_note_on(0, 0, 62, 100)}};

  MidiClipSchedule open;
  open.id = 12;
  open.destination_id = 7;
  open.events = {{20, sonare::midi::make_midi1_note_on(0, 0, 65, 100)}};

  seq.set_midi_clips({ending, open});
  seq.acquire_midi_clips();

  const std::vector<int> boundaries = event_offsets(seq, 0, 96);
  REQUIRE(boundaries.size() == 3);
  REQUIRE(boundaries[0] == 10);
  REQUIRE(boundaries[1] == 20);
  REQUIRE(boundaries[2] == 50);

  seq.process_block(SequencerClock::aligned(0), 96);

  REQUIRE(sink.events.size() == 3);
  REQUIRE(sink.events[0].event.render_frame == 10);
  REQUIRE(sink.events[0].event.ump.is_note_on());
  REQUIRE(sink.events[0].event.ump.note_number() == 60);
  REQUIRE(sink.events[1].event.render_frame == 20);
  REQUIRE(sink.events[1].event.ump.is_note_on());
  REQUIRE(sink.events[1].event.ump.note_number() == 65);
  REQUIRE(sink.events[2].event.render_frame == 50);
  REQUIRE(sink.events[2].event.ump.is_note_off());
  REQUIRE(sink.events[2].event.ump.note_number() == 60);
  REQUIRE(seq.active_note_count() == 1);

  sink.events.clear();
  seq.all_notes_off(DeviceFrame{96});
  size_t note_offs = 0;
  for (const auto& cap : sink.events) {
    if (cap.event.ump.is_note_off()) {
      ++note_offs;
      REQUIRE(cap.event.ump.note_number() == 65);
    }
  }
  REQUIRE(note_offs == 1);
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer loops MIDI clip schedules on the RT path", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule clip;
  clip.id = 21;
  clip.start_sample = 0;
  clip.length_samples = 120;
  clip.loop_mode = sonare::midi::MidiLoopMode::kLoop;
  clip.loop_length_samples = 40;
  clip.destination_id = 9;
  clip.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                 {20, sonare::midi::make_midi1_note_off(0, 0, 60, 0)},
                 {30, sonare::midi::make_midi1_note_on(0, 0, 64, 100)}};

  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();

  const std::vector<int> boundaries = event_offsets(seq, 0, 128);
  REQUIRE(boundaries.size() == 10);
  REQUIRE(boundaries[0] == 0);
  REQUIRE(boundaries[1] == 20);
  REQUIRE(boundaries[2] == 30);
  REQUIRE(boundaries[3] == 40);
  REQUIRE(boundaries[4] == 60);
  REQUIRE(boundaries[5] == 70);
  REQUIRE(boundaries[6] == 80);
  REQUIRE(boundaries[7] == 100);
  REQUIRE(boundaries[8] == 110);
  REQUIRE(boundaries[9] == 120);

  seq.process_block(SequencerClock::aligned(0), 128);

  REQUIRE(sink.events.size() == 12);
  REQUIRE(sink.events[0].event.render_frame == 0);
  REQUIRE(sink.events[1].event.render_frame == 20);
  REQUIRE(sink.events[2].event.render_frame == 30);
  REQUIRE(sink.events[3].event.render_frame == 40);
  REQUIRE(sink.events[3].event.ump.is_note_off());
  REQUIRE(sink.events[3].event.ump.note_number() == 64);
  REQUIRE(sink.events[4].event.render_frame == 40);
  REQUIRE(sink.events[4].event.ump.is_note_on());
  REQUIRE(sink.events[5].event.render_frame == 60);
  REQUIRE(sink.events[6].event.render_frame == 70);
  REQUIRE(sink.events[7].event.render_frame == 80);
  REQUIRE(sink.events[7].event.ump.is_note_off());
  REQUIRE(sink.events[7].event.ump.note_number() == 64);
  REQUIRE(sink.events[8].event.render_frame == 80);
  REQUIRE(sink.events[8].event.ump.is_note_on());
  REQUIRE(sink.events[9].event.render_frame == 100);
  REQUIRE(sink.events[10].event.render_frame == 110);
  REQUIRE(sink.events[11].event.render_frame == 120);
  REQUIRE(sink.events[11].event.ump.is_note_off());
  REQUIRE(sink.events[11].event.ump.note_number() == 64);
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer treats MIDI 1.0 note-on velocity zero as note-off", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 3;
  clip.events = {
      {10, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
      {20, sonare::midi::make_midi1_note_on(0, 0, 60, 0)},
  };
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 128);

  REQUIRE(sink.events.size() == 2);
  REQUIRE(seq.active_note_count() == 0);
  REQUIRE(sink.events[1].event.ump.is_note_off());
}

TEST_CASE("MidiSequencer surfaces active-note overflow without growing", "[midi]") {
  MidiSequencer seq;
  sonare::midi::NullMidiEventSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  // Drive more than kMaxActiveNotes simultaneous note-ons across channels/notes.
  const size_t total = MidiSequencer::kMaxActiveNotes + 10;
  MidiClipSchedule clip;
  clip.id = 1;
  clip.events.reserve(total);
  for (size_t i = 0; i < total; ++i) {
    const uint8_t channel = static_cast<uint8_t>((i / 128) & 0x0Fu);
    const uint8_t note = static_cast<uint8_t>(i % 128);
    clip.events.push_back(
        {static_cast<int64_t>(i), sonare::midi::make_midi1_note_on(0, channel, note, 100)});
  }
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), static_cast<int>(total) + 1);

  REQUIRE(seq.active_note_count() == MidiSequencer::kMaxActiveNotes);
  REQUIRE(seq.active_note_overflow_count() == 10);
  // all_notes_off must still cleanly release the tracked notes.
  seq.all_notes_off(DeviceFrame{static_cast<int64_t>(total)});
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer note-off is keyed by destination", "[midi]") {
  // Same group/channel/note sounding on two destinations. A note-off on one
  // destination must release only that destination's note, never the other's.
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule a;
  a.id = 1;
  a.destination_id = 10;
  a.events = {{10, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};  // held, never released
  MidiClipSchedule b;
  b.id = 2;
  b.destination_id = 20;
  b.events = {{20, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
              {30, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  seq.set_midi_clips({a, b});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 128);

  // Destination 20's note-off released only destination 20's note; destination
  // 10's identically-pitched note is still sounding.
  REQUIRE(seq.active_note_count() == 1);

  sink.events.clear();
  seq.all_notes_off(DeviceFrame{128});
  // 1 note-off (dest 10) plus one 4-message controller reset for each
  // destination that received a channel event. Destination 20's note-off
  // already removed its active note, but its channel state remains retained
  // until this global reset.
  REQUIRE(sink.events.size() == 1 + 2 * 4);
  size_t note_offs = 0;
  size_t destination10_events = 0;
  size_t destination20_events = 0;
  for (const auto& cap : sink.events) {
    REQUIRE((cap.destination == 10 || cap.destination == 20));
    if (cap.destination == 10) {
      ++destination10_events;
    } else {
      ++destination20_events;
    }
    if (cap.event.ump.is_note_off()) ++note_offs;
  }
  REQUIRE(note_offs == 1);
  REQUIRE(destination10_events == 1 + 4);
  REQUIRE(destination20_events == 4);
  REQUIRE(sink.events[0].event.ump.is_note_off());  // note-off dispatched before the resets
  REQUIRE(sink.events[0].destination == 10);
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer suppresses dispatch of untracked note-on on overflow", "[midi]") {
  // A note-on the active table cannot track would hang (all_notes_off can never
  // release it), so the sequencer must NOT dispatch it.
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  const size_t total = MidiSequencer::kMaxActiveNotes + 5;
  MidiClipSchedule clip;
  clip.id = 1;
  clip.events.reserve(total);
  for (size_t i = 0; i < total; ++i) {
    const uint8_t channel = static_cast<uint8_t>((i / 128) & 0x0Fu);
    const uint8_t note = static_cast<uint8_t>(i % 128);
    clip.events.push_back(
        {static_cast<int64_t>(i), sonare::midi::make_midi1_note_on(0, channel, note, 100)});
  }
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), static_cast<int>(total) + 1);

  REQUIRE(seq.active_note_count() == MidiSequencer::kMaxActiveNotes);
  REQUIRE(seq.active_note_overflow_count() == 5);
  // The 5 overflow note-ons were suppressed, not dispatched.
  REQUIRE(sink.events.size() == MidiSequencer::kMaxActiveNotes);
}

TEST_CASE("MidiSequencer reports the frames of a block's events", "[midi]") {
  MidiSequencer seq;
  sonare::midi::NullMidiEventSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule clip;
  clip.id = 1;
  clip.events = {
      {0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
      {64, sonare::midi::make_midi1_note_on(0, 0, 62, 100)},
      {64, sonare::midi::make_midi1_note_off(0, 0, 60, 0)},  // duplicate offset deduped.
      {200, sonare::midi::make_midi1_note_off(0, 0, 62, 0)},
      {400, sonare::midi::make_midi1_note_on(0, 0, 64, 100)},  // out of block.
  };
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();

  const std::vector<int> out = event_offsets(seq, 0, 256);
  REQUIRE(out.size() == 3);
  REQUIRE(out[0] == 0);
  REQUIRE(out[1] == 64);
  REQUIRE(out[2] == 200);
}

TEST_CASE("MidiSequencer takes a clip event's UMP group from its own word0", "[midi]") {
  // A UMP carries its group in word[0] bits 24..27; Ump::group is a cache of
  // that nibble, and every core constructor writes both from one argument. The
  // binding surfaces do not: SonareEngineMidiEvent.group and the WASM event
  // object's `group` are separate fields that DEFAULT TO 0, while word0 is
  // authored by the caller (the C header documents packing the group into it).
  // A caller doing exactly what the header describes therefore hands the core a
  // pair that disagrees, and the two halves would then be read by different
  // consumers: routing / note tracking / MIDI FX read Ump::group, while the
  // bytes written to a device or an SMF2 file come from word0.
  constexpr uint8_t kGroup = 5;
  const auto packed = [](uint8_t status, uint8_t note, uint8_t velocity) {
    Ump ump;
    ump.words[0] = (uint32_t{0x2} << 28) | (uint32_t{kGroup} << 24) | (uint32_t{status} << 20) |
                   (uint32_t{note} << 8) | uint32_t{velocity};
    ump.word_count = 1;
    ump.group = 0;  // left at the default every binding surface supplies
    return ump;
  };

  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 3;
  clip.events = {
      {32, packed(0x9, 60, 100)},
      {96, packed(0x8, 60, 0)},
  };
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 256);

  REQUIRE(sink.events.size() == 2);
  // The delivered events agree with the wire form they were authored in.
  REQUIRE(sink.events[0].event.ump.group == kGroup);
  REQUIRE(sink.events[1].event.ump.group == kGroup);
  REQUIRE(sink.events[0].event.ump.words[0] == clip.events[0].ump.words[0]);
  // The note-off matches the note-on it belongs to. Read under two different
  // groups the pair never pairs up and the note is left sounding forever.
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer keeps a clip event's group when it already agrees with word0", "[midi]") {
  // Normalization must be a no-op for every event the core itself mints, so a
  // conforming caller sees no behaviour change at all.
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 3;
  clip.events = {
      {32, sonare::midi::make_midi1_note_on(9, 2, 60, 100)},
      {96, sonare::midi::make_midi2_note_off(9, 2, 60, 0)},
  };
  const Ump before_on = clip.events[0].ump;
  const Ump before_off = clip.events[1].ump;
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 256);

  REQUIRE(sink.events.size() == 2);
  REQUIRE(sink.events[0].event.ump == before_on);
  REQUIRE(sink.events[1].event.ump == before_off);
  REQUIRE(sink.events[0].event.ump.group == 9);
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer gives a groupless clip event group 0, not the packed nibble", "[midi]") {
  // Two message types carry no group and put something else in word[0] bits
  // 24..27, so neither the caller-supplied field nor a blind read of the nibble
  // yields a group for them. A UMP Stream Start packet is the sharp case: its
  // `form` field alone puts 0b01 in the top two bits of that nibble, so reading
  // it as a group returns 4 on entirely well-formed input.
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  Ump stream;
  stream.words[0] = (uint32_t{0xF} << 28) | (uint32_t{0b01} << 26);
  stream.word_count = 4;
  stream.group = 7;  // a caller-supplied group that the message cannot carry
  Ump utility;
  utility.words[0] = (uint32_t{0x0} << 28) | (uint32_t{6} << 24) | 0x0002'0000u;
  utility.word_count = 1;
  utility.group = 7;

  // Reading the nibble is what the derivation would otherwise return, and it is
  // neither 0 nor the caller's 7 -- so this case separates all three readings.
  REQUIRE(((stream.words[0] >> 24) & 0x0Fu) == 4u);
  REQUIRE(((utility.words[0] >> 24) & 0x0Fu) == 6u);

  MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 3;
  clip.events = {{32, stream}, {96, utility}};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 256);

  REQUIRE(sink.events.size() == 2);
  REQUIRE(sink.events[0].event.ump.group == 0);
  REQUIRE(sink.events[1].event.ump.group == 0);
  // word0 is untouched: normalization only ever rewrites the cached field.
  REQUIRE(sink.events[0].event.ump.words[0] == stream.words[0]);
  REQUIRE(sink.events[1].event.ump.words[0] == utility.words[0]);
}

TEST_CASE("MidiSequencer clip removal preserves another clip's gate on the same track", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);
  MidiFxChain fx;
  sonare::midi::QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 100;
  fx.set_quantize(quantize);
  REQUIRE(seq.set_midi_fx(7, fx));
  seq.acquire_midi_fx(DeviceFrame{0});

  MidiClipSchedule removed;
  removed.id = 901;
  removed.track_id = 10;
  removed.destination_id = 7;
  removed.length_samples = 1000;
  removed.events = {{60, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  MidiClipSchedule surviving = removed;
  surviving.id = 902;
  surviving.events = {{70, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                      {190, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  seq.set_midi_clips({removed, surviving});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 128);
  REQUIRE(seq.active_note_count() == 2);
  sink.events.clear();
  seq.set_midi_clips({surviving});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(128), 128);
  REQUIRE(sink.events.size() == 2);
  CHECK(sink.events[0].event.render_frame == 128);
  CHECK(sink.events[1].event.render_frame == 220);
  CHECK(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer pending clip trim preserves an earlier overlapping note's gate",
          "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);
  MidiFxChain fx;
  sonare::midi::QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 100;
  fx.set_quantize(quantize);
  REQUIRE(seq.set_midi_fx(7, fx));
  seq.acquire_midi_fx(DeviceFrame{0});
  MidiClipSchedule clip;
  clip.id = 903;
  clip.track_id = 10;
  clip.destination_id = 7;
  clip.length_samples = 1000;
  clip.events = {{99, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                 {151, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                 {195, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 128);
  seq.process_block(SequencerClock::aligned(128), 64);
  REQUIRE(seq.active_note_count() == 1);
  sink.events.clear();

  // Cancel the second onset at 200 while keeping the first onset's +1 shift.
  clip.length_samples = 199;
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(192), 64);
  REQUIRE(sink.events.size() == 1);
  CHECK(sink.events[0].event.ump.is_note_off());
  CHECK(sink.events[0].event.render_frame == 196);
  CHECK(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer saturates a clip ending past INT64_MAX like the clip envelope", "[midi]") {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule clip;
  clip.id = 31;
  clip.destination_id = 3;
  clip.start_sample = kMax - 100;
  clip.length_samples = 1000;
  clip.gain = 0.5f;
  clip.events = {{kMax - 90, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  const std::vector<MidiClipSchedule> clips{clip};
  seq.set_midi_clips(clips);
  seq.acquire_midi_clips();

  // The envelope saturates the end at INT64_MAX: active just before it, ended at it.
  REQUIRE(sonare::midi::find_midi_clip_envelope_winner(clips, 3, kMax - 1).active != nullptr);
  REQUIRE(sonare::midi::find_midi_clip_envelope_winner(clips, 3, kMax).active == nullptr);

  const std::vector<int> boundaries = event_offsets(seq, kMax - 200, 512);
  REQUIRE(boundaries.size() == 1);
  REQUIRE(boundaries[0] == 110);

  seq.process_block(SequencerClock::aligned(kMax - 200), 512);
  REQUIRE(sink.events.size() == 2);
  CHECK(sink.events[0].event.render_frame == kMax - 90);
  CHECK(sink.events[0].event.ump.is_note_on());
  // Released at the same saturated end the envelope reports.
  CHECK(sink.events[1].event.render_frame == kMax);
  CHECK(sink.events[1].event.ump.is_note_off());
  CHECK(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer loops a clip whose iterations run past INT64_MAX", "[midi]") {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule clip;
  clip.id = 32;
  clip.destination_id = 3;
  clip.start_sample = kMax - 100;
  clip.loop_mode = sonare::midi::MidiLoopMode::kLoop;
  clip.loop_length_samples = 30;
  clip.events = {{kMax - 95, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                 {kMax - 80, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();

  const std::vector<int> boundaries = event_offsets(seq, kMax - 100, 512);
  const std::vector<int> expected_offsets{5, 20, 30, 35, 50, 60, 65, 80, 90, 95};
  REQUIRE(boundaries.size() == expected_offsets.size());
  for (size_t i = 0; i < expected_offsets.size(); ++i) CHECK(boundaries[i] == expected_offsets[i]);

  seq.process_block(SequencerClock::aligned(kMax - 100), 512);
  const std::vector<int64_t> expected_frames{kMax - 95, kMax - 80, kMax - 65, kMax - 50,
                                             kMax - 35, kMax - 20, kMax - 5};
  REQUIRE(sink.events.size() == expected_frames.size());
  for (size_t i = 0; i < expected_frames.size(); ++i) {
    CHECK(sink.events[i].event.render_frame == expected_frames[i]);
    CHECK(sink.events[i].event.ump.is_note_on() == (i % 2 == 0));
  }
  CHECK(seq.active_note_count() == 1);
}

TEST_CASE("MidiSequencer dispatches a late block of long clips in merged order", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  constexpr int kEvents = 10000;
  MidiClipSchedule even;
  even.id = 41;
  even.destination_id = 5;
  MidiClipSchedule odd;
  odd.id = 42;
  odd.destination_id = 5;
  MidiClipSchedule looped;
  looped.id = 43;
  looped.destination_id = 6;
  looped.loop_mode = sonare::midi::MidiLoopMode::kLoop;
  looped.loop_length_samples = 1000;
  for (int i = 0; i < kEvents; ++i) {
    const bool on = i % 2 == 0;
    even.events.push_back({int64_t{10} * i, on ? sonare::midi::make_midi1_note_on(0, 0, 60, 100)
                                               : sonare::midi::make_midi1_note_off(0, 0, 60, 0)});
    odd.events.push_back(
        {int64_t{10} * i + 5, on ? sonare::midi::make_midi1_note_on(0, 1, 62, 100)
                                 : sonare::midi::make_midi1_note_off(0, 1, 62, 0)});
  }
  for (int64_t local = 0; local < 1000; local += 7) {
    looped.events.push_back({local, sonare::midi::make_midi1_control_change(0, 2, 1, 64)});
  }
  seq.set_midi_clips({even, odd, looped});
  seq.acquire_midi_clips();

  // Fifty loop iterations in, and midway through the one-shot clips.
  const int64_t block_start = 50333;
  const int block_frames = 256;
  std::vector<std::pair<int64_t, uint32_t>> expected;
  for (int64_t frame = block_start; frame < block_start + block_frames; ++frame) {
    const bool has_even_clip_event = frame % 10 == 0;
    const bool has_odd_clip_event = frame % 10 == 5;
    const bool even_is_on = has_even_clip_event && ((frame / 10) % 2 == 0);
    const bool odd_is_on = has_odd_clip_event && (((frame - 5) / 10) % 2 == 0);
    const bool has_loop_controller = (frame % 1000) % 7 == 0;

    // This expected stream is the same merged set as before, with the
    // same_time_rank contract made explicit for collisions: note-off (rank 0),
    // general controller (rank 4), then note-on (rank 5). The odd/even source
    // event index identifies whether each note is an off or an on.
    if (has_even_clip_event && !even_is_on) expected.emplace_back(frame, 5u);
    if (has_odd_clip_event && !odd_is_on) expected.emplace_back(frame, 5u);
    if (has_loop_controller) expected.emplace_back(frame, 6u);
    if (has_even_clip_event && even_is_on) expected.emplace_back(frame, 5u);
    if (has_odd_clip_event && odd_is_on) expected.emplace_back(frame, 5u);
  }

  seq.process_block(SequencerClock::aligned(block_start), block_frames);
  REQUIRE(sink.events.size() == expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    CHECK(sink.events[i].event.render_frame == expected[i].first);
    CHECK(sink.events[i].destination == expected[i].second);
  }
}
