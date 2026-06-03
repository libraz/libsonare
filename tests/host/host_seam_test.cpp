/// @file host_seam_test.cpp
/// @brief host seam tests: compile + exercise mock implementations of the
///        AudioDevice, InstrumentProvider and MIDI I/O seams; prove the
///        InstrumentProvider plugs into the RealtimeEngine host-instrument wiring point; and
///        statically guard that src/host/*.h pull in NO OS / plugin SDK headers.
///
/// The canonical SDK-include guard is the CI grep in the plan (section 4,
/// "core must not include OS/plugin SDK headers"); the in-test scan below is a
/// fast local mirror of it.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "engine/realtime_engine.h"
#include "host/audio_device.h"
#include "host/midi_io.h"
#include "host/plugin_host.h"
#include "midi/instrument.h"
#include "midi/midi_clip.h"
#include "midi/midi_event.h"
#include "midi/sequencer.h"
#include "midi/ump.h"
#include "rt/command.h"

namespace {

using sonare::host::AudioBufferView;
using sonare::host::AudioDeviceCallback;
using sonare::host::AudioStreamConfig;
using sonare::host::FixedMidiInputSource;
using sonare::host::FixedMidiOutputSink;
using sonare::host::InstrumentProvider;
using sonare::host::MidiInputSource;
using sonare::host::MidiOutputSink;
using sonare::host::PluginDescriptor;
using sonare::host::PluginKind;
using sonare::midi::MidiEvent;
using sonare::midi::MidiInstrument;
using sonare::midi::Ump;

// ===========================================================================
// Mock seam implementations
// ===========================================================================

// A mock audio device callback: captures the negotiated config and, on render,
// writes a constant ramp into the output buffers so we can assert it ran.
class MockAudioDevice final : public AudioDeviceCallback {
 public:
  bool open(const AudioStreamConfig& config) override {
    config_ = config;
    opened_ = true;
    return true;
  }
  void render(const AudioBufferView& buffers) noexcept override {
    rendered_frames_ += buffers.num_frames;
    last_sample_time_ = buffers.time.sample_time;
    for (int c = 0; c < buffers.num_output_channels; ++c) {
      for (int i = 0; i < buffers.num_frames; ++i) {
        buffers.outputs[c][i] = static_cast<float>(i);
      }
    }
  }
  void close() noexcept override { opened_ = false; }

  AudioStreamConfig config_{};
  bool opened_ = false;
  int64_t rendered_frames_ = 0;
  int64_t last_sample_time_ = 0;
};

// A mock MIDI instrument: a midi::MidiInstrument (IS-A rt::ProcessorBase +
// MidiEventSink) with a known fixed latency. It counts received events and, on
// process(), emits a constant DC so we can detect audio output.
class MockInstrument final : public MidiInstrument {
 public:
  explicit MockInstrument(int latency) : latency_(latency) {}

  void prepare(double sample_rate, int max_block_size) override {
    (void)sample_rate;
    (void)max_block_size;
    prepared_ = true;
  }
  void process(float* const* channels, int num_channels, int num_samples) override {
    // Emit DC only while at least one note is sounding, so we can prove the
    // sequencer's note-on reached us before render.
    const float value = note_on_count_ > note_off_count_ ? 0.25f : 0.0f;
    for (int c = 0; c < num_channels; ++c) {
      for (int i = 0; i < num_samples; ++i) channels[c][i] += value;
    }
  }
  void reset() override {
    note_on_count_ = 0;
    note_off_count_ = 0;
  }
  int latency_samples() const noexcept override { return latency_; }

  void on_event(uint32_t destination_id, const MidiEvent& event) noexcept override {
    (void)destination_id;
    ++received_events_;
    if (event.ump.is_note_on()) ++note_on_count_;
    if (event.ump.is_note_off()) ++note_off_count_;
  }

  bool prepared_ = false;
  int received_events_ = 0;
  int note_on_count_ = 0;
  int note_off_count_ = 0;

 private:
  int latency_ = 0;
};

// A mock instrument provider: a factory that yields a MockInstrument with a
// descriptor-independent known latency, and reports that latency up front for
// the compiler PDC query without instantiating.
class MockInstrumentProvider final : public InstrumentProvider {
 public:
  static constexpr int kLatency = 48;

