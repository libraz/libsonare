/// @file midi_no_alloc_test.cpp
/// @brief MIDI core: the MidiSequencer audio path performs ZERO heap
///        allocation after prepare().
///
/// Reuses the SAME allocation-counting mechanism as tests/mixing/no_alloc_test:
/// the global operator new / operator delete overrides defined there feed the
/// shared sonare::test counters, and AllocationGuard (support/alloc_guard.h)
/// arms / disarms them. We do NOT redefine the global overrides here (that would
/// be a duplicate-symbol error); we only consume the shared guard.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "midi/midi_clip.h"
#include "midi/midi_event.h"
#include "midi/sequencer.h"
#include "midi/ump.h"
#include "support/alloc_guard.h"

namespace {

using sonare::midi::MidiClipSchedule;
using sonare::midi::MidiEvent;
using sonare::midi::MidiEventSink;
using sonare::midi::MidiFxChain;
using sonare::midi::MidiSequencer;
using sonare::test::AllocationGuard;

// A fixed-size counter sink: on_event only bumps a counter, so it performs no
// allocation itself. This isolates the measurement to the sequencer path.
class CounterSink final : public MidiEventSink {
 public:
  void on_event(uint32_t, const MidiEvent&) noexcept override { ++count; }
  uint64_t count = 0;
};

}  // namespace

TEST_CASE("MidiSequencer audio path performs no heap allocation after prepare", "[midi][rt]") {
  MidiSequencer seq;
  CounterSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 5;
  // A spread of note-on/off events across several blocks.
  for (int i = 0; i < 64; ++i) {
    const int64_t frame = static_cast<int64_t>(i) * 16;
    const uint8_t note = static_cast<uint8_t>(40 + (i % 24));
    if (i % 2 == 0) {
      clip.events.push_back({frame, sonare::midi::make_midi1_note_on(0, 0, note, 100)});
    } else {
      clip.events.push_back({frame, sonare::midi::make_midi1_note_off(0, 0, note, 0)});
    }
  }
  seq.set_midi_clips({clip});  // control thread, may allocate.

  constexpr int kBlock = 128;
  // Warm-up: adopt the published clip set and run one block before counting.
  seq.acquire_midi_clips();
  seq.process_block(0, kBlock);

  AllocationGuard guard;
  // The full audio-path surface: acquire, process, boundaries, all-notes-off.
  seq.acquire_midi_clips();
  seq.process_block(kBlock, kBlock);

  MidiSequencer::BoundaryOffsets boundaries;
  seq.collect_boundaries(kBlock, kBlock, &boundaries);

  seq.all_notes_off(static_cast<int64_t>(2 * kBlock));

  REQUIRE(guard.count() == 0);
  REQUIRE(seq.active_note_count() == 0);
}

TEST_CASE("MidiSequencer live MIDI FX path performs no heap allocation after prepare",
          "[midi][rt]") {
  MidiSequencer seq;
  CounterSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiFxChain fx;
  sonare::midi::ChordConfig chord;
  chord.enabled = true;
  chord.count = 3;
  chord.intervals[0] = 0;
  chord.intervals[1] = 4;
  chord.intervals[2] = 7;
  fx.set_chord(chord);
  sonare::midi::ArpeggiatorConfig arp;
  arp.enabled = true;
  arp.steps = 2;
  arp.intervals[0] = 0;
  arp.intervals[1] = 12;
  arp.step_frames = 64;
  arp.gate_frames = 24;
  fx.set_arpeggiator(arp);
  REQUIRE(seq.set_midi_fx(5, fx));

  MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 5;
  clip.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                 {256, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  seq.set_midi_clips({clip});  // control thread, may allocate.
  seq.acquire_midi_clips();

  AllocationGuard guard;
  seq.process_block(0, 64);
  seq.process_block(64, 64);
  seq.all_notes_off(128);

  REQUIRE(guard.count() == 0);
  REQUIRE(seq.active_note_count() == 0);
  REQUIRE(seq.midi_fx_pending_overflow_count() == 0);
}
