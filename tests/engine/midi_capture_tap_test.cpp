#include <catch2/catch_test_macros.hpp>

#if defined(SONARE_WITH_ARRANGEMENT)

#include <array>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstddef>
#include <cstdint>

#include "engine/realtime_engine.h"
#include "host/midi_io.h"
#include "midi/clock_sync.h"
#include "midi/ump.h"
#include "util/constants.h"

namespace {

using namespace sonare;
using namespace sonare::engine;
using Catch::Matchers::WithinAbs;

constexpr double kSampleRate = constants::kDefaultDawSampleRate;
constexpr double kBpm = constants::kDefaultBpm;
constexpr double kPpqTolerance = 1.0e-9;

double ppq_delta(int64_t frames, double bpm = kBpm) {
  return static_cast<double>(frames) * bpm / (kSampleRate * constants::kSecondsPerMinute);
}

void enqueue_command(RealtimeEngine& engine, rt::CommandType type, int64_t frame,
                     int64_t sample = 0) {
  rt::Command command{};
  command.type = type;
  command.sample_time = frame;
  command.arg.i = sample;
  REQUIRE(engine.push_command(command));
}

}  // namespace

TEST_CASE("MIDI capture follows the rolling playhead across a loop wrap",
          "[engine][midi_capture_tap]") {
  constexpr int kFrames = 32;
  constexpr int kLoopStart = 16;
  constexpr int kLoopEnd = 48;
  constexpr int kWrapFrame = 8;
  RealtimeEngine engine;
  host::FixedMidiInputSource<16> input;
  engine.prepare(kSampleRate, kFrames);
  engine.set_loop(ppq_delta(kLoopStart), ppq_delta(kLoopEnd), true);
  engine.set_midi_input_source(&input);
  engine.start_midi_capture();

  enqueue_command(engine, rt::CommandType::kTransportSeekSample, 0, kLoopEnd - kWrapFrame);
  enqueue_command(engine, rt::CommandType::kTransportPlay, 0);
  const auto note_before = midi::make_midi1_note_on(0, 0, 60, 100);
  const auto note_at = midi::make_midi2_note_on(0, 0, 62, 45001);
  const auto note_after = midi::make_midi1_note_on(0, 0, 64, 90);
  REQUIRE(input.push_event_at_render_frame(note_after, kWrapFrame + 4));
  REQUIRE(input.push_event_at_render_frame(note_before, kWrapFrame - 1));
  REQUIRE(input.push_event_at_render_frame(note_at, kWrapFrame));

  engine.process(nullptr, 0, kFrames);
  std::array<CapturedMidiEvent, 4> captured{};
  REQUIRE(engine.drain_captured_midi(captured.data(), captured.size()) == 3);
  CHECK(captured[0].ump == note_before);
  CHECK(captured[1].ump == note_at);
  CHECK(captured[2].ump == note_after);
  CHECK_THAT(captured[0].ppq, WithinAbs(ppq_delta(kLoopEnd - 1), kPpqTolerance));
  CHECK_THAT(captured[1].ppq, WithinAbs(ppq_delta(kLoopStart), kPpqTolerance));
  CHECK_THAT(captured[2].ppq, WithinAbs(ppq_delta(kLoopStart + 4), kPpqTolerance));
  CHECK(engine.drain_captured_midi(captured.data(), captured.size()) == 0);
}

