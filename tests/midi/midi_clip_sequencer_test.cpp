/// @file midi_clip_sequencer_test.cpp
/// @brief MIDI core: MidiClip ordering / note-pair validation /
///        PPQ->frame rendering, and the RT MidiSequencer dispatch +
///        hang-note safety + overflow + boundary collection.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "midi/midi_clip.h"
#include "midi/midi_event.h"
#include "midi/sequencer.h"
#include "midi/ump.h"
#include "transport/tempo_map.h"

namespace {

using sonare::midi::MidiClip;
using sonare::midi::MidiClipEvent;
using sonare::midi::MidiClipSchedule;
using sonare::midi::MidiEvent;
using sonare::midi::MidiEventSink;
using sonare::midi::MidiSequencer;
using sonare::midi::Ump;

MidiClipEvent ev(double ppq, const Ump& ump) {
  MidiClipEvent e;
  e.ppq = ppq;
  e.ump = ump;
  return e;
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

}  // namespace

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
  seq.process_block(0, 256);
  REQUIRE(sink.events.size() == 2);
  REQUIRE(sink.events[0].destination == 7);
  REQUIRE(sink.events[0].event.render_frame == 100);
  REQUIRE(sink.events[0].event.ump.is_note_on());
  REQUIRE(sink.events[1].event.render_frame == 150);
  REQUIRE(sink.events[1].event.ump.is_note_off());

  // Block 1: [256,512) captures the third event only.
  seq.process_block(256, 256);
  REQUIRE(sink.events.size() == 3);
  REQUIRE(sink.events[2].event.render_frame == 300);

  // Block 2: [512,768) captures the fourth.
  seq.process_block(512, 256);
  REQUIRE(sink.events.size() == 4);
  REQUIRE(sink.events[3].event.render_frame == 600);
  REQUIRE(seq.dispatched_event_count() == 4);
  REQUIRE(seq.active_note_count() == 0);  // every note-on was released.
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
  seq.process_block(0, 128);
  REQUIRE(seq.active_note_count() == 3);

  const uint32_t before = seq.dispatched_event_count();
  sink.events.clear();
  seq.all_notes_off(/*render_frame=*/128);
  REQUIRE(seq.active_note_count() == 0);
  REQUIRE(sink.events.size() == 3);
  for (const auto& cap : sink.events) {
    REQUIRE(cap.destination == 3);
    REQUIRE(cap.event.render_frame == 128);
    REQUIRE(cap.event.ump.is_note_off());
  }
  REQUIRE(seq.dispatched_event_count() == before + 3);

  // A second all_notes_off is a no-op (nothing sounding).
  sink.events.clear();
  seq.all_notes_off(256);
  REQUIRE(sink.events.empty());
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
  seq.process_block(0, 128);

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
  seq.process_block(0, static_cast<int>(total) + 1);

  REQUIRE(seq.active_note_count() == MidiSequencer::kMaxActiveNotes);
  REQUIRE(seq.active_note_overflow_count() == 10);
  // all_notes_off must still cleanly release the tracked notes.
  seq.all_notes_off(static_cast<int64_t>(total));
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer collect_boundaries returns in-block event offsets", "[midi]") {
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

  MidiSequencer::BoundaryOffsets out;
  seq.collect_boundaries(0, 256, &out);
  REQUIRE_FALSE(out.overflowed);
  REQUIRE(out.size == 3);
  REQUIRE(out.offsets[0] == 0);
  REQUIRE(out.offsets[1] == 64);
  REQUIRE(out.offsets[2] == 200);
}
