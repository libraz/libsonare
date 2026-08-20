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

TEST_CASE("CoreMIDI input reassembles SysEx7 into a host-store handle", "[host][coremidi]") {
  using sonare::host::backends::CoreMidiInput;
  CoreMidiInput input;
  sonare::midi::Ump start{};
  start.word_count = 2;
  start.group = 3;
  start.words[0] = (0x3u << 28u) | (3u << 24u) | (1u << 20u) | (2u << 16u) | (0x01u << 8u) | 0x02u;
  sonare::midi::Ump end{};
  end.word_count = 2;
  end.group = 3;
  end.words[0] = (0x3u << 28u) | (3u << 24u) | (3u << 20u) | (1u << 16u) | (0x03u << 8u);

  REQUIRE(input.push_event(start, 0));
  REQUIRE(input.pending_count() == 0);
  REQUIRE(input.push_event(end, 0));
  std::array<sonare::midi::MidiEvent, 2> out{};
  REQUIRE(input.drain(out.data(), out.size(), 0) == 1);
  REQUIRE(out[0].ump.sysex_handle != 0);
  const auto* payload = input.sysex_store()->lookup(out[0].ump.sysex_handle);
  REQUIRE(payload != nullptr);
  REQUIRE(*payload == std::vector<uint8_t>{0xF0u, 0x01u, 0x02u, 0x03u, 0xF7u});
  REQUIRE(input.sysex_overflow_count() == 0);
  REQUIRE(input.sysex_interleave_count() == 0);
}

TEST_CASE("CoreMIDI input preserves the caller's timestamp for a SysEx completed via push_event",
          "[host][coremidi]") {
  using sonare::host::backends::CoreMidiInput;
  // consume_sysex7() has no MIDIEventPacket to derive a timestamp from when
  // reached through the public push_event() API (unlike the live callback
  // path), so it must fall back to the port_time_samples the caller actually
  // passed rather than silently stamping the reassembled message at offset 0.
  CoreMidiInput input;
  sonare::midi::Ump start{};
  start.word_count = 2;
  start.group = 3;
  start.words[0] = (0x3u << 28u) | (3u << 24u) | (1u << 20u) | (2u << 16u) | (0x01u << 8u) | 0x02u;
  sonare::midi::Ump end{};
  end.word_count = 2;
  end.group = 3;
  end.words[0] = (0x3u << 28u) | (3u << 24u) | (3u << 20u) | (1u << 16u) | (0x03u << 8u);

  REQUIRE(input.push_event(start, 999));  // Start does not complete the message: ignored
  REQUIRE(input.push_event(end, 37));     // End completes it at this offset
  std::array<sonare::midi::MidiEvent, 2> out{};
  REQUIRE(input.drain(out.data(), out.size(), /*block_start_frame=*/1000) == 1);
  REQUIRE(out[0].render_frame == 1037);  // block_start_frame + the End event's port_time_samples
  REQUIRE(out[0].ump.sysex_handle != 0);
}

