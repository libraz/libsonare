/// @file midi_routing_test.cpp
/// @brief MIDI routing / cc_map / clock_sync / capture tests. Covers filter /
///        remap / thru + overflow telemetry + alloc-0 routing; CC <-> automation
///        mapping + MIDI learn; clock / MTC / SPP generate+parse round-trips; and
///        capture draining queued events into a MidiClip with correct PPQ/order.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "midi/capture.h"
#include "midi/cc_map.h"
#include "midi/clock_sync.h"
#include "midi/midi_event.h"
#include "midi/routing.h"
#include "midi/ump.h"
#include "support/alloc_guard.h"
#include "transport/tempo_map.h"

namespace {

using sonare::midi::CcBinding;
using sonare::midi::CcMap;
using sonare::midi::ClockByteOutput;
using sonare::midi::ClockGenerator;
using sonare::midi::ClockParser;
using sonare::midi::kCcAnyChannel;
using sonare::midi::kRouteAnyChannel;
using sonare::midi::kRouteAnyGroup;
using sonare::midi::kRouteNoRemap;
using sonare::midi::make_midi1_control_change;
using sonare::midi::make_midi1_note_off;
using sonare::midi::make_midi1_note_on;
using sonare::midi::MidiCapture;
using sonare::midi::MidiClip;
using sonare::midi::MidiEvent;
using sonare::midi::MidiRouteConfig;
using sonare::midi::MidiRouteOutput;
using sonare::midi::MidiRouter;
using sonare::midi::MtcQuarterFrameGenerator;
using sonare::midi::MtcTime;
using sonare::test::AllocationGuard;
using sonare::transport::TempoMap;
using sonare::transport::TempoSegment;

MidiEvent note_on_event(int64_t frame, uint8_t group, uint8_t channel, uint8_t note) {
  MidiEvent ev;
  ev.render_frame = frame;
  ev.ump = make_midi1_note_on(group, channel, note, 100);
  return ev;
}

// TempoMap is non-copyable/non-movable (it owns RtSnapshot members), so callers
// declare it locally and pass it here to be configured in place.
void configure_tempo_map(TempoMap* map, double bpm) {
  map->prepare(48000.0);
  TempoSegment seg;
  seg.start_ppq = 0.0;
  seg.bpm = bpm;
  seg.start_sample = 0.0;
  map->set_segments({seg});
}

}  // namespace

// ===========================================================================
// routing
// ===========================================================================

TEST_CASE("MidiRouter filters by group and channel", "[midi]") {
  MidiRouter router;
  MidiRouteConfig cfg;
  cfg.filter_group = 1;
  cfg.filter_channel = 3;
  router.set_config(cfg);

  std::vector<MidiEvent> input = {
      note_on_event(0, 1, 3, 60),  // pass
      note_on_event(1, 1, 4, 61),  // wrong channel
      note_on_event(2, 0, 3, 62),  // wrong group
      note_on_event(3, 1, 3, 63),  // pass
  };
  MidiRouteOutput out;
  const size_t written = router.process(input.data(), input.size(), &out);
  REQUIRE(written == 2);
  REQUIRE(out.size == 2);
  REQUIRE(out.events[0].ump.note_number() == 60);
  REQUIRE(out.events[1].ump.note_number() == 63);
}

TEST_CASE("MidiRouter remaps channel", "[midi]") {
  MidiRouter router;
  MidiRouteConfig cfg;
  cfg.filter_channel = kRouteAnyChannel;
  cfg.remap_channel = 7;
  router.set_config(cfg);

  std::vector<MidiEvent> input = {note_on_event(0, 0, 2, 64)};
  MidiRouteOutput out;
  router.process(input.data(), input.size(), &out);
  REQUIRE(out.size == 1);
  REQUIRE(out.events[0].ump.channel() == 7);
  REQUIRE(out.events[0].ump.note_number() == 64);
  REQUIRE(out.events[0].ump.is_note_on());
}

