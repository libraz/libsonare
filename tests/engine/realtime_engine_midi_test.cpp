/// @file realtime_engine_midi_test.cpp
/// @brief Engine-level MIDI integration: hang-note safety across seek / stop and
///        the stopped-transport gate (a stopped playhead dispatches nothing and
///        renders no instrument audio). Covers brush-up findings H-1, H-2.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/realtime_engine.h"
#include "host/midi_io.h"
#include "midi/clock_sync.h"
#include "midi/instrument.h"
#include "midi/midi_clip.h"
#include "midi/midi_event.h"
#include "midi/ump.h"
#include "rt/command.h"

namespace {

using sonare::engine::RealtimeEngine;
using sonare::midi::MidiEvent;
using sonare::midi::MidiInstrument;

// A minimal instrument that counts events and emits DC while a note sounds, so
// audio output (peak) reflects whether a note is still ringing.
class CountingInstrument final : public MidiInstrument {
 public:
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    const float value = note_on_count_ > note_off_count_ ? 0.5f : 0.0f;
    for (int c = 0; c < num_channels; ++c)
      for (int i = 0; i < num_samples; ++i) channels[c][i] += value;
  }
  void reset() override {
    note_on_count_ = 0;
    note_off_count_ = 0;
  }
  void on_event(uint32_t, const MidiEvent& event) noexcept override {
    ++received_events_;
    if (event.ump.is_note_on()) ++note_on_count_;
    if (event.ump.is_note_off()) ++note_off_count_;
  }

  int received_events_ = 0;
  int note_on_count_ = 0;
  int note_off_count_ = 0;
};

// Records the channel-reset controllers it receives, so a test can assert that a
// discontinuity (seek/stop) sends the standard reset sequence on a held channel.
class ControllerRecordingInstrument final : public MidiInstrument {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  void on_event(uint32_t, const MidiEvent& event) noexcept override {
    const uint32_t w = event.ump.words[0];
    const uint8_t status = static_cast<uint8_t>((w >> 16) & 0xF0u);
    if (status == 0xB0u) {  // control change
      const uint8_t controller = static_cast<uint8_t>((w >> 8) & 0x7Fu);
      const uint8_t value = static_cast<uint8_t>(w & 0x7Fu);
      if (controller == 64 && value == 0) sustain_off_ = true;
      if (controller == 121) reset_all_controllers_ = true;
      if (controller == 123) all_notes_off_cc_ = true;
    } else if (status == 0xE0u) {  // pitch bend
      pitch_bend_seen_ = true;
    }
  }
  bool sustain_off_ = false;
  bool reset_all_controllers_ = false;
  bool all_notes_off_cc_ = false;
  bool pitch_bend_seen_ = false;
};

class SyncByteSink final : public RealtimeEngine::MidiSyncSink {
 public:
  struct Event {
    int64_t render_frame = 0;
    uint8_t byte = 0;
  };
  void on_midi_sync_byte(int64_t render_frame, uint8_t byte) noexcept override {
    events.push_back({render_frame, byte});
  }
  std::vector<Event> events;
};

// One held note: note-on at frame 0, note-off far beyond any test block so the
// note stays sounding until an explicit discontinuity (seek / stop) releases it.
std::vector<sonare::midi::MidiClipSchedule> held_note_clip() {
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.start_sample = 0;
  clip.length_samples = 1 << 20;
  clip.destination_id = 0;
  clip.events.push_back(MidiEvent{0, sonare::midi::make_midi1_note_on(0, 0, 64, 100)});
  clip.events.push_back(MidiEvent{1 << 19, sonare::midi::make_midi1_note_off(0, 0, 64, 0)});
  return {clip};
}

float block_peak(const std::vector<float>& buf) {
  float peak = 0.0f;
  for (float v : buf) peak = std::max(peak, std::abs(v));
  return peak;
}