  bool can_create(const PluginDescriptor& descriptor) const noexcept override {
    return descriptor.kind == PluginKind::kInstrument && descriptor.format == "mock";
  }
  std::unique_ptr<MidiInstrument> create_instrument(const PluginDescriptor& descriptor) override {
    if (!can_create(descriptor)) return nullptr;
    return std::make_unique<MockInstrument>(kLatency);
  }
  int latency_samples(const PluginDescriptor& descriptor) const noexcept override {
    return can_create(descriptor) ? kLatency : 0;
  }
};

// A mock MIDI input seam backed by a fixed ring of pending UMP records.
class MockMidiInput final : public MidiInputSource {
 public:
  bool push_event(const Ump& ump, int64_t port_time_samples) noexcept override {
    if (count_ >= kCapacity) return false;
    buffer_[count_++] = {ump, port_time_samples};
    return true;
  }
  size_t drain(MidiEvent* out, size_t capacity, int64_t block_start_frame) noexcept override {
    size_t n = 0;
    for (; n < count_ && n < capacity; ++n) {
      // Map the host's port time to an in-block render frame.
      out[n].render_frame = block_start_frame + buffer_[n].port_time;
      out[n].ump = buffer_[n].ump;
    }
    // Shift remaining (none in these tests; keep it simple and correct).
    const size_t remaining = count_ - n;
    for (size_t i = 0; i < remaining; ++i) buffer_[i] = buffer_[n + i];
    count_ = remaining;
    return n;
  }
  size_t pending_count() const noexcept override { return count_; }

 private:
  static constexpr size_t kCapacity = 32;
  struct Slot {
    Ump ump{};
    int64_t port_time = 0;
  };
  std::array<Slot, kCapacity> buffer_{};
  size_t count_ = 0;
};

// A mock MIDI output seam capturing sent events into a fixed queue.
class MockMidiOutput final : public MidiOutputSink {
 public:
  bool send(const MidiEvent& event) noexcept override {
    if (count_ >= kCapacity) return false;
    sent_[count_++] = event;
    return true;
  }
  size_t queued_count() const noexcept override { return count_; }

  static constexpr size_t kCapacity = 32;
  std::array<MidiEvent, kCapacity> sent_{};
  size_t count_ = 0;
};

}  // namespace

// ===========================================================================
// SEAM COMPILE: instantiate and exercise each mock seam.
// ===========================================================================

TEST_CASE("host seams instantiate and run as mocks", "[host]") {
  // Audio device callback seam.
  MockAudioDevice device;
  AudioStreamConfig cfg;
  cfg.sample_rate = 48000.0;
  cfg.max_block_size = 64;
  cfg.num_output_channels = 2;
  REQUIRE(device.open(cfg));
  REQUIRE(device.opened_);

  std::vector<float> l(64, 0.0f), r(64, 0.0f);
  float* outs[] = {l.data(), r.data()};
  AudioBufferView view;
  view.outputs = outs;
  view.num_output_channels = 2;
  view.num_frames = 64;
  view.time.sample_time = 1000;
  device.render(view);
  REQUIRE(device.rendered_frames_ == 64);
  REQUIRE(device.last_sample_time_ == 1000);
  REQUIRE(r[63] == 63.0f);
  device.close();
  REQUIRE_FALSE(device.opened_);

  // Instrument provider seam: PDC query before instantiation, then create.
  MockInstrumentProvider provider;
  PluginDescriptor desc;
  desc.id = "mock.synth";
  desc.name = "Mock Synth";
  desc.format = "mock";
  desc.kind = PluginKind::kInstrument;
  REQUIRE(provider.can_create(desc));
  REQUIRE(provider.latency_samples(desc) == MockInstrumentProvider::kLatency);
  auto inst = provider.create_instrument(desc);
  REQUIRE(inst != nullptr);
  REQUIRE(inst->latency_samples() == MockInstrumentProvider::kLatency);

  // Unsupported descriptor → null + zero latency.
  PluginDescriptor other = desc;
  other.format = "vst3";
  REQUIRE_FALSE(provider.can_create(other));
  REQUIRE(provider.create_instrument(other) == nullptr);
  REQUIRE(provider.latency_samples(other) == 0);
}

// ===========================================================================
// MIDI I/O seam: push -> drain (input), send -> capture (output).
// ===========================================================================

