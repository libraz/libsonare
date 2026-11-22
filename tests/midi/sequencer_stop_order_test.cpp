#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "midi/control_value.h"
#include "midi/midi_event.h"
#include "midi/sequencer.h"
#include "midi/ump.h"

using sonare::midi::DeviceFrame;
using sonare::midi::MidiEvent;
using sonare::midi::MidiEventSink;
using sonare::midi::MidiSequencer;
using sonare::midi::Ump;

namespace {

class RecordingSink final : public MidiEventSink {
 public:
  void on_event(uint32_t /*destination*/, const MidiEvent& event) noexcept override {
    umps.push_back(event.ump);
  }
  std::vector<Ump> umps;
};

constexpr uint32_t kDestination = 3;

void hold_note_under_damper(MidiSequencer& seq) {
  seq.inject_event(kDestination, DeviceFrame{0},
                   sonare::midi::make_midi1_control_change(0, 0, 64, 127));
  seq.inject_event(kDestination, DeviceFrame{1}, sonare::midi::make_midi1_note_on(0, 0, 60, 100));
}

std::vector<Ump> stop_events(bool destination_stop) {
  MidiSequencer seq;
  RecordingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);
  hold_note_under_damper(seq);
  sink.umps.clear();
  if (destination_stop) {
    seq.all_notes_off_for_destination(kDestination, DeviceFrame{8});
  } else {
    seq.all_notes_off(DeviceFrame{8});
  }
  return sink.umps;
}

}  // namespace

TEST_CASE("Global and destination stops release a held note before lifting the damper", "[midi]") {
  const std::vector<Ump> global = stop_events(false);
  const std::vector<Ump> destination = stop_events(true);

  REQUIRE(global.size() == 5);
  CHECK(global.front() == sonare::midi::make_midi1_note_off(0, 0, 60, 0));
  CHECK(global[1] == sonare::midi::make_midi1_control_change(0, 0, 64, 0));
  CHECK(destination == global);
}

namespace {

constexpr uint32_t kOtherDestination = 4;
constexpr uint32_t kReleasedOrigin = 5;
constexpr uint32_t kOtherOrigin = 6;

}  // namespace

TEST_CASE("Releasing a live origin releases only the notes, pedals and bend it holds", "[midi]") {
  using sonare::midi::make_midi1_control_change;
  using sonare::midi::make_midi1_note_on;
  using sonare::midi::make_midi1_pitch_bend;
  MidiSequencer seq;
  RecordingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);
  const DeviceFrame at{0};
  seq.inject_event(kDestination, at, make_midi1_note_on(0, 0, 60, 100), kReleasedOrigin);
  seq.inject_event(kDestination, at, make_midi1_note_on(0, 0, 61, 100));
  seq.inject_event(kDestination, at, make_midi1_note_on(0, 0, 62, 100), kOtherOrigin);
  seq.inject_event(kOtherDestination, at, make_midi1_note_on(0, 0, 63, 100), kReleasedOrigin);
  seq.inject_event(kDestination, at, make_midi1_control_change(0, 0, 64, 127), kReleasedOrigin);
  seq.inject_event(kDestination, at, make_midi1_control_change(0, 0, 66, 127));
  seq.inject_event(kDestination, at, make_midi1_control_change(0, 0, 67, 127), kReleasedOrigin);
  seq.inject_event(kDestination, at, make_midi1_control_change(0, 0, 67, 100), kOtherOrigin);
  seq.inject_event(kDestination, at, make_midi1_pitch_bend(0, 1, 0x3000), kReleasedOrigin);
  REQUIRE(seq.active_note_count() == 4);
  sink.umps.clear();

  seq.release_live_origin(kDestination, kReleasedOrigin, DeviceFrame{8});

  const std::vector<Ump> expected = {
      sonare::midi::make_midi1_note_off(0, 0, 60, 0),
      make_midi1_control_change(0, 0, 64, 0),
      make_midi1_pitch_bend(0, 1, sonare::midi::kBend14Center),
  };
  CHECK(sink.umps == expected);
  CHECK(seq.active_note_count() == 3);

  sink.umps.clear();
  seq.release_live_origin(kDestination, kReleasedOrigin, DeviceFrame{9});
  CHECK(sink.umps.empty());
}

TEST_CASE("A pedal a live origin let go of or a later writer took over is not released by it",
          "[midi]") {
  using sonare::midi::make_midi1_control_change;
  using sonare::midi::make_midi2_control_change;
  MidiSequencer seq;
  RecordingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);
  const DeviceFrame at{0};
  // Damper: down then up by the origin itself.
  seq.inject_event(kDestination, at, make_midi1_control_change(0, 0, 64, 127), kReleasedOrigin);
  seq.inject_event(kDestination, at, make_midi1_control_change(0, 0, 64, 0), kReleasedOrigin);
  // Sostenuto: down by the origin, then rewritten by a host command.
  seq.inject_event(kDestination, at, make_midi1_control_change(0, 0, 66, 127), kReleasedOrigin);
  seq.inject_event(kDestination, at, make_midi1_control_change(0, 0, 66, 127));
  // Soft pedal on another channel, held at MIDI 2.0 width, then cleared by Reset All Controllers.
  seq.inject_event(kDestination, at, make_midi2_control_change(0, 2, 67, 0xFFFFFFFFu),
                   kReleasedOrigin);
  seq.inject_event(kDestination, at, make_midi1_control_change(0, 2, 121, 0), kReleasedOrigin);
  // Damper on a third channel held at MIDI 2.0 width stays the origin's.
  seq.inject_event(kDestination, at, make_midi2_control_change(0, 3, 64, 0x80000000u),
                   kReleasedOrigin);
  sink.umps.clear();

  seq.release_live_origin(kDestination, kReleasedOrigin, DeviceFrame{8});

  const std::vector<Ump> expected = {make_midi1_control_change(0, 3, 64, 0)};
  CHECK(sink.umps == expected);
}