// Models a host instrument with internal latency L: a note-on received at
// absolute render frame F produces a single unit impulse L samples later (the
// instrument's audible attack lags the note by its reported latency). Used to
// verify plugin-delay compensation (PDC) realigns instrument audio with clip
// audio. Allocation-free (one pending impulse frame).
class LatencyImpulseInstrument final : public MidiInstrument {
 public:
  explicit LatencyImpulseInstrument(int latency) : latency_(latency) {}
  void prepare(double, int) override {
    frame_ = 0;
    impulse_frame_ = -1;
  }
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int i = 0; i < num_samples; ++i) {
      const float value = (frame_ + i) == impulse_frame_ ? 1.0f : 0.0f;
      if (value != 0.0f) {
        for (int c = 0; c < num_channels; ++c) channels[c][i] += value;
      }
    }
    frame_ += num_samples;
  }
  void reset() override {
    frame_ = 0;
    impulse_frame_ = -1;
  }
  int latency_samples() const noexcept override { return latency_; }
  void on_event(uint32_t, const MidiEvent& event) noexcept override {
    if (event.ump.is_note_on()) impulse_frame_ = event.render_frame + latency_;
  }

 private:
  int latency_;
  int64_t frame_ = 0;
  int64_t impulse_frame_ = -1;
};

// Reports a fractional (sub-sample) latency via latency_samples_q8() while
// rendering no audio. Used to verify the engine threads Q8 latency into PDC and
// applies a fractional delay to the clip bus (M-45).
class FractionalLatencyInstrument final : public MidiInstrument {
 public:
  explicit FractionalLatencyInstrument(int latency_q8) : latency_q8_(latency_q8) {}
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  int latency_samples() const noexcept override { return latency_q8_ >> 8; }
  int latency_samples_q8() const noexcept override { return latency_q8_; }
  void on_event(uint32_t, const MidiEvent&) noexcept override {}

 private:
  int latency_q8_;
};

// A note-on at frame 0 (no note-off in range), routed to destination 0.
std::vector<sonare::midi::MidiClipSchedule> note_on_at_zero() {
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.start_sample = 0;
  clip.length_samples = 1 << 20;
  clip.destination_id = 0;
  clip.events.push_back(MidiEvent{0, sonare::midi::make_midi1_note_on(0, 0, 64, 100)});
  return {clip};
}

// A stereo clip carrying a unit impulse at frame 0.
sonare::engine::ClipSchedule impulse_clip(int64_t length) {
  auto storage = std::make_shared<sonare::engine::ClipAudioStorage>();
  storage->channels = {std::vector<float>(static_cast<size_t>(length), 0.0f),
                       std::vector<float>(static_cast<size_t>(length), 0.0f)};
  storage->channels[0][0] = 1.0f;
  storage->channels[1][0] = 1.0f;
  storage->channel_ptrs = {storage->channels[0].data(), storage->channels[1].data()};
  sonare::engine::ClipSchedule clip;
  clip.id = 2;
  clip.buffer.channels = storage->channel_ptrs.data();
  clip.buffer.num_channels = 2;
  clip.buffer.num_samples = length;
  clip.start_sample = 0;
  clip.length_samples = length;
  clip.gain = 1.0f;
  clip.storage = std::move(storage);
  return clip;
}

void push_play(RealtimeEngine& engine) {
  sonare::rt::Command c{};
  c.type = sonare::rt::CommandType::kTransportPlay;
  c.sample_time = -1;  // due immediately (clamped to block head)
  REQUIRE(engine.push_command(c));
}

}  // namespace

