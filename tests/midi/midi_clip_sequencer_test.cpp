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
using sonare::midi::MidiFxChain;
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

TEST_CASE("MidiSequencer dispatches pre-resolved SysEx payload views", "[midi]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  const std::vector<uint8_t> payload = {0xF0, 0x7D, 0x20, 0x21, 0xF7};
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
  seq.acquire_midi_clips();

  seq.process_block(0, 256);
  REQUIRE(sink.events.size() == 1);
  REQUIRE(sink.events.front().destination == 7);
  REQUIRE(sink.events.front().event.render_frame == 120);
  REQUIRE(sink.events.front().event.ump.sysex_handle == 77);
  REQUIRE(sink.events.front().event.sysex_payload == payload.data());
  REQUIRE(sink.events.front().event.sysex_payload_size == payload.size());
  REQUIRE(std::vector<uint8_t>(sink.events.front().event.sysex_payload,
                               sink.events.front().event.sysex_payload +
                                   sink.events.front().event.sysex_payload_size) == payload);
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
  seq.process_block(0, 64);

  REQUIRE(sink.events.size() == 6);
  REQUIRE(sink.events[0].destination == 7);
  REQUIRE(sink.events[0].event.ump.note_number() == 72);
  REQUIRE(sink.events[1].destination == 7);
  REQUIRE(sink.events[1].event.ump.note_number() == 79);
  REQUIRE(sink.events[2].destination == 7);
  REQUIRE(sink.events[2].event.ump.is_note_off());
  REQUIRE(sink.events[2].event.ump.note_number() == 72);
  REQUIRE(sink.events[3].destination == 7);
  REQUIRE(sink.events[3].event.ump.note_number() == 79);
  REQUIRE(sink.events[4].destination == 8);
  REQUIRE(sink.events[4].event.ump.note_number() == 60);
  REQUIRE(sink.events[5].destination == 8);
  REQUIRE(sink.events[5].event.ump.note_number() == 60);
  REQUIRE(seq.active_note_count() == 0);
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

  MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 9;
  clip.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                 {120, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();

  seq.process_block(0, 32);
  REQUIRE(sink.events.size() == 2);
  REQUIRE(sink.events[0].event.render_frame == 0);
  REQUIRE(sink.events[0].event.ump.is_note_on());
  REQUIRE(sink.events[1].event.render_frame == 10);
  REQUIRE(sink.events[1].event.ump.is_note_off());
  REQUIRE(seq.active_note_count() == 0);

  seq.process_block(32, 32);
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

  MidiClipSchedule clip;
  clip.id = 121;
  clip.destination_id = 9;
  clip.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();

  seq.process_block(0, 32);
  REQUIRE(sink.events.size() == 2);

  sink.events.clear();
  seq.process_block(64, 32);
  REQUIRE(sink.events.size() == 2);
  REQUIRE(sink.events[0].event.render_frame == 64);
  REQUIRE(sink.events[0].event.ump.is_note_on());
  REQUIRE(sink.events[0].event.ump.note_number() == 72);
  REQUIRE(sink.events[1].event.render_frame == 64);
  REQUIRE(sink.events[1].event.ump.is_note_off());
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

  seq.process_block(0, 20);
  REQUIRE(sink.events.size() == 2);
  REQUIRE(sink.events[0].event.render_frame == 0);
  REQUIRE(sink.events[1].event.render_frame == 5);

  sink.events.clear();
  seq.process_block(20, 20);

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

  MidiClipSchedule clip;
  clip.id = 111;
  clip.destination_id = 7;
  clip.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                 {100, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(0, 64);

  REQUIRE(seq.active_note_count() == 1);
  REQUIRE(sink.events.size() == 1);
  REQUIRE(sink.events[0].event.ump.note_number() == 72);

  MidiFxChain bypass;
  sink.events.clear();
  REQUIRE(seq.set_midi_fx(7, bypass, 64));

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
  // 3 note-offs plus the 4-message controller-reset sequence (damper / reset-all
  // / all-notes-off / pitch-bend) on each of the 3 distinct channels (M-4).
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
  seq.all_notes_off(256);
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

  MidiSequencer::BoundaryOffsets boundaries;
  seq.collect_boundaries(0, 96, &boundaries);
  REQUIRE_FALSE(boundaries.overflowed);
  REQUIRE(boundaries.size == 3);
  REQUIRE(boundaries.offsets[0] == 10);
  REQUIRE(boundaries.offsets[1] == 20);
  REQUIRE(boundaries.offsets[2] == 50);

  seq.process_block(0, 96);

  REQUIRE(sink.events.size() == 3);
  REQUIRE(sink.events[0].event.render_frame == 10);
  REQUIRE(sink.events[0].event.ump.is_note_on());
  REQUIRE(sink.events[0].event.ump.note_number() == 60);
  REQUIRE(sink.events[1].event.render_frame == 50);
  REQUIRE(sink.events[1].event.ump.is_note_off());
  REQUIRE(sink.events[1].event.ump.note_number() == 60);
  REQUIRE(sink.events[2].event.render_frame == 20);
  REQUIRE(sink.events[2].event.ump.is_note_on());
  REQUIRE(sink.events[2].event.ump.note_number() == 65);
  REQUIRE(seq.active_note_count() == 1);

  sink.events.clear();
  seq.all_notes_off(96);
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

  MidiSequencer::BoundaryOffsets boundaries;
  seq.collect_boundaries(0, 128, &boundaries);
  REQUIRE_FALSE(boundaries.overflowed);
  REQUIRE(boundaries.size == 10);
  REQUIRE(boundaries.offsets[0] == 0);
  REQUIRE(boundaries.offsets[1] == 20);
  REQUIRE(boundaries.offsets[2] == 30);
  REQUIRE(boundaries.offsets[3] == 40);
  REQUIRE(boundaries.offsets[4] == 60);
  REQUIRE(boundaries.offsets[5] == 70);
  REQUIRE(boundaries.offsets[6] == 80);
  REQUIRE(boundaries.offsets[7] == 100);
  REQUIRE(boundaries.offsets[8] == 110);
  REQUIRE(boundaries.offsets[9] == 120);

  seq.process_block(0, 128);

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
  seq.process_block(0, 128);

  // Destination 20's note-off released only destination 20's note; destination
  // 10's identically-pitched note is still sounding.
  REQUIRE(seq.active_note_count() == 1);

  sink.events.clear();
  seq.all_notes_off(128);
  // 1 note-off (dest 10) plus its channel's 4-message controller reset (M-4).
  REQUIRE(sink.events.size() == 1 + 4);
  size_t note_offs = 0;
  for (const auto& cap : sink.events) {
    REQUIRE(cap.destination == 10);  // every released/reset message belongs to dest 10
    if (cap.event.ump.is_note_off()) ++note_offs;
  }
  REQUIRE(note_offs == 1);
  REQUIRE(sink.events[0].event.ump.is_note_off());  // note-off dispatched before the resets
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
  seq.process_block(0, static_cast<int>(total) + 1);

  REQUIRE(seq.active_note_count() == MidiSequencer::kMaxActiveNotes);
  REQUIRE(seq.active_note_overflow_count() == 5);
  // The 5 overflow note-ons were suppressed, not dispatched.
  REQUIRE(sink.events.size() == MidiSequencer::kMaxActiveNotes);
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
