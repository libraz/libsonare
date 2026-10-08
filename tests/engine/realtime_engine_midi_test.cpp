/// @file realtime_engine_midi_test.cpp
/// @brief Engine-level MIDI integration: hang-note safety across seek / stop,
///        the stopped-transport gate (a stopped playhead dispatches no clip
///        events) and sample-accurate placement of every event.

#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "automation/automation_lane.h"
#include "engine/realtime_engine.h"
#include "host/midi_io.h"
#include "mastering/api/insert_factory.h"
#include "midi/builtin_synth.h"
#include "midi/clock_sync.h"
#include "midi/instrument.h"
#include "midi/midi_clip.h"
#include "midi/midi_event.h"
#include "midi/midi_fx.h"
#include "midi/prepared_sysex.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "rt/command.h"
#include "support/alloc_guard.h"
#include "support/sf2_builder.h"
#include "util/exception.h"

namespace {

using sonare::engine::RealtimeEngine;
using sonare::midi::MidiEvent;
using sonare::midi::MidiInstrument;
using sonare::midi::PreparedMidiSysEx;

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

// Captures the transport frame most recently pushed when each event arrives,
// alongside the event frame. A dense SysEx schedule is sample-accurate only when
// every event starts a sub-block, which event.render_frame alone cannot show.
class BoundaryTimingInstrument final : public MidiInstrument {
 public:
  struct Observation {
    int64_t event_frame = 0;
    int64_t callback_frame = 0;
  };

  void prepare(double, int) override {
    count_ = 0;
    callback_frame_ = 0;
  }
  void process(float* const*, int, int) override {}
  void reset() override { count_ = 0; }
  void set_transport(const sonare::transport::TransportState& state) noexcept override {
    callback_frame_ = state.render_frame;
  }
  void on_event(uint32_t, const MidiEvent& event) noexcept override {
    if (event.ump.message_type() != sonare::midi::UmpMessageType::kData64 ||
        event.sysex_payload_size == 0 || count_ >= observations_.size()) {
      return;
    }
    observations_[count_++] = {event.render_frame, callback_frame_};
  }

  std::array<Observation, 128> observations_{};
  size_t count_ = 0;

 private:
  int64_t callback_frame_ = 0;
};

struct TestPreparedMidiSysEx final : sonare::midi::PreparedMidiSysEx {
  TestPreparedMidiSysEx(uint32_t domain, uint32_t serial) : domain(domain), serial(serial) {}
  uint32_t domain = 0;
  uint32_t serial = 0;
};

// A small instrument whose SysEx preparation identifies the instrument domain.
// It also records the raw event pointer, proving the audio path borrows the
// CONTROL-owned token without copying or dropping it.
class PreparedTokenInstrument final : public MidiInstrument {
 public:
  explicit PreparedTokenInstrument(uint32_t domain, bool fail = false, bool throw_bad_alloc = false,
                                   bool legacy = false)
      : domain_(domain), fail_(fail), throw_bad_alloc_(throw_bad_alloc), legacy_(legacy) {}

  void prepare(double, int) override {
    ++prepare_calls_;
    observed_count_ = 0;
  }
  void process(float* const*, int, int) override { ++process_calls_; }
  void reset() override { observed_count_ = 0; }
  bool prepare_sysex(const uint8_t*, size_t,
                     std::shared_ptr<const PreparedMidiSysEx>& out) override {
    if (throw_bad_alloc_) throw std::bad_alloc();
    if (fail_) {
      out.reset();
      return false;
    }
    if (legacy_) {
      out.reset();
      return true;
    }
    auto token = std::make_shared<TestPreparedMidiSysEx>(domain_, next_serial_++);
    prepared_.push_back(token);
    out = std::move(token);
    return true;
  }
  void on_control_sysex(const uint8_t* data, size_t size) noexcept override {
    ++control_calls_;
    if (legacy_ && data != nullptr && size > 2) control_markers_.push_back(data[2]);
  }
  void on_event(uint32_t, const MidiEvent& event) noexcept override {
    if (event.prepared_sysex == nullptr || observed_count_ >= observed_.size()) return;
    const auto* token = static_cast<const TestPreparedMidiSysEx*>(event.prepared_sysex);
    observed_[observed_count_++] = {event.render_frame, token->domain, token->serial};
  }

  struct Observation {
    int64_t render_frame = 0;
    uint32_t domain = 0;
    uint32_t serial = 0;
  };
  uint32_t domain_ = 0;
  bool fail_ = false;
  bool throw_bad_alloc_ = false;
  bool legacy_ = false;
  uint32_t next_serial_ = 0;
  int prepare_calls_ = 0;
  int process_calls_ = 0;
  int control_calls_ = 0;
  std::vector<uint8_t> control_markers_;
  std::vector<std::weak_ptr<const TestPreparedMidiSysEx>> prepared_;
  std::array<Observation, 128> observed_{};
  size_t observed_count_ = 0;
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
// applies a fractional delay to the clip bus.
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

// Exposes one automatable parameter ("level") and records the value the engine
// pushed, so a test can watch a destination's automation slots across a bind.
class AutomatableInstrument final : public MidiInstrument {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  void on_event(uint32_t, const MidiEvent&) noexcept override {}

  int parameter_id_for_key(const std::string& key) const noexcept override {
    return key == "level" ? 0 : -1;
  }
  bool apply_parameter(unsigned int param_id, float value) noexcept override {
    if (param_id != 0) return false;
    level = value;
    return true;
  }

