/// @file sequencer_chase_test.cpp
/// @brief MidiSequencer controller chase: after a loop wrap, seek or stop, the
///        first rolling dispatch re-sends the channel state clips set before it.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "midi/midi_clip.h"
#include "midi/midi_event.h"
#include "midi/sequencer.h"
#include "midi/ump.h"

namespace {

using sonare::midi::DeviceFrame;
using sonare::midi::MidiClipSchedule;
using sonare::midi::MidiEvent;
using sonare::midi::MidiEventSink;
using sonare::midi::MidiLoopMode;
using sonare::midi::MidiSequencer;
using sonare::midi::SequencerClock;
using sonare::midi::TimelineFrame;
using sonare::midi::Ump;

class CapturingSink final : public MidiEventSink {
 public:
  void on_event(uint32_t, const MidiEvent& event) noexcept override { events.push_back(event); }
  std::vector<MidiEvent> events;
};

MidiEvent at(int64_t frame, const Ump& ump) { return {frame, ump}; }

Ump cc(uint8_t controller, uint8_t value, uint8_t channel = 0) {
  return sonare::midi::make_midi1_control_change(0, channel, controller, value);
}

SequencerClock jump_clock(int64_t device, int64_t timeline) {
  return {DeviceFrame{device}, TimelineFrame{timeline}, true};
}

std::vector<uint32_t> words_at(const std::vector<MidiEvent>& events, int64_t frame) {
  std::vector<uint32_t> words;
  for (const MidiEvent& event : events) {
    if (event.render_frame == frame) words.push_back(event.ump.words[0]);
  }
  return words;
}

}  // namespace

TEST_CASE("MidiSequencer chases controller state at a seek, bank and program first",
          "[midi][chase]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 3;
  clip.events = {at(10, cc(7, 100)),
                 at(15, cc(6, 2)),  // data entry: part of an RPN sequence, never chased
                 at(20, cc(11, 50)),
                 at(25, cc(0, 1)),
                 at(30, sonare::midi::make_midi1_program_change(0, 0, 5)),
                 at(40, sonare::midi::make_midi1_pitch_bend(0, 0, 0x3000)),
                 at(50, sonare::midi::make_midi1_note_on(0, 0, 60, 100)),
                 at(60, cc(7, 90))};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 100);

  seq.release_clip_playback(DeviceFrame{100});
  sink.events.clear();
  seq.dispatch_due(jump_clock(100, 50));

  const std::vector<uint32_t> expected{
      cc(0, 1).words[0],
      sonare::midi::make_midi1_program_change(0, 0, 5).words[0],
      cc(7, 100).words[0],
      cc(11, 50).words[0],
      sonare::midi::make_midi1_pitch_bend(0, 0, 0x3000).words[0],
      sonare::midi::make_midi1_note_on(0, 0, 60, 100).words[0],
  };
  REQUIRE(words_at(sink.events, 100) == expected);
}

TEST_CASE("MidiSequencer chases a looping clip from the iteration before the jump",
          "[midi][chase]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule clip;
  clip.id = 2;
  clip.destination_id = 4;
  clip.loop_mode = MidiLoopMode::kLoop;
  clip.loop_length_samples = 40;
  clip.events = {at(10, cc(1, 20)), at(30, cc(1, 80))};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();

  SECTION("inside the first iteration") {
    seq.release_clip_playback(DeviceFrame{0});
    seq.dispatch_due(jump_clock(0, 15));
    REQUIRE(words_at(sink.events, 0) == std::vector<uint32_t>{cc(1, 20).words[0]});
  }
  SECTION("early in a later iteration, before its first event") {
    seq.release_clip_playback(DeviceFrame{0});
    seq.dispatch_due(jump_clock(0, 45));
    REQUIRE(words_at(sink.events, 0) == std::vector<uint32_t>{cc(1, 80).words[0]});
  }
}

