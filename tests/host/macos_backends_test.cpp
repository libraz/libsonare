/// @file macos_backends_test.cpp
/// @brief Tests for the macOS host backends (CoreAudio / CoreMIDI / AU host).
///
/// Built only when the matching BUILD_* option is on (the file is compiled into
/// sonare_tests with a per-backend define). Tests that touch a live device,
/// endpoint or a system Audio Unit are tagged "[.]" so they are excluded from
/// the default ctest run and only execute when named explicitly; they are
/// inherently hardware / environment sensitive.

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "midi/midi_event.h"
#include "midi/ump.h"

#if defined(SONARE_HOST_TEST_COREMIDI)
#include "host/backends/coremidi/coremidi_io.h"

TEST_CASE("CoreMIDI input buffers and drains UMP records", "[host][coremidi]") {
  using sonare::host::backends::CoreMidiInput;
  CoreMidiInput input;  // not opened to any endpoint; exercise the seam buffer

  const sonare::midi::Ump note = sonare::midi::make_midi1_note_on(0, 0, 60, 100);
  REQUIRE(input.push_event(note, 0));
  REQUIRE(input.pending_count() == 1);

  std::array<sonare::midi::MidiEvent, 8> out{};
  const size_t drained = input.drain(out.data(), out.size(), 1000);
  REQUIRE(drained == 1);
  REQUIRE(out[0].render_frame == 1000);
  REQUIRE(out[0].ump.is_note_on());
  REQUIRE(out[0].ump.note_number() == 60);
  REQUIRE(input.pending_count() == 0);
}

TEST_CASE("CoreMIDI output queues RT-safe and holds without a device", "[host][coremidi]") {
  using sonare::host::backends::CoreMidiOutput;
  CoreMidiOutput output;  // not opened: flush must be a no-op, queue retained

  const sonare::midi::Ump note = sonare::midi::make_midi1_note_off(0, 0, 60, 0);
  REQUIRE(output.send(sonare::midi::MidiEvent{0, note}));
  REQUIRE(output.queued_count() == 1);
  REQUIRE(output.flush_output() == 0);  // no destination connected
  REQUIRE(output.queued_count() == 1);  // unflushed events stay queued
}

TEST_CASE("CoreMIDI output queues a SysEx-handle event against an attached store",
          "[host][coremidi]") {
  using sonare::host::backends::CoreMidiOutput;
  sonare::midi::SysExStore store;
  const std::vector<uint8_t> payload = {0xF0u, 0x7Eu, 0x7Fu, 0x09u, 0x01u, 0xF7u};
  const sonare::midi::SysExHandle handle = store.add(payload);
  REQUIRE(handle != 0);

  CoreMidiOutput output;
  output.set_sysex_store(&store);  // control-thread wiring; resolved at flush time
  // A SysEx-handle UMP is RT-safe to enqueue (fixed size); resolution to SysEx7
  // packets happens in flush_output against the store. Without a device flush is
  // a no-op, but the handle must still queue rather than being dropped at send.
  const sonare::midi::Ump sx = sonare::midi::make_sysex_handle(/*group=*/0, handle);
  REQUIRE(output.send(sonare::midi::MidiEvent{0, sx}));
  REQUIRE(output.queued_count() == 1);
  REQUIRE(output.flush_output() == 0);  // no destination connected
  REQUIRE(output.queued_count() == 1);  // retained for a later flush
}

