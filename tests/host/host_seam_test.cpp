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
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
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
using sonare::host::AudioBusDescriptor;
using sonare::host::AudioBusDirection;
using sonare::host::AudioCallbackTime;
using sonare::host::AudioDeviceCallback;
using sonare::host::AudioStreamConfig;
using sonare::host::FixedMidiInputSource;
using sonare::host::FixedMidiOutputSink;
using sonare::host::InstrumentProvider;
using sonare::host::MidiInputSource;
using sonare::host::MidiOutputSink;
using sonare::host::PluginBusDescriptor;
using sonare::host::PluginBusRole;
using sonare::host::PluginDescriptor;
using sonare::host::PluginKind;
using sonare::host::PluginParameterDescriptor;
using sonare::host::PluginParameterUnit;
using sonare::host::PluginPresetDescriptor;
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

class MockEffect final : public sonare::rt::ProcessorBase {
 public:
  static constexpr int kTailSamples = 192;

  void prepare(double, int) override { prepared_ = true; }
  void process(float* const* channels, int num_channels, int num_samples) override {
    ++process_count_;
    for (int ch = 0; ch < num_channels; ++ch) {
      for (int i = 0; i < num_samples; ++i) channels[ch][i] *= 0.5f;
    }
  }
  void reset() override { ++reset_count_; }
  int tail_samples() const noexcept override { return kTailSamples; }

  bool prepared_ = false;
  int process_count_ = 0;
  int reset_count_ = 0;
};

// A mock instrument provider: a factory that yields a MockInstrument with a
// descriptor-independent known latency, and reports that latency up front for
// the compiler PDC query without instantiating.
class MockInstrumentProvider final : public InstrumentProvider {
 public:
  static constexpr int kLatency = 48;