TEST_CASE("MidiRouter thru toggle suppresses all output", "[midi]") {
  MidiRouter router;
  MidiRouteConfig cfg;
  cfg.thru = false;
  router.set_config(cfg);

  std::vector<MidiEvent> input = {note_on_event(0, 0, 0, 60), note_on_event(1, 0, 0, 61)};
  MidiRouteOutput out;
  const size_t written = router.process(input.data(), input.size(), &out);
  REQUIRE(written == 0);
  REQUIRE(out.size == 0);
  REQUIRE(out.overflowed == false);
  REQUIRE(router.overflow_count() == 0);  // suppression is not overflow
}

TEST_CASE("MidiRouter overflow telemetry counts dropped events", "[midi]") {
  MidiRouter router;
  router.set_config(MidiRouteConfig{});  // pass everything

  const size_t cap = MidiRouteOutput::kCapacity;
  std::vector<MidiEvent> input;
  for (size_t i = 0; i < cap + 10; ++i) {
    input.push_back(note_on_event(static_cast<int64_t>(i), 0, 0, 60));
  }
  MidiRouteOutput out;
  const size_t written = router.process(input.data(), input.size(), &out);
  REQUIRE(written == cap);
  REQUIRE(out.overflowed == true);
  REQUIRE(router.overflow_count() == 10);
}

TEST_CASE("MidiRouter process performs no allocation", "[midi][rt]") {
  MidiRouter router;
  router.set_config(MidiRouteConfig{});
  std::vector<MidiEvent> input;
  for (int i = 0; i < 64; ++i) {
    input.push_back(note_on_event(i, 0, static_cast<uint8_t>(i % 16), 60));
  }
  MidiRouteOutput out;
  router.process(input.data(), input.size(), &out);  // warm-up

  AllocationGuard guard;
  router.process(input.data(), input.size(), &out);
  REQUIRE(guard.count() == 0);
  REQUIRE(out.size == 64);
}

// ===========================================================================
// cc_map
// ===========================================================================

TEST_CASE("CcMap binds CC to parameter and maps value", "[midi]") {
  CcMap map;
  CcBinding b;
  b.cc_number = 7;  // volume
  b.channel = 0;
  b.param_id = 42;
  b.min_value = 0.0f;
  b.max_value = 2.0f;
  REQUIRE(map.bind(b));

  uint32_t param = 0;
  REQUIRE(map.lookup_param(7, 0, &param));
  REQUIRE(param == 42);

  // CC value 127 -> normalized 1.0 -> unit 2.0.
  const auto cc = make_midi1_control_change(0, 0, 7, 127);
  std::vector<sonare::automation::Breakpoint> points;
  REQUIRE(map.cc_to_breakpoint(cc, 4.0, &points));
  REQUIRE(points.size() == 1);
  REQUIRE(points[0].ppq == 4.0);
  REQUIRE(points[0].value == 2.0f);
}

TEST_CASE("CcMap any-channel binding and exact precedence", "[midi]") {
  CcMap map;
  map.bind(CcBinding{11, kCcAnyChannel, 100, 0.0f, 1.0f});
  map.bind(CcBinding{11, 5, 200, 0.0f, 1.0f});

  uint32_t param = 0;
  REQUIRE(map.lookup_param(11, 5, &param));
  REQUIRE(param == 200);  // exact channel wins
  REQUIRE(map.lookup_param(11, 9, &param));
  REQUIRE(param == 100);  // falls back to any-channel
}