TEST_CASE("RealtimeEngine emits MIDI clock and transport bytes while rolling", "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 24000);
  engine.set_tempo(120.0);
  SyncByteSink sink;
  engine.set_midi_sync_sink(&sink);

  sonare::rt::Command play;
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = 0;
  REQUIRE(engine.push_command(play));

  std::vector<float> left(24000, 0.0f);
  std::vector<float> right(24000, 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.process(channels, 2, 24000);

  REQUIRE(sink.events.size() == 25);
  REQUIRE(sink.events[0].render_frame == 0);
  REQUIRE(sink.events[0].byte == sonare::midi::kStatusStart);
  for (size_t i = 1; i < sink.events.size(); ++i) {
    REQUIRE(sink.events[i].byte == sonare::midi::kStatusClock);
    REQUIRE(sink.events[i].render_frame == static_cast<int64_t>((i - 1) * 1000));
  }

  sonare::rt::Command stop;
  stop.type = sonare::rt::CommandType::kTransportStop;
  stop.sample_time = 36000;
  REQUIRE(engine.push_command(stop));
  sink.events.clear();
  engine.process(channels, 2, 24000);

  REQUIRE(sink.events.size() == 13);
  for (size_t i = 0; i < 12; ++i) {
    REQUIRE(sink.events[i].byte == sonare::midi::kStatusClock);
    REQUIRE(sink.events[i].render_frame == static_cast<int64_t>(24000 + i * 1000));
  }
  REQUIRE(sink.events[12].render_frame == 36000);
  REQUIRE(sink.events[12].byte == sonare::midi::kStatusStop);
}

TEST_CASE("RealtimeEngine drains live MIDI input into instruments while stopped",
          "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  CountingInstrument instrument;
  REQUIRE(engine.set_midi_instrument(0, &instrument));

  sonare::host::FixedMidiInputSource<8> input;
  engine.set_midi_input_source(&input, 0);

  REQUIRE(input.push_event(sonare::midi::make_midi1_note_on(0, 0, 64, 100), 4));

  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.process(channels, 2, 64);

  REQUIRE(instrument.note_on_count_ == 1);
  REQUIRE(instrument.note_off_count_ == 0);
  REQUIRE(block_peak(left) == Catch::Approx(0.5f));

  std::fill(left.begin(), left.end(), 0.0f);
  std::fill(right.begin(), right.end(), 0.0f);
  REQUIRE(input.push_event(sonare::midi::make_midi1_note_off(0, 0, 64, 0), 0));
  engine.process(channels, 2, 64);

  REQUIRE(instrument.note_off_count_ == 1);
  REQUIRE(block_peak(left) == Catch::Approx(0.0f));
}

TEST_CASE("RealtimeEngine mirrors sequenced MIDI to live output sink", "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  CountingInstrument instrument;
  REQUIRE(engine.set_midi_instrument(0, &instrument));
  sonare::host::FixedMidiOutputSink<8> output;
  engine.set_midi_output_sink(&output);

  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.start_sample = 0;
  clip.length_samples = 128;
  clip.destination_id = 0;
  clip.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 64, 100)},
                 {32, sonare::midi::make_midi1_note_off(0, 0, 64, 0)}};
  engine.set_midi_clips({clip});
  push_play(engine);

  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.process(channels, 2, 64);

  REQUIRE(instrument.note_on_count_ == 1);
  REQUIRE(instrument.note_off_count_ == 1);
  REQUIRE(output.queued_count() == 2);

  std::array<MidiEvent, 4> drained{};
  REQUIRE(output.drain_queued(drained.data(), drained.size()) == 2);
  REQUIRE(drained[0].render_frame == 0);
  REQUIRE(drained[0].ump.is_note_on());
  REQUIRE(drained[1].render_frame == 32);
  REQUIRE(drained[1].ump.is_note_off());
}