TEST_CASE("CoreMIDI input masks an out-of-range UMP group before indexing SysEx state",
          "[host][coremidi]") {
  using sonare::host::backends::CoreMidiInput;
  // group is a 4-bit UMP field (0-15). push_event() is a public entry point
  // that accepts any caller-constructed Ump — host/midi_io.h documents a full
  // internal buffer as its ONLY failure mode, nothing about invalid groups —
  // so a value outside that range must not become an unmasked index into the
  // 16-slot SysEx reassembly array. The wire-format group nibble embedded in
  // words[0] is left valid (3); only the separate .group field, the one
  // actually used to index, is set out of range.
  CoreMidiInput input;
  sonare::midi::Ump start{};
  start.word_count = 2;
  start.group = 19;  // 19 & 0x0F == 3, but 19 itself is out of the 16-slot range
  start.words[0] = (0x3u << 28u) | (3u << 24u) | (1u << 20u) | (2u << 16u) | (0x01u << 8u) | 0x02u;
  sonare::midi::Ump end{};
  end.word_count = 2;
  end.group = 19;
  end.words[0] = (0x3u << 28u) | (3u << 24u) | (3u << 20u) | (1u << 16u) | (0x03u << 8u);

  REQUIRE(input.push_event(start, 0));
  REQUIRE(input.pending_count() == 0);
  REQUIRE(input.push_event(end, 0));
  std::array<sonare::midi::MidiEvent, 2> out{};
  REQUIRE(input.drain(out.data(), out.size(), 0) == 1);
  REQUIRE(out[0].ump.sysex_handle != 0);
  const auto* payload = input.sysex_store()->lookup(out[0].ump.sysex_handle);
  REQUIRE(payload != nullptr);
  REQUIRE(*payload == std::vector<uint8_t>{0xF0u, 0x01u, 0x02u, 0x03u, 0xF7u});
  REQUIRE(input.sysex_overflow_count() == 0);
  REQUIRE(input.sysex_interleave_count() == 0);
}