TEST_CASE("CcMap MIDI learn binds next observed CC", "[midi]") {
  CcMap map;
  REQUIRE_FALSE(map.is_learning());
  map.begin_learn(77, 0.0f, 1.0f);
  REQUIRE(map.is_learning());

  // A note-on does not satisfy learn.
  REQUIRE_FALSE(map.observe_for_learn(make_midi1_note_on(0, 0, 60, 100)));
  REQUIRE(map.is_learning());

  CcBinding learned;
  REQUIRE(map.observe_for_learn(make_midi1_control_change(0, 4, 74, 64), &learned));
  REQUIRE_FALSE(map.is_learning());
  REQUIRE(learned.cc_number == 74);
  REQUIRE(learned.channel == 4);
  REQUIRE(learned.param_id == 77);

  uint32_t param = 0;
  REQUIRE(map.lookup_param(74, 4, &param));
  REQUIRE(param == 77);
}

TEST_CASE("CcMap MIDI learn assembles 14-bit CC pairs", "[midi]") {
  CcMap map;
  map.begin_learn(88, -1.0f, 1.0f);

  CcBinding learned;
  REQUIRE_FALSE(map.observe_for_learn(make_midi1_control_change(0, 2, 1, 64), &learned));
  REQUIRE(map.is_learning());
  REQUIRE(map.observe_for_learn(make_midi1_control_change(0, 2, 33, 12), &learned));
  REQUIRE_FALSE(map.is_learning());

  REQUIRE(learned.kind == sonare::midi::CcBindingKind::kControlChange14);
  REQUIRE(learned.cc_number == 1);
  REQUIRE(learned.cc_lsb_number == 33);
  REQUIRE(learned.channel == 2);
  REQUIRE(learned.param_id == 88);
  REQUIRE(learned.min_value == -1.0f);
  REQUIRE(learned.max_value == 1.0f);
}

TEST_CASE("CcMap MIDI learn assembles RPN and NRPN selectors", "[midi]") {
  CcMap map;

  SECTION("RPN") {
    map.begin_learn(90);
    CcBinding learned;
    REQUIRE_FALSE(map.observe_for_learn(make_midi1_control_change(0, 3, 101, 0), &learned));
    REQUIRE_FALSE(map.observe_for_learn(make_midi1_control_change(0, 3, 100, 1), &learned));
    REQUIRE(map.observe_for_learn(make_midi1_control_change(0, 3, 6, 64), &learned));

    REQUIRE(learned.kind == sonare::midi::CcBindingKind::kRpn);
    REQUIRE(learned.cc_number == 6);
    REQUIRE(learned.channel == 3);
    REQUIRE(learned.selector_msb == 0);
    REQUIRE(learned.selector_lsb == 1);
    REQUIRE(learned.param_id == 90);
  }

  SECTION("NRPN") {
    map.begin_learn(91);
    CcBinding learned;
    REQUIRE_FALSE(map.observe_for_learn(make_midi1_control_change(0, 4, 99, 12), &learned));
    REQUIRE_FALSE(map.observe_for_learn(make_midi1_control_change(0, 4, 98, 34), &learned));
    REQUIRE(map.observe_for_learn(make_midi1_control_change(0, 4, 38, 5), &learned));

    REQUIRE(learned.kind == sonare::midi::CcBindingKind::kNrpn);
    REQUIRE(learned.cc_number == 38);
    REQUIRE(learned.channel == 4);
    REQUIRE(learned.selector_msb == 12);
    REQUIRE(learned.selector_lsb == 34);
    REQUIRE(learned.param_id == 91);
  }
}

TEST_CASE("CcMap param_to_cc round-trips value", "[midi]") {
  CcMap map;
  map.bind(CcBinding{10, 2, 55, 0.0f, 1.0f});

  sonare::midi::Ump out;
  REQUIRE(map.param_to_cc(55, 1.0f, 0, &out));
  uint8_t cc = 0;
  REQUIRE(sonare::midi::cc_number_of(out, &cc));
  REQUIRE(cc == 10);
  REQUIRE(out.channel() == 2);
  float norm = 0.0f;
  REQUIRE(sonare::midi::cc_normalized_value(out, &norm));
  REQUIRE(norm == 1.0f);  // 127/127
}