TEST_CASE("MIDI I/O seams exchange fixed records", "[host]") {
  MockMidiInput input;
  const Ump on = sonare::midi::make_midi1_note_on(0, 0, 60, 100);
  const Ump off = sonare::midi::make_midi1_note_off(0, 0, 60, 0);
  REQUIRE(input.push_event(on, 10));
  REQUIRE(input.push_event(off, 20));
  REQUIRE(input.pending_count() == 2);

  std::array<MidiEvent, 8> drained{};
  const size_t n = input.drain(drained.data(), drained.size(), 5000);
  REQUIRE(n == 2);
  REQUIRE(input.pending_count() == 0);
  REQUIRE(drained[0].render_frame == 5010);  // block_start 5000 + port_time 10
  REQUIRE(drained[0].ump.is_note_on());
  REQUIRE(drained[1].render_frame == 5020);
  REQUIRE(drained[1].ump.is_note_off());

  MockMidiOutput output;
  REQUIRE(output.send(MidiEvent{12345, on}));
  REQUIRE(output.send_ump(off, 12346));
  REQUIRE(output.queued_count() == 2);
  REQUIRE(output.sent_[0].render_frame == 12345);
  REQUIRE(output.sent_[0].ump.is_note_on());
  REQUIRE(output.sent_[1].render_frame == 12346);
  REQUIRE(output.sent_[1].ump.is_note_off());
}

TEST_CASE("fixed MIDI I/O implementations preserve order and telemetry", "[host]") {
  FixedMidiInputSource<3> input;
  const Ump on = sonare::midi::make_midi1_note_on(0, 0, 60, 100);
  const Ump cc = sonare::midi::make_midi1_control_change(0, 0, 74, 64);
  const Ump bend = sonare::midi::make_midi1_pitch_bend(0, 0, 8192);
  const Ump off = sonare::midi::make_midi1_note_off(0, 0, 60, 0);

  REQUIRE(input.push_event(on, 1));
  REQUIRE(input.push_event(cc, 2));
  REQUIRE(input.push_event(bend, 3));
  REQUIRE_FALSE(input.push_event(off, 4));
  REQUIRE(input.pending_count() == 3);
  REQUIRE(input.dropped_count() == 1);

  std::array<MidiEvent, 2> first{};
  REQUIRE(input.drain(first.data(), first.size(), 1000) == 2);
  REQUIRE(input.pending_count() == 1);
  REQUIRE(first[0].render_frame == 1001);
  REQUIRE(first[0].ump.is_note_on());
  REQUIRE(first[1].render_frame == 1002);
  REQUIRE(first[1].ump.status_nibble() ==
          static_cast<uint8_t>(sonare::midi::UmpStatus::kControlChange));

  std::array<MidiEvent, 2> second{};
  REQUIRE(input.drain(second.data(), second.size(), 2000) == 1);
  REQUIRE(input.pending_count() == 0);
  REQUIRE(second[0].render_frame == 2003);
  REQUIRE(second[0].ump.status_nibble() ==
          static_cast<uint8_t>(sonare::midi::UmpStatus::kPitchBend));
  input.reset_telemetry();
  REQUIRE(input.dropped_count() == 0);

  FixedMidiOutputSink<2> output;
  REQUIRE(output.send(MidiEvent{10, on}));
  REQUIRE(output.send_ump(off, 11));
  REQUIRE_FALSE(output.send_ump(cc, 12));
  REQUIRE(output.queued_count() == 2);
  REQUIRE(output.dropped_count() == 1);

  std::array<MidiEvent, 1> out_first{};
  REQUIRE(output.drain_queued(out_first.data(), out_first.size()) == 1);
  REQUIRE(output.queued_count() == 1);
  REQUIRE(out_first[0].render_frame == 10);
  REQUIRE(out_first[0].ump.is_note_on());

  std::array<MidiEvent, 2> out_second{};
  REQUIRE(output.drain_queued(out_second.data(), out_second.size()) == 1);
  REQUIRE(output.queued_count() == 0);
  REQUIRE(out_second[0].render_frame == 11);
  REQUIRE(out_second[0].ump.is_note_off());
  output.reset_telemetry();
  REQUIRE(output.dropped_count() == 0);
}

// ===========================================================================
// MOCK INSTRUMENT through the engine (proves the host provider seam connects to
// the RealtimeEngine::set_midi_instrument injection point).
// ===========================================================================