TEST_CASE("CoreMIDI input maps host timestamps onto absolute render frames", "[host][coremidi]") {
  sonare::host::MidiHostTimeMapper mapper;
  mapper.publish_anchor(/*host_time_ns=*/2'000'000'000u, /*render_frame=*/1000,
                        /*sample_rate=*/48'000.0);

  sonare::host::backends::CoreMidiInput input;
  input.set_time_mapper(&mapper);
  const sonare::midi::Ump note = sonare::midi::make_midi1_note_on(0, 0, 67, 100);
  REQUIRE(input.push_event_at_host_time(note, 2'002'500'000u));

  std::array<sonare::midi::MidiEvent, 4> out{};
  REQUIRE(input.drain_block(out.data(), out.size(), 1000, 256) == 1);
  REQUIRE(out[0].render_frame == 1120);  // 2.5 ms at 48 kHz
  REQUIRE(out[0].ump.note_number() == 67);
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

TEST_CASE("CoreMIDI input orders injected events by their own timestamps", "[host][coremidi]") {
  using sonare::host::backends::CoreMidiInput;
  // Manual injection lands in a ring the live OS callback never writes, so
  // drain() has to order it on its own rather than inheriting the arrival
  // order the callback path would have imposed. Push out of timestamp order to
  // prove the drain sorts rather than replaying insertion order.
  CoreMidiInput input;
  REQUIRE(input.push_event(sonare::midi::make_midi1_note_on(0, 0, 60, 100), 90));
  REQUIRE(input.push_event(sonare::midi::make_midi1_note_on(0, 0, 62, 100), 10));
  REQUIRE(input.push_event(sonare::midi::make_midi1_note_on(0, 0, 64, 100), 50));
  REQUIRE(input.pending_count() == 3);

  std::array<sonare::midi::MidiEvent, 4> out{};
  REQUIRE(input.drain_block(out.data(), out.size(), /*block_start_frame=*/1000,
                            /*num_frames=*/128) == 3);
  REQUIRE(out[0].render_frame == 1010);
  REQUIRE(out[1].render_frame == 1050);
  REQUIRE(out[2].render_frame == 1090);
  REQUIRE(out[0].ump.note_number() == 62);
  REQUIRE(out[1].ump.note_number() == 64);
  REQUIRE(out[2].ump.note_number() == 60);
  REQUIRE(input.pending_count() == 0);
}

TEST_CASE("CoreMIDI input keeps manual injection available while a live source is connected",
          "[host][coremidi][.]") {
  using sonare::host::backends::CoreMidiInput;
  // Requires at least one source present on the host.
  if (CoreMidiInput::source_count() == 0) {
    SUCCEED("no CoreMIDI sources present; skipping live injection check");
    return;
  }
  CoreMidiInput input;
  REQUIRE(input.open(0));
  // An on-screen keyboard driven alongside a plugged-in hardware controller is
  // an ordinary configuration, so injection must not stop the moment a device
  // is connected. host/midi_io.h's single-producer invariant is satisfied by
  // giving each producer its own ring and SysEx reassembly state: the OS
  // callback owns one pair, these two entry points the other, and neither ring
  // ever has a second writer.
  const sonare::midi::Ump note = sonare::midi::make_midi1_note_on(0, 0, 60, 100);
  REQUIRE(input.push_event(note, 0));
  REQUIRE(input.push_event_at_host_time(note, 1));
  input.close();
  // close() ends the timeline both rings were stamped against, so whatever was
  // still queued is dropped rather than replayed against the next device's
  // frame numbering.
  REQUIRE(input.pending_count() == 0);
  REQUIRE(input.push_event(note, 0));
  REQUIRE(input.pending_count() == 1);
}
#endif  // SONARE_HOST_TEST_COREMIDI

#if defined(SONARE_HOST_TEST_AU)
#include "host/backends/plughost/au_instrument_provider.h"

TEST_CASE("AU effect factory rejects mismatched descriptor kind and component type", "[host][au]") {
  using sonare::host::PluginDescriptor;
  using sonare::host::PluginKind;
  using sonare::host::backends::AuInstrumentProvider;

  AuInstrumentProvider provider;
  PluginDescriptor descriptor;
  descriptor.format = "au";

  // A validly encoded effect component must still be rejected when persisted
  // metadata labels it as an instrument.
  descriptor.kind = PluginKind::kInstrument;
  descriptor.id = "61756678:00000000:00000000";  // 'aufx'
  REQUIRE(provider.can_create(descriptor));
  REQUIRE(provider.create_effect(descriptor) == nullptr);

  // Conversely, kEffect metadata cannot turn a MusicDevice component into an
  // effect. Reject before AudioComponent lookup/instantiation.
  descriptor.kind = PluginKind::kEffect;
  descriptor.id = "61756d75:00000000:00000000";  // 'aumu'
  REQUIRE(provider.can_create(descriptor));
  REQUIRE(provider.create_effect(descriptor) == nullptr);
}

TEST_CASE("AU process paths make render calls without control-plane calls", "[host][au]") {
  const auto result = sonare::host::backends::detail::run_au_process_call_spy();
  REQUIRE(result.instrument_controls_unchanged);
  REQUIRE(result.effect_controls_unchanged);
  REQUIRE(result.render_calls == 6);
}

TEST_CASE("AU effect input callback never reads past the current block's host planes",
          "[host][au]") {
  // The AU input render callback (AuEffectProcessor::input_trampoline) must
  // only read as many frames from the caller's planes as process() was called
  // with — the caller's contract (rt::ProcessorBase::process) guarantees
  // exactly num_samples frames are valid, even though the AU was prepared for
  // a larger maximum block and may internally request more from its input
  // callback (normal at a render tail or throughout a variable-block-size
  // host). This probe backs the host planes with heap buffers sized to the
  // exact rendered block, so a regression back to clamping against the
  // prepared maximum instead of the current block reads out of bounds under
  // ASan.
  const auto result = sonare::host::backends::detail::run_au_effect_undersized_block_probe();
  REQUIRE(result.ran);
  REQUIRE(result.block_samples == 64);
  REQUIRE(result.probe_frames == 512);
}

TEST_CASE("AU MusicDevice instrument's dropped-event counter is reachable and counts overflow",
          "[host][au]") {
  // create_instrument() returns a plain midi::MidiInstrument*; before this fix
  // AuMidiInstrument::dropped_count() had no seam back to that interface, so
  // no real caller could ever observe it and a dense MIDI block (more events
  // in one block than the fixed queue depth) silently lost notes with zero
  // telemetry. AuInstrumentTelemetry (au_instrument_provider.h) is the fix: a
  // caller downcasts via dynamic_cast from the same interface pointer type
  // create_instrument() actually returns.
  const auto result = sonare::host::backends::detail::run_au_instrument_dropped_event_probe();
  REQUIRE(result.telemetry_reachable);
  REQUIRE(result.dropped_before_overflow == 0);
  REQUIRE(result.dropped_after_overflow == 1);
}

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

TEST_CASE("AU host renders mono then stereo without audio-thread reconfiguration",
          "[host][au][.]") {
  // The AU is negotiated once (stereo) in prepare(); process() must NOT
  // re-negotiate the stream format, because AudioUnitUninitialize/Initialize on
  // the audio thread violates the render-thread no-allocation/no-I-O contract.
  // A host rendering a different channel count is adapted to the negotiated
  // format instead: a missing channel is backed by pre-sized scratch, an extra
  // host channel is silenced. This checks that rendering mono then stereo both
  // stay finite and neither crashes despite the mismatch.
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

  // Render mono (adapted to the stereo negotiation via scratch), then stereo:
  // both must stay finite.
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
    last_sample_time_.store(buffers.time.sample_time, std::memory_order_relaxed);
    if (buffers.time.host_time_ns != 0) host_timestamps_.fetch_add(1, std::memory_order_relaxed);
    int seen = max_frames_seen_.load(std::memory_order_relaxed);
    while (buffers.num_frames > seen && !max_frames_seen_.compare_exchange_weak(
                                            seen, buffers.num_frames, std::memory_order_relaxed)) {
    }
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
  std::atomic<int> max_frames_seen_{0};
  std::atomic<int> host_timestamps_{0};
  std::atomic<int64_t> last_sample_time_{0};
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
  REQUIRE(callback.config_.max_block_size > 0);
  REQUIRE(callback.max_frames_seen_.load() <= callback.config_.max_block_size);
  REQUIRE(callback.host_timestamps_.load() > 0);
  uint64_t mapped_host_time = 0;
  REQUIRE(device.midi_time_mapper().render_frame_to_host_time(callback.last_sample_time_.load(),
                                                              &mapped_host_time));
  REQUIRE(mapped_host_time > 0);
  REQUIRE(xruns <= static_cast<uint32_t>(callbacks) / 4u + 1u);

  // The config the callback's own open() received must carry the SAME latency
  // figures as the out-of-band output_latency_samples()/input_latency_samples()
  // getters, not the AudioStreamConfig default of 0 ("unknown / not reported")
  // — a callback that seeds PDC compensation from its own open(config) argument
  // has no other way to see this.
  REQUIRE(callback.config_.output_latency_samples == device.output_latency_samples());
  REQUIRE(callback.config_.input_latency_samples == device.input_latency_samples());

  // config_.sample_rate must be the rate the render callback is actually
  // driven at — the AU's negotiated input-scope stream format — not just
  // echoed back from the originally requested config.sample_rate above, since
  // the latency figures asserted above are counted in the device's own
  // hardware clock domain and only match "samples at sample_rate" once
  // converted into this domain (see scale_coreaudio_device_domain_samples).
  // On a device whose nominal rate matches the request (the common case on a
  // dev machine) this equals the device's own nominal rate too; a machine
  // whose default output negotiates a different rate is exactly the case this
  // fix targets, so no exact cross-check against a second, independent query
  // is asserted here — that would just re-derive whichever value CoreAudio
  // actually negotiated. The domain-agreement invariant itself is covered
  // without hardware by the coreaudio_sample_clock_domains_match /
  // scale_coreaudio_device_domain_samples unit tests in host_seam_test.cpp.
  REQUIRE(std::isfinite(callback.config_.sample_rate));
  REQUIRE(callback.config_.sample_rate > 0.0);

  device.close();
  REQUIRE(device.output_latency_samples() >= 0);
}
#endif  // SONARE_HOST_TEST_COREAUDIO