TEST_CASE("CcMap param_to_cc rejects RPN and NRPN single-UMP emission", "[midi]") {
  CcMap map;
  CcBinding rpn;
  rpn.kind = sonare::midi::CcBindingKind::kRpn;
  rpn.cc_number = 6;
  rpn.channel = 0;
  rpn.param_id = 90;
  rpn.selector_msb = 0;
  rpn.selector_lsb = 1;
  REQUIRE(map.bind(rpn));

  CcBinding nrpn = rpn;
  nrpn.kind = sonare::midi::CcBindingKind::kNrpn;
  nrpn.channel = 1;
  nrpn.param_id = 91;
  nrpn.selector_msb = 12;
  nrpn.selector_lsb = 34;
  REQUIRE(map.bind(nrpn));

  sonare::midi::Ump out;
  REQUIRE_FALSE(map.param_to_cc(90, 0.5f, 0, &out));
  REQUIRE_FALSE(map.param_to_cc(91, 0.5f, 0, &out));
}

TEST_CASE("CcMap lookup_param performs no allocation", "[midi][rt]") {
  CcMap map;
  for (uint8_t i = 0; i < 32; ++i) {
    map.bind(CcBinding{i, 0, static_cast<uint32_t>(i + 1), 0.0f, 1.0f});
  }
  uint32_t param = 0;
  map.lookup_param(20, 0, &param);  // warm-up

  AllocationGuard guard;
  float unit = 0.0f;
  const bool found = map.lookup_param(20, 0, &param) && map.value_to_unit(20, 0, 0.5f, &unit);
  REQUIRE(guard.count() == 0);
  REQUIRE(found);
}

// ===========================================================================
// clock_sync
// ===========================================================================

TEST_CASE("ClockGenerator emits 24 PPQN clock and parser counts ticks", "[midi]") {
  TempoMap map;
  configure_tempo_map(&map, 120.0);
  ClockGenerator gen;
  gen.prepare(&map);

  // At 120 BPM a quarter note = 0.5 s = 24000 frames at 48 kHz, so 24 ticks span
  // 24000 frames -> 1000 frames/tick. One full quarter note block (24000 frames)
  // should contain exactly 24 ticks.
  ClockByteOutput out;
  const size_t ticks = gen.generate_clock_block(0, 24000, &out);
  REQUIRE(ticks == 24);
  REQUIRE(out.size == 24);
  for (size_t i = 0; i < out.size; ++i) {
    REQUIRE(out.bytes[i] == sonare::midi::kStatusClock);
  }

  ClockParser parser;
  parser.reset();
  for (size_t i = 0; i < out.size; ++i) {
    parser.parse_byte(out.bytes[i]);
  }
  REQUIRE(parser.clock_ticks() == 24);
  // 24 ticks == one quarter note of elapsed musical time.
  REQUIRE(parser.position_ppq() == 1.0);
}

TEST_CASE("SPP generate and parse round-trip", "[midi]") {
  // 4 quarter notes == 16 sixteenth notes == SPP beat value 16.
  const uint16_t beats = sonare::midi::ppq_to_spp_beats(4.0);
  REQUIRE(beats == 16);
  REQUIRE(sonare::midi::spp_beats_to_ppq(beats) == 4.0);

  uint8_t bytes[3] = {0, 0, 0};
  REQUIRE(sonare::midi::encode_spp(beats, bytes, sizeof(bytes)) == 3);
  REQUIRE(bytes[0] == sonare::midi::kStatusSongPosition);

  ClockParser parser;
  parser.reset();
  for (uint8_t b : bytes) {
    parser.parse_byte(b);
  }
  REQUIRE(parser.has_spp());
  REQUIRE(parser.spp_beats() == 16);
  REQUIRE(parser.position_ppq() == 4.0);
}

