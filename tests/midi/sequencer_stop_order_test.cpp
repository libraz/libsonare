#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

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