TEST_CASE("seek releases sounding notes (no hang)", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 256;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  CountingInstrument inst;
  engine.set_midi_instrument(&inst);
  engine.set_midi_clips(held_note_clip());

  push_play(engine);

  std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
  float* io[] = {l.data(), r.data()};
  engine.process(io, 2, kBlock);

  // The held note is sounding after block 1.
  REQUIRE(inst.note_on_count_ == 1);
  REQUIRE(engine.midi_sequencer().active_note_count() == 1);
  REQUIRE(block_peak(l) > 0.0f);

  // Seek the playhead away (to a region with no events). The seek must release
  // the sounding note rather than leave it hanging.
  sonare::rt::Command seek{};
  seek.type = sonare::rt::CommandType::kTransportSeekSample;
  seek.arg.i = 1 << 16;       // jump well past the note-off
  seek.sample_time = kBlock;  // apply at the head of block 2
  REQUIRE(engine.push_command(seek));

  std::fill(l.begin(), l.end(), 0.0f);
  std::fill(r.begin(), r.end(), 0.0f);
  engine.process(io, 2, kBlock);

  // Hang-note invariant: after a seek the active-note table is empty and a
  // note-off was emitted for the previously-sounding note.
  REQUIRE(engine.midi_sequencer().active_note_count() == 0);
  REQUIRE(inst.note_off_count_ >= 1);
  // The instrument no longer rings.
  REQUIRE(block_peak(l) == 0.0f);

  engine.set_midi_instrument(nullptr);
}

TEST_CASE("seek resets controllers on held channels (no stuck sustain/bend)", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 256;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  ControllerRecordingInstrument inst;
  engine.set_midi_instrument(&inst);
  engine.set_midi_clips(held_note_clip());

  push_play(engine);
  std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
  float* io[] = {l.data(), r.data()};
  engine.process(io, 2, kBlock);
  REQUIRE(engine.midi_sequencer().active_note_count() == 1);

  // Seek away: the discontinuity must lift the damper, reset controllers, send
  // all-notes-off and recenter pitch bend on the held channel (M-4).
  sonare::rt::Command seek{};
  seek.type = sonare::rt::CommandType::kTransportSeekSample;
  seek.arg.i = 1 << 16;
  seek.sample_time = kBlock;
  REQUIRE(engine.push_command(seek));
  engine.process(io, 2, kBlock);

  REQUIRE(inst.sustain_off_);
  REQUIRE(inst.reset_all_controllers_);
  REQUIRE(inst.all_notes_off_cc_);
  REQUIRE(inst.pitch_bend_seen_);

  engine.set_midi_instrument(nullptr);
}

TEST_CASE("stop chokes sounding notes", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 256;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  CountingInstrument inst;
  engine.set_midi_instrument(&inst);
  engine.set_midi_clips(held_note_clip());

  push_play(engine);
  std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
  float* io[] = {l.data(), r.data()};
  engine.process(io, 2, kBlock);
  REQUIRE(engine.midi_sequencer().active_note_count() == 1);

  sonare::rt::Command stop{};
  stop.type = sonare::rt::CommandType::kTransportStop;
  stop.sample_time = kBlock;
  REQUIRE(engine.push_command(stop));

  std::fill(l.begin(), l.end(), 0.0f);
  std::fill(r.begin(), r.end(), 0.0f);
  engine.process(io, 2, kBlock);

  REQUIRE(engine.midi_sequencer().active_note_count() == 0);
  REQUIRE(inst.note_off_count_ >= 1);
  REQUIRE(block_peak(l) == 0.0f);

  engine.set_midi_instrument(nullptr);
}

TEST_CASE("stopped transport dispatches nothing and renders no instrument", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 256;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  CountingInstrument inst;
  engine.set_midi_instrument(&inst);
  engine.set_midi_clips(held_note_clip());

  // No play command: the transport stays stopped.
  std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
  float* io[] = {l.data(), r.data()};
  for (int block = 0; block < 4; ++block) {
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    engine.process(io, 2, kBlock);
    // A stopped playhead re-scans the same frozen window; the gate must keep it
    // from re-dispatching the note-on every block or rendering any audio.
    REQUIRE(block_peak(l) == 0.0f);
  }
  REQUIRE(inst.received_events_ == 0);
  REQUIRE(engine.midi_sequencer().active_note_count() == 0);

  engine.set_midi_instrument(nullptr);
}