TEST_CASE("MTC quarter-frame generate and parse round-trip", "[midi]") {
  MtcTime t;
  t.hours = 1;
  t.minutes = 23;
  t.seconds = 45;
  t.frames = 12;
  t.rate = sonare::midi::MtcFrameRate::kFps25;

  ClockParser parser;
  parser.reset();
  for (int piece = 0; piece < 8; ++piece) {
    uint8_t bytes[2] = {0, 0};
    REQUIRE(sonare::midi::encode_mtc_quarter_frame(t, piece, bytes, sizeof(bytes)) == 2);
    parser.parse_byte(bytes[0]);
    parser.parse_byte(bytes[1]);
  }
  REQUIRE(parser.has_mtc());
  REQUIRE(parser.mtc_time() == t);
}

TEST_CASE("MTC quarter-frame stream generator advances after one full cycle", "[midi]") {
  MtcTime t;
  t.hours = 1;
  t.minutes = 23;
  t.seconds = 45;
  t.frames = 12;
  t.rate = sonare::midi::MtcFrameRate::kFps25;

  MtcQuarterFrameGenerator gen;
  REQUIRE(gen.reset(t));
  REQUIRE(gen.next_piece() == 0);

  ClockParser parser;
  parser.reset();
  for (int piece = 0; piece < 8; ++piece) {
    uint8_t bytes[2] = {};
    REQUIRE(gen.next(bytes, sizeof(bytes)) == 2);
    REQUIRE(bytes[0] == sonare::midi::kStatusMtcQuarterFrame);
    REQUIRE(((bytes[1] >> 4u) & 0x07u) == static_cast<uint8_t>(piece));
    parser.parse_byte(bytes[0]);
    parser.parse_byte(bytes[1]);
  }
  REQUIRE(parser.has_mtc());
  REQUIRE(parser.mtc_time() == t);
  REQUIRE(gen.next_piece() == 0);
  REQUIRE(gen.time().frames == 14);

  MtcTime near_wrap;
  near_wrap.hours = 23;
  near_wrap.minutes = 59;
  near_wrap.seconds = 59;
  near_wrap.frames = 24;
  near_wrap.rate = sonare::midi::MtcFrameRate::kFps25;
  REQUIRE(gen.reset(near_wrap));
  for (int piece = 0; piece < 8; ++piece) {
    uint8_t bytes[2] = {};
    REQUIRE(gen.next(bytes, sizeof(bytes)) == 2);
  }
  REQUIRE(gen.time().hours == 0);
  REQUIRE(gen.time().minutes == 0);
  REQUIRE(gen.time().seconds == 0);
  REQUIRE(gen.time().frames == 1);

  near_wrap.frames = 25;
  REQUIRE_FALSE(gen.reset(near_wrap));
}

TEST_CASE("MTC full-frame encodes universal SysEx", "[midi]") {
  MtcTime t;
  t.hours = 1;
  t.minutes = 23;
  t.seconds = 45;
  t.frames = 12;
  t.rate = sonare::midi::MtcFrameRate::kFps25;

  uint8_t bytes[10] = {};
  REQUIRE(sonare::midi::encode_mtc_full_frame(t, /*device_id=*/0x7F, bytes, sizeof(bytes)) == 10);
  REQUIRE(bytes[0] == 0xF0u);
  REQUIRE(bytes[1] == 0x7Fu);
  REQUIRE(bytes[2] == 0x7Fu);
  REQUIRE(bytes[3] == 0x01u);
  REQUIRE(bytes[4] == 0x01u);
  REQUIRE(bytes[5] == static_cast<uint8_t>((1u << 5u) | 1u));  // 25 fps + hour 1.
  REQUIRE(bytes[6] == 23u);
  REQUIRE(bytes[7] == 45u);
  REQUIRE(bytes[8] == 12u);
  REQUIRE(bytes[9] == 0xF7u);

  t.frames = 30;  // Invalid for 25 fps.
  REQUIRE(sonare::midi::encode_mtc_full_frame(t, 0x7F, bytes, sizeof(bytes)) == 0);
}