  bool can_create(const PluginDescriptor& descriptor) const noexcept override {
    return descriptor.format == "mock" &&
           (descriptor.kind == PluginKind::kInstrument || descriptor.kind == PluginKind::kEffect);
  }
  std::unique_ptr<MidiInstrument> create_instrument(const PluginDescriptor& descriptor) override {
    if (!can_create(descriptor) || descriptor.kind != PluginKind::kInstrument) return nullptr;
    return std::make_unique<MockInstrument>(kLatency);
  }
  std::unique_ptr<sonare::rt::ProcessorBase> create_effect(
      const PluginDescriptor& descriptor) override {
    if (!can_create(descriptor) || descriptor.kind != PluginKind::kEffect) return nullptr;
    return std::make_unique<MockEffect>();
  }
  int latency_samples(const PluginDescriptor& descriptor) const noexcept override {
    return can_create(descriptor) ? kLatency : 0;
  }
  int tail_samples(const PluginDescriptor& descriptor) const noexcept override {
    return can_create(descriptor) && descriptor.kind == PluginKind::kEffect
               ? MockEffect::kTailSamples
               : 0;
  }
  bool supports_bypass(const PluginDescriptor& descriptor) const noexcept override {
    return can_create(descriptor);
  }
  size_t parameter_count(const PluginDescriptor& descriptor) const noexcept override {
    return can_create(descriptor) ? 2 : 0;
  }
  bool parameter_descriptor(const PluginDescriptor& descriptor, size_t index,
                            PluginParameterDescriptor* out) const noexcept override {
    if (!can_create(descriptor) || out == nullptr || index >= 2) return false;
    if (index == 0) {
      *out = PluginParameterDescriptor{
          10u, "Cutoff", PluginParameterUnit::kHertz, 1000.0f, 20.0f, 20000.0f, true, true};
    } else {
      *out = PluginParameterDescriptor{
          11u, "Oversampling", PluginParameterUnit::kBoolean, 0.0f, 0.0f, 1.0f, false, false};
    }
    return true;
  }
  // Q8 latency carries a fractional sub-sample (kLatency and a half sample).
  int latency_samples_q8(const PluginDescriptor& descriptor) const noexcept override {
    return can_create(descriptor) ? (kLatency << 8) + 128 : 0;
  }
  // Port 1 (an aux out) is zero-latency; all other ports share the main figure.
  int output_latency_samples_q8(const PluginDescriptor& descriptor,
                                int output_port) const noexcept override {
    if (!can_create(descriptor)) return 0;
    if (output_port == 1) return 0;
    return latency_samples_q8(descriptor);
  }
  size_t preset_count(const PluginDescriptor& descriptor) const noexcept override {
    return can_create(descriptor) ? 2 : 0;
  }
  bool preset_descriptor(const PluginDescriptor& descriptor, size_t index,
                         PluginPresetDescriptor* out) const noexcept override {
    if (!can_create(descriptor) || out == nullptr || index >= 2) return false;
    *out = index == 0 ? PluginPresetDescriptor{0u, "Init"} : PluginPresetDescriptor{1u, "Warm Pad"};
    return true;
  }
  size_t bus_count(const PluginDescriptor& descriptor) const noexcept override {
    if (!can_create(descriptor)) return 0;
    return descriptor.kind == PluginKind::kInstrument ? 1 : 3;
  }
  bool bus_descriptor(const PluginDescriptor& descriptor, size_t index,
                      PluginBusDescriptor* out) const noexcept override {
    if (!can_create(descriptor) || out == nullptr || index >= bus_count(descriptor)) return false;
    if (descriptor.kind == PluginKind::kInstrument) {
      *out = PluginBusDescriptor{PluginBusRole::kMainOutput, 0u, "Main Out", 1u, 2u, 2u, true};
      return true;
    }
    if (index == 0) {
      *out = PluginBusDescriptor{PluginBusRole::kMainInput, 0u, "Main In", 1u, 2u, 2u, true};
    } else if (index == 1) {
      *out = PluginBusDescriptor{PluginBusRole::kMainOutput, 0u, "Main Out", 1u, 2u, 2u, true};
    } else {
      *out = PluginBusDescriptor{PluginBusRole::kSidechainInput, 0u, "Key", 1u, 2u, 1u, false};
    }
    return true;
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
  REQUIRE(provider.parameter_count(desc) == 2);
  PluginParameterDescriptor param;
  REQUIRE(provider.parameter_descriptor(desc, 0, &param));
  REQUIRE(param.id == 10u);
  REQUIRE(param.name == "Cutoff");
  REQUIRE(param.unit == PluginParameterUnit::kHertz);
  REQUIRE(param.default_value == 1000.0f);
  REQUIRE(param.min_value == 20.0f);
  REQUIRE(param.max_value == 20000.0f);
  REQUIRE(param.automatable);
  REQUIRE(param.realtime_safe);
  REQUIRE(provider.parameter_descriptor(desc, 1, &param));
  REQUIRE(param.id == 11u);
  REQUIRE(param.unit == PluginParameterUnit::kBoolean);
  REQUIRE_FALSE(param.automatable);
  REQUIRE_FALSE(param.realtime_safe);
  REQUIRE_FALSE(provider.parameter_descriptor(desc, 2, &param));
  REQUIRE(provider.bus_count(desc) == 1);
  PluginBusDescriptor bus;
  REQUIRE(provider.bus_descriptor(desc, 0, &bus));
  REQUIRE(bus.role == PluginBusRole::kMainOutput);
  REQUIRE(bus.name == "Main Out");
  REQUIRE(bus.min_channels == 1);
  REQUIRE(bus.max_channels == 2);
  REQUIRE(bus.default_channels == 2);
  REQUIRE(bus.required);
  REQUIRE_FALSE(provider.bus_descriptor(desc, 1, &bus));

  // Q8 / per-port latency seam: integer floor stays the legacy value, Q8 keeps
  // the fractional sub-sample, and the aux port reports its own (zero) latency.
  REQUIRE(provider.latency_samples(desc) == MockInstrumentProvider::kLatency);
  REQUIRE(provider.latency_samples_q8(desc) == (MockInstrumentProvider::kLatency << 8) + 128);
  REQUIRE(provider.output_latency_samples_q8(desc, 0) ==
          (MockInstrumentProvider::kLatency << 8) + 128);
  REQUIRE(provider.output_latency_samples_q8(desc, 1) == 0);

  // Preset enumeration seam: names/indices only, no payload bytes.
  REQUIRE(provider.preset_count(desc) == 2);
  PluginPresetDescriptor preset;
  REQUIRE(provider.preset_descriptor(desc, 0, &preset));
  REQUIRE(preset.index == 0u);
  REQUIRE(preset.name == "Init");
  REQUIRE(provider.preset_descriptor(desc, 1, &preset));
  REQUIRE(preset.index == 1u);
  REQUIRE(preset.name == "Warm Pad");
  REQUIRE_FALSE(provider.preset_descriptor(desc, 2, &preset));

  auto inst = provider.create_instrument(desc);
  REQUIRE(inst != nullptr);
  REQUIRE(inst->latency_samples() == MockInstrumentProvider::kLatency);

  PluginDescriptor effect = desc;
  effect.kind = PluginKind::kEffect;
  REQUIRE(provider.can_create(effect));
  REQUIRE(provider.bus_count(effect) == 3);
  REQUIRE(provider.bus_descriptor(effect, 0, &bus));
  REQUIRE(bus.role == PluginBusRole::kMainInput);
  REQUIRE(bus.default_channels == 2);
  REQUIRE(provider.bus_descriptor(effect, 1, &bus));
  REQUIRE(bus.role == PluginBusRole::kMainOutput);
  REQUIRE(provider.bus_descriptor(effect, 2, &bus));
  REQUIRE(bus.role == PluginBusRole::kSidechainInput);
  REQUIRE(bus.name == "Key");
  REQUIRE(bus.default_channels == 1);
  REQUIRE_FALSE(bus.required);
  REQUIRE(provider.tail_samples(effect) == MockEffect::kTailSamples);
  REQUIRE(provider.supports_bypass(effect));
  auto effect_instance = provider.create_effect(effect);
  REQUIRE(effect_instance != nullptr);
  REQUIRE(effect_instance->tail_samples() == MockEffect::kTailSamples);
  REQUIRE_FALSE(effect_instance->bypassed());
  REQUIRE(effect_instance->set_bypassed(true, true));
  REQUIRE(effect_instance->bypassed());
  auto* mock_effect = dynamic_cast<MockEffect*>(effect_instance.get());
  REQUIRE(mock_effect != nullptr);
  REQUIRE(mock_effect->reset_count_ == 1);
  REQUIRE(effect_instance->set_bypassed(false));
  REQUIRE_FALSE(effect_instance->bypassed());

  // Unsupported descriptor → null + zero latency.
  PluginDescriptor other = desc;
  other.format = "vst3";
  REQUIRE_FALSE(provider.can_create(other));
  REQUIRE(provider.create_instrument(other) == nullptr);
  REQUIRE(provider.latency_samples(other) == 0);
  REQUIRE(provider.parameter_count(other) == 0);
  REQUIRE_FALSE(provider.parameter_descriptor(other, 0, &param));
  REQUIRE(provider.bus_count(other) == 0);
  REQUIRE_FALSE(provider.bus_descriptor(other, 0, &bus));
  REQUIRE(provider.tail_samples(other) == 0);
  REQUIRE_FALSE(provider.supports_bypass(other));
  REQUIRE(provider.latency_samples_q8(other) == 0);
  REQUIRE(provider.output_latency_samples_q8(other, 0) == 0);
  REQUIRE(provider.preset_count(other) == 0);
  REQUIRE_FALSE(provider.preset_descriptor(other, 0, &preset));
}

TEST_CASE("instrument provider base defaults thread Q8 latency and expose no presets", "[host]") {
  // A provider that overrides only the integer latency must still surface a
  // consistent Q8 value (floor << 8) and per-port latency (shared) by default,
  // and expose no presets.
  struct IntLatencyProvider final : public InstrumentProvider {
    bool can_create(const PluginDescriptor&) const noexcept override { return true; }
    std::unique_ptr<MidiInstrument> create_instrument(const PluginDescriptor&) override {
      return nullptr;
    }
    int latency_samples(const PluginDescriptor&) const noexcept override { return 64; }
  } prov;
  PluginDescriptor d;
  REQUIRE(prov.latency_samples(d) == 64);
  REQUIRE(prov.latency_samples_q8(d) == (64 << 8));
  REQUIRE(prov.output_latency_samples_q8(d, 0) == (64 << 8));
  REQUIRE(prov.output_latency_samples_q8(d, 3) == (64 << 8));
  REQUIRE(prov.preset_count(d) == 0);
  PluginPresetDescriptor preset;
  REQUIRE_FALSE(prov.preset_descriptor(d, 0, &preset));
}

TEST_CASE("audio device seam reports I/O latency for PDC alignment", "[host]") {
  // The negotiated config carries hardware latency; default is 0 (unknown).
  AudioStreamConfig cfg;
  REQUIRE(cfg.input_latency_samples == 0);
  REQUIRE(cfg.output_latency_samples == 0);
  cfg.input_latency_samples = 96;
  cfg.output_latency_samples = 128;
  REQUIRE(cfg.input_latency_samples == 96);
  REQUIRE(cfg.output_latency_samples == 128);

  // A backend may report the true (post-open, driver-rounded) latency via the
  // device accessors; a backend that cannot report leaves them at the 0 default.
  struct LatencyDevice final : public sonare::host::AudioDevice {
    bool open(const AudioStreamConfig&, AudioDeviceCallback*) override { return true; }
    bool start() override { return true; }
    void stop() noexcept override {}
    void close() noexcept override {}
    bool is_running() const noexcept override { return false; }
    int input_latency_samples() const noexcept override { return 96; }
    int output_latency_samples() const noexcept override { return 128; }
  } device;
  REQUIRE(device.input_latency_samples() == 96);
  REQUIRE(device.output_latency_samples() == 128);

  struct PlainDevice final : public sonare::host::AudioDevice {
    bool open(const AudioStreamConfig&, AudioDeviceCallback*) override { return true; }
    bool start() override { return true; }
    void stop() noexcept override {}
    void close() noexcept override {}
    bool is_running() const noexcept override { return false; }
  } plain;
  REQUIRE(plain.input_latency_samples() == 0);
  REQUIRE(plain.output_latency_samples() == 0);
}

TEST_CASE("audio device seam describes input/output buses and reports xruns", "[host]") {
  // The device can declare its buses with explicit direction (input vs output)
  // rather than leaving the host to infer them from a flat channel count.
  struct BusDevice final : public sonare::host::AudioDevice {
    bool open(const AudioStreamConfig&, AudioDeviceCallback*) override { return true; }
    bool start() override { return true; }
    void stop() noexcept override {}
    void close() noexcept override {}
    bool is_running() const noexcept override { return false; }
    size_t bus_count() const noexcept override { return 2; }
    bool bus_descriptor(size_t index, AudioBusDescriptor* out) const noexcept override {
      if (out == nullptr || index >= 2) return false;
      if (index == 0) {
        *out = AudioBusDescriptor{AudioBusDirection::kInput, 0, 1, true};
      } else {
        *out = AudioBusDescriptor{AudioBusDirection::kOutput, 0, 2, true};
      }
      return true;
    }
    uint32_t xrun_count() const noexcept override { return 7; }
  } device;

  REQUIRE(device.bus_count() == 2);
  AudioBusDescriptor bus;
  REQUIRE(device.bus_descriptor(0, &bus));
  REQUIRE(bus.direction == AudioBusDirection::kInput);
  REQUIRE(bus.channel_count == 1);
  REQUIRE(bus.is_main);
  REQUIRE(device.bus_descriptor(1, &bus));
  REQUIRE(bus.direction == AudioBusDirection::kOutput);
  REQUIRE(bus.channel_count == 2);
  REQUIRE_FALSE(device.bus_descriptor(2, &bus));
  REQUIRE(device.xrun_count() == 7);

  // A device with no bus model and no xrun detection keeps the safe defaults.
  struct PlainDevice final : public sonare::host::AudioDevice {
    bool open(const AudioStreamConfig&, AudioDeviceCallback*) override { return true; }
    bool start() override { return true; }
    void stop() noexcept override {}
    void close() noexcept override {}
    bool is_running() const noexcept override { return false; }
  } plain;
  REQUIRE(plain.bus_count() == 0);
  REQUIRE_FALSE(plain.bus_descriptor(0, &bus));
  REQUIRE(plain.xrun_count() == 0);

  // The render callback receives the per-callback xrun delta on AudioCallbackTime.
  AudioCallbackTime time;
  REQUIRE(time.input_xruns == 0);
  time.input_xruns = 3;
  AudioBufferView view;
  view.time = time;
  REQUIRE(view.time.input_xruns == 3);
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

TEST_CASE("fixed MIDI input drain clamps negative offsets and sorts in block", "[host]") {
  FixedMidiInputSource<4> input;
  const Ump late = sonare::midi::make_midi1_note_on(0, 0, 64, 100);
  const Ump early = sonare::midi::make_midi1_note_on(0, 0, 60, 100);
  const Ump middle = sonare::midi::make_midi1_note_on(0, 0, 62, 100);

  REQUIRE(input.push_event(late, 12));
  REQUIRE(input.push_event(early, -5));
  REQUIRE(input.push_event(middle, 4));

  std::array<MidiEvent, 4> drained{};
  REQUIRE(input.drain(drained.data(), drained.size(), 1000) == 3);
  REQUIRE(drained[0].render_frame == 1000);
  REQUIRE(drained[0].ump.note_number() == 60);
  REQUIRE(drained[1].render_frame == 1004);
  REQUIRE(drained[1].ump.note_number() == 62);
  REQUIRE(drained[2].render_frame == 1012);
  REQUIRE(drained[2].ump.note_number() == 64);
}

TEST_CASE("fixed MIDI input drain_block clamps offsets to block bounds", "[host]") {
  FixedMidiInputSource<4> input;
  const Ump early = sonare::midi::make_midi1_note_on(0, 0, 60, 100);
  const Ump in_block = sonare::midi::make_midi1_note_on(0, 0, 62, 100);
  const Ump late = sonare::midi::make_midi1_note_on(0, 0, 64, 100);

  REQUIRE(input.push_event(late, 99));
  REQUIRE(input.push_event(early, -8));
  REQUIRE(input.push_event(in_block, 7));

  std::array<MidiEvent, 4> drained{};
  REQUIRE(input.drain_block(drained.data(), drained.size(), 2000, 16) == 3);
  REQUIRE(drained[0].render_frame == 2000);
  REQUIRE(drained[0].ump.note_number() == 60);
  REQUIRE(drained[1].render_frame == 2007);
  REQUIRE(drained[1].ump.note_number() == 62);
  REQUIRE(drained[2].render_frame == 2015);
  REQUIRE(drained[2].ump.note_number() == 64);
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
// H-3: opaque instance-state save / restore seam.
// ===========================================================================

namespace {
// An instrument carrying a single opaque scalar of state, serialized as raw
// bytes through the rt::ProcessorBase save_state / load_state seam.
class StatefulInstrument final : public MidiInstrument {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  void on_event(uint32_t, const MidiEvent&) noexcept override {}

  bool save_state(std::vector<uint8_t>& out) const override {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value_);
    out.insert(out.end(), bytes, bytes + sizeof(value_));
    return true;
  }
  bool load_state(const uint8_t* data, size_t len) override {
    if (data == nullptr || len != sizeof(value_)) return false;
    std::memcpy(&value_, data, sizeof(value_));
    return true;
  }

  int32_t value_ = 0;
};

// A provider that builds StatefulInstruments, used to prove the default
// create_instrument(descriptor, state, len) rehydrates from a saved blob.
class StatefulProvider final : public InstrumentProvider {
 public:
  bool can_create(const PluginDescriptor& d) const noexcept override {
    return d.format == "stateful";
  }
  std::unique_ptr<MidiInstrument> create_instrument(const PluginDescriptor& d) override {
    return can_create(d) ? std::make_unique<StatefulInstrument>() : nullptr;
  }
};
}  // namespace

TEST_CASE("instrument opaque state round-trips through the host save/restore seam", "[host]") {
  StatefulProvider provider;
  PluginDescriptor desc;
  desc.format = "stateful";

  // Author an instrument, mutate its state, and serialize it opaquely.
  auto authored = provider.create_instrument(desc);
  REQUIRE(authored != nullptr);
  static_cast<StatefulInstrument*>(authored.get())->value_ = 0x0BADF00D;
  std::vector<uint8_t> blob;
  REQUIRE(authored->save_state(blob));
  REQUIRE(blob.size() == sizeof(int32_t));

  // Rehydrate a fresh instance from the blob via the provider's default
  // state-taking create overload.
  auto restored = provider.create_instrument_with_state(desc, blob.data(), blob.size());
  REQUIRE(restored != nullptr);
  REQUIRE(static_cast<StatefulInstrument*>(restored.get())->value_ == 0x0BADF00D);

  // A null/empty state span yields a default instance (no rehydration).
  auto fresh = provider.create_instrument_with_state(desc, nullptr, 0);
  REQUIRE(fresh != nullptr);
  REQUIRE(static_cast<StatefulInstrument*>(fresh.get())->value_ == 0);

  // The default ProcessorBase is stateless: save_state reports false.
  MockInstrument stateless(0);
  std::vector<uint8_t> empty;
  REQUIRE_FALSE(stateless.save_state(empty));
  REQUIRE(empty.empty());
}

// ===========================================================================
// H-4: per-block transport sync pushed to the hosted instrument.
// ===========================================================================

namespace {
// Records the most recent transport snapshot the engine pushed before process().
class TransportProbeInstrument final : public MidiInstrument {
 public:
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  void on_event(uint32_t, const MidiEvent&) noexcept override {}
  void set_transport(const sonare::transport::TransportState& state) noexcept override {
    last_ = state;
    ++push_count_;
  }
  sonare::transport::TransportState last_{};
  int push_count_ = 0;
};
}  // namespace

TEST_CASE("engine pushes per-block transport to the hosted instrument", "[host]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 128;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  TransportProbeInstrument probe;
  engine.set_midi_instrument(&probe);
  engine.set_tempo(120.0);

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
  float* io[] = {l.data(), r.data()};
  engine.process(io, 2, kBlock);

  // The instrument was handed a rolling transport carrying the engine tempo.
  REQUIRE(probe.push_count_ >= 1);
  REQUIRE(probe.last_.playing);
  REQUIRE(probe.last_.bpm == Catch::Approx(120.0));
  REQUIRE(probe.last_.sample_rate == Catch::Approx(kSr));

  engine.set_midi_instrument(nullptr);
}

// ===========================================================================
// M-29: per-destination multi-instrument routing through the engine rack.
// ===========================================================================

TEST_CASE("engine routes MIDI to the instrument bound to each destination", "[host]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 256;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(kSr, kBlock);

  MockInstrument inst_a(0);
  MockInstrument inst_b(0);
  REQUIRE(engine.set_midi_instrument(7u, &inst_a));
  REQUIRE(engine.set_midi_instrument(9u, &inst_b));
  REQUIRE(engine.midi_instrument_count() == 2);
  REQUIRE(engine.midi_instrument(7u) == &inst_a);
  REQUIRE(engine.midi_instrument(9u) == &inst_b);

  // Two clips on distinct destinations; each note must reach only its instrument.
  sonare::midi::MidiClipSchedule clip_a;
  clip_a.id = 1;
  clip_a.start_sample = 0;
  clip_a.length_samples = kBlock;
  clip_a.destination_id = 7u;
  clip_a.events.push_back(MidiEvent{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)});
  clip_a.events.push_back(MidiEvent{kBlock - 1, sonare::midi::make_midi1_note_off(0, 0, 60, 0)});
  sonare::midi::MidiClipSchedule clip_b = clip_a;
  clip_b.id = 2;
  clip_b.destination_id = 9u;
  engine.set_midi_clips({clip_a, clip_b});

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));

  std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
  float* io[] = {l.data(), r.data()};
  engine.process(io, 2, kBlock);

  // Each instrument received exactly its own destination's events (note-on +
  // note-off), never the other's.
  REQUIRE(inst_a.note_on_count_ == 1);
  REQUIRE(inst_b.note_on_count_ == 1);
  REQUIRE(inst_a.received_events_ == 2);
  REQUIRE(inst_b.received_events_ == 2);

  engine.set_midi_instrument(7u, nullptr);
  engine.set_midi_instrument(9u, nullptr);
  REQUIRE(engine.midi_instrument_count() == 0);
}

