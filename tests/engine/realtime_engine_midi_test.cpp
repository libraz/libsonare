/// @file realtime_engine_midi_test.cpp
/// @brief Engine-level MIDI integration: hang-note safety across seek / stop and
///        the stopped-transport gate (a stopped playhead dispatches nothing and
///        renders no instrument audio). Covers brush-up findings H-1, H-2.

#include <algorithm>
#include <array>
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
#include "midi/synth/sf2_player.h"
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

// Records the exact SysEx payloads it receives, so a test can assert that a live
// (queued) SysEx pushed via RealtimeEngine::push_midi_sysex reaches the addressed
// destination instrument byte-for-byte.
class SysExRecordingInstrument final : public MidiInstrument {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override { payloads_.clear(); }
  void on_event(uint32_t, const MidiEvent& event) noexcept override {
    if (event.sysex_payload != nullptr && event.sysex_payload_size > 0) {
      payloads_.emplace_back(event.sysex_payload, event.sysex_payload + event.sysex_payload_size);
    }
  }
  std::vector<std::vector<uint8_t>> payloads_;
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

#if defined(SONARE_WITH_MIXING)
std::vector<sonare::midi::MidiClipSchedule> note_on_at_zero(uint32_t destination_id) {
  auto clips = note_on_at_zero();
  clips[0].destination_id = destination_id;
  return clips;
}
#endif  // defined(SONARE_WITH_MIXING)

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

#if defined(SONARE_WITH_MIXING)
sonare::engine::ClipSchedule constant_track_clip(uint32_t track_id, int64_t length, float value) {
  auto storage = std::make_shared<sonare::engine::ClipAudioStorage>();
  storage->channels = {std::vector<float>(static_cast<size_t>(length), value),
                       std::vector<float>(static_cast<size_t>(length), value)};
  storage->channel_ptrs = {storage->channels[0].data(), storage->channels[1].data()};
  sonare::engine::ClipSchedule clip;
  clip.id = track_id;
  clip.track_id = track_id;
  clip.buffer.channels = storage->channel_ptrs.data();
  clip.buffer.num_channels = 2;
  clip.buffer.num_samples = length;
  clip.start_sample = 0;
  clip.length_samples = length;
  clip.gain = 1.0f;
  clip.storage = std::move(storage);
  return clip;
}
#endif

void push_play(RealtimeEngine& engine) {
  sonare::rt::Command c{};
  c.type = sonare::rt::CommandType::kTransportPlay;
  c.sample_time = -1;  // due immediately (clamped to block head)
  REQUIRE(engine.push_command(c));
}

void seek_to_zero(RealtimeEngine& engine) {
  sonare::rt::Command seek{};
  seek.type = sonare::rt::CommandType::kTransportSeekSample;
  seek.arg.i = 0;
  seek.sample_time = -1;  // due immediately (clamped to block head)
  REQUIRE(engine.push_command(seek));
}

// Renders a span in `chunks` equal calls and returns the concatenated left
// channel. `finalize` is what each chunk passes; a chunked bounce wants false on
// every chunk and one finish_offline_render() at the end.
std::vector<float> render_in_chunks(RealtimeEngine& engine, int64_t chunk_frames, int chunks,
                                    int block, bool finalize) {
  std::vector<float> joined;
  joined.reserve(static_cast<size_t>(chunk_frames) * static_cast<size_t>(chunks));
  for (int chunk = 0; chunk < chunks; ++chunk) {
    std::vector<float> l(static_cast<size_t>(chunk_frames), 0.0f);
    std::vector<float> r(static_cast<size_t>(chunk_frames), 0.0f);
    float* io[] = {l.data(), r.data()};
    engine.render_offline(io, 2, chunk_frames, block, finalize);
    joined.insert(joined.end(), l.begin(), l.end());
  }
  return joined;
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

TEST_CASE("RealtimeEngine routes live MIDI input to the configured destination", "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  CountingInstrument default_instrument;
  CountingInstrument routed_instrument;
  REQUIRE(engine.set_midi_instrument(0, &default_instrument));
  REQUIRE(engine.set_midi_instrument(7, &routed_instrument));

  sonare::host::FixedMidiInputSource<8> input;
  engine.set_midi_input_source(&input, 7);

  REQUIRE(input.push_event(sonare::midi::make_midi1_note_on(0, 0, 64, 100), 4));

  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.process(channels, 2, 64);

  REQUIRE(default_instrument.note_on_count_ == 0);
  REQUIRE(routed_instrument.note_on_count_ == 1);
  REQUIRE(block_peak(left) == Catch::Approx(0.5f));
}

TEST_CASE("RealtimeEngine delivers a live SysEx to the addressed destination", "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  SysExRecordingInstrument target;
  SysExRecordingInstrument other;
  REQUIRE(engine.set_midi_instrument(3, &target));
  REQUIRE(engine.set_midi_instrument(5, &other));

  // A "GM System On" universal SysEx frame (0xF0..0xF7). The transport treats the
  // bytes as opaque; the instrument records exactly what it receives.
  const std::vector<uint8_t> sysex = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
  REQUIRE(engine.push_midi_sysex(3, sysex.data(), sysex.size(), /*render_frame=*/-1));

  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.process(channels, 2, 64);

  REQUIRE(target.payloads_.size() == 1);
  REQUIRE(target.payloads_[0] == sysex);
  REQUIRE(other.payloads_.empty());
}

TEST_CASE("RealtimeEngine realises a live GS EFX SysEx on the control thread", "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  sonare::midi::synth::Sf2Player player;
  player.prepare(48000.0, 64);
  REQUIRE(engine.set_midi_instrument(2, &player));
  REQUIRE_FALSE(player.gs_efx().assigned);

  // Select Overdrive (01 10) on the single EFX unit (Roland DT1, address 40 03 00).
  const uint8_t od_type[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                             0x03, 0x00, 0x01, 0x10, 0x2C, 0xF7};
  REQUIRE(engine.push_midi_sysex(2, od_type, sizeof(od_type), /*render_frame=*/-1));

  // push_midi_sysex runs the instrument's control-thread realise as soon as the
  // audio-thread event is enqueued, so on success the EFX mirror is already live
  // when the call returns.
  REQUIRE(player.gs_efx().assigned);
  REQUIRE(player.gs_efx().type == 0x0110);

  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.process(channels, 2, 64);

  engine.set_midi_instrument(2, nullptr);
}

TEST_CASE("push_midi_sysex leaves the EFX mirror unrealised when the command queue is full",
          "[engine][midi]") {
  RealtimeEngine engine;
  // A small command queue so it fills after a few control-thread pushes.
  engine.prepare(48000.0, 64, /*command_capacity=*/4, /*telemetry_capacity=*/4);
  sonare::midi::synth::Sf2Player player;
  player.prepare(48000.0, 64);
  REQUIRE(engine.set_midi_instrument(2, &player));
  REQUIRE_FALSE(player.gs_efx().assigned);

  // Fill the command queue without draining it (no process() call), so the next
  // push is guaranteed to be rejected.
  sonare::rt::Command filler{};
  filler.type = sonare::rt::CommandType::kTransportPlay;
  filler.sample_time = -1;
  while (engine.push_command(filler)) {
    // Keep pushing until the bounded queue reports overflow.
  }

  // A GS EFX-select SysEx now cannot enqueue its audio-thread command. It must
  // report failure AND leave the control-side EFX mirror untouched: realising it
  // here would adopt the new effect chain while the queued channel state never
  // arrives -- a half-applied SysEx that diverges from an offline bounce.
  const uint8_t od_type[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                             0x03, 0x00, 0x01, 0x10, 0x2C, 0xF7};
  REQUIRE_FALSE(engine.push_midi_sysex(2, od_type, sizeof(od_type), /*render_frame=*/-1));
  REQUIRE_FALSE(player.gs_efx().assigned);

  engine.set_midi_instrument(2, nullptr);
}

TEST_CASE("RealtimeEngine rejects an invalid live SysEx payload", "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  SysExRecordingInstrument target;
  REQUIRE(engine.set_midi_instrument(0, &target));

  // A payload larger than the bounded store is rejected outright (nothing queued).
  std::vector<uint8_t> oversized(1024, 0x00);
  oversized.front() = 0xF0;
  oversized.back() = 0xF7;
  REQUIRE_FALSE(engine.push_midi_sysex(0, oversized.data(), oversized.size(), -1));
  // A null / zero-length payload is rejected too.
  REQUIRE_FALSE(engine.push_midi_sysex(0, nullptr, 0, -1));

  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.process(channels, 2, 64);
  REQUIRE(target.payloads_.empty());
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("RealtimeEngine routes instrument destinations through track lanes", "[engine][midi]") {
  constexpr int kBlock = 128;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  CountingInstrument lane_a;
  CountingInstrument lane_b;
  REQUIRE(engine.set_midi_instrument(10, &lane_a));
  REQUIRE(engine.set_midi_instrument(20, &lane_b));
  auto clips = note_on_at_zero(10);
  auto second = note_on_at_zero(20);
  second[0].id = 2;
  clips.push_back(second[0]);
  engine.set_midi_clips(clips);
  REQUIRE(engine.set_track_lanes({{10}, {20}}));
  push_play(engine);

  std::vector<float> left(kBlock, 0.0f);
  std::vector<float> right(kBlock, 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.process(channels, 2, kBlock);
  REQUIRE(block_peak(left) == Catch::Approx(1.0f));

  REQUIRE(engine.track_mixer().set_lane_solo_mute(0, true, false));
  // 24 blocks is ~64 ms, comfortably past the 10 ms gate smoother. The lane
  // smoothers advance once per block: they used to advance twice whenever a
  // block ran both a clip pass and an instrument pass, because each pass opened
  // its own lane/bus staging, so the same solo ramp settled in half the time.
  for (int i = 0; i < 24; ++i) {
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    engine.process(channels, 2, kBlock);
  }
  REQUIRE(block_peak(left) > 0.45f);
  REQUIRE(block_peak(left) < 0.55f);
}
#endif

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

TEST_CASE("RealtimeEngine does not mirror external destinations to the merged output sink",
          "[engine][midi]") {
  // A destination marked external routes to its own device queue INSTEAD of the
  // rack; it must not also be mirrored to the merged output sink, or a host
  // using both would emit the event twice to the device path.
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  CountingInstrument internal;
  REQUIRE(engine.set_midi_instrument(0, &internal));
  sonare::host::FixedMidiOutputSink<8> output;
  engine.set_midi_output_sink(&output);
  engine.set_midi_destination_external(5, true);

  sonare::midi::MidiClipSchedule internal_clip;
  internal_clip.id = 1;
  internal_clip.start_sample = 0;
  internal_clip.length_samples = 128;
  internal_clip.destination_id = 0;
  internal_clip.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                          {32, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  sonare::midi::MidiClipSchedule external_clip = internal_clip;
  external_clip.id = 2;
  external_clip.destination_id = 5;
  external_clip.events = {{0, sonare::midi::make_midi1_note_on(0, 1, 64, 110)},
                          {48, sonare::midi::make_midi1_note_off(0, 1, 64, 0)}};
  engine.set_midi_clips({internal_clip, external_clip});
  push_play(engine);

  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.process(channels, 2, 64);

  // The merged sink mirrors ONLY the internal destination's two events.
  REQUIRE(output.queued_count() == 2);
  std::array<MidiEvent, 8> drained{};
  const size_t mirrored = output.drain_queued(drained.data(), drained.size());
  REQUIRE(mirrored == 2);
  REQUIRE(drained[0].render_frame == 0);
  REQUIRE(drained[1].render_frame == 32);

  // The external destination's two events went to the external queue only.
  std::array<sonare::host::ExternalMidiRecord, 8> ext{};
  const size_t n = engine.drain_external_midi(ext.data(), ext.size());
  REQUIRE(n == 2);
  REQUIRE(ext[0].destination_id == 5);
  REQUIRE(ext[1].destination_id == 5);
}

TEST_CASE("RealtimeEngine routes external destinations to the output queue, bypassing the rack",
          "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  CountingInstrument internal;
  CountingInstrument external_slot;
  REQUIRE(engine.set_midi_instrument(0, &internal));
  REQUIRE(engine.set_midi_instrument(5, &external_slot));
  // Route destination 5 to the external output INSTEAD of its instrument.
  engine.set_midi_destination_external(5, true);

  sonare::midi::MidiClipSchedule internal_clip;
  internal_clip.id = 1;
  internal_clip.start_sample = 0;
  internal_clip.length_samples = 128;
  internal_clip.destination_id = 0;
  internal_clip.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                          {32, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  sonare::midi::MidiClipSchedule external_clip = internal_clip;
  external_clip.id = 2;
  external_clip.destination_id = 5;
  external_clip.events = {{0, sonare::midi::make_midi1_note_on(0, 1, 64, 110)},
                          {48, sonare::midi::make_midi1_note_off(0, 1, 64, 0)}};
  engine.set_midi_clips({internal_clip, external_clip});
  push_play(engine);

  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.process(channels, 2, 64);

  // The internal destination still drives its instrument.
  REQUIRE(internal.note_on_count_ == 1);
  REQUIRE(internal.note_off_count_ == 1);
  // The external destination's instrument is bypassed (no double-trigger).
  REQUIRE(external_slot.received_events_ == 0);

  // The external events are queued, each tagged with its destination.
  std::array<sonare::host::ExternalMidiRecord, 8> drained{};
  const size_t n = engine.drain_external_midi(drained.data(), drained.size());
  REQUIRE(n == 2);
  REQUIRE(drained[0].destination_id == 5);
  REQUIRE(drained[0].event.render_frame == 0);
  REQUIRE(drained[0].event.ump.is_note_on());
  REQUIRE(drained[1].destination_id == 5);
  REQUIRE(drained[1].event.render_frame == 48);
  REQUIRE(drained[1].event.ump.is_note_off());

  // Clearing the external routing restores internal-rack delivery. Rewind first
  // so the note-on at frame 0 fires again (the first block advanced the head).
  engine.set_midi_destination_external(5, false);
  engine.set_midi_clips({external_clip});
  sonare::rt::Command rewind{};
  rewind.type = sonare::rt::CommandType::kTransportSeekSample;
  rewind.arg.i = 0;
  rewind.sample_time = -1;
  REQUIRE(engine.push_command(rewind));
  push_play(engine);
  std::fill(left.begin(), left.end(), 0.0f);
  std::fill(right.begin(), right.end(), 0.0f);
  engine.process(channels, 2, 64);
  REQUIRE(external_slot.received_events_ > 0);
  REQUIRE(engine.drain_external_midi(drained.data(), drained.size()) == 0);
}

TEST_CASE("RealtimeEngine forwards MIDI clock/transport to the external output queue",
          "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 24000);
  engine.set_tempo(120.0);
  engine.set_external_midi_clock_enabled(true);

  sonare::rt::Command play;
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = 0;
  REQUIRE(engine.push_command(play));

  std::vector<float> left(24000, 0.0f);
  std::vector<float> right(24000, 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.process(channels, 2, 24000);

  // One Start followed by 24 clock ticks (one every 1000 samples at 120 BPM).
  std::array<sonare::host::ExternalMidiRecord, 64> drained{};
  const size_t n = engine.drain_external_midi(drained.data(), drained.size());
  REQUIRE(n == 25);
  for (size_t i = 0; i < n; ++i) {
    REQUIRE(drained[i].destination_id == sonare::host::kTransportDestination);
    REQUIRE(drained[i].event.ump.message_type() == sonare::midi::UmpMessageType::kSystem);
  }
  const auto status_byte = [](const sonare::host::ExternalMidiRecord& r) {
    return static_cast<uint8_t>((r.event.ump.words[0] >> 16) & 0xFFu);
  };
  REQUIRE(status_byte(drained[0]) == sonare::midi::kStatusStart);
  REQUIRE(drained[0].event.render_frame == 0);
  REQUIRE(status_byte(drained[1]) == sonare::midi::kStatusClock);

  // Disabling forwarding stops further bytes from queueing.
  engine.set_external_midi_clock_enabled(false);
  std::fill(left.begin(), left.end(), 0.0f);
  std::fill(right.begin(), right.end(), 0.0f);
  engine.process(channels, 2, 24000);
  REQUIRE(engine.drain_external_midi(drained.data(), drained.size()) == 0);
}

TEST_CASE("RealtimeEngine caps MIDI-clock work and reports overflow telemetry",
          "[engine][midi][rt]") {
  RealtimeEngine engine;
  engine.prepare(8000.0, 512);
  engine.set_tempo(sonare::transport::kMaxPublicTempoBpm);
  engine.set_external_midi_clock_enabled(true);
  push_play(engine);

  std::array<float, 512> left{};
  std::array<float, 512> right{};
  float* channels[] = {left.data(), right.data()};
  engine.process(channels, 2, 512);

  bool reported = false;
  sonare::engine::Telemetry telemetry{};
  while (engine.pop_telemetry(telemetry)) {
    reported =
        reported || telemetry.error == sonare::engine::TelemetryErrorCode::kMidiClockOverflow;
  }
  REQUIRE(reported);
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

TEST_CASE("render_offline releases held MIDI notes at the end", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 128;
  constexpr int64_t kFrames = 256;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  CountingInstrument inst;
  engine.set_midi_instrument(&inst);
  engine.set_midi_clips(held_note_clip());

  push_play(engine);
  std::vector<float> out_l(static_cast<size_t>(kFrames), 0.0f);
  std::vector<float> out_r(static_cast<size_t>(kFrames), 0.0f);
  float* out[] = {out_l.data(), out_r.data()};
  engine.render_offline(out, 2, kFrames, kBlock);

  REQUIRE(inst.note_on_count_ == 1);
  REQUIRE(inst.note_off_count_ >= 1);
  REQUIRE(engine.midi_sequencer().active_note_count() == 0);

  std::vector<float> next_l(static_cast<size_t>(kBlock), 0.0f);
  std::vector<float> next_r(static_cast<size_t>(kBlock), 0.0f);
  float* next[] = {next_l.data(), next_r.data()};
  engine.process(next, 2, kBlock);
  REQUIRE(block_peak(next_l) == Catch::Approx(0.0f));

  engine.set_midi_instrument(nullptr);
}

TEST_CASE("render_offline flushes PDC delay tails before returning", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 64;
  constexpr int kLatency = 96;
  constexpr int64_t kFrames = 32;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  FractionalLatencyInstrument inst(kLatency << 8);
  engine.set_midi_instrument(&inst);
  engine.set_clips({impulse_clip(kFrames)});

  push_play(engine);
  std::vector<float> out_l(static_cast<size_t>(kFrames), 0.0f);
  std::vector<float> out_r(static_cast<size_t>(kFrames), 0.0f);
  float* out[] = {out_l.data(), out_r.data()};
  engine.render_offline(out, 2, kFrames, kBlock);
  REQUIRE(block_peak(out_l) == Catch::Approx(0.0f));

  engine.set_clips({});
  std::vector<float> next_l(static_cast<size_t>(kLatency + kBlock), 0.0f);
  std::vector<float> next_r(static_cast<size_t>(kLatency + kBlock), 0.0f);
  float* next[] = {next_l.data(), next_r.data()};
  engine.render_offline(next, 2, static_cast<int64_t>(next_l.size()), kBlock);
  REQUIRE(block_peak(next_l) == Catch::Approx(0.0f));

  engine.set_midi_instrument(nullptr);
}

#if defined(SONARE_WITH_MIXING)
TEST_CASE("render_offline flushes PDC delay tails from lane-routed clips", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 64;
  constexpr int kLatency = 96;
  constexpr int64_t kFrames = 32;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  FractionalLatencyInstrument inst(kLatency << 8);
  engine.set_midi_instrument(&inst);
  auto clip = impulse_clip(kFrames);
  clip.track_id = 10;
  engine.set_clips({clip});
  REQUIRE(engine.set_track_lanes({{10}}));

  push_play(engine);
  std::vector<float> out_l(static_cast<size_t>(kFrames), 0.0f);
  std::vector<float> out_r(static_cast<size_t>(kFrames), 0.0f);
  float* out[] = {out_l.data(), out_r.data()};
  engine.render_offline(out, 2, kFrames, kBlock);
  REQUIRE(block_peak(out_l) == Catch::Approx(0.0f));

  engine.set_clips({});
  std::vector<float> next_l(static_cast<size_t>(kLatency + kBlock), 0.0f);
  std::vector<float> next_r(static_cast<size_t>(kLatency + kBlock), 0.0f);
  float* next[] = {next_l.data(), next_r.data()};
  engine.render_offline(next, 2, static_cast<int64_t>(next_l.size()), kBlock);
  REQUIRE(block_peak(next_l) == Catch::Approx(0.0f));

  engine.set_midi_instrument(nullptr);
}
#endif

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

TEST_CASE("PDC scratch follows prepared channels and is reclaimed when unbound", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 128;
  constexpr int kLatency = 37;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock, 16, 16, 64);
  const size_t without_pdc = engine.prepared_scratch_bytes();

  LatencyImpulseInstrument inst(kLatency);
  engine.set_midi_instrument(&inst);
  const size_t with_pdc = engine.prepared_scratch_bytes();
  REQUIRE(with_pdc > without_pdc);

  // PDC is channel-planar just like the engine scratch. Repreparing from the
  // full 64-plane bound to stereo must shrink the delay banks as well.
  engine.prepare(kSr, kBlock, 16, 16, 2);
  const size_t stereo_with_pdc = engine.prepared_scratch_bytes();
  REQUIRE(stereo_with_pdc == with_pdc / 32);

  engine.set_midi_instrument(nullptr);
  REQUIRE(engine.prepared_scratch_bytes() ==
          stereo_with_pdc - 2u * static_cast<size_t>(kLatency) * sizeof(float));
  // The slowest instrument's own bank is zero-delay; only the clip bank above
  // contributes storage, and no superseded 64-plane or unbound bank remains.
  REQUIRE(engine.prepared_scratch_bytes() < stereo_with_pdc);
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

#if defined(SONARE_WITH_MIXING)
TEST_CASE("PDC clip bus still routes through track lanes", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 256;
  constexpr int kLatency = 96;
  constexpr int64_t kFrames = kBlock * 10;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  FractionalLatencyInstrument inst(kLatency << 8);
  engine.set_midi_instrument(99, &inst);
  engine.set_clips({constant_track_clip(10, kFrames, 1.0f)});
  REQUIRE(engine.set_track_lanes({{10}}));
  REQUIRE(engine.track_mixer().set_lane_parameter(0, sonare::engine::TrackMixerRuntime::kFaderDb,
                                                  -12.0f));
  push_play(engine);

  std::vector<float> out_l(static_cast<size_t>(kBlock), 0.0f);
  std::vector<float> out_r(static_cast<size_t>(kBlock), 0.0f);
  float* io[] = {out_l.data(), out_r.data()};
  for (int block = 0; block < 7; ++block) {
    std::fill(out_l.begin(), out_l.end(), 0.0f);
    std::fill(out_r.begin(), out_r.end(), 0.0f);
    engine.process(io, 2, kBlock);
  }

  REQUIRE(block_peak(out_l) > 0.20f);
  REQUIRE(block_peak(out_l) < 0.35f);
  engine.set_midi_instrument(99, nullptr);
}
#endif

TEST_CASE("stopped transport renders no clip audio", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 128;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  engine.set_clips({impulse_clip(kBlock)});

  // While stopped the playhead is frozen, so the clip bus must stay silent —
  // rendering the frozen window every block would emit a sustained buzz.
  std::vector<float> out_l(static_cast<size_t>(kBlock), 0.0f);
  std::vector<float> out_r(static_cast<size_t>(kBlock), 0.0f);
  float* io[] = {out_l.data(), out_r.data()};
  engine.process(io, 2, kBlock);
  REQUIRE(block_peak(out_l) == Catch::Approx(0.0f));
  REQUIRE(block_peak(out_r) == Catch::Approx(0.0f));

  // Rolling the transport renders the clip impulse at frame 0.
  push_play(engine);
  engine.process(io, 2, kBlock);
  REQUIRE(out_l[0] == Catch::Approx(1.0f));
}

TEST_CASE("stopped transport renders no metronome click", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 128;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  engine.set_tempo(120.0);
  engine.set_time_signature(4, 4);
  engine.set_metronome_config(sonare::engine::MetronomeConfig{
      true,
      0.25f,
      0.75f,
      32,
      0.0,
  });

  std::vector<float> out_l(static_cast<size_t>(kBlock), 0.0f);
  std::vector<float> out_r(static_cast<size_t>(kBlock), 0.0f);
  float* io[] = {out_l.data(), out_r.data()};

  engine.process(io, 2, kBlock);
  REQUIRE(block_peak(out_l) == Catch::Approx(0.0f));
  REQUIRE(block_peak(out_r) == Catch::Approx(0.0f));

  push_play(engine);
  engine.process(io, 2, kBlock);
  REQUIRE(block_peak(out_l) > 0.7f);
  REQUIRE(block_peak(out_r) > 0.7f);
}

TEST_CASE("render_offline rolls a stopped transport and restores it", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 128;
  constexpr int64_t kFrames = 256;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  engine.set_clips({impulse_clip(kFrames)});

  std::vector<float> out_l(static_cast<size_t>(kFrames), 0.0f);
  std::vector<float> out_r(static_cast<size_t>(kFrames), 0.0f);
  float* io[] = {out_l.data(), out_r.data()};
  engine.render_offline(io, 2, kFrames, kBlock);

  REQUIRE(out_l[0] == Catch::Approx(1.0f));
  REQUIRE(engine.transport().sample_position() == kFrames);
  REQUIRE_FALSE(engine.transport().playing());
}

TEST_CASE("render_offline re-renders a span identically after seeking back", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 128;
  constexpr int64_t kFrames = 4096;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  CountingInstrument inst;
  engine.set_midi_instrument(&inst);
  // A note-on with no note-off in range: it is still sounding when the span
  // ends, which is the state a non-finalizing render is required to preserve.
  // The audio clip's impulse puts a transient in the span too, so the comparison
  // is not over a constant.
  engine.set_midi_clips(note_on_at_zero());
  engine.set_clips({impulse_clip(kFrames)});

  // Both passes are entered with the same command sequence (seek to 0, then
  // play), so the only thing that can make them differ is state the previous
  // render left behind -- which is what this pins.
  seek_to_zero(engine);
  push_play(engine);
  std::vector<float> first_l(static_cast<size_t>(kFrames), 0.0f);
  std::vector<float> first_r(static_cast<size_t>(kFrames), 0.0f);
  float* first[] = {first_l.data(), first_r.data()};
  engine.render_offline(first, 2, kFrames, kBlock, /*finalize=*/false);
  REQUIRE(block_peak(first_l) > 0.0f);
  REQUIRE(engine.midi_sequencer().active_note_count() == 1);

  seek_to_zero(engine);
  push_play(engine);
  std::vector<float> second_l(static_cast<size_t>(kFrames), 0.0f);
  std::vector<float> second_r(static_cast<size_t>(kFrames), 0.0f);
  float* second[] = {second_l.data(), second_r.data()};
  engine.render_offline(second, 2, kFrames, kBlock, /*finalize=*/false);

  REQUIRE(second_l == first_l);
  REQUIRE(second_r == first_r);

  engine.finish_offline_render();
  engine.set_midi_instrument(nullptr);
}

TEST_CASE("a chunked render_offline concatenates to one continuous render", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 128;
  constexpr int64_t kChunk = 48000;  // one second
  constexpr int kChunks = 3;
  constexpr int64_t kTotal = kChunk * kChunks;

  // Reference: the whole span in one call. The finalize this call does happens
  // after its last sample, so it cannot affect the span it just rendered.
  RealtimeEngine continuous;
  continuous.prepare(kSr, kBlock);
  CountingInstrument continuous_inst;
  continuous.set_midi_instrument(&continuous_inst);
  continuous.set_midi_clips(note_on_at_zero());
  push_play(continuous);
  std::vector<float> whole_l(static_cast<size_t>(kTotal), 0.0f);
  std::vector<float> whole_r(static_cast<size_t>(kTotal), 0.0f);
  float* whole[] = {whole_l.data(), whole_r.data()};
  continuous.render_offline(whole, 2, kTotal, kBlock);
  continuous.set_midi_instrument(nullptr);

  // The pad has to be audible for the whole span, or "chunk 2 matches" would
  // hold trivially between two silences.
  REQUIRE(whole_l.front() > 0.0f);
  REQUIRE(whole_l.back() > 0.0f);

  RealtimeEngine chunked;
  chunked.prepare(kSr, kBlock);
  CountingInstrument chunked_inst;
  chunked.set_midi_instrument(&chunked_inst);
  chunked.set_midi_clips(note_on_at_zero());
  push_play(chunked);
  const std::vector<float> joined =
      render_in_chunks(chunked, kChunk, kChunks, kBlock, /*finalize=*/false);
  chunked.finish_offline_render();
  chunked.set_midi_instrument(nullptr);

  REQUIRE(joined.size() == whole_l.size());
  REQUIRE(joined == whole_l);

  // The boundary itself: no dropout and no amplitude step across it.
  for (int chunk = 1; chunk < kChunks; ++chunk) {
    const size_t boundary = static_cast<size_t>(kChunk) * static_cast<size_t>(chunk);
    REQUIRE(joined[boundary] > 0.0f);
    REQUIRE(joined[boundary] == Catch::Approx(joined[boundary - 1]));
  }

  // Non-vacuity: finalizing every chunk is the defect this flag exists to fix.
  // The pad's note-off fires at the end of chunk 1 and no note-on is re-sent, so
  // chunk 2 onwards is silent -- if this did NOT differ, the comparison above
  // would prove nothing about the flag.
  RealtimeEngine finalized;
  finalized.prepare(kSr, kBlock);
  CountingInstrument finalized_inst;
  finalized.set_midi_instrument(&finalized_inst);
  finalized.set_midi_clips(note_on_at_zero());
  push_play(finalized);
  const std::vector<float> per_chunk_finalized =
      render_in_chunks(finalized, kChunk, kChunks, kBlock, /*finalize=*/true);
  finalized.set_midi_instrument(nullptr);
  REQUIRE(per_chunk_finalized[static_cast<size_t>(kChunk)] == Catch::Approx(0.0f));
  REQUIRE(per_chunk_finalized != whole_l);
}

TEST_CASE("finish_offline_render releases what a chunked render left sounding", "[engine][midi]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 128;
  constexpr int64_t kFrames = 512;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  CountingInstrument inst;
  engine.set_midi_instrument(&inst);
  engine.set_midi_clips(note_on_at_zero());
  push_play(engine);

  std::vector<float> out_l(static_cast<size_t>(kFrames), 0.0f);
  std::vector<float> out_r(static_cast<size_t>(kFrames), 0.0f);
  float* out[] = {out_l.data(), out_r.data()};
  engine.render_offline(out, 2, kFrames, kBlock, /*finalize=*/false);

  // Still held: that is the whole point of not finalizing.
  REQUIRE(inst.note_on_count_ == 1);
  REQUIRE(inst.note_off_count_ == 0);
  REQUIRE(engine.midi_sequencer().active_note_count() == 1);

  engine.finish_offline_render();
  REQUIRE(inst.note_off_count_ >= 1);
  REQUIRE(engine.midi_sequencer().active_note_count() == 0);

  // Idempotent: a second call on a settled engine releases nothing further.
  const int note_offs = inst.note_off_count_;
  engine.finish_offline_render();
  REQUIRE(inst.note_off_count_ == note_offs);
  REQUIRE(engine.midi_sequencer().active_note_count() == 0);

  engine.set_midi_instrument(nullptr);
}
