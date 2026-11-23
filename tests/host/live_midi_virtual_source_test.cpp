#include <catch2/catch_test_macros.hpp>

#if defined(SONARE_WITH_ARRANGEMENT)

#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include <array>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "engine/realtime_engine.h"
#include "host/backends/apple_time.h"
#include "host/backends/coremidi/coremidi_io.h"
#include "midi/ump.h"
#include "rt/command.h"
#include "util/constants.h"

namespace {

using namespace sonare;
using namespace sonare::engine;
using Catch::Matchers::WithinAbs;
using sonare::host::backends::CoreMidiInput;
using sonare::host::backends::detail::host_ticks_to_ns;

constexpr int kBlockSize = 256;
constexpr double kSampleRate = constants::kDefaultDawSampleRate;
constexpr double kBpm = constants::kDefaultBpm;
constexpr double kPpqTolerance = 1.0e-9;

double ppq_for_frame(int64_t frame) {
  return static_cast<double>(frame) * kBpm / (kSampleRate * constants::kSecondsPerMinute);
}

class VirtualMidiSource {
 public:
  ~VirtualMidiSource() {
    if (endpoint_ != 0) MIDIEndpointDispose(endpoint_);
    if (client_ != 0) MIDIClientDispose(client_);
  }

  VirtualMidiSource(const VirtualMidiSource&) = delete;
  VirtualMidiSource& operator=(const VirtualMidiSource&) = delete;
  VirtualMidiSource() = default;

  OSStatus create(MIDIProtocolID protocol) {
    OSStatus status =
        MIDIClientCreate(CFSTR("sonare.virtual-source.test"), nullptr, nullptr, &client_);
    if (status != noErr) return status;
    status =
        MIDISourceCreateWithProtocol(client_, CFSTR("sonare virtual source"), protocol, &endpoint_);
    if (status != noErr) {
      MIDIClientDispose(client_);
      client_ = 0;
    }
    return status;
  }

  MIDIEndpointRef endpoint() const noexcept { return endpoint_; }

  MIDIUniqueID unique_id() const {
    SInt32 value = 0;
    if (endpoint_ == 0 ||
        MIDIObjectGetIntegerProperty(endpoint_, kMIDIPropertyUniqueID, &value) != noErr) {
      return kMIDIInvalidUniqueID;
    }
    return static_cast<MIDIUniqueID>(value);
  }

  bool send(const midi::Ump& ump, MIDITimeStamp timestamp, MIDIProtocolID protocol) const {
    alignas(MIDIEventList) std::array<uint8_t, sizeof(MIDIEventList) + 128> storage{};
    auto* list = reinterpret_cast<MIDIEventList*>(storage.data());
    MIDIEventPacket* packet = MIDIEventListInit(list, protocol);
    if (packet == nullptr || MIDIEventListAdd(list, static_cast<ByteCount>(storage.size()), packet,
                                              timestamp, ump.word_count, ump.words) == nullptr) {
      return false;
    }
    return MIDIReceivedEventList(endpoint_, list) == noErr;
  }

