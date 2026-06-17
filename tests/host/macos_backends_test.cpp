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
  device.close();
  REQUIRE(callback.callbacks_.load() > 0);
  REQUIRE(device.output_latency_samples() >= 0);
}
#endif  // SONARE_HOST_TEST_COREAUDIO