TEST_CASE("host provider instrument drives engine via host-instrument wiring", "[host]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 256;

  // Obtain the instrument from the host provider seam.
  MockInstrumentProvider provider;
  PluginDescriptor desc;
  desc.id = "mock.synth";
  desc.format = "mock";
  desc.kind = PluginKind::kInstrument;
  auto instrument = provider.create_instrument(desc);
  REQUIRE(instrument != nullptr);

  sonare::engine::RealtimeEngine engine;
  engine.prepare(kSr, kBlock);

  // Register via the host-instrument wiring point.
  engine.set_midi_instrument(instrument.get());
  REQUIRE(engine.midi_instrument() == instrument.get());

  // The instrument's latency is reported through the engine for the PDC summary.
  REQUIRE(engine.midi_instrument_latency_samples() == MockInstrumentProvider::kLatency);

  // Drive a one-note MIDI clip: note-on at frame 0, note-off near the end.
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.start_sample = 0;
  clip.length_samples = kBlock;
  clip.destination_id = 0;
  clip.events.push_back(MidiEvent{0, sonare::midi::make_midi1_note_on(0, 0, 64, 100)});
  clip.events.push_back(MidiEvent{kBlock - 1, sonare::midi::make_midi1_note_off(0, 0, 64, 0)});
  std::vector<sonare::midi::MidiClipSchedule> clips{clip};
  engine.set_midi_clips(std::move(clips));
  REQUIRE(engine.midi_clip_count() == 1);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::vector<float> left(kBlock, 0.0f), right(kBlock, 0.0f);
  float* io[] = {left.data(), right.data()};
  engine.process(io, 2, kBlock);

  // The sequencer dispatched the note-on to the instrument (proving the seam
  // connected), and the instrument produced audio while the note was sounding.
  auto* mock = static_cast<MockInstrument*>(instrument.get());
  REQUIRE(mock->received_events_ >= 1);
  REQUIRE(mock->note_on_count_ >= 1);

  float peak = 0.0f;
  for (int i = 0; i < kBlock; ++i) peak = std::max(peak, std::abs(left[i]));
  REQUIRE(peak > 0.0f);

  // Clearing restores the no-instrument behavior.
  engine.set_midi_instrument(nullptr);
  REQUIRE(engine.midi_instrument() == nullptr);
  REQUIRE(engine.midi_instrument_latency_samples() == 0);
}

// ===========================================================================
// SDK-INCLUDE STATIC GUARD: scan src/host/*.h for forbidden OS/plugin SDK
// includes. (Canonical guard is the CI grep in plan section 4.)
// ===========================================================================

TEST_CASE("host headers include no OS / plugin SDK headers", "[host]") {
  namespace fs = std::filesystem;

  // Forbidden include substrings (matched inside the quoted/angled include path
  // of a `#include` line). Covers the device + plugin SDKs the plan names.
  const std::vector<std::string> forbidden = {
      "<CoreAudio/", "<AudioUnit/",      "<AudioToolbox/", "<CoreMIDI/",
      "<windows.h>", "<Windows.h>",      "<mmsystem.h>",   "<mmdeviceapi.h>",
      "<alsa/",      "<jack/",           "<portaudio.h>",  "<pulse/",
      "<vst",        "<pluginterfaces/", "<clap/",         "<lv2",
  };

  // Resolve src/host relative to this test file so the test is path-independent.
  const fs::path host_dir =
      fs::path(__FILE__).parent_path().parent_path().parent_path() / "src" / "host";
  REQUIRE(fs::is_directory(host_dir));

  size_t scanned = 0;
  for (const auto& entry : fs::directory_iterator(host_dir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".h") continue;
    ++scanned;
    std::ifstream in(entry.path());
    REQUIRE(in.good());
    std::string line;
    while (std::getline(in, line)) {
      // Only inspect actual include directives.
      const auto hash = line.find('#');
      if (hash == std::string::npos) continue;
      if (line.find("include", hash) == std::string::npos) continue;
      for (const auto& bad : forbidden) {
        INFO("forbidden include '" << bad << "' in " << entry.path().string() << ": " << line);
        REQUIRE(line.find(bad) == std::string::npos);
      }
    }
  }
  // The three host seam headers must all have been scanned.
  REQUIRE(scanned >= 3);
}