 private:
  MIDIClientRef client_ = 0;
  MIDIEndpointRef endpoint_ = 0;
};

bool wait_for_source(const std::string& uid, CoreMidiInput::SourceInfo* out) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    for (const auto& source : CoreMidiInput::sources()) {
      if (source.uid == uid) {
        *out = source;
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool wait_for_event(CoreMidiInput& input) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    if (input.pending_count() != 0) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return input.pending_count() != 0;
}

void queue_transport_command(RealtimeEngine& engine, rt::CommandType type, int64_t frame) {
  rt::Command command{};
  command.type = type;
  command.sample_time = frame;
  REQUIRE(engine.push_command(command));
}

}  // namespace

TEST_CASE("CoreMIDI virtual sources retain protocol fidelity and engine timing",
          "[host][coremidi][live_midi]") {
  VirtualMidiSource midi1_source;
  if (midi1_source.create(kMIDIProtocol_1_0) != noErr) {
    SKIP("CoreMIDI virtual MIDI 1.0 source creation is unavailable");
  }
  VirtualMidiSource midi2_source;
  if (midi2_source.create(kMIDIProtocol_2_0) != noErr) {
    SKIP("CoreMIDI virtual MIDI 2.0 source creation is unavailable");
  }
  REQUIRE(midi1_source.endpoint() != 0);
  const MIDIUniqueID midi1_unique_id = midi1_source.unique_id();
  REQUIRE(midi1_unique_id != kMIDIInvalidUniqueID);

  CoreMidiInput input;
  const auto uid = std::to_string(midi1_unique_id);
  CHECK_FALSE(input.open_uid("not-a-decimal-uid"));
  CoreMidiInput::SourceInfo midi1_info;
  REQUIRE(wait_for_source(uid, &midi1_info));
  CHECK(midi1_info.uid == uid);
  CHECK_FALSE(midi1_info.name.empty());

  mach_timebase_info_data_t timebase{};
  REQUIRE(mach_timebase_info(&timebase) == KERN_SUCCESS);
  REQUIRE(timebase.numer != 0);
  host::MidiHostTimeMapper mapper;
  input.set_time_mapper(&mapper);
  REQUIRE(input.open_uid(midi1_info.uid));

  RealtimeEngine engine;
  engine.prepare(kSampleRate, kBlockSize);
  engine.set_midi_input_source(&input);
  engine.start_midi_capture();
  queue_transport_command(engine, rt::CommandType::kTransportPlay, 0);
  for (int block = 0; block < 4; ++block) engine.process(nullptr, 0, kBlockSize);

  const std::array<midi::Ump, 2> playing_notes{
      midi::make_midi1_note_on(0, 0, 60, 91),
      midi::make_midi1_note_on(0, 0, 62, 73),
  };
  for (const auto& note : playing_notes) {
    const int64_t frame = engine.transport().render_frame();
    const MIDITimeStamp timestamp = mach_absolute_time();
    uint64_t host_time_ns = 0;
    REQUIRE(host_ticks_to_ns(timestamp, timebase, &host_time_ns));
    mapper.publish_anchor(host_time_ns, frame, kSampleRate);
    int64_t mapped_frame = -1;
    REQUIRE(mapper.host_time_to_render_frame(host_time_ns, &mapped_frame));
    REQUIRE(mapped_frame == frame);
    REQUIRE(input.pending_count() == 0);
    REQUIRE(midi1_source.send(note, timestamp, kMIDIProtocol_1_0));
    REQUIRE(wait_for_event(input));
    engine.process(nullptr, 0, kBlockSize);
    std::array<CapturedMidiEvent, 4> captured{};
    REQUIRE(engine.drain_captured_midi(captured.data(), captured.size()) == 1);
    CHECK(captured[0].ump == note);
    CHECK_THAT(captured[0].ppq, WithinAbs(ppq_for_frame(frame), kPpqTolerance));
    for (int block = 1; block < 4; ++block) engine.process(nullptr, 0, kBlockSize);
  }

  const int64_t stop_frame = engine.transport().render_frame();
  queue_transport_command(engine, rt::CommandType::kTransportStop, stop_frame);
  engine.process(nullptr, 0, kBlockSize);
  CHECK_FALSE(engine.transport().snapshot().playing);
  const int64_t stopped_position = engine.transport().snapshot().sample_position;

  const midi::Ump stopped_first = midi::make_midi1_note_on(0, 0, 64, 88);
  const int64_t first_render_frame = engine.transport().render_frame();
  MIDITimeStamp timestamp = mach_absolute_time();
  uint64_t host_time_ns = 0;
  REQUIRE(host_ticks_to_ns(timestamp, timebase, &host_time_ns));
  mapper.publish_anchor(host_time_ns, first_render_frame, kSampleRate);
  REQUIRE(midi1_source.send(stopped_first, timestamp, kMIDIProtocol_1_0));
  REQUIRE(wait_for_event(input));
  engine.process(nullptr, 0, kBlockSize);
  std::array<CapturedMidiEvent, 4> captured{};
  REQUIRE(engine.drain_captured_midi(captured.data(), captured.size()) == 1);
  CHECK(captured[0].ump == stopped_first);
  CHECK_THAT(captured[0].ppq, WithinAbs(ppq_for_frame(stopped_position), kPpqTolerance));

  const midi::Ump stopped_second = midi::make_midi1_note_on(0, 0, 65, 81);
  const int64_t second_render_frame = engine.transport().render_frame();
  timestamp = mach_absolute_time();
  REQUIRE(host_ticks_to_ns(timestamp, timebase, &host_time_ns));
  mapper.publish_anchor(host_time_ns, second_render_frame, kSampleRate);
  REQUIRE(midi1_source.send(stopped_second, timestamp, kMIDIProtocol_1_0));
  REQUIRE(wait_for_event(input));
  engine.process(nullptr, 0, kBlockSize);
  REQUIRE(engine.drain_captured_midi(captured.data(), captured.size()) == 1);
  CHECK(captured[0].ump == stopped_second);
  CHECK_THAT(captured[0].ppq,
             WithinAbs(ppq_for_frame(stopped_position + second_render_frame - first_render_frame),
                       kPpqTolerance));

  engine.stop_midi_capture();
  engine.set_midi_input_source(nullptr);
  input.close();

  REQUIRE(midi2_source.endpoint() != 0);
  const MIDIUniqueID midi2_unique_id = midi2_source.unique_id();
  REQUIRE(midi2_unique_id != kMIDIInvalidUniqueID);
  CoreMidiInput midi2_input;
  CoreMidiInput::SourceInfo midi2_info;
  REQUIRE(wait_for_source(std::to_string(midi2_unique_id), &midi2_info));
  host::MidiHostTimeMapper midi2_mapper;
  midi2_input.set_time_mapper(&midi2_mapper);
  REQUIRE(midi2_input.open_uid(midi2_info.uid));

  RealtimeEngine midi2_engine;
  midi2_engine.prepare(kSampleRate, kBlockSize);
  midi2_engine.set_midi_input_source(&midi2_input);
  midi2_engine.start_midi_capture();
  queue_transport_command(midi2_engine, rt::CommandType::kTransportPlay, 0);
  for (int block = 0; block < 2; ++block) midi2_engine.process(nullptr, 0, kBlockSize);
  const int64_t midi2_frame = midi2_engine.transport().render_frame();
  timestamp = mach_absolute_time();
  REQUIRE(host_ticks_to_ns(timestamp, timebase, &host_time_ns));
  midi2_mapper.publish_anchor(host_time_ns, midi2_frame, kSampleRate);
  const midi::Ump native_midi2 = midi::make_midi2_note_on(0, 2, 67, 0x1234u);
  REQUIRE(midi2_source.send(native_midi2, timestamp, kMIDIProtocol_2_0));
  REQUIRE(wait_for_event(midi2_input));
  midi2_engine.process(nullptr, 0, kBlockSize);
  REQUIRE(midi2_engine.drain_captured_midi(captured.data(), captured.size()) == 1);
  CHECK(captured[0].ump == native_midi2);
  CHECK_THAT(captured[0].ppq, WithinAbs(ppq_for_frame(midi2_frame), kPpqTolerance));
  midi2_engine.stop_midi_capture();
  midi2_engine.set_midi_input_source(nullptr);
}

#endif