  float level = 0.0f;
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

// A stereo clip carrying steady DC, so a hole punched into a delay bank shows up
// as a run of samples that are not @p value.
sonare::engine::ClipSchedule constant_clip(int64_t length, float value) {
  auto storage = std::make_shared<sonare::engine::ClipAudioStorage>();
  storage->channels = {std::vector<float>(static_cast<size_t>(length), value),
                       std::vector<float>(static_cast<size_t>(length), value)};
  storage->channel_ptrs = {storage->channels[0].data(), storage->channels[1].data()};
  sonare::engine::ClipSchedule clip;
  clip.id = 3;
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

namespace {

// Pushes a live CC through the queued scalar entry point (the packing is
// rt/command.h's kMidiCcImmediate encoding).
void push_live_cc(RealtimeEngine& engine, uint8_t group, uint8_t channel, uint8_t controller,
                  uint8_t value) {
  const uint64_t packed = static_cast<uint64_t>(value) | (static_cast<uint64_t>(controller) << 8) |
                          (static_cast<uint64_t>(channel) << 16) |
                          (static_cast<uint64_t>(group) << 24);
  sonare::rt::Command c{};
  c.type = sonare::rt::CommandType::kMidiCcImmediate;
  c.sample_time = -1;
  c.arg.i = static_cast<int64_t>(packed);
  REQUIRE(engine.push_command(c));
}

// Pushes the same gesture through the queued raw-UMP entry point (the one the
// WASM pushMidiUmp uses).
void push_live_ump(RealtimeEngine& engine, const sonare::midi::Ump& ump) {
  sonare::rt::Command c{};
  c.type = sonare::rt::CommandType::kMidiUmpImmediate;
  c.sample_time = -1;
  c.arg.i = static_cast<int64_t>(ump.words[0]);
  REQUIRE(engine.push_command(c));
}

}  // namespace

// The engine has three ways to deliver a live controller message, and they used
// to disagree: the queued scalar CC resolved through the cc_number-only lookup
// (MSB-only 7 bits, no RPN/NRPN), the queued raw UMP never consulted the binding
// table at all, and only the engine-owned input source ran the kind-aware
// decoder. The same gesture therefore drove a parameter, drove it at the wrong
// resolution, or drove nothing, depending purely on which call a surface made.
//
// `unknown_target_count` is the probe: the binding points at a parameter id
// nothing is bound to, so it increments once per RESOLVED message and stays put
// when a path skipped the table.
TEST_CASE("every live CC entry point resolves through the same kind-aware decoder",
          "[engine][midi]") {
  constexpr uint32_t kUnboundParam = 7777;
  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  float* io[] = {left.data(), right.data()};

  sonare::midi::CcBinding wide;
  wide.kind = sonare::midi::CcBindingKind::kControlChange14;
  wide.cc_number = 1;
  wide.cc_lsb_number = 33;
  wide.channel = 0;
  wide.param_id = kUnboundParam;
  wide.min_value = 0.0f;
  wide.max_value = 1.0f;

  SECTION("the queued scalar CC path") {
    RealtimeEngine engine;
    engine.prepare(48000.0, 64);
    REQUIRE(engine.bind_midi_cc(wide));
    push_live_cc(engine, 0, 0, 1, 64);
    engine.process(io, 2, 64);
    REQUIRE(engine.automation().unknown_target_count() == 1);
  }

  SECTION("the queued raw UMP path") {
    RealtimeEngine engine;
    engine.prepare(48000.0, 64);
    REQUIRE(engine.bind_midi_cc(wide));
    push_live_ump(engine, sonare::midi::make_midi1_control_change(0, 0, 1, 64));
    engine.process(io, 2, 64);
    // Zero here is the defect this closes: the raw-UMP path reached the
    // sequencer without ever looking at the binding table.
    REQUIRE(engine.automation().unknown_target_count() == 1);
  }

  SECTION("the engine-owned live input path") {
    RealtimeEngine engine;
    engine.prepare(48000.0, 64);
    REQUIRE(engine.bind_midi_cc(wide));
    sonare::host::FixedMidiInputSource<8> input;
    engine.set_midi_input_source(&input, 0);
    REQUIRE(input.push_event(sonare::midi::make_midi1_control_change(0, 0, 1, 64), 0));
    engine.process(io, 2, 64);
    REQUIRE(engine.automation().unknown_target_count() == 1);
  }

  SECTION("an unbound controller resolves on no path") {
    RealtimeEngine engine;
    engine.prepare(48000.0, 64);
    REQUIRE(engine.bind_midi_cc(wide));
    push_live_cc(engine, 0, 0, 90, 64);
    push_live_ump(engine, sonare::midi::make_midi1_control_change(0, 0, 90, 64));
    engine.process(io, 2, 64);
    REQUIRE(engine.automation().unknown_target_count() == 0);
  }

  SECTION("a 14-bit gesture holds its LSB on every queued path") {
    // The kind-aware decoder emits at MSB resolution, then again once the LSB
    // completes the pair: two resolutions for one gesture. The old cc_number-only
    // lookup saw the LSB (CC 33) as an unbound controller and dropped it.
    RealtimeEngine engine;
    engine.prepare(48000.0, 64);
    REQUIRE(engine.bind_midi_cc(wide));
    push_live_cc(engine, 0, 0, 1, 64);
    push_live_cc(engine, 0, 0, 33, 127);
    engine.process(io, 2, 64);
    REQUIRE(engine.automation().unknown_target_count() == 2);
  }
}

TEST_CASE("RealtimeEngine does not join controller gestures across input sources",
          "[engine][midi]") {
  constexpr uint32_t kUnboundParam = 7777;
  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  float* io[] = {left.data(), right.data()};
  sonare::midi::CcBinding wide;
  wide.kind = sonare::midi::CcBindingKind::kControlChange14;
  wide.cc_number = 1;
  wide.cc_lsb_number = 33;
  wide.channel = 0;
  wide.param_id = kUnboundParam;
  wide.min_value = 0.0f;
  wide.max_value = 1.0f;

  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  REQUIRE(engine.bind_midi_cc(wide));
  sonare::host::FixedMidiInputSource<8> source_a;
  sonare::host::FixedMidiInputSource<8> source_b;
  engine.set_midi_input_source(&source_a, 0);
  REQUIRE(source_a.push_event(sonare::midi::make_midi1_control_change(0, 0, 1, 64), 0));
  engine.process(io, 2, 64);
  REQUIRE(engine.automation().unknown_target_count() == 1);

  // B's lone LSB must not complete the MSB A left pending.
  engine.set_midi_input_source(&source_b, 0);
  REQUIRE(source_b.push_event(sonare::midi::make_midi1_control_change(0, 0, 33, 127), 0));
  engine.process(io, 2, 64);
  REQUIRE(engine.automation().unknown_target_count() == 1);

  // Within one source, a binding added mid-gesture keeps the pending MSB.
  REQUIRE(source_b.push_event(sonare::midi::make_midi1_control_change(0, 0, 1, 64), 0));
  engine.process(io, 2, 64);
  REQUIRE(engine.automation().unknown_target_count() == 2);
  sonare::midi::CcBinding other = wide;
  other.cc_number = 2;
  other.cc_lsb_number = 34;
  REQUIRE(engine.bind_midi_cc(other));
  REQUIRE(source_b.push_event(sonare::midi::make_midi1_control_change(0, 0, 33, 127), 0));
  engine.process(io, 2, 64);
  REQUIRE(engine.automation().unknown_target_count() == 3);
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

TEST_CASE("RealtimeEngine does not retain borrowed SysEx in asynchronous MIDI sinks",
          "[engine][midi][sysex]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  PreparedTokenInstrument internal(61);
  REQUIRE(engine.set_midi_instrument(0, &internal));

  sonare::host::FixedMidiOutputSink<8> merged_output;
  engine.set_midi_output_sink(&merged_output);
  std::vector<uint8_t> payload{0xF0, 0x7D, 0x61, 0xF7};
  REQUIRE(engine.push_midi_sysex(0, payload.data(), payload.size(), -1));

  // The instrument consumes the borrowed view synchronously, while the
  // retained output queue must not receive a pointer into the slot or its
  // stack-local copy.
  std::fill(payload.begin(), payload.end(), 0x00);
  std::array<float, 64> audio{};
  float* io[] = {audio.data()};
  engine.process(io, 1, 64);
  REQUIRE(internal.observed_count_ == 1);
  REQUIRE(merged_output.queued_count() == 0);

  // External routing uses a second asynchronous fixed queue. Its record must
  // be dropped for the same lifetime reason, including after the slot is
  // released and reused by a later push.
  REQUIRE(engine.set_midi_destination_external(7, true));
  REQUIRE(engine.push_midi_sysex(7, payload.data(), payload.size(), -1));
  engine.process(io, 1, 64);
  std::array<sonare::host::ExternalMidiRecord, 8> drained{};
  REQUIRE(engine.drain_external_midi(drained.data(), drained.size()) == 0);
}

TEST_CASE("Fixed MIDI output queues reject borrowed SysEx views", "[engine][midi][sysex]") {
  sonare::host::FixedMidiOutputSink<4> merged_output;
  sonare::host::FixedExternalMidiOutputQueue<4> external_output;
  const std::array<uint8_t, 4> stack_payload{0xF0, 0x7D, 0x63, 0xF7};
  const TestPreparedMidiSysEx prepared(63, 1);

  MidiEvent borrowed{};
  borrowed.ump = sonare::midi::make_sysex_handle(0, 1);
  borrowed.sysex_payload = stack_payload.data();
  borrowed.sysex_payload_size = stack_payload.size();
  borrowed.prepared_sysex = &prepared;
  REQUIRE_FALSE(merged_output.send(borrowed));
  REQUIRE_FALSE(external_output.send(9, borrowed));
  REQUIRE(merged_output.queued_count() == 0);
  REQUIRE(external_output.pending_count() == 0);

  MidiEvent token_only = borrowed;
  token_only.sysex_payload = nullptr;
  token_only.sysex_payload_size = 0;
  REQUIRE_FALSE(merged_output.send(token_only));
  REQUIRE_FALSE(external_output.send(9, token_only));

  MidiEvent size_only = borrowed;
  size_only.sysex_payload = nullptr;
  size_only.prepared_sysex = nullptr;
  REQUIRE_FALSE(merged_output.send(size_only));
  REQUIRE_FALSE(external_output.send(9, size_only));

  const MidiEvent normal{12, sonare::midi::make_midi1_note_on(0, 0, 60, 100)};
  REQUIRE(merged_output.send(normal));
  REQUIRE(external_output.send(9, normal));
  REQUIRE(merged_output.queued_count() == 1);
  REQUIRE(external_output.pending_count() == 1);
}

TEST_CASE("RealtimeEngine reports the live SysEx rejection reason", "[engine][midi][sysex]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64, /*command_capacity=*/4096);
  const std::array<uint8_t, 4> payload{0xF0, 0x7D, 0x62, 0xF7};

  REQUIRE_FALSE(engine.push_midi_sysex(0, nullptr, payload.size(), -1));
  REQUIRE(engine.last_midi_sysex_push_status() ==
          sonare::engine::MidiSysExPushStatus::kInvalidPayload);

  PreparedTokenInstrument rejected(62, true);
  REQUIRE(engine.set_midi_instrument(0, &rejected));
  REQUIRE_FALSE(engine.push_midi_sysex(0, payload.data(), payload.size(), -1));
  REQUIRE(engine.last_midi_sysex_push_status() ==
          sonare::engine::MidiSysExPushStatus::kPreparationFailed);

  PreparedTokenInstrument out_of_memory(63, false, true);
  REQUIRE(engine.set_midi_instrument(0, &out_of_memory));
  REQUIRE_FALSE(engine.push_midi_sysex(0, payload.data(), payload.size(), -1));
  REQUIRE(engine.last_midi_sysex_push_status() ==
          sonare::engine::MidiSysExPushStatus::kOutOfMemory);
}

TEST_CASE("RealtimeEngine preserves dense scheduled SysEx sub-block boundaries",
          "[engine][midi][boundary]") {
  constexpr int kFrames = 4096;
  constexpr int kEvents = 60;
  RealtimeEngine engine;
  engine.prepare(48000.0, kFrames);

  BoundaryTimingInstrument instrument;
  REQUIRE(engine.set_midi_instrument(0, &instrument));

  // Any automation lane activates the control-period boundary path, whose 64
  // cadence points share the block's boundary storage with every MIDI event.
  sonare::automation::AutomationLane lane(0);
  lane.set_points({{0.0, 0.0f, sonare::automation::CurveType::Linear},
                   {1.0, 1.0f, sonare::automation::CurveType::Linear}});
  engine.automation().set_lanes({lane});

  const std::vector<uint8_t> payload{0xF0, 0x7D, 0x55, 0xF7};
  sonare::midi::MidiClipSchedule clip;
  clip.destination_id = 0;
  clip.length_samples = kFrames;
  for (int i = 1; i <= kEvents; ++i) {
    MidiEvent event;
    event.render_frame = static_cast<int64_t>(i * 64);
    event.ump = sonare::midi::make_sysex_handle(0, static_cast<uint32_t>(i));
    event.sysex_payload = payload.data();
    event.sysex_payload_size = payload.size();
    clip.events.push_back(event);
  }
  engine.set_midi_clips({clip});

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::array<float, kFrames> audio{};
  float* io[] = {audio.data()};
  engine.process(io, 1, kFrames);

  REQUIRE(instrument.count_ == kEvents);
  for (size_t i = 0; i < instrument.count_; ++i) {
    REQUIRE(instrument.observations_[i].callback_frame == instrument.observations_[i].event_frame);
  }
}

TEST_CASE("RealtimeEngine keeps prepared SysEx tokens alive across 64 out-of-order slots",
          "[engine][midi][sysex]") {
  constexpr int kFrames = 4096;
  constexpr int kMessages = 64;
  RealtimeEngine engine;
  engine.prepare(48000.0, kFrames);
  PreparedTokenInstrument instrument(17);
  REQUIRE(engine.set_midi_instrument(0, &instrument));

  for (int i = 0; i < kMessages; ++i) {
    const std::array<uint8_t, 4> payload{0xF0, 0x7D, static_cast<uint8_t>(i), 0xF7};
    // Reverse enqueue order. The command queue must still dispatch by the
    // sample timestamp while each slot retains its own prepared domain.
    REQUIRE(engine.push_midi_sysex(0, payload.data(), payload.size(), (kMessages - 1 - i) * 64));
  }

  std::array<float, kFrames> audio{};
  float* io[] = {audio.data()};
  engine.process(io, 1, kFrames);

  REQUIRE(instrument.control_calls_ == 0);
  REQUIRE(instrument.observed_count_ == kMessages);
  for (size_t i = 0; i < instrument.observed_count_; ++i) {
    REQUIRE(instrument.observed_[i].domain == 17);
    if (i > 0) {
      REQUIRE(instrument.observed_[i - 1].render_frame < instrument.observed_[i].render_frame);
    }
  }
  // AUDIO release only hands the generations back; token destruction waits for
  // CONTROL reuse, so all 64 event-time domains remain alive after the block.
  REQUIRE(instrument.prepared_.size() == kMessages);
  for (const auto& token : instrument.prepared_) REQUIRE_FALSE(token.expired());

  // Reuse is the CONTROL-side reclamation point. The next push may rebuild a
  // fresh operation, but consumed slots must release every old strong owner
  // before that preparation starts.
  const std::vector<uint8_t> reused{0xF0, 0x7D, 0x5A, 0xF7};
  REQUIRE(engine.push_midi_sysex(0, reused.data(), reused.size(), -1));
  REQUIRE(instrument.prepared_.size() == kMessages + 1);
  for (size_t i = 0; i < kMessages; ++i) REQUIRE(instrument.prepared_[i].expired());
  REQUIRE_FALSE(instrument.prepared_.back().expired());
}

TEST_CASE("RealtimeEngine replays accepted legacy SysEx in push order on rebind",
          "[engine][midi][sysex]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 256);
  PreparedTokenInstrument old_instrument(1, false, false, true);
  PreparedTokenInstrument new_instrument(2, false, false, true);
  REQUIRE(engine.set_midi_instrument(0, &old_instrument));

  const std::vector<uint8_t> first{0xF0, 0x7D, 0x31, 0xF7};
  const std::vector<uint8_t> second{0xF0, 0x7D, 0x32, 0xF7};
  REQUIRE(engine.push_midi_sysex(0, first.data(), first.size(), 128));
  REQUIRE(engine.push_midi_sysex(0, second.data(), second.size(), 64));
  REQUIRE(old_instrument.control_markers_ == std::vector<uint8_t>{0x31, 0x32});

  REQUIRE(engine.set_midi_instrument(0, &new_instrument));
  REQUIRE(new_instrument.control_markers_ == std::vector<uint8_t>{0x31, 0x32});
}

TEST_CASE("RealtimeEngine preserves the latest control clip snapshot on rebind",
          "[engine][midi][sysex]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  PreparedTokenInstrument old_instrument(1);
  PreparedTokenInstrument new_instrument(2);
  REQUIRE(engine.set_midi_instrument(0, &old_instrument));

  const std::array<uint8_t, 4> payload{0xF0, 0x7D, 0x70, 0xF7};
  const auto make_schedule = [&](uint32_t id) {
    MidiEvent event;
    event.render_frame = 0;
    event.ump = sonare::midi::make_sysex_handle(0, id + 1);
    event.sysex_payload = payload.data();
    event.sysex_payload_size = payload.size();
    sonare::midi::MidiClipSchedule clip;
    clip.id = id;
    clip.destination_id = 0;
    clip.events = {event};
    return std::vector<sonare::midi::MidiClipSchedule>{clip};
  };

  engine.set_midi_clips(make_schedule(0));
  engine.midi_sequencer().acquire_midi_clips();
  for (uint32_t id = 1; id <= 70; ++id) engine.set_midi_clips(make_schedule(id));
  // The audio view may still be part-way through its retire ring. Rebinding
  // must stage from the latest accepted CONTROL snapshot instead.
  engine.midi_sequencer().acquire_midi_clips();

  REQUIRE(engine.set_midi_instrument(0, &new_instrument));
  const auto* clips = engine.midi_sequencer().current_clips();
  REQUIRE(clips != nullptr);
  REQUIRE(clips->size() == 1);
  REQUIRE(clips->front().id == 70);
  REQUIRE(clips->front().events.size() == 1);
  const auto* prepared =
      static_cast<const TestPreparedMidiSysEx*>(clips->front().events.front().prepared_sysex);
  REQUIRE(prepared != nullptr);
  REQUIRE(prepared->domain == 2);
}

TEST_CASE("RealtimeEngine adopts a pending rebind after an exactly full clip ring",
          "[engine][midi][sysex]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  PreparedTokenInstrument old_instrument(1);
  PreparedTokenInstrument new_instrument(2);
  REQUIRE(engine.set_midi_instrument(0, &old_instrument));

  const std::array<uint8_t, 4> payload{0xF0, 0x7D, 0x71, 0xF7};
  const auto make_schedule = [&](uint32_t id) {
    MidiEvent event;
    event.render_frame = 0;
    event.ump = sonare::midi::make_sysex_handle(0, id + 1);
    event.sysex_payload = payload.data();
    event.sysex_payload_size = payload.size();
    sonare::midi::MidiClipSchedule clip;
    clip.id = id;
    clip.destination_id = 0;
    clip.events = {event};
    return std::vector<sonare::midi::MidiClipSchedule>{clip};
  };

  engine.set_midi_clips(make_schedule(0));
  engine.midi_sequencer().acquire_midi_clips();
  // Leave the exactly full hand-off ring unacquired. The rebind publication
  // then occupies the pending slot, which a single ordinary acquire cannot
  // adopt after it fills the retire ring with all 64 older snapshots.
  for (uint32_t id = 1; id <= sonare::rt::RtPublisher<int>::kCapacity; ++id) {
    engine.set_midi_clips(make_schedule(id));
  }

  REQUIRE(engine.set_midi_instrument(0, &new_instrument));
  const auto* clips = engine.midi_sequencer().current_clips();
  REQUIRE(clips != nullptr);
  REQUIRE(clips->size() == 1);
  REQUIRE(clips->front().id == sonare::rt::RtPublisher<int>::kCapacity);
  const auto* prepared =
      static_cast<const TestPreparedMidiSysEx*>(clips->front().events.front().prepared_sysex);
  REQUIRE(prepared != nullptr);
  REQUIRE(prepared->domain == 2);
}

TEST_CASE("RealtimeEngine rejects binding one instrument to two destinations", "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  PreparedTokenInstrument shared(7);
  PreparedTokenInstrument distinct(8);
  REQUIRE(engine.set_midi_instrument(1, &shared));
  const int prepare_calls = shared.prepare_calls_;

  REQUIRE_FALSE(engine.set_midi_instrument(2, &shared));
  REQUIRE(engine.midi_instrument(1) == &shared);
  REQUIRE(engine.midi_instrument(2) == nullptr);
  REQUIRE(engine.midi_instrument_count() == 1);
  REQUIRE(shared.prepare_calls_ == prepare_calls);

  REQUIRE(engine.set_midi_instrument(2, &distinct));
  REQUIRE(engine.midi_instrument(1) == &shared);
  REQUIRE(engine.midi_instrument(2) == &distinct);
  REQUIRE(engine.midi_instrument_count() == 2);
}

TEST_CASE("RealtimeEngine rebinds clip and queued SysEx domains transactionally",
          "[engine][midi][sysex]") {
  constexpr int kFrames = 256;
  const std::vector<uint8_t> payload{0xF0, 0x7D, 0x21, 0xF7};
  RealtimeEngine engine;
  engine.prepare(48000.0, kFrames);
  PreparedTokenInstrument old_instrument(11);
  PreparedTokenInstrument new_instrument(22);
  REQUIRE(engine.set_midi_instrument(0, &old_instrument));

  sonare::midi::MidiClipSchedule clip;
  clip.destination_id = 0;
  clip.length_samples = kFrames;
  MidiEvent scheduled;
  scheduled.render_frame = 64;
  scheduled.ump = sonare::midi::make_sysex_handle(0, 9);
  scheduled.sysex_payload = payload.data();
  scheduled.sysex_payload_size = payload.size();
  clip.events = {scheduled};
  engine.set_midi_clips({clip});
  REQUIRE(engine.push_midi_sysex(0, payload.data(), payload.size(), 64));

  REQUIRE(engine.set_midi_instrument(0, &new_instrument));
  REQUIRE(engine.midi_instrument(0) == &new_instrument);
  push_play(engine);

  std::array<float, kFrames> audio{};
  float* io[] = {audio.data()};
  engine.process(io, 1, kFrames);

  REQUIRE(old_instrument.observed_count_ == 0);
  REQUIRE(new_instrument.observed_count_ == 2);
  for (size_t i = 0; i < new_instrument.observed_count_; ++i) {
    REQUIRE(new_instrument.observed_[i].domain == 22);
  }
}

TEST_CASE("RealtimeEngine rejects a failed SysEx rebind without replacing its domain",
          "[engine][midi][sysex]") {
  constexpr int kFrames = 256;
  const std::vector<uint8_t> payload{0xF0, 0x7D, 0x31, 0xF7};
  RealtimeEngine engine;
  engine.prepare(48000.0, kFrames);
  PreparedTokenInstrument old_instrument(31);
  PreparedTokenInstrument failing_instrument(42, true);
  REQUIRE(engine.set_midi_instrument(0, &old_instrument));

  sonare::midi::MidiClipSchedule clip;
  clip.destination_id = 0;
  clip.length_samples = kFrames;
  MidiEvent scheduled;
  scheduled.render_frame = 64;
  scheduled.ump = sonare::midi::make_sysex_handle(0, 10);
  scheduled.sysex_payload = payload.data();
  scheduled.sysex_payload_size = payload.size();
  clip.events = {scheduled};
  engine.set_midi_clips({clip});

  REQUIRE_FALSE(engine.set_midi_instrument(0, &failing_instrument));
  REQUIRE(engine.midi_instrument(0) == &old_instrument);
  push_play(engine);

  std::array<float, kFrames> audio{};
  float* io[] = {audio.data()};
  engine.process(io, 1, kFrames);

  REQUIRE(old_instrument.observed_count_ == 1);
  REQUIRE(old_instrument.observed_[0].domain == 31);
  REQUIRE(failing_instrument.observed_count_ == 0);
}

TEST_CASE("RealtimeEngine hands live SysEx slots back after audio consumption", "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64, /*command_capacity=*/4096);
  SysExRecordingInstrument target;
  REQUIRE(engine.set_midi_instrument(0, &target));

  constexpr size_t kSlotCount = 64;
  std::array<std::vector<uint8_t>, kSlotCount> expected;
  for (size_t i = 0; i < expected.size(); ++i) {
    expected[i] = {0xF0, 0x7E, 0x7F, 0x09, static_cast<uint8_t>(i), 0xF7};
    REQUIRE(engine.push_midi_sysex(0, expected[i].data(), expected[i].size(), -1));
  }

  // A slot remains owned by its queued command until the audio thread consumes
  // that command. The next push must fail without overwriting the first frame.
  const std::vector<uint8_t> rejected = {0xF0, 0x7E, 0x7F, 0x09, 0x40, 0xF7};
  REQUIRE_FALSE(engine.push_midi_sysex(0, rejected.data(), rejected.size(), -1));

  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  float* channels[] = {left.data(), right.data()};
  for (int block = 0; block < 2; ++block) engine.process(channels, 2, 64);

  REQUIRE(target.payloads_.size() == expected.size());
  REQUIRE(target.payloads_ == std::vector<std::vector<uint8_t>>(expected.begin(), expected.end()));

  // Once consumed, the first slot can be reused by a subsequent push.
  const std::vector<uint8_t> reused = {0xF0, 0x7E, 0x7F, 0x09, 0x41, 0xF7};
  REQUIRE(engine.push_midi_sysex(0, reused.data(), reused.size(), -1));
  engine.process(channels, 2, 64);
  REQUIRE(target.payloads_.size() == expected.size() + 1);
  REQUIRE(target.payloads_.back() == reused);
}

TEST_CASE("RealtimeEngine skips a future SysEx slot when finding free storage", "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64, /*command_capacity=*/4096);
  SysExRecordingInstrument target;
  REQUIRE(engine.set_midi_instrument(0, &target));

  const auto make_payload = [](uint8_t marker) {
    return std::vector<uint8_t>{0xF0, 0x7E, 0x7F, 0x09, marker, 0xF7};
  };
  const std::vector<uint8_t> future = make_payload(0x70);
  REQUIRE(engine.push_midi_sysex(0, future.data(), future.size(), /*render_frame=*/128));

  std::array<std::vector<uint8_t>, 63> immediate;
  for (size_t i = 0; i < immediate.size(); ++i) {
    immediate[i] = make_payload(static_cast<uint8_t>(i));
    REQUIRE(engine.push_midi_sysex(0, immediate[i].data(), immediate[i].size(), -1));
  }

  // Slot zero is still occupied by the future command, while every other slot
  // is occupied by an immediate command. A 65th command has no free storage.
  const std::vector<uint8_t> rejected = make_payload(0x72);
  REQUIRE_FALSE(engine.push_midi_sysex(0, rejected.data(), rejected.size(), -1));

  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.process(channels, 2, 64);
  REQUIRE(target.payloads_.size() == immediate.size());
  for (size_t i = 0; i < immediate.size(); ++i) REQUIRE(target.payloads_[i] == immediate[i]);

  // The cursor is at the still-busy future slot, so finding the first released
  // slot requires a bounded scan rather than recycling slot zero.
  const std::vector<uint8_t> reused = make_payload(0x71);
  REQUIRE(engine.push_midi_sysex(0, reused.data(), reused.size(), -1));
  engine.process(channels, 2, 64);
  REQUIRE(target.payloads_.size() == immediate.size() + 1);
  REQUIRE(target.payloads_.back() == reused);

  // The future payload remains pending and arrives intact at its due frame.
  engine.process(channels, 2, 64);
  REQUIRE(target.payloads_.size() == immediate.size() + 2);
  REQUIRE(target.payloads_.back() == future);
}

#if defined(SONARE_MIDI_WITH_FX) && defined(SONARE_WITH_MASTERING)
TEST_CASE("RealtimeEngine applies live GS EFX to audible audio at its due frame",
          "[engine][midi]") {
  using sonare::midi::synth::Sf2Player;
  sonare::test::Sf2Builder builder;
  std::vector<float> tone(8192);
  for (size_t i = 0; i < tone.size(); ++i)
    tone[i] = static_cast<float>(std::sin(6.28318530717958647692 * i / 64.0));
  const int sample = builder.add_sample("tone", tone, 48000, 60, 0, tone.size());
  sonare::test::Sf2Builder::ZoneSpec zone;
  zone.target = sample;
  const int instrument = builder.add_instrument("tone", {zone});
  zone.target = instrument;
  builder.add_preset("tone", 0, 1, {zone});
  const auto bytes = builder.build();
  auto sf2 = std::make_shared<sonare::midi::synth::Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));

  sonare::midi::synth::Sf2PlayerConfig config;
  config.gain = 1.0f;
  config.bank_rig_binding = false;
  config.effects.enable_reverb = false;
  config.effects.enable_chorus = false;
  config.effects.enable_delay = false;
  config.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  Sf2Player player(config);
  Sf2Player dry(config);
  player.set_soundfont(sf2);
  dry.set_soundfont(sf2);
  RealtimeEngine engine;
  RealtimeEngine oracle;
  engine.prepare(48000.0, 256);
  oracle.prepare(48000.0, 256);
  REQUIRE(engine.set_midi_instrument(2, &player));
  REQUIRE(oracle.set_midi_instrument(2, &dry));
  for (Sf2Player* target : {&player, &dry}) {
    MidiEvent program{};
    program.ump = sonare::midi::make_midi1_program_change(0, 0, 1);
    target->on_event(0, program);
    MidiEvent note{};
    note.ump = sonare::midi::make_midi1_note_on(0, 0, 60, 127);
    target->on_event(0, note);
  }
  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  REQUIRE(oracle.push_command(play));
  const uint8_t part_on[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x01, 0x5C, 0xF7};
  const uint8_t od_type[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                             0x03, 0x00, 0x01, 0x10, 0x2C, 0xF7};
  REQUIRE(engine.push_midi_sysex(2, part_on, sizeof(part_on), 4096));
  REQUIRE(engine.push_midi_sysex(2, od_type, sizeof(od_type), 4096));

  std::array<float, 256> left{}, right{}, dry_left{}, dry_right{};
  float* channels[] = {left.data(), right.data()};
  float* dry_channels[] = {dry_left.data(), dry_right.data()};
  double early_energy = 0.0, early_error = 0.0;
  double due_energy = 0.0, due_error = 0.0;
  for (int block = 0; block <= 16; ++block) {
    engine.process(channels, 2, 256);
    oracle.process(dry_channels, 2, 256);
    for (size_t i = 0; i < left.size(); ++i) {
      const double energy = static_cast<double>(dry_left[i]) * dry_left[i] +
                            static_cast<double>(dry_right[i]) * dry_right[i];
      const double dl = static_cast<double>(left[i]) - dry_left[i];
      const double dr = static_cast<double>(right[i]) - dry_right[i];
      if (block < 16) {
        early_energy += energy;
        early_error += dl * dl + dr * dr;
      } else {
        due_energy += energy;
        due_error += dl * dl + dr * dr;
      }
    }
  }
  REQUIRE(early_energy > 1e-8);
  REQUIRE(std::sqrt(early_error / early_energy) < 1e-6);
  REQUIRE(due_energy > 1e-8);
  REQUIRE(std::sqrt(due_error / due_energy) > 1e-3);
  // The diagnostic getter is CONTROL-owned. AUDIO dispatch must change the
  // audible processor without mutating that cross-thread diagnostic mirror.
  REQUIRE_FALSE(player.gs_efx().assigned);
}
#endif

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
  size_t capacity = 0;
  while (engine.push_command(filler)) ++capacity;
  REQUIRE(capacity > 0);