TEST_CASE("CoreMIDI live endpoint round-trip", "[host][coremidi][.]") {
  using sonare::host::backends::CoreMidiInput;
  using sonare::host::backends::CoreMidiOutput;
  // Requires at least one source and destination present on the host.
  if (CoreMidiInput::source_count() == 0 || CoreMidiOutput::destination_count() == 0) {
    SUCCEED("no CoreMIDI endpoints present; skipping live round-trip");
    return;
  }
  CoreMidiOutput output;
  REQUIRE(output.open(0));
  REQUIRE(output.send(sonare::midi::MidiEvent{0, sonare::midi::make_midi1_note_on(0, 0, 64, 90)}));
  REQUIRE(output.flush_output() == 1);

  // A SysEx-handle event resolves through the attached store and flushes as one
  // source event (expanded into SysEx7 packets under the hood) rather than being
  // silently dropped.
  sonare::midi::SysExStore store;
  const std::vector<uint8_t> payload = {0xF0u, 0x7Eu, 0x7Fu, 0x09u, 0x01u, 0xF7u};
  const sonare::midi::SysExHandle handle = store.add(payload);
  output.set_sysex_store(&store);
  REQUIRE(output.send(sonare::midi::MidiEvent{0, sonare::midi::make_sysex_handle(0, handle)}));
  REQUIRE(output.flush_output() == 1);
  output.close();
}
#endif  // SONARE_HOST_TEST_COREMIDI

#if defined(SONARE_HOST_TEST_AU)
#include "host/backends/plughost/au_instrument_provider.h"

TEST_CASE("AU host enumerates and renders a system instrument", "[host][au][.]") {
  using sonare::host::backends::AuInstrumentProvider;
  const auto instruments = AuInstrumentProvider::enumerate(sonare::host::PluginKind::kInstrument);
  if (instruments.empty()) {
    SUCCEED("no Audio Unit instruments installed; skipping render");
    return;
  }
  AuInstrumentProvider provider;
  const auto& desc = instruments.front();
  REQUIRE(provider.can_create(desc));

  auto instrument = provider.create_instrument(desc);
  REQUIRE(instrument != nullptr);
  instrument->prepare(48000.0, 512);

  // A note-on at block start, then render a block: output must stay finite.
  instrument->on_event(0,
                       sonare::midi::MidiEvent{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)});
  std::array<float, 512> left{};
  std::array<float, 512> right{};
  std::array<float*, 2> channels{left.data(), right.data()};
  instrument->process(channels.data(), 2, 512);
  for (float s : left) REQUIRE(std::isfinite(s));
  REQUIRE(instrument->latency_samples() >= 0);
}

TEST_CASE("AU host renders mono then stereo through per-call channel negotiation",
          "[host][au][.]") {
  // The adapter used to hardcode a 2-channel stream format at prepare(). A host
  // rendering a single channel then pointed a 1-buffer list at an AU whose format
  // still claimed two planar buffers. This exercises the new per-call
  // negotiation: rendering mono re-negotiates the AU to one channel, then
  // switching to stereo re-negotiates again. Both must stay finite and neither
  // may crash (an AU produces well-defined finite output only when its format
  // matches the buffer list the host supplies).
  using sonare::host::backends::AuInstrumentProvider;
  const auto instruments = AuInstrumentProvider::enumerate(sonare::host::PluginKind::kInstrument);
  if (instruments.empty()) {
    SUCCEED("no Audio Unit instruments installed; skipping mono render");
    return;
  }
  AuInstrumentProvider provider;
  auto instrument = provider.create_instrument(instruments.front());
  REQUIRE(instrument != nullptr);
  instrument->prepare(48000.0, 512);
  instrument->on_event(0,
                       sonare::midi::MidiEvent{0, sonare::midi::make_midi1_note_on(0, 0, 60, 100)});

  // Render mono (re-negotiates from the stereo default), then switch back to
  // stereo (re-negotiates again): both must stay finite.
  std::array<float, 512> mono{};
  std::array<float*, 1> mono_channels{mono.data()};
  instrument->process(mono_channels.data(), 1, 512);
  for (float s : mono) REQUIRE(std::isfinite(s));

  std::array<float, 512> left{};
  std::array<float, 512> right{};
  std::array<float*, 2> stereo{left.data(), right.data()};
  instrument->process(stereo.data(), 2, 512);
  for (float s : left) REQUIRE(std::isfinite(s));
  for (float s : right) REQUIRE(std::isfinite(s));
}