TEST_CASE("MTC quarter-frame rejects invalid timecode ranges", "[midi]") {
  MtcTime invalid;
  invalid.hours = 1;
  invalid.minutes = 23;
  invalid.seconds = 63;
  invalid.frames = 12;
  invalid.rate = sonare::midi::MtcFrameRate::kFps25;
  uint8_t bytes[2] = {0, 0};
  REQUIRE(sonare::midi::encode_mtc_quarter_frame(invalid, 0, bytes, sizeof(bytes)) == 0);

  ClockParser parser;
  parser.reset();
  // Manually feed an invalid seconds value (0x3f) while keeping the other pieces
  // syntactically complete. The parser must not expose a complete MTC frame.
  const uint8_t pieces[8] = {
      0x00,  // frames low
      0x10,  // frames high
      0x2F,  // seconds low = 15
      0x33,  // seconds high = 3 => 63 seconds, invalid
      0x40,  // minutes low
      0x50,  // minutes high
      0x60,  // hours low
      0x72,  // hours high + 25 fps
  };
  for (uint8_t data : pieces) {
    REQUIRE_FALSE(parser.parse_byte(sonare::midi::kStatusMtcQuarterFrame));
    parser.parse_byte(data);
  }
  REQUIRE_FALSE(parser.has_mtc());
}

TEST_CASE("MIDI transport command encoder emits start continue stop only", "[midi]") {
  uint8_t byte = 0;
  REQUIRE(sonare::midi::encode_transport_command(sonare::midi::kStatusStart, &byte, 1) == 1);
  REQUIRE(byte == sonare::midi::kStatusStart);
  REQUIRE(sonare::midi::encode_transport_command(sonare::midi::kStatusContinue, &byte, 1) == 1);
  REQUIRE(byte == sonare::midi::kStatusContinue);
  REQUIRE(sonare::midi::encode_transport_command(sonare::midi::kStatusStop, &byte, 1) == 1);
  REQUIRE(byte == sonare::midi::kStatusStop);
  REQUIRE(sonare::midi::encode_transport_command(sonare::midi::kStatusClock, &byte, 1) == 0);
  REQUIRE(sonare::midi::encode_transport_command(sonare::midi::kStatusStart, nullptr, 1) == 0);
}

TEST_CASE("MTC quarter-frame piece zero starts a fresh assembly", "[midi]") {
  MtcTime first;
  first.hours = 1;
  first.minutes = 2;
  first.seconds = 3;
  first.frames = 4;
  first.rate = sonare::midi::MtcFrameRate::kFps25;

  MtcTime second = first;
  second.seconds = 8;

  ClockParser parser;
  parser.reset();
  for (int piece = 0; piece < 8; ++piece) {
    uint8_t bytes[2] = {0, 0};
    REQUIRE(sonare::midi::encode_mtc_quarter_frame(first, piece, bytes, sizeof(bytes)) == 2);
    parser.parse_byte(bytes[0]);
    parser.parse_byte(bytes[1]);
  }
  REQUIRE(parser.has_mtc());
  REQUIRE(parser.mtc_time() == first);

  uint8_t piece0[2] = {0, 0};
  REQUIRE(sonare::midi::encode_mtc_quarter_frame(second, 0, piece0, sizeof(piece0)) == 2);
  parser.parse_byte(piece0[0]);
  parser.parse_byte(piece0[1]);
  REQUIRE_FALSE(parser.has_mtc());
}

TEST_CASE("ClockGenerator block generation performs no allocation", "[midi][rt]") {
  TempoMap map;
  configure_tempo_map(&map, 120.0);
  ClockGenerator gen;
  gen.prepare(&map);
  ClockByteOutput out;
  gen.generate_clock_block(0, 4096, &out);  // warm-up

  AllocationGuard guard;
  gen.generate_clock_block(4096, 4096, &out);
  REQUIRE(guard.count() == 0);
}

// ===========================================================================
// capture
// ===========================================================================