TEST_CASE("Stopped MIDI capture starts at the playhead and follows tempo after the first event",
          "[engine][midi_capture_tap]") {
  constexpr int kFrames = 16;
  constexpr int64_t kPlayhead = 1200;
  constexpr int64_t kTempoChangeFrame = kPlayhead + 8;
  constexpr double kLaterBpm = kBpm / 2.0;
  const double change_ppq = ppq_delta(kTempoChangeFrame);
  const auto ppq = [&](int64_t sample) {
    return sample <= kTempoChangeFrame
               ? ppq_delta(sample)
               : change_ppq + ppq_delta(sample - kTempoChangeFrame, kLaterBpm);
  };

  RealtimeEngine engine;
  host::FixedMidiInputSource<16> input;
  engine.prepare(kSampleRate, kFrames);
  engine.set_tempo_segments({{0.0, kBpm}, {change_ppq, kLaterBpm}});
  engine.set_midi_input_source(&input);
  engine.start_midi_capture();
  enqueue_command(engine, rt::CommandType::kTransportSeekSample, 0, kPlayhead);

  const auto note = midi::make_midi1_note_on(0, 0, 60, 100);
  midi::Ump clock{};
  clock.words[0] = (uint32_t{0x1} << 28) | (uint32_t{midi::kStatusClock} << 16);
  clock.word_count = 1;
  REQUIRE(input.push_event_at_render_frame(clock, 1));
  REQUIRE(input.push_event_at_render_frame(note, 4));
  REQUIRE(input.push_event_at_render_frame(note, kFrames + 4));
  engine.process(nullptr, 0, kFrames);
  engine.process(nullptr, 0, kFrames);

  std::array<CapturedMidiEvent, 4> captured{};
  REQUIRE(engine.drain_captured_midi(captured.data(), captured.size()) == 2);
  CHECK(captured[0].ump == note);
  CHECK(captured[1].ump == note);
  CHECK_THAT(captured[0].ppq, WithinAbs(ppq(kPlayhead), kPpqTolerance));
  CHECK_THAT(captured[1].ppq, WithinAbs(ppq(kPlayhead + kFrames), kPpqTolerance));

  constexpr int64_t kPlayFrame = 2 * kFrames + 2;
  constexpr int64_t kStopFrame = 2 * kFrames + 8;
  const int64_t stopped_playhead = kPlayhead + (kStopFrame - kPlayFrame);
  enqueue_command(engine, rt::CommandType::kTransportPlay, kPlayFrame);
  enqueue_command(engine, rt::CommandType::kTransportStop, kStopFrame);
  REQUIRE(input.push_event_at_render_frame(note, kStopFrame));
  REQUIRE(input.push_event_at_render_frame(note, kStopFrame + 4));
  engine.process(nullptr, 0, kFrames);
  REQUIRE(engine.drain_captured_midi(captured.data(), captured.size()) == 2);
  CHECK_THAT(captured[0].ppq, WithinAbs(ppq(stopped_playhead), kPpqTolerance));
  CHECK_THAT(captured[1].ppq, WithinAbs(ppq(stopped_playhead + 4), kPpqTolerance));

  constexpr int64_t kRestartFrame = 3 * kFrames + 2;
  enqueue_command(engine, rt::CommandType::kTransportPlay, kRestartFrame);
  enqueue_command(engine, rt::CommandType::kTransportStop, kRestartFrame);
  REQUIRE(input.push_event_at_render_frame(note, kRestartFrame));
  engine.process(nullptr, 0, kFrames);
  REQUIRE(engine.drain_captured_midi(captured.data(), captured.size()) == 1);
  CHECK_THAT(captured[0].ppq, WithinAbs(ppq(stopped_playhead), kPpqTolerance));
}

TEST_CASE("MIDI capture drops events when full and keeps unread events when stopped",
          "[engine][midi_capture_tap]") {
  constexpr std::size_t kCaptureCapacity = 65536;
  constexpr std::size_t kBatch = 64;
  RealtimeEngine engine;
  host::FixedMidiInputSource<kBatch> input;
  engine.prepare(kSampleRate, 1);
  engine.set_midi_input_source(&input);
  engine.start_midi_capture();
  const auto note = midi::make_midi1_note_on(0, 1, 62, 88);
  for (std::size_t frames = 0; frames <= kCaptureCapacity; frames += kBatch) {
    for (std::size_t i = 0; i < kBatch; ++i) REQUIRE(input.push_event(note, 0));
    engine.process(nullptr, 0, 1);
  }
  CHECK(engine.dropped_midi_events() == kBatch);

  engine.stop_midi_capture();
  REQUIRE(input.push_event(note, 0));
  engine.process(nullptr, 0, 1);
  std::array<CapturedMidiEvent, kBatch> captured{};
  std::size_t count = 0;
  while (const auto drained = engine.drain_captured_midi(captured.data(), captured.size())) {
    count += drained;
  }
  CHECK(count == kCaptureCapacity);
  CHECK(engine.dropped_midi_events() == kBatch);
  engine.start_midi_capture();
  CHECK(engine.dropped_midi_events() == 0);
  CHECK(engine.drain_captured_midi(captured.data(), captured.size()) == 0);
}

#endif