TEST_CASE("AU host parameter enumeration is consistent across the cached instance",
          "[host][au][.]") {
  // parameter_count / parameter_descriptor reuse one cached AU instance per
  // descriptor. Correctness must be identical to instantiating fresh each call:
  // repeated queries return the same count and per-index descriptors.
  using sonare::host::backends::AuInstrumentProvider;
  const auto instruments = AuInstrumentProvider::enumerate(sonare::host::PluginKind::kInstrument);
  AuInstrumentProvider provider;

  const sonare::host::PluginDescriptor* with_params = nullptr;
  for (const auto& d : instruments) {
    if (provider.parameter_count(d) > 0) {
      with_params = &d;
      break;
    }
  }
  if (with_params == nullptr) {
    SUCCEED("no AU instrument with parameters installed; skipping cache-consistency check");
    return;
  }

  const size_t count = provider.parameter_count(*with_params);
  REQUIRE(provider.parameter_count(*with_params) == count);  // stable across calls

  sonare::host::PluginParameterDescriptor first{};
  REQUIRE(provider.parameter_descriptor(*with_params, 0, &first));
  sonare::host::PluginParameterDescriptor first_again{};
  REQUIRE(provider.parameter_descriptor(*with_params, 0, &first_again));
  REQUIRE(first.id == first_again.id);
  REQUIRE(first.min_value == first_again.min_value);
  REQUIRE(first.max_value == first_again.max_value);
  REQUIRE(first.default_value == first_again.default_value);

  // An out-of-range index must fail cleanly (cache stays valid afterwards).
  sonare::host::PluginParameterDescriptor oob{};
  REQUIRE_FALSE(provider.parameter_descriptor(*with_params, count, &oob));
  REQUIRE(provider.parameter_count(*with_params) == count);
}
#endif  // SONARE_HOST_TEST_AU

#if defined(SONARE_HOST_TEST_COREAUDIO)
#include "host/audio_device.h"
#include "host/backends/coreaudio/coreaudio_device.h"

namespace {
// Emits a quiet sine so the device has well-defined finite output.
class SineCallback final : public sonare::host::AudioDeviceCallback {
 public:
  bool open(const sonare::host::AudioStreamConfig& config) override {
    config_ = config;
    return true;
  }
  void render(const sonare::host::AudioBufferView& buffers) noexcept override {
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    for (int c = 0; c < buffers.num_output_channels; ++c) {
      for (int i = 0; i < buffers.num_frames; ++i) {
        phase_ += 0.01f;
        buffers.outputs[c][i] = 0.05f * std::sin(phase_);
      }
    }
  }
  void close() noexcept override {}

  sonare::host::AudioStreamConfig config_{};
  std::atomic<int> callbacks_{0};
  float phase_ = 0.0f;
};
}  // namespace

TEST_CASE("CoreAudio opens the default output device", "[host][coreaudio][.]") {
  using sonare::host::backends::CoreAudioDevice;
  sonare::host::AudioStreamConfig config;
  config.sample_rate = 48000.0;
  config.max_block_size = 512;
  config.num_output_channels = 2;

  CoreAudioDevice device;
  SineCallback callback;
  if (!device.open(config, &callback)) {
    SUCCEED("no default output device available; skipping");
    return;
  }
  REQUIRE(device.start());
  REQUIRE(device.is_running());
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  device.stop();
  REQUIRE_FALSE(device.is_running());

  // xrun telemetry: a clean run on an idle device must not report a per-callback
  // false positive (a broken sample-clock check would flag nearly every block).
  // Allow a small margin for a genuine startup discontinuity on a busy host.
  const uint32_t xruns = device.xrun_count();
  const int callbacks = callback.callbacks_.load();
  REQUIRE(callbacks > 0);
  REQUIRE(xruns <= static_cast<uint32_t>(callbacks) / 4u + 1u);

  device.close();
  REQUIRE(device.output_latency_samples() >= 0);
}
#endif  // SONARE_HOST_TEST_COREAUDIO