TEST_CASE("MidiCapture drains queued events into a MidiClip", "[midi]") {
  TempoMap map;
  configure_tempo_map(&map, 120.0);  // 24000 frames per quarter note
  MidiCapture capture;
  capture.prepare(&map, 64);

  // Push events out of order; capture should sort them. Frame 24000 == 1.0 PPQ.
  REQUIRE(capture.push(note_on_event(24000, 0, 0, 62)));
  REQUIRE(capture.push(note_on_event(0, 0, 0, 60)));
  {
    MidiEvent off;
    off.render_frame = 12000;  // 0.5 PPQ
    off.ump = make_midi1_note_off(0, 0, 60, 0);
    REQUIRE(capture.push(off));
  }

  MidiClip clip;
  sonare::midi::CaptureConfig cfg;
  cfg.clip_start_ppq = 0.0;
  const size_t drained = capture.drain(cfg, &clip);
  REQUIRE(drained == 3);

  const auto& events = clip.events();
  REQUIRE(events.size() == 3);
  // Sorted ascending by PPQ.
  REQUIRE(events[0].ppq == 0.0);
  REQUIRE(events[1].ppq == 0.5);
  REQUIRE(events[2].ppq == 1.0);
  REQUIRE(events[0].ump.note_number() == 60);
  REQUIRE(events[1].ump.is_note_off());
  REQUIRE(events[2].ump.note_number() == 62);
}

TEST_CASE("MidiCapture clip_start_ppq offset and quantize", "[midi]") {
  TempoMap map;
  configure_tempo_map(&map, 120.0);
  MidiCapture capture;
  capture.prepare(&map, 64);

  // Frame 36000 == 1.5 PPQ absolute. With clip_start_ppq = 1.0 it is 0.5 rel.
  capture.push(note_on_event(36000, 0, 0, 64));
  // Slightly off-grid: frame 25200 == 1.05 PPQ -> 0.05 rel, quantize to 0.0.
  capture.push(note_on_event(25200, 0, 0, 65));

  MidiClip clip;
  sonare::midi::CaptureConfig cfg;
  cfg.clip_start_ppq = 1.0;
  cfg.quantize.enabled = true;
  cfg.quantize.grid_ppq = 0.5;
  cfg.quantize.strength = 1.0;
  const size_t drained = capture.drain(cfg, &clip);
  REQUIRE(drained == 2);

  const auto& events = clip.events();
  REQUIRE(events.size() == 2);
  REQUIRE(events[0].ppq == 0.0);  // 0.05 snapped to grid 0.0
  REQUIRE(events[1].ppq == 0.5);  // 0.5 already on grid
}

TEST_CASE("MidiCapture quantize supports swung odd grid lines", "[midi]") {
  TempoMap map;
  configure_tempo_map(&map, 120.0);
  MidiCapture capture;
  capture.prepare(&map, 64);

  // 120 BPM at 48 kHz: 24000 frames per quarter.
  // With grid 0.25 PPQ and full swing, odd grid lines move by 0.125 PPQ:
  // line 1 -> 0.375, line 2 -> 0.5.
  capture.push(note_on_event(7200, 0, 0, 60));   // 0.30 PPQ -> 0.375
  capture.push(note_on_event(12480, 0, 0, 61));  // 0.52 PPQ -> 0.5

  MidiClip clip;
  sonare::midi::CaptureConfig cfg;
  cfg.quantize.enabled = true;
  cfg.quantize.grid_ppq = 0.25;
  cfg.quantize.strength = 1.0;
  cfg.quantize.swing = 1.0;
  const size_t drained = capture.drain(cfg, &clip);
  REQUIRE(drained == 2);

  const auto& events = clip.events();
  REQUIRE(events.size() == 2);
  REQUIRE(events[0].ppq > 0.374);
  REQUIRE(events[0].ppq < 0.376);
  REQUIRE(events[1].ppq > 0.499);
  REQUIRE(events[1].ppq < 0.501);
}