// ===========================================================================
// M-1: swapping/clearing an instrument releases its sounding notes (no hang).
// ===========================================================================

TEST_CASE("clearing a bound instrument flushes its sounding notes", "[host]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 256;
  sonare::engine::RealtimeEngine engine;
  engine.prepare(kSr, kBlock);
  MockInstrument inst(0);
  engine.set_midi_instrument(3u, &inst);

  // A held note on destination 3: note-on now, note-off far in the future.
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.start_sample = 0;
  clip.length_samples = 1 << 20;
  clip.destination_id = 3u;
  clip.events.push_back(MidiEvent{0, sonare::midi::make_midi1_note_on(0, 0, 64, 100)});
  clip.events.push_back(MidiEvent{1 << 19, sonare::midi::make_midi1_note_off(0, 0, 64, 0)});
  engine.set_midi_clips({clip});

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
  float* io[] = {l.data(), r.data()};
  engine.process(io, 2, kBlock);
  REQUIRE(inst.note_on_count_ == 1);
  REQUIRE(engine.midi_sequencer().active_note_count() == 1);

  // Clearing the destination must release the sounding note (note-off dispatched
  // to the outgoing instrument) before unbinding, leaving no hanging note.
  engine.set_midi_instrument(3u, nullptr);
  REQUIRE(inst.note_off_count_ >= 1);
  REQUIRE(engine.midi_sequencer().active_note_count() == 0);
}

// ===========================================================================
// Rack capacity: a new binding past kMaxInstruments fails without disturbing
// existing bindings.
// ===========================================================================

TEST_CASE("instrument rack rejects bindings past capacity", "[host]") {
  sonare::engine::RealtimeEngine engine;
  engine.prepare(48000.0, 64);
  std::vector<std::unique_ptr<MockInstrument>> insts;
  for (size_t i = 0; i < sonare::engine::InstrumentRack::kMaxInstruments; ++i) {
    insts.push_back(std::make_unique<MockInstrument>(0));
    REQUIRE(engine.set_midi_instrument(static_cast<uint32_t>(i + 1), insts.back().get()));
  }
  MockInstrument overflow(0);
  REQUIRE_FALSE(engine.set_midi_instrument(9999u, &overflow));
  REQUIRE(engine.midi_instrument_count() == sonare::engine::InstrumentRack::kMaxInstruments);
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