TEST_CASE("MidiSequencer chases the latest value across clips on one destination",
          "[midi][chase]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule early;
  early.id = 1;
  early.destination_id = 5;
  early.events = {at(5, cc(7, 20))};
  MidiClipSchedule late = early;
  late.id = 2;
  late.events = {at(10, cc(7, 10))};
  MidiClipSchedule other = early;
  other.id = 3;
  other.destination_id = 6;
  seq.set_midi_clips({late, early, other});
  seq.acquire_midi_clips();

  seq.all_notes_off(DeviceFrame{0});
  sink.events.clear();
  seq.dispatch_due(jump_clock(0, 30));
  REQUIRE(words_at(sink.events, 0) ==
          std::vector<uint32_t>{cc(7, 10).words[0], cc(7, 20).words[0]});
}

TEST_CASE("MidiSequencer chases only after a discontinuity", "[midi][chase]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 3;
  clip.events = {at(10, cc(7, 100))};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();
  seq.process_block(SequencerClock::aligned(0), 50);
  REQUIRE(sink.events.size() == 1);

  // Continuous playback resumes without re-sending, and a stopped clock holds the chase.
  seq.dispatch_due(SequencerClock::aligned(50));
  REQUIRE(sink.events.size() == 1);
  seq.release_clip_playback(DeviceFrame{60});
  sink.events.clear();
  seq.dispatch_due({DeviceFrame{60}, TimelineFrame{40}, false});
  REQUIRE(sink.events.empty());
  seq.dispatch_due(jump_clock(70, 40));
  REQUIRE(words_at(sink.events, 70) == std::vector<uint32_t>{cc(7, 100).words[0]});
}

TEST_CASE("MidiSequencer chase does not replay values a reset already cleared", "[midi][chase]") {
  MidiSequencer seq;
  CapturingSink sink;
  seq.prepare(48000.0);
  seq.set_sink(&sink);

  const std::vector<uint8_t> gm_on{0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
  MidiEvent system_reset = at(40, sonare::midi::make_sysex_handle(0, 1));
  system_reset.sysex_payload = gm_on.data();
  system_reset.sysex_payload_size = gm_on.size();

  MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 3;
  clip.events = {at(10, cc(1, 100)),     // cleared by the CC121 below
                 at(12, cc(11, 20, 1)),  // channel 1: untouched by a channel 0 reset
                 at(15, cc(7, 80)),      // volume survives a reset
                 at(20, cc(121, 0)),    at(30, cc(11, 60)),  // set after the reset: still chased
                 at(35, cc(1, 70, 2)),                       // cleared by the system reset
                 system_reset};
  seq.set_midi_clips({clip});
  seq.acquire_midi_clips();

  SECTION("a channel reset retires only its own channel's earlier values") {
    seq.release_clip_playback(DeviceFrame{0});
    seq.dispatch_due(jump_clock(0, 38));
    const std::vector<uint32_t> expected{cc(7, 80).words[0], cc(11, 20, 1).words[0],
                                         cc(11, 60).words[0], cc(1, 70, 2).words[0]};
    std::vector<uint32_t> got = words_at(sink.events, 0);
    for (const uint32_t word : expected) {
      INFO("word " << word);
      CHECK(std::count(got.begin(), got.end(), word) == 1);
    }
    CHECK(std::count(got.begin(), got.end(), cc(1, 100).words[0]) == 0);
  }
  SECTION("a system reset retires the values of every channel before it") {
    seq.release_clip_playback(DeviceFrame{0});
    seq.dispatch_due(jump_clock(0, 45));
    const std::vector<uint32_t> got = words_at(sink.events, 0);
    CHECK(std::count(got.begin(), got.end(), cc(1, 100).words[0]) == 0);
    CHECK(std::count(got.begin(), got.end(), cc(1, 70, 2).words[0]) == 0);
    CHECK(std::count(got.begin(), got.end(), cc(11, 20, 1).words[0]) == 0);
    CHECK(std::count(got.begin(), got.end(), cc(11, 60).words[0]) == 0);
    CHECK(std::count(got.begin(), got.end(), cc(7, 80).words[0]) == 1);
  }
}