TEST_CASE("MidiCapture quantize supports repeating groove templates", "[midi]") {
  TempoMap map;
  configure_tempo_map(&map, 120.0);
  MidiCapture capture;
  capture.prepare(&map, 64);

  capture.push(note_on_event(6600, 0, 0, 60));   // 0.275 PPQ -> line 1 + 0.025
  capture.push(note_on_event(11280, 0, 0, 61));  // 0.47 PPQ -> line 2 - 0.025
  capture.push(note_on_event(18600, 0, 0, 62));  // 0.775 PPQ -> line 3 + 0.0625

  MidiClip clip;
  sonare::midi::CaptureConfig cfg;
  cfg.quantize.enabled = true;
  cfg.quantize.grid_ppq = 0.25;
  cfg.quantize.strength = 1.0;
  cfg.quantize.groove_steps = 4;
  cfg.quantize.groove_offsets = {0.0, 0.10, -0.10, 0.25};
  const size_t drained = capture.drain(cfg, &clip);
  REQUIRE(drained == 3);

  const auto& events = clip.events();
  REQUIRE(events.size() == 3);
  REQUIRE(events[0].ppq > 0.274);
  REQUIRE(events[0].ppq < 0.276);
  REQUIRE(events[1].ppq > 0.474);
  REQUIRE(events[1].ppq < 0.476);
  REQUIRE(events[2].ppq > 0.812);
  REQUIRE(events[2].ppq < 0.813);
}

TEST_CASE("MidiCapture quantize preserves note length", "[midi]") {
  TempoMap map;
  configure_tempo_map(&map, 120.0);
  MidiCapture capture;
  capture.prepare(&map, 64);

  // 120 BPM at 48 kHz: 24000 frames per quarter. Note-on 0.45 PPQ snaps to 0.5;
  // the note-off keeps the same +0.05 PPQ shift instead of independently
  // snapping to 0.5 and collapsing the note.
  capture.push(note_on_event(10800, 0, 2, 67));
  MidiEvent off;
  off.render_frame = 13200;  // 0.55 PPQ
  off.ump = make_midi1_note_off(0, 2, 67, 0);
  capture.push(off);

  MidiClip clip;
  sonare::midi::CaptureConfig cfg;
  cfg.quantize.enabled = true;
  cfg.quantize.grid_ppq = 0.5;
  cfg.quantize.strength = 1.0;
  const size_t drained = capture.drain(cfg, &clip);
  REQUIRE(drained == 2);

  const auto& events = clip.events();
  REQUIRE(events.size() == 2);
  REQUIRE(events[0].ump.is_note_on());
  REQUIRE(events[1].ump.is_note_off());
  REQUIRE(events[0].ppq == 0.5);
  REQUIRE(events[1].ppq > 0.59);
  REQUIRE(events[1].ppq < 0.61);
}

TEST_CASE("MidiCapture push drop telemetry on full queue", "[midi]") {
  TempoMap map;
  configure_tempo_map(&map, 120.0);
  MidiCapture capture;
  capture.prepare(&map, 4);  // capacity 4

  int accepted = 0;
  for (int i = 0; i < 10; ++i) {
    if (capture.push(note_on_event(i, 0, 0, 60))) {
      ++accepted;
    }
  }
  REQUIRE(accepted == 4);
  REQUIRE(capture.dropped_count() == 6);
}

TEST_CASE("MidiCapture push performs no allocation", "[midi][rt]") {
  TempoMap map;
  configure_tempo_map(&map, 120.0);
  MidiCapture capture;
  capture.prepare(&map, 64);
  capture.push(note_on_event(0, 0, 0, 60));  // warm-up

  AllocationGuard guard;
  for (int i = 1; i < 32; ++i) {
    capture.push(note_on_event(i, 0, 0, 60));
  }
  REQUIRE(guard.count() == 0);
}