TEST_CASE("binding a latency instrument reports and applies graph latency (PDC)",
          "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 256;
  constexpr int kLatency = 100;
  constexpr int64_t kFrames = 512;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  LatencyImpulseInstrument inst(kLatency);
  engine.set_midi_instrument(&inst);

  // The engine adopts the instrument's reported latency as its graph latency.
  REQUIRE(engine.midi_instrument_latency_samples() == kLatency);
  REQUIRE(engine.graph_latency_samples_q8() == (kLatency << 8));

  // A clip impulse at frame 0, no MIDI: PDC delays the clip bus by the bound
  // instrument's latency so the impulse emerges at output frame kLatency, not 0.
  engine.set_clips({impulse_clip(kFrames)});
  push_play(engine);
  std::vector<float> out_l(static_cast<size_t>(kFrames), 0.0f);
  std::vector<float> out_r(static_cast<size_t>(kFrames), 0.0f);
  float* io[] = {out_l.data(), out_r.data()};
  engine.render_offline(io, 2, kFrames, kBlock);

  REQUIRE(out_l[0] == 0.0f);
  REQUIRE(out_l[static_cast<size_t>(kLatency)] == Catch::Approx(1.0f));

  engine.set_midi_instrument(nullptr);
}

TEST_CASE("PDC threads and applies fractional (Q8) instrument latency", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 256;
  constexpr int kLatencyQ8 = 64 * 256 + 128;  // 64.5 samples
  constexpr int64_t kFrames = 512;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  FractionalLatencyInstrument inst(kLatencyQ8);
  engine.set_midi_instrument(&inst);

  // Graph latency reports the exact Q8 figure (sub-sample preserved).
  REQUIRE(engine.graph_latency_samples_q8() == kLatencyQ8);

  engine.set_clips({impulse_clip(kFrames)});
  push_play(engine);
  std::vector<float> out_l(static_cast<size_t>(kFrames), 0.0f);
  std::vector<float> out_r(static_cast<size_t>(kFrames), 0.0f);
  float* io[] = {out_l.data(), out_r.data()};
  engine.render_offline(io, 2, kFrames, kBlock);

  // A 64.5-sample fractional delay spreads the unit impulse across the taps
  // around 64-65 (Lagrange), unlike an integer-64 delay (single sample at 64).
  REQUIRE(out_l[0] == 0.0f);
  REQUIRE(out_l[64] != 0.0f);
  REQUIRE(out_l[65] != 0.0f);
  // The interpolation kernel sums to unity, so the energy around the fractional
  // position recovers the impulse amplitude.
  float window_sum = 0.0f;
  for (int i = 62; i <= 67; ++i) window_sum += out_l[static_cast<size_t>(i)];
  REQUIRE(window_sum == Catch::Approx(1.0f).margin(0.02f));

  engine.set_midi_instrument(nullptr);
}

TEST_CASE("PDC aligns instrument audio with clip audio", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 256;
  constexpr int kLatency = 100;
  constexpr int64_t kFrames = 512;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  LatencyImpulseInstrument inst(kLatency);
  engine.set_midi_instrument(&inst);
  // A clip impulse at musical frame 0 AND a MIDI note-on at musical frame 0.
  // Without PDC the instrument's attack would lag the clip by kLatency; with PDC
  // the clip bus is delayed to meet the (internally late) instrument, so both
  // land on the SAME output frame and sum.
  engine.set_clips({impulse_clip(kFrames)});
  engine.set_midi_clips(note_on_at_zero());
  push_play(engine);

  std::vector<float> out_l(static_cast<size_t>(kFrames), 0.0f);
  std::vector<float> out_r(static_cast<size_t>(kFrames), 0.0f);
  float* io[] = {out_l.data(), out_r.data()};
  engine.render_offline(io, 2, kFrames, kBlock);

  // Nothing audible before the compensated arrival; clip (1.0) + instrument
  // (1.0) coincide at output frame kLatency.
  for (int64_t i = 0; i < kLatency; ++i) {
    REQUIRE(out_l[static_cast<size_t>(i)] == 0.0f);
  }
  REQUIRE(out_l[static_cast<size_t>(kLatency)] == Catch::Approx(2.0f));

  engine.set_midi_instrument(nullptr);
}