  // A GS EFX-select SysEx now cannot enqueue its audio-thread command. It must
  // report queue pressure AND leave the control-side EFX mirror untouched:
  // realising it here would adopt the new effect chain while the queued channel
  // state never arrives -- a half-applied SysEx that diverges from a bounce.
  const uint8_t od_type[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                             0x03, 0x00, 0x01, 0x10, 0x2C, 0xF7};
  REQUIRE_FALSE(engine.push_midi_sysex(2, od_type, sizeof(od_type), /*render_frame=*/-1));
  REQUIRE(engine.last_midi_sysex_push_status() == sonare::engine::MidiSysExPushStatus::kQueueFull);
  REQUIRE_FALSE(player.gs_efx().assigned);

  // Draining the queue frees it, and the rejected push held no slot: the full
  // queue capacity is accepted again.
  std::array<float, 64> left{}, right{};
  float* channels[] = {left.data(), right.data()};
  engine.process(channels, 2, 64);
  const uint8_t gm_on[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
  for (size_t i = 0; i < capacity; ++i) {
    REQUIRE(engine.push_midi_sysex(2, gm_on, sizeof(gm_on), /*render_frame=*/-1));
  }

  engine.process(channels, 2, 64);
  engine.set_midi_instrument(2, nullptr);
}

TEST_CASE("a SysEx push refused for queue pressure hands its payload slot back",
          "[engine][midi][sysex]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64, /*command_capacity=*/4, /*telemetry_capacity=*/4);
  SysExRecordingInstrument target;
  REQUIRE(engine.set_midi_instrument(0, &target));

  sonare::rt::Command filler{};
  filler.type = sonare::rt::CommandType::kTransportPlay;
  filler.sample_time = -1;
  while (engine.push_command(filler)) {
  }

  // More refusals than there are payload slots: a leaked slot would turn the
  // later ones into kPayloadSlotsFull instead of kQueueFull.
  const std::vector<uint8_t> payload{0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
  for (int i = 0; i < 2 * 64 + 1; ++i) {
    REQUIRE_FALSE(engine.push_midi_sysex(0, payload.data(), payload.size(), -1));
    REQUIRE(engine.last_midi_sysex_push_status() ==
            sonare::engine::MidiSysExPushStatus::kQueueFull);
  }

  std::array<float, 64> left{}, right{};
  float* channels[] = {left.data(), right.data()};
  engine.process(channels, 2, 64);
  REQUIRE(engine.push_midi_sysex(0, payload.data(), payload.size(), -1));
  REQUIRE(engine.last_midi_sysex_push_status() == sonare::engine::MidiSysExPushStatus::kAccepted);
  engine.process(channels, 2, 64);
  REQUIRE(target.payloads_.size() == 1);
  REQUIRE(target.payloads_.front() == payload);
}

TEST_CASE("prepare re-prepares the newest clip snapshot after more than 64 publishes",
          "[engine][midi][sysex]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  PreparedTokenInstrument instrument(5);
  REQUIRE(engine.set_midi_instrument(0, &instrument));

  const std::array<uint8_t, 4> payload{0xF0, 0x7D, 0x72, 0xF7};
  const auto make_schedule = [&](uint32_t id) {
    MidiEvent event;
    event.render_frame = 0;
    event.ump = sonare::midi::make_sysex_handle(0, id + 1);
    event.sysex_payload = payload.data();
    event.sysex_payload_size = payload.size();
    sonare::midi::MidiClipSchedule clip;
    clip.id = id;
    clip.destination_id = 0;
    clip.events = {event};
    return std::vector<sonare::midi::MidiClipSchedule>{clip};
  };

  // Overrun the hand-off ring without an audio acquire in between.
  constexpr auto kPublishes = static_cast<uint32_t>(sonare::rt::RtPublisher<int>::kCapacity + 6);
  for (uint32_t id = 1; id <= kPublishes; ++id) engine.set_midi_clips(make_schedule(id));

  engine.prepare(48000.0, 64);
  const auto* clips = engine.midi_sequencer().current_clips();
  REQUIRE(clips != nullptr);
  REQUIRE(clips->size() == 1);
  REQUIRE(clips->front().id == kPublishes);
  const auto* prepared = clips->front().events.front().prepared_sysex;
  REQUIRE(prepared != nullptr);
  // The surviving token is the one prepare() built after re-preparing the instrument.
  REQUIRE_FALSE(instrument.prepared_.empty());
  REQUIRE(prepared == instrument.prepared_.back().lock().get());
}

TEST_CASE("prepare reports a scheduled SysEx the bound instrument refuses",
          "[engine][midi][sysex]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  PreparedTokenInstrument instrument(6);
  REQUIRE(engine.set_midi_instrument(0, &instrument));

  const std::array<uint8_t, 4> payload{0xF0, 0x7D, 0x73, 0xF7};
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 0;
  MidiEvent event;
  event.render_frame = 0;
  event.ump = sonare::midi::make_sysex_handle(0, 1);
  event.sysex_payload = payload.data();
  event.sysex_payload_size = payload.size();
  clip.events = {event};
  engine.set_midi_clips({clip});

  // The re-prepared instrument now refuses: prepare commits, clears the clips
  // (all or none) and reports the refusal instead of returning normally.
  instrument.fail_ = true;
  const int prepare_calls = instrument.prepare_calls_;
  bool refused = false;
  try {
    engine.prepare(48000.0, 64);
  } catch (const sonare::SonareException& error) {
    refused = error.code() == sonare::ErrorCode::InvalidParameter;
  }
  REQUIRE(refused);
  REQUIRE(instrument.prepare_calls_ == prepare_calls + 1);
  REQUIRE(engine.midi_instrument(0) == &instrument);
  const auto* clips = engine.midi_sequencer().current_clips();
  REQUIRE((clips == nullptr || clips->empty()));

  // The engine stayed prepared: a block renders through the bound instrument.
  const int process_calls = instrument.process_calls_;
  push_play(engine);
  std::array<float, 64> audio{};
  float* io[] = {audio.data()};
  engine.process(io, 1, 64);
  REQUIRE(instrument.process_calls_ > process_calls);
}

TEST_CASE("RealtimeEngine reports why an instrument bind was refused", "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  PreparedTokenInstrument shared(1);
  REQUIRE(engine.set_midi_instrument(1, &shared));
  REQUIRE(engine.last_midi_instrument_bind_status() ==
          sonare::engine::MidiInstrumentBindStatus::kBound);
  REQUIRE_FALSE(engine.set_midi_instrument(2, &shared));
  REQUIRE(engine.last_midi_instrument_bind_status() ==
          sonare::engine::MidiInstrumentBindStatus::kAlreadyBoundElsewhere);

  const std::array<uint8_t, 4> payload{0xF0, 0x7D, 0x74, 0xF7};
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 3;
  MidiEvent event;
  event.ump = sonare::midi::make_sysex_handle(0, 1);
  event.sysex_payload = payload.data();
  event.sysex_payload_size = payload.size();
  clip.events = {event};
  engine.set_midi_clips({clip});

  PreparedTokenInstrument refusing(2, /*fail=*/true);
  REQUIRE_FALSE(engine.set_midi_instrument(3, &refusing));
  REQUIRE(engine.last_midi_instrument_bind_status() ==
          sonare::engine::MidiInstrumentBindStatus::kPreparationFailed);
  PreparedTokenInstrument out_of_memory(3, false, /*throw_bad_alloc=*/true);
  REQUIRE_FALSE(engine.set_midi_instrument(3, &out_of_memory));
  REQUIRE(engine.last_midi_instrument_bind_status() ==
          sonare::engine::MidiInstrumentBindStatus::kOutOfMemory);
  REQUIRE(engine.midi_instrument(3) == nullptr);
}

TEST_CASE("live command events reach an instrument after their sub-block's transport",
          "[engine][midi][sysex]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 256);
  BoundaryTimingInstrument instrument;
  REQUIRE(engine.set_midi_instrument(0, &instrument));

  const std::vector<uint8_t> payload{0xF0, 0x7D, 0x56, 0xF7};
  REQUIRE(engine.push_midi_sysex(0, payload.data(), payload.size(), /*render_frame=*/128));
  std::array<float, 256> audio{};
  float* io[] = {audio.data()};
  engine.process(io, 1, 256);

  REQUIRE(instrument.count_ == 1);
  REQUIRE(instrument.observations_[0].event_frame == 128);
  REQUIRE(instrument.observations_[0].callback_frame == 128);
}

TEST_CASE("RealtimeEngine forwards a handle-carrying clip SysEx to the merged output sink",
          "[engine][midi][sysex]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  PreparedTokenInstrument internal(64);
  REQUIRE(engine.set_midi_instrument(0, &internal));
  sonare::host::FixedMidiOutputSink<8> merged_output;
  engine.set_midi_output_sink(&merged_output);

  const std::array<uint8_t, 4> payload{0xF0, 0x7D, 0x64, 0xF7};
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 0;
  MidiEvent event;
  event.render_frame = 16;
  event.ump = sonare::midi::make_sysex_handle(0, 42);
  event.sysex_payload = payload.data();
  event.sysex_payload_size = payload.size();
  clip.events = {event};
  engine.set_midi_clips({clip});
  push_play(engine);

  std::array<float, 64> audio{};
  float* io[] = {audio.data()};
  engine.process(io, 1, 64);

  // The rack consumes the borrowed token; the queue keeps only the handle UMP.
  REQUIRE(internal.observed_count_ == 1);
  REQUIRE(merged_output.queued_count() == 1);
  REQUIRE(merged_output.dropped_count() == 0);
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

  // The old external route retains the channel's controller state even though
  // the clip's note-off already removed its active note. Route adoption sends
  // the standard destination reset through that old route before committing
  // the internal route.
  const size_t reset_count = engine.drain_external_midi(drained.data(), drained.size());
  REQUIRE(reset_count == 4);
  const std::array<uint8_t, 4> expected_status = {0xB, 0xB, 0xB, 0xE};
  const std::array<uint8_t, 3> expected_controllers = {64, 121, 123};
  for (size_t i = 0; i < reset_count; ++i) {
    const uint32_t word = drained[i].event.ump.words[0];
    REQUIRE(drained[i].destination_id == 5);
    REQUIRE(((word >> 28) & 0x0Fu) == 0x2u);  // MIDI 1.0 channel voice
    REQUIRE(((word >> 24) & 0x0Fu) == 0u);    // group 0 on the wire
    REQUIRE(((word >> 16) & 0x0Fu) == 1u);    // channel 1 on the wire
    REQUIRE(((word >> 20) & 0x0Fu) == expected_status[i]);
    REQUIRE_FALSE(drained[i].event.ump.is_note_on());
    REQUIRE_FALSE(drained[i].event.ump.is_note_off());
    if (i < expected_controllers.size()) {
      REQUIRE(((word >> 8) & 0x7Fu) == expected_controllers[i]);
      REQUIRE((word & 0x7Fu) == 0u);
    } else {
      REQUIRE((((word & 0x7Fu) << 7u) | ((word >> 8u) & 0x7Fu)) == 8192u);
    }
  }
}

TEST_CASE("RealtimeEngine retries an external note-off a full output queue refused",
          "[engine][midi]") {
  constexpr uint32_t kDestination = 5;
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  engine.set_midi_destination_external(kDestination, true);

  // Controllers at frame 1 fill the external queue before the note-off at 2 arrives.
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.start_sample = 0;
  clip.length_samples = 4096;
  clip.destination_id = kDestination;
  clip.events.push_back({0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)});
  for (int i = 0; i < 1100; ++i) {
    clip.events.push_back({1, sonare::midi::make_midi1_control_change(0, 0, 1, i & 0x7F)});
  }
  clip.events.push_back({2, sonare::midi::make_midi1_note_off(0, 0, 60, 0)});
  engine.set_midi_clips({clip});
  push_play(engine);

  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.process(channels, 2, 64);

  std::vector<sonare::host::ExternalMidiRecord> drained(2048);
  size_t first = engine.drain_external_midi(drained.data(), drained.size());
  const auto count_note_offs = [&](size_t n) {
    return std::count_if(
        drained.begin(), drained.begin() + static_cast<std::ptrdiff_t>(n),
        [](const sonare::host::ExternalMidiRecord& r) { return r.event.ump.is_note_off(); });
  };
  REQUIRE(count_note_offs(first) == 0);

  engine.process(channels, 2, 64);
  const size_t second = engine.drain_external_midi(drained.data(), drained.size());
  REQUIRE(count_note_offs(second) == 1);
  // Delivered once: a later block does not repeat it.
  engine.process(channels, 2, 64);
  const size_t third = engine.drain_external_midi(drained.data(), drained.size());
  REQUIRE(count_note_offs(third) == 0);
}

TEST_CASE("a block refused for its channel count still dispatches its note-off", "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64, 16, 16, 2);
  CountingInstrument inst;
  engine.set_midi_instrument(&inst);

  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.start_sample = 0;
  // The clip outlasts the test, so only the in-span note-off can release the note.
  clip.length_samples = 4096;
  clip.destination_id = 0;
  clip.events = {{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                 {96, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  engine.set_midi_clips({clip});
  push_play(engine);

  std::vector<float> a(64, 0.0f), b(64, 0.0f), c(64, 0.0f);
  float* two[] = {a.data(), b.data()};
  float* three[] = {a.data(), b.data(), c.data()};
  engine.process(two, 2, 64);
  REQUIRE(engine.midi_sequencer().active_note_count() == 1);
  engine.process(three, 3, 64);
  engine.process(two, 2, 64);

  REQUIRE(inst.note_off_count_ == 1);
  REQUIRE(engine.midi_sequencer().active_note_count() == 0);
  engine.set_midi_instrument(nullptr);
}

TEST_CASE("RealtimeEngine releases an internal note through the old route on an external flip",
          "[engine][midi]") {
  constexpr uint32_t kDestination = 5;
  constexpr int kBlock = 64;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  CountingInstrument internal;
  REQUIRE(engine.set_midi_instrument(kDestination, &internal));

  auto clips = note_on_at_zero();
  clips.front().destination_id = kDestination;
  engine.set_midi_clips(std::move(clips));
  push_play(engine);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  engine.process(io, 2, kBlock);
  REQUIRE(internal.note_on_count_ == 1);
  REQUIRE(internal.note_off_count_ == 0);
  REQUIRE(engine.midi_sequencer().active_note_count() == 1);

  // The control call is adopted at the next audio block. The note-off must be
  // sent through the internal route that created the note before the new
  // external route becomes active.
  REQUIRE(engine.set_midi_destination_external(kDestination, true));
  std::fill(left.begin(), left.end(), 0.0f);
  std::fill(right.begin(), right.end(), 0.0f);
  engine.process(io, 2, kBlock);
  REQUIRE(internal.note_off_count_ == 1);
  REQUIRE(engine.midi_sequencer().active_note_count() == 0);

  std::array<sonare::host::ExternalMidiRecord, 8> drained{};
  REQUIRE(engine.drain_external_midi(drained.data(), drained.size()) == 0);

  // The adopted route is usable immediately after the boundary release.
  const auto note_on = sonare::midi::make_midi1_note_on(0, 0, 67, 100);
  REQUIRE(engine.push_midi_ump(kDestination, note_on.words, note_on.word_count, -1) ==
          sonare::engine::MidiUmpPushResult::kQueued);
  std::fill(left.begin(), left.end(), 0.0f);
  std::fill(right.begin(), right.end(), 0.0f);
  engine.process(io, 2, kBlock);
  const size_t count = engine.drain_external_midi(drained.data(), drained.size());
  REQUIRE(count == 1);
  REQUIRE(drained[0].destination_id == kDestination);
  REQUIRE(drained[0].event.ump.is_note_on());
  REQUIRE(drained[0].event.ump.note_number() == 67);
  REQUIRE(internal.note_on_count_ == 1);
}

TEST_CASE("RealtimeEngine releases an external note through the old route on an internal flip",
          "[engine][midi]") {
  constexpr uint32_t kDestination = 5;
  constexpr int kBlock = 64;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  CountingInstrument internal;
  REQUIRE(engine.set_midi_instrument(kDestination, &internal));
  REQUIRE(engine.set_midi_destination_external(kDestination, true));

  auto clips = note_on_at_zero();
  clips.front().destination_id = kDestination;
  engine.set_midi_clips(std::move(clips));
  push_play(engine);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  engine.process(io, 2, kBlock);
  REQUIRE(internal.received_events_ == 0);
  std::array<sonare::host::ExternalMidiRecord, 8> drained{};
  REQUIRE(engine.drain_external_midi(drained.data(), drained.size()) == 1);
  REQUIRE(drained[0].event.ump.is_note_on());
  REQUIRE(engine.midi_sequencer().active_note_count() == 1);

  // A route flip must return the note-off and controller reset messages to the
  // external device before the destination is allowed back into the rack.
  REQUIRE(engine.set_midi_destination_external(kDestination, false));
  std::fill(left.begin(), left.end(), 0.0f);
  std::fill(right.begin(), right.end(), 0.0f);
  engine.process(io, 2, kBlock);
  REQUIRE(internal.note_off_count_ == 0);
  REQUIRE(engine.midi_sequencer().active_note_count() == 0);
  const size_t released = engine.drain_external_midi(drained.data(), drained.size());
  bool saw_note_off = false;
  for (size_t i = 0; i < released; ++i) {
    saw_note_off = saw_note_off || drained[i].event.ump.is_note_off();
    REQUIRE(drained[i].destination_id == kDestination);
  }
  REQUIRE(saw_note_off);

  // Subsequent events use the newly adopted internal route.
  const auto note_on = sonare::midi::make_midi1_note_on(0, 0, 67, 100);
  REQUIRE(engine.push_midi_ump(kDestination, note_on.words, note_on.word_count, -1) ==
          sonare::engine::MidiUmpPushResult::kQueued);
  std::fill(left.begin(), left.end(), 0.0f);
  std::fill(right.begin(), right.end(), 0.0f);
  engine.process(io, 2, kBlock);
  REQUIRE(internal.note_on_count_ == 1);
  REQUIRE(engine.drain_external_midi(drained.data(), drained.size()) == 0);
}

TEST_CASE("RealtimeEngine route flips discard pending arpeggiator events", "[engine][midi]") {
  constexpr uint32_t kDestination = 5;
  constexpr int kBlock = 64;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  CountingInstrument internal;
  REQUIRE(engine.set_midi_instrument(kDestination, &internal));

  sonare::midi::MidiFxChain fx;
  sonare::midi::ArpeggiatorConfig arp;
  arp.enabled = true;
  arp.steps = 2;
  arp.intervals[0] = 0;
  arp.intervals[1] = 12;
  arp.step_frames = kBlock;
  arp.gate_frames = kBlock / 2;
  fx.set_arpeggiator(arp);
  REQUIRE(engine.set_midi_fx(kDestination, fx));

  auto clips = note_on_at_zero();
  clips.front().destination_id = kDestination;
  engine.set_midi_clips(std::move(clips));
  push_play(engine);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  engine.process(io, 2, kBlock);
  REQUIRE(internal.note_on_count_ == 1);
  REQUIRE(internal.note_off_count_ == 1);

  // The second arpeggiator step is pending at frame 64. Changing the route at
  // this boundary must clear that pending event along with the old destination
  // state, so it cannot be emitted through either route later.
  REQUIRE(engine.set_midi_destination_external(kDestination, true));
  std::fill(left.begin(), left.end(), 0.0f);
  std::fill(right.begin(), right.end(), 0.0f);
  engine.process(io, 2, kBlock);
  std::array<sonare::host::ExternalMidiRecord, 8> drained{};
  REQUIRE(engine.drain_external_midi(drained.data(), drained.size()) == 0);
  REQUIRE(internal.note_on_count_ == 1);
  REQUIRE(internal.note_off_count_ == 1);
}

TEST_CASE("RealtimeEngine coalesces a route flip before the next audio block", "[engine][midi]") {
  constexpr uint32_t kDestination = 5;
  constexpr int kBlock = 64;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  CountingInstrument internal;
  REQUIRE(engine.set_midi_instrument(kDestination, &internal));

  auto clips = note_on_at_zero();
  clips.front().destination_id = kDestination;
  engine.set_midi_clips(std::move(clips));
  push_play(engine);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  engine.process(io, 2, kBlock);
  REQUIRE(internal.note_on_count_ == 1);
  REQUIRE(engine.midi_sequencer().active_note_count() == 1);

  // The final requested route is the original internal route. No transient
  // external state was audible, so adopting this coalesced request must not
  // manufacture a note-off or clear the held note.
  REQUIRE(engine.set_midi_destination_external(kDestination, true));
  REQUIRE(engine.set_midi_destination_external(kDestination, false));
  std::fill(left.begin(), left.end(), 0.0f);
  std::fill(right.begin(), right.end(), 0.0f);
  engine.process(io, 2, kBlock);
  REQUIRE(internal.note_off_count_ == 0);
  REQUIRE(engine.midi_sequencer().active_note_count() == 1);
  std::array<sonare::host::ExternalMidiRecord, 8> drained{};
  REQUIRE(engine.drain_external_midi(drained.data(), drained.size()) == 0);
}

TEST_CASE("RealtimeEngine route cleanup resets a released channel through its old route",
          "[engine][midi]") {
  constexpr uint32_t kDestination = 5;
  constexpr int kBlock = 64;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  ControllerRecordingInstrument internal;
  REQUIRE(engine.set_midi_instrument(kDestination, &internal));

  const auto sustain_down = sonare::midi::make_midi1_control_change(0, 0, 64, 127);
  const auto note_on = sonare::midi::make_midi1_note_on(0, 0, 60, 100);
  const auto note_off = sonare::midi::make_midi1_note_off(0, 0, 60, 0);
  REQUIRE(engine.push_midi_ump(kDestination, sustain_down.words, sustain_down.word_count, -1) ==
          sonare::engine::MidiUmpPushResult::kQueued);
  REQUIRE(engine.push_midi_ump(kDestination, note_on.words, note_on.word_count, -1) ==
          sonare::engine::MidiUmpPushResult::kQueued);
  REQUIRE(engine.push_midi_ump(kDestination, note_off.words, note_off.word_count, -1) ==
          sonare::engine::MidiUmpPushResult::kQueued);

  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* io[] = {left.data(), right.data()};
  engine.process(io, 2, kBlock);
  REQUIRE_FALSE(internal.sustain_off_);
  REQUIRE(engine.midi_sequencer().active_note_count() == 0);

  // The key is already up, but the channel's damper state belongs to the old
  // internal route. The route boundary must still reset that channel before
  // the requested external table is committed.
  REQUIRE(engine.set_midi_destination_external(kDestination, true));
  std::fill(left.begin(), left.end(), 0.0f);
  std::fill(right.begin(), right.end(), 0.0f);
  engine.process(io, 2, kBlock);
  REQUIRE(internal.sustain_off_);
  REQUIRE(internal.reset_all_controllers_);
  REQUIRE(internal.all_notes_off_cc_);
  REQUIRE(internal.pitch_bend_seen_);
  std::array<sonare::host::ExternalMidiRecord, 8> drained{};
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
  // all-notes-off and recenter pitch bend on the held channel.
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

TEST_CASE("an instrument bind leaves every unchanged PDC bank's history alone",
          "[engine][midi][pdc]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 128;
  constexpr int kLatency = 64;
  constexpr int64_t kFrames = kBlock * 16;
  RealtimeEngine engine;
  engine.prepare(kSr, kBlock);

  // One latent instrument fixes the whole project's compensation: the clip bus
  // is delayed by kLatency to meet it, and that delay bank fills with audio.
  LatencyImpulseInstrument slow(kLatency);
  REQUIRE(engine.set_midi_instrument(1, &slow));
  engine.set_clips({constant_clip(kFrames, 0.5f)});
  push_play(engine);

  std::vector<float> out_l(static_cast<size_t>(kBlock), 0.0f);
  std::vector<float> out_r(static_cast<size_t>(kBlock), 0.0f);
  float* io[] = {out_l.data(), out_r.data()};
  const auto render_block = [&]() {
    std::fill(out_l.begin(), out_l.end(), 0.0f);
    std::fill(out_r.begin(), out_r.end(), 0.0f);
    engine.process(io, 2, kBlock);
  };
  const auto require_steady = [&](const char* what) {
    for (size_t i = 0; i < out_l.size(); ++i) {
      INFO(what << ", sample " << i);
      REQUIRE(out_l[i] == Catch::Approx(0.5f));
    }
  };
  for (int block = 0; block < 4; ++block) render_block();
  require_steady("warm-up");

  const uint64_t generation = engine.pdc_storage_generation();
  REQUIRE(generation > 0);

  // A second instrument reporting the SAME latency: the maximum does not move,
  // so no source's compensation changes and no bank may be rebuilt. Rebuilding
  // one zero-fills it, and the hole lands on every MIDI track and on the clip
  // bus -- not just on the destination the bind named.
  LatencyImpulseInstrument same(kLatency);
  REQUIRE(engine.set_midi_instrument(2, &same));
  REQUIRE(engine.pdc_storage_generation() == generation);
  render_block();
  require_steady("after bind");

  // Unbinding it again is the same non-event from the other direction, and it
  // repacks the surviving instrument's slot, which must move its bank rather
  // than rebuild it.
  REQUIRE(engine.set_midi_instrument(2, nullptr));
  REQUIRE(engine.pdc_storage_generation() == generation);
  render_block();
  require_steady("after unbind");

  // A bind that genuinely changes the compensation still rebuilds what it must:
  // the guard is a shape comparison, not a blanket refusal to reallocate.
  LatencyImpulseInstrument slower(kLatency * 2);
  REQUIRE(engine.set_midi_instrument(3, &slower));
  REQUIRE(engine.pdc_storage_generation() > generation);

  engine.set_midi_instrument(1, nullptr);
  engine.set_midi_instrument(3, nullptr);
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

TEST_CASE("a bind whose compensation cannot be allocated is refused, not left half-done",
          "[engine][midi][pdc]") {
  // Reallocating the PDC banks is the one part of a bind that can fail, and it
  // fails only on a heap failure -- the delay is an int, so no length_error is
  // reachable and a latency large enough to exhaust memory honestly would take
  // gigabytes. The failure is therefore injected, thresholded so that it lands
  // on the delay line and not on the test framework's own bookkeeping.
  //
  // What must hold is that the refusal is total. Answering false while leaving
  // the instrument in the rack would strand a raw pointer that the C-ABI and
  // WASM wrappers free the moment they see the false, so a partial bind is a
  // use-after-free rather than a missing feature.
  constexpr int kLatency = 300000;  // ~1.2 MB per lane at 4 bytes a sample
  constexpr std::size_t kFailFromBytes = 1u << 20;

  RealtimeEngine engine;
  engine.prepare(48000.0, 512);
  LatencyImpulseInstrument instrument(kLatency);

  bool bound = true;
  {
    sonare::test::AllocationFailureGuard fail_large_allocations(kFailFromBytes);
    bound = engine.set_midi_instrument(7, &instrument);
  }

  REQUIRE_FALSE(bound);
  REQUIRE(engine.midi_instrument_count() == 0);

  // The engine is still usable afterwards: the same bind succeeds once the
  // allocation can be served, which is what distinguishes falling back cleanly
  // from being wedged by the failure.
  REQUIRE(engine.set_midi_instrument(7, &instrument));
  REQUIRE(engine.midi_instrument_count() == 1);
  engine.set_midi_instrument(7, nullptr);
}

TEST_CASE("a failed rebind leaves the destination's previous instrument bound",
          "[engine][midi][pdc]") {
  // The refusal above had nothing to lose: the destination was empty. A swap
  // does -- reporting false after clearing the destination would silence an
  // instrument the host was just told is still its own to keep.
  constexpr int kBoundLatency = 1024;
  constexpr int kIncomingLatency = 300000;
  constexpr std::size_t kFailFromBytes = 1u << 20;

  RealtimeEngine engine;
  engine.prepare(48000.0, 512);
  LatencyImpulseInstrument bound_instrument(kBoundLatency);
  LatencyImpulseInstrument incoming(kIncomingLatency);

  REQUIRE(engine.set_midi_instrument(7, &bound_instrument));
  REQUIRE(engine.midi_instrument(7) == &bound_instrument);

  bool rebound = true;
  {
    sonare::test::AllocationFailureGuard fail_large_allocations(kFailFromBytes);
    rebound = engine.set_midi_instrument(7, &incoming);
  }
  REQUIRE_FALSE(rebound);

  // The pointer is what discriminates: the count reads 1 whichever of the two
  // the destination ended up driving, and the incoming one is about to be
  // freed by the caller that was handed the false.
  REQUIRE(engine.midi_instrument(7) == &bound_instrument);
  REQUIRE(engine.midi_instrument_count() == 1);

  // The engine is not wedged by the refusal: the same swap takes once the
  // allocation can be served, and then the destination really has moved.
  REQUIRE(engine.set_midi_instrument(7, &incoming));
  REQUIRE(engine.midi_instrument(7) == &incoming);
  engine.set_midi_instrument(7, nullptr);
}

TEST_CASE("a failed rebind leaves the destination's automation slots assigned",
          "[engine][midi][pdc][automation]") {
  // The other half of the same refusal: a swap retires the destination's
  // automation smoothers, which must not happen for a swap that did not take.
  constexpr int kBlock = 256;
  constexpr int kIncomingLatency = 300000;
  constexpr std::size_t kFailFromBytes = 1u << 20;
  constexpr uint32_t kDestination = 5;
  constexpr float kSettled = 0.5f;

  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  AutomatableInstrument bound_instrument;
  LatencyImpulseInstrument incoming(kIncomingLatency);

  REQUIRE(engine.set_midi_instrument(kDestination, &bound_instrument));
  const int64_t level_id = engine.resolve_instrument_automation_id(kDestination, "level");
  REQUIRE(level_id >= 0);
  push_play(engine);

  std::vector<float> left(static_cast<size_t>(kBlock), 0.0f);
  std::vector<float> right(static_cast<size_t>(kBlock), 0.0f);
  auto render_blocks = [&](int blocks) {
    for (int b = 0; b < blocks; ++b) {
      std::fill(left.begin(), left.end(), 0.0f);
      std::fill(right.begin(), right.end(), 0.0f);
      float* io[] = {left.data(), right.data()};
      engine.process(io, 2, kBlock);
    }
  };
  auto drive_level = [&](float value) {
    sonare::rt::Command set{};
    set.type = sonare::rt::CommandType::kSetParam;
    set.target_id = static_cast<uint32_t>(level_id);
    set.arg.f = value;
    set.sample_time = -1;
    REQUIRE(engine.push_command(set));
  };

  // Park the slot at a value. A slot claimed fresh snaps to its first target
  // within one block, which is also what a retired slot would read at the
  // probe below, so this is the reference that probe is read against.
  drive_level(kSettled);
  render_blocks(1);
  REQUIRE(bound_instrument.level == kSettled);

  bool rebound = true;
  {
    sonare::test::AllocationFailureGuard fail_large_allocations(kFailFromBytes);
    rebound = engine.set_midi_instrument(kDestination, &incoming);
  }
  REQUIRE_FALSE(rebound);
  REQUIRE(engine.midi_instrument(kDestination) == &bound_instrument);

  // The id proves nothing on its own: it is minted from the destination table,
  // which retiring a slot never touches, so it matches either way.
  REQUIRE(engine.resolve_instrument_automation_id(kDestination, "level") == level_id);

  // The two bounds rule out the two ways this can go wrong: a retired slot is
  // claimed fresh and snaps to the target, an unbound destination applies
  // nothing at all, and only a surviving smoother lands between.
  drive_level(0.0f);
  render_blocks(1);
  REQUIRE(bound_instrument.level > 0.0f);
  REQUIRE(bound_instrument.level < kSettled);

  // It is live rather than merely stuck part-way: the ramp still converges.
  render_blocks(16);
  REQUIRE(bound_instrument.level == Catch::Approx(0.0f).margin(1.0e-3));

  engine.set_midi_instrument(kDestination, nullptr);
}

namespace {

using sonare::engine::MidiUmpPushResult;

// Records every UMP it receives in a fixed array, so it performs no allocation
// on the audio thread and a test can compare delivered words exactly.
class UmpRecordingInstrument final : public MidiInstrument {
 public:
  static constexpr size_t kCapacity = 512;
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override { count_ = 0; }
  void on_event(uint32_t, const MidiEvent& event) noexcept override {
    if (count_ < kCapacity) received_[count_] = event.ump;
    ++count_;
  }
  size_t count_ = 0;
  std::array<sonare::midi::Ump, kCapacity> received_{};
};

void process_block(RealtimeEngine& engine) {
  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  float* io[] = {left.data(), right.data()};
  engine.process(io, 2, 64);
}

MidiUmpPushResult push_ump(RealtimeEngine& engine, uint32_t destination_id,
                           const sonare::midi::Ump& ump) {
  return engine.push_midi_ump(destination_id, ump.words, ump.word_count, -1);
}

// A 4-word message the live path neither rejects nor discards: MT 0xE is a
// reserved 128-bit type, forwarded as-is like any other unhandled type.
sonare::midi::Ump reserved_128_bit_ump() {
  sonare::midi::Ump ump{};
  ump.words[0] = 0xE3123456u;
  ump.words[1] = 0x89ABCDEFu;
  ump.words[2] = 0x01234567u;
  ump.words[3] = 0xFEDCBA98u;
  ump.word_count = 4;
  return ump;
}

}  // namespace

TEST_CASE("push_midi_ump delivers 2-word and 4-word UMPs to the destination", "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  UmpRecordingInstrument target;
  UmpRecordingInstrument other;
  REQUIRE(engine.set_midi_instrument(3, &target));
  REQUIRE(engine.set_midi_instrument(5, &other));

  const auto note_on = sonare::midi::make_midi2_note_on(2, 9, 61, 0x1234u, 3, 0xBEEFu);
  const auto four_word = reserved_128_bit_ump();
  const auto one_word = sonare::midi::make_midi1_control_change(2, 9, 7, 99);
  REQUIRE(push_ump(engine, 3, note_on) == MidiUmpPushResult::kQueued);
  REQUIRE(push_ump(engine, 3, four_word) == MidiUmpPushResult::kQueued);
  REQUIRE(push_ump(engine, 3, one_word) == MidiUmpPushResult::kQueued);
  process_block(engine);

  REQUIRE(other.count_ == 0);
  REQUIRE(target.count_ == 3);
  for (size_t w = 0; w < 4; ++w) {
    CHECK(target.received_[0].words[w] == note_on.words[w]);
    CHECK(target.received_[1].words[w] == four_word.words[w]);
  }
  CHECK(target.received_[0].word_count == 2);
  CHECK(target.received_[0].group == 2);
  CHECK(target.received_[1].word_count == 4);
  CHECK(target.received_[2].words[0] == one_word.words[0]);
  CHECK(target.received_[2].word_count == 1);
}

TEST_CASE("push_midi_ump delivers without allocating on the audio thread", "[engine][midi][rt]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  UmpRecordingInstrument target;
  REQUIRE(engine.set_midi_instrument(0, &target));
  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  float* io[] = {left.data(), right.data()};
  engine.process(io, 2, 64);  // warm-up

  REQUIRE(push_ump(engine, 0, sonare::midi::make_midi2_note_on(0, 0, 60, 0x8000u)) ==
          MidiUmpPushResult::kQueued);
  REQUIRE(push_ump(engine, 0, reserved_128_bit_ump()) == MidiUmpPushResult::kQueued);
  sonare::midi::Ump stream{};
  stream.words[0] = 0xF0000000u;
  stream.word_count = 4;
  REQUIRE(push_ump(engine, 0, stream) == MidiUmpPushResult::kQueued);
  {
    sonare::test::AllocationGuard guard;
    engine.process(io, 2, 64);
    REQUIRE(guard.count() == 0);
  }
  REQUIRE(target.count_ == 2);
}

TEST_CASE("push_midi_ump rejects data messages and malformed word counts", "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  UmpRecordingInstrument target;
  REQUIRE(engine.set_midi_instrument(0, &target));

  const uint32_t sysex7[2] = {0x30160102u, 0x03040506u};
  const uint32_t data128[4] = {0x50000000u, 0u, 0u, 0u};
  REQUIRE(engine.push_midi_ump(0, sysex7, 2, -1) == MidiUmpPushResult::kInvalidMessage);
  REQUIRE(engine.push_midi_ump(0, data128, 4, -1) == MidiUmpPushResult::kInvalidMessage);

  const auto note_on = sonare::midi::make_midi2_note_on(0, 0, 60, 0x8000u);
  REQUIRE(engine.push_midi_ump(0, note_on.words, 1, -1) == MidiUmpPushResult::kInvalidMessage);
  REQUIRE(engine.push_midi_ump(0, note_on.words, 4, -1) == MidiUmpPushResult::kInvalidMessage);
  REQUIRE(engine.push_midi_ump(0, note_on.words, 0, -1) == MidiUmpPushResult::kInvalidMessage);
  REQUIRE(engine.push_midi_ump(0, note_on.words, 5, -1) == MidiUmpPushResult::kInvalidMessage);
  REQUIRE(engine.push_midi_ump(0, nullptr, 2, -1) == MidiUmpPushResult::kInvalidMessage);
  const auto cc = sonare::midi::make_midi1_control_change(0, 0, 7, 1);
  REQUIRE(engine.push_midi_ump(0, cc.words, 2, -1) == MidiUmpPushResult::kInvalidMessage);

  REQUIRE(RealtimeEngine::is_pushable_midi_ump(note_on.words, 2));
  REQUIRE_FALSE(RealtimeEngine::is_pushable_midi_ump(sysex7, 2));
  REQUIRE_FALSE(RealtimeEngine::is_pushable_midi_ump(data128, 4));

  process_block(engine);
  REQUIRE(target.count_ == 0);
}

TEST_CASE("live Utility, Flex Data and UMP Stream messages are counted, not delivered",
          "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  UmpRecordingInstrument target;
  UmpRecordingInstrument other;
  REQUIRE(engine.set_midi_instrument(3, &target));
  REQUIRE(engine.set_midi_instrument(5, &other));

  const uint32_t noop[1] = {0x00000000u};
  const uint32_t jr_timestamp[1] = {0x00201234u};
  const uint32_t flex_tempo[4] = {0xD0100000u, 0x02FAF080u, 0u, 0u};
  const uint32_t stream_endpoint[4] = {0xF0000101u, 0u, 0u, 0u};
  REQUIRE(engine.push_midi_ump(3, noop, 1, -1) == MidiUmpPushResult::kQueued);
  REQUIRE(engine.push_midi_ump(3, jr_timestamp, 1, -1) == MidiUmpPushResult::kQueued);
  REQUIRE(engine.push_midi_ump(3, flex_tempo, 4, -1) == MidiUmpPushResult::kQueued);
  REQUIRE(engine.push_midi_ump(3, stream_endpoint, 4, -1) == MidiUmpPushResult::kQueued);
  REQUIRE(engine.push_midi_ump(5, stream_endpoint, 4, -1) == MidiUmpPushResult::kQueued);
  // A channel-voice message on the same destination still arrives.
  REQUIRE(push_ump(engine, 3, sonare::midi::make_midi2_note_on(0, 0, 60, 0x8000u)) ==
          MidiUmpPushResult::kQueued);
  process_block(engine);

  REQUIRE(target.count_ == 1);
  REQUIRE(target.received_[0].message_type() == sonare::midi::UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(other.count_ == 0);
  REQUIRE(engine.midi_ump_discarded_count(3) == 4);
  REQUIRE(engine.midi_ump_discarded_count(5) == 1);
  REQUIRE(engine.midi_ump_discarded_count(7) == 0);
  REQUIRE(engine.midi_ump_discarded_total() == 5);

  // The engine-owned live input path applies the same rule.
  sonare::host::FixedMidiInputSource<8> input;
  engine.set_midi_input_source(&input, 5);
  sonare::midi::Ump flex{};
  std::copy(std::begin(flex_tempo), std::end(flex_tempo), flex.words);
  flex.word_count = 4;
  REQUIRE(input.push_event(flex, 0));
  REQUIRE(input.push_event(sonare::midi::make_midi2_note_on(0, 1, 62, 0x8000u), 0));
  process_block(engine);
  REQUIRE(other.count_ == 1);
  REQUIRE(engine.midi_ump_discarded_count(5) == 2);
  REQUIRE(engine.midi_ump_discarded_total() == 6);
}

TEST_CASE("push_midi_ump fails and counts when the slot ring is full", "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64, /*command_capacity=*/4096);
  UmpRecordingInstrument target;
  REQUIRE(engine.set_midi_instrument(0, &target));

  // A controller rather than a note-on, so the sequencer's active-note table
  // cannot be what limits delivery.
  const auto wide_cc = sonare::midi::make_midi2_control_change(0, 0, 74, 0x12345678u);
  for (size_t i = 0; i < RealtimeEngine::kMidiUmpSlots; ++i) {
    REQUIRE(push_ump(engine, 0, wide_cc) == MidiUmpPushResult::kQueued);
  }
  REQUIRE(engine.midi_ump_slot_overflow_count() == 0);
  REQUIRE(push_ump(engine, 0, wide_cc) == MidiUmpPushResult::kSlotsFull);
  REQUIRE(engine.midi_ump_slot_overflow_count() == 1);
  // A single-word message does not need a slot.
  REQUIRE(push_ump(engine, 0, sonare::midi::make_midi1_control_change(0, 0, 7, 1)) ==
          MidiUmpPushResult::kQueued);

  // The audio thread releases slots as it consumes them. The per-block command
  // cap spreads the backlog over several blocks.
  for (int block = 0; block < 8; ++block) process_block(engine);
  REQUIRE(target.count_ == RealtimeEngine::kMidiUmpSlots + 1);
  REQUIRE(push_ump(engine, 0, wide_cc) == MidiUmpPushResult::kQueued);
  process_block(engine);
  REQUIRE(target.count_ == RealtimeEngine::kMidiUmpSlots + 2);
}

TEST_CASE("prepare returns the UMP slots held by discarded commands", "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64, /*command_capacity=*/4096);
  UmpRecordingInstrument target;
  REQUIRE(engine.set_midi_instrument(0, &target));

  const auto wide_cc = sonare::midi::make_midi2_control_change(0, 0, 74, 0x12345678u);
  for (size_t i = 0; i < RealtimeEngine::kMidiUmpSlots; ++i) {
    REQUIRE(push_ump(engine, 0, wide_cc) == MidiUmpPushResult::kQueued);
  }
  REQUIRE(push_ump(engine, 0, wide_cc) == MidiUmpPushResult::kSlotsFull);

  // Re-preparing drops the queued commands; their slots must come back with them.
  engine.prepare(48000.0, 64, /*command_capacity=*/4096);
  REQUIRE(push_ump(engine, 0, wide_cc) == MidiUmpPushResult::kQueued);
  process_block(engine);
  REQUIRE(target.count_ == 1);
}

TEST_CASE("a UMP slot command whose generation does not match is ignored", "[engine][midi]") {
  RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  UmpRecordingInstrument target;
  REQUIRE(engine.set_midi_instrument(0, &target));

  const auto forge = [&](uint64_t slot, uint64_t generation) {
    sonare::rt::Command c{};
    c.type = sonare::rt::CommandType::kMidiUmpSlotImmediate;
    c.sample_time = -1;
    c.arg.i = static_cast<int64_t>(slot | (generation << 32));
    REQUIRE(engine.push_command(c));
  };

  // Never-written slots, including the initial generation.
  forge(0, 0);
  forge(1, 2);
  forge(uint64_t{0xFFFFFFFFu}, 2);
  process_block(engine);
  REQUIRE(target.count_ == 0);

  // The first push lands in slot 0 at generation 2 and is delivered once.
  REQUIRE(push_ump(engine, 0, sonare::midi::make_midi2_note_on(0, 0, 60, 0x8000u)) ==
          MidiUmpPushResult::kQueued);
  process_block(engine);
  REQUIRE(target.count_ == 1);

  // Replaying the consumed reference, or naming a generation not yet written,
  // delivers nothing.
  forge(0, 2);
  forge(0, 4);
  forge(0, 3);
  process_block(engine);
  REQUIRE(target.count_ == 1);
}

TEST_CASE("sonare_engine_set_part_rig refuses what it cannot apply", "[engine][midi][part_rig]") {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  SonareSynthInstrumentBinding synth{};
  synth.destination_id = 1;
  synth.use_gm_programs = 1;
  REQUIRE(sonare_engine_set_synth_instrument_binding(engine, &synth) == SONARE_OK);
  constexpr const char* kChain = R"([{"processor":"saturation.softClipper","params":"{}"}])";

  // Control: the same destination takes a valid rig.
  REQUIRE(sonare_engine_set_part_rig(engine, 1, 0, SONARE_PART_RIG_CHAIN, kChain) == SONARE_OK);

  REQUIRE(sonare_engine_set_part_rig(nullptr, 1, 0, SONARE_PART_RIG_NONE, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_part_rig(engine, 1, 16, SONARE_PART_RIG_NONE, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_part_rig(engine, 1, 0, 3, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_part_rig(engine, 1, 0, -1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  // Inserts must be present for a chain and absent otherwise.
  REQUIRE(sonare_engine_set_part_rig(engine, 1, 0, SONARE_PART_RIG_CHAIN, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_part_rig(engine, 1, 0, SONARE_PART_RIG_NONE, kChain) ==
          SONARE_ERROR_INVALID_PARAMETER);
  // Malformed JSON and a wrong shape are a format error with a message.
  REQUIRE(sonare_engine_set_part_rig(engine, 1, 0, SONARE_PART_RIG_CHAIN, "[{") ==
          SONARE_ERROR_INVALID_FORMAT);
  REQUIRE(std::string(sonare_last_error_message()) != "");
  REQUIRE(sonare_engine_set_part_rig(engine, 1, 0, SONARE_PART_RIG_CHAIN, "{}") ==
          SONARE_ERROR_INVALID_FORMAT);
  // Valid shape, unknown processor, an empty chain, and an unbound destination.
  REQUIRE(sonare_engine_set_part_rig(engine, 1, 0, SONARE_PART_RIG_CHAIN,
                                     R"([{"processor":"no.such.insert"}])") ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_part_rig(engine, 1, 0, SONARE_PART_RIG_CHAIN, "[]") ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_engine_set_part_rig(engine, 99, 0, SONARE_PART_RIG_NONE, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  sonare_engine_destroy(engine);
}

namespace {

// Records every event with the transport frame pushed before it, so a test can
// see both when an event is stamped and which sub-block it was delivered to.
class EventLogInstrument final : public MidiInstrument {
 public:
  struct Entry {
    int64_t event_frame = 0;
    int64_t callback_frame = 0;
    sonare::midi::Ump ump{};
  };
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override { log.clear(); }
  void set_transport(const sonare::transport::TransportState& state) noexcept override {
    callback_frame_ = state.render_frame;
  }
  void on_event(uint32_t, const MidiEvent& event) noexcept override {
    if (log.size() < log.capacity())
      log.push_back({event.render_frame, callback_frame_, event.ump});
  }
  std::vector<Entry> notes(bool on) const {
    std::vector<Entry> out;
    for (const Entry& entry : log) {
      if (on ? entry.ump.is_note_on() : entry.ump.is_note_off()) out.push_back(entry);
    }
    return out;
  }

  std::vector<Entry> log = [] {
    std::vector<Entry> reserved;
    reserved.reserve(1024);
    return reserved;
  }();

 private:
  int64_t callback_frame_ = 0;
};

void push_live_note(RealtimeEngine& engine, uint32_t destination_id, bool on, uint8_t note,
                    int64_t render_frame) {
  sonare::rt::Command c{};
  c.type = on ? sonare::rt::CommandType::kMidiNoteOnImmediate
              : sonare::rt::CommandType::kMidiNoteOffImmediate;
  c.target_id = destination_id;
  c.sample_time = render_frame;
  c.arg.i = static_cast<int64_t>(uint64_t{100} | (uint64_t{note} << 8));
  REQUIRE(engine.push_command(c));
}

void push_transport(RealtimeEngine& engine, sonare::rt::CommandType type, int64_t render_frame,
                    int64_t seek_sample = 0) {
  sonare::rt::Command c{};
  c.type = type;
  c.sample_time = render_frame;
  c.arg.i = seek_sample;
  REQUIRE(engine.push_command(c));
}

// Renders @p total frames in blocks of @p block and returns the left channel.
std::vector<float> render_blocks(RealtimeEngine& engine, int total, int block) {
  std::vector<float> out;
  for (int done = 0; done < total; done += block) {
    std::vector<float> left(static_cast<size_t>(block), 0.0f);
    std::vector<float> right(static_cast<size_t>(block), 0.0f);
    float* io[] = {left.data(), right.data()};
    engine.process(io, 2, block);
    out.insert(out.end(), left.begin(), left.end());
  }
  return out;
}

sonare::midi::BuiltinSynthConfig sustained_synth_config() {
  sonare::midi::BuiltinSynthConfig config;
  config.attack_ms = 0.1f;
  config.decay_ms = 0.1f;
  config.sustain = 1.0f;
  config.release_ms = 20.0f;
  return config;
}

std::vector<float> arpeggiated_render(int block) {
  RealtimeEngine engine;
  engine.prepare(48000.0, 128);
  sonare::midi::BuiltinSynth synth(sustained_synth_config());
  REQUIRE(engine.set_midi_instrument(0, &synth));
  sonare::midi::MidiFxChain fx;
  sonare::midi::ArpeggiatorConfig arp;
  arp.enabled = true;
  arp.steps = 2;
  arp.intervals[0] = 0;
  arp.intervals[1] = 12;
  arp.step_frames = 40;
  arp.gate_frames = 30;
  fx.set_arpeggiator(arp);
  REQUIRE(engine.set_midi_fx(0, fx));
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.events = {MidiEvent{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                 MidiEvent{100, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  engine.set_midi_clips({clip});
  push_play(engine);
  return render_blocks(engine, 128, block);
}

}  // namespace

TEST_CASE("RealtimeEngine renders MIDI-FX output identically for any host block size",
          "[engine][midi]") {
  const std::vector<float> whole = arpeggiated_render(128);
  const std::vector<float> split = arpeggiated_render(16);
  REQUIRE(whole.size() == split.size());
  float peak = 0.0f;
  float difference = 0.0f;
  for (size_t i = 0; i < whole.size(); ++i) {
    peak = std::max(peak, std::abs(whole[i]));
    difference = std::max(difference, std::abs(whole[i] - split[i]));
  }
  REQUIRE(peak > 0.01f);
  REQUIRE(difference <= 1.0e-6f * peak);
}

TEST_CASE("RealtimeEngine renders a clip's last span before its clip-end release",
          "[engine][midi]") {
  constexpr int kBlock = 64;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  sonare::midi::BuiltinSynth synth(sustained_synth_config());
  REQUIRE(engine.set_midi_instrument(0, &synth));
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.length_samples = kBlock;
  clip.events = {MidiEvent{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  engine.set_midi_clips({clip});
  push_play(engine);

  // The note spans the whole block: its sustain plays over [0, 64) at full level.
  const std::vector<float> first = render_blocks(engine, kBlock, kBlock);
  float late_peak = 0.0f;
  for (int i = kBlock / 2; i < kBlock; ++i) late_peak = std::max(late_peak, std::abs(first[i]));
  REQUIRE(late_peak > 0.03f);
}

TEST_CASE("RealtimeEngine fires a live MIDI-FX event at its device frame while stopped",
          "[engine][midi]") {
  constexpr int kBlock = 64;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  EventLogInstrument log;
  REQUIRE(engine.set_midi_instrument(0, &log));
  sonare::midi::MidiFxChain fx;
  sonare::midi::ArpeggiatorConfig arp;
  arp.enabled = true;
  arp.steps = 2;
  arp.intervals[0] = 0;
  arp.intervals[1] = 12;
  arp.step_frames = 100;
  arp.gate_frames = 10;
  fx.set_arpeggiator(arp);
  REQUIRE(engine.set_midi_fx(0, fx));

  // A seek in between moves the timeline but not the device clock.
  push_live_note(engine, 0, true, 60, 10);
  push_transport(engine, sonare::rt::CommandType::kTransportSeekSample, 50, 9000);
  render_blocks(engine, 4 * kBlock, kBlock);

  const auto on = log.notes(true);
  REQUIRE(on.size() == 2);
  CHECK(on[0].event_frame == 10);
  CHECK(on[1].event_frame == 110);
  CHECK(on[1].callback_frame == 110);
  CHECK(on[1].ump.note_number() == 72);
}

TEST_CASE("RealtimeEngine applies a queued Play's loop wraps from the first block",
          "[engine][midi]") {
  constexpr int kBlock = 64;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  engine.set_tempo(120.0);
  EventLogInstrument log;
  REQUIRE(engine.set_midi_instrument(0, &log));
  // 16-sample loop at 24000 samples per quarter note.
  engine.set_loop(0.0, 16.0 / 24000.0, true);
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.events = {MidiEvent{4, sonare::midi::make_midi1_note_on(0, 0, 60, 100)}};
  engine.set_midi_clips({clip});
  push_transport(engine, sonare::rt::CommandType::kTransportPlay, 0);
  render_blocks(engine, kBlock, kBlock);

  const auto on = log.notes(true);
  REQUIRE(on.size() == 4);
  for (size_t i = 0; i < on.size(); ++i) {
    CHECK(on[i].event_frame == static_cast<int64_t>(4 + 16 * i));
    CHECK(on[i].callback_frame == on[i].event_frame);
  }
}

TEST_CASE("RealtimeEngine places MIDI after a loop wrap at its own device frame",
          "[engine][midi]") {
  constexpr int kBlock = 64;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  engine.set_tempo(120.0);
  EventLogInstrument log;
  REQUIRE(engine.set_midi_instrument(0, &log));
  engine.set_loop(0.0, 128.0 / 24000.0, true);
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.events = {MidiEvent{8, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                 MidiEvent{16, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  engine.set_midi_clips({clip});
  push_transport(engine, sonare::rt::CommandType::kTransportSeekSample, 0, 96);
  push_transport(engine, sonare::rt::CommandType::kTransportPlay, 0);
  render_blocks(engine, kBlock, kBlock);

  // Wrap at 32; the note plays timeline [8, 16), i.e. device [40, 48).
  const auto on = log.notes(true);
  REQUIRE(on.size() == 1);
  CHECK(on[0].event_frame == 40);
  CHECK(on[0].callback_frame == 40);
  bool released = false;
  for (const auto& entry : log.notes(false)) {
    if (entry.ump.note_number() == 60 && entry.event_frame == 48) {
      released = entry.callback_frame == 48;
    }
  }
  CHECK(released);
}

TEST_CASE("RealtimeEngine keeps a live note-on queued for the loop-wrap frame", "[engine][midi]") {
  constexpr int kBlock = 64;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  engine.set_tempo(120.0);
  EventLogInstrument log;
  REQUIRE(engine.set_midi_instrument(0, &log));
  engine.set_loop(0.0, 96.0 / 24000.0, true);
  push_transport(engine, sonare::rt::CommandType::kTransportPlay, 0);
  render_blocks(engine, kBlock, kBlock);
  // The second block starts at timeline 64 and wraps at device frame 96; the note
  // is queued for exactly that frame.
  push_live_note(engine, 0, true, 72, 96);
  render_blocks(engine, kBlock, kBlock);

  bool on_at_wrap = false;
  bool killed = false;
  for (const auto& entry : log.log) {
    if (entry.ump.is_note_on() && entry.ump.note_number() == 72) on_at_wrap = true;
    if (on_at_wrap && entry.ump.is_note_off() && entry.ump.note_number() == 72) {
      killed = true;
    }
  }
  CHECK(on_at_wrap);
  CHECK_FALSE(killed);
  CHECK(engine.midi_sequencer().active_note_count() == 1);
}

TEST_CASE("RealtimeEngine drains external clock and notes in time order", "[engine][midi]") {
  constexpr int kBlock = 2048;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  engine.set_tempo(120.0);
  engine.set_external_midi_clock_enabled(true);
  REQUIRE(engine.set_midi_destination_external(3, true));
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = 3;
  clip.events = {MidiEvent{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)},
                 MidiEvent{1500, sonare::midi::make_midi1_note_off(0, 0, 60, 0)}};
  engine.set_midi_clips({clip});
  push_transport(engine, sonare::rt::CommandType::kTransportPlay, 0);
  render_blocks(engine, kBlock, kBlock);

  std::array<sonare::host::ExternalMidiRecord, 64> drained{};
  const size_t n = engine.drain_external_midi(drained.data(), drained.size());
  REQUIRE(n >= 5);  // Start, note-on, clocks at 0/1000/2000, note-off.
  size_t notes = 0;
  for (size_t i = 0; i < n; ++i) {
    if (drained[i].destination_id == 3) ++notes;
    if (i > 0) CHECK(drained[i].event.render_frame >= drained[i - 1].event.render_frame);
  }
  CHECK(notes == 2);
}

TEST_CASE("RealtimeEngine quantizes live input on the timeline grid", "[engine][midi]") {
  constexpr int kBlock = 64;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  EventLogInstrument log;
  REQUIRE(engine.set_midi_instrument(0, &log));
  sonare::midi::MidiFxChain fx;
  sonare::midi::QuantizeConfig quantize;
  quantize.enabled = true;
  quantize.grid_frames = 100;
  fx.set_quantize(quantize);
  REQUIRE(engine.set_midi_fx(0, fx));

  // One stopped block moves the device clock to 64; the timeline then starts at 30.
  render_blocks(engine, kBlock, kBlock);
  push_transport(engine, sonare::rt::CommandType::kTransportSeekSample, 64, 30);
  push_transport(engine, sonare::rt::CommandType::kTransportPlay, 64);
  // Device 120 is timeline 86, whose nearest grid line, timeline 100, is device 134.
  push_live_note(engine, 0, true, 60, 120);
  render_blocks(engine, 2 * kBlock, kBlock);

  const auto on = log.notes(true);
  REQUIRE(on.size() == 1);
  CHECK(on[0].event_frame == 134);
  CHECK(on[0].callback_frame == 134);
}

TEST_CASE("RealtimeEngine keeps a stopped live note's release and sustain sounding",
          "[engine][midi]") {
  constexpr int kBlock = 256;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock);
  sonare::midi::BuiltinSynthConfig config = sustained_synth_config();
  config.release_ms = 50.0f;
  sonare::midi::BuiltinSynth synth(config);
  REQUIRE(engine.set_midi_instrument(0, &synth));
  const auto peak_of = [](const std::vector<float>& audio) {
    float peak = 0.0f;
    for (float value : audio) peak = std::max(peak, std::abs(value));
    return peak;
  };

  push_live_note(engine, 0, true, 60, 0);
  const float held = peak_of(render_blocks(engine, kBlock, kBlock));
  REQUIRE(held > 0.03f);
  push_live_note(engine, 0, false, 60, kBlock);
  // The release decays over the following blocks instead of stopping dead.
  const float release_start = peak_of(render_blocks(engine, kBlock, kBlock));
  const float release_later = peak_of(render_blocks(engine, kBlock, kBlock));
  CHECK(release_start > 0.01f);
  CHECK(release_later > 0.0f);
  CHECK(release_later < release_start);
  render_blocks(engine, 40 * kBlock, kBlock);
  CHECK(peak_of(render_blocks(engine, kBlock, kBlock)) < 1.0e-4f);

  // With the damper down, the released note sustains until the pedal lifts.
  sonare::rt::Command pedal{};
  pedal.type = sonare::rt::CommandType::kMidiCcImmediate;
  pedal.sample_time = -1;
  pedal.arg.i = static_cast<int64_t>(uint64_t{127} | (uint64_t{64} << 8));
  REQUIRE(engine.push_command(pedal));
  push_live_note(engine, 0, true, 64, -1);
  render_blocks(engine, kBlock, kBlock);
  push_live_note(engine, 0, false, 64, -1);
  render_blocks(engine, 4 * kBlock, kBlock);
  CHECK(peak_of(render_blocks(engine, kBlock, kBlock)) > 0.03f);
}

TEST_CASE("RealtimeEngine freeze withdraws the MIDI it baked so it plays once",
          "[engine][midi][freeze]") {
  constexpr int kBlock = 128;
  RealtimeEngine engine;
  engine.prepare(48000.0, kBlock, 16, 16, 1);
  CountingInstrument instrument;
  REQUIRE(engine.set_midi_instrument(0, &instrument));
  engine.set_midi_clips(held_note_clip());
  const auto seek_to_start = [&] {
    push_transport(engine, sonare::rt::CommandType::kTransportSeekSample, -1, 0);
  };

  std::array<float, kBlock> live{};
  float* live_io[] = {live.data()};
  engine.render_offline(live_io, 1, kBlock, kBlock);
  REQUIRE(live[kBlock - 1] == Catch::Approx(0.5f));

  seek_to_start();
  engine.freeze_offline(1, kBlock, kBlock, 9, 0.0, 1.0f);
  REQUIRE(engine.clip_count() == 1);
  REQUIRE(engine.midi_clip_count() == 0);

  seek_to_start();
  std::array<float, kBlock> replay{};
  float* replay_io[] = {replay.data()};
  engine.render_offline(replay_io, 1, kBlock, kBlock);
  for (int i = 0; i < kBlock; ++i) {
    REQUIRE(replay[static_cast<size_t>(i)] == Catch::Approx(live[static_cast<size_t>(i)]));
  }
}
