/// @file live_session_test.cpp
/// @brief host::LiveSession — refusals without hardware, enumeration and a
///        sustained run against real devices.
///
/// The cases that open a device carry "[.]" and stay out of the default ctest
/// run, like the rest of tests/host. They render silence: an underrun is a
/// timing property of the callback, so nothing here needs to be audible, and a
/// test that makes noise is one nobody runs on a machine in use.

#include "host/live_session.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <thread>

#include "engine/realtime_engine.h"

namespace {

using sonare::engine::RealtimeEngine;
using sonare::host::LiveDeviceInfo;
using sonare::host::LiveOpenResult;
using sonare::host::LiveSession;

}  // namespace

TEST_CASE("LiveSession refuses a configuration no session can be opened from", "[host][live]") {
  // The engine outlives the session deliberately: ~LiveSession unbinds the
  // engine's MIDI input, so a session destroyed second would reach a dead one.
  RealtimeEngine engine;
  LiveSession session;
  LiveSession::Config config;

  // A null engine is refused before any device is touched, which is what keeps
  // a failed open from leaving a device held by nothing.
  REQUIRE(LiveSession{}.open(nullptr, config) == LiveOpenResult::kInvalidArgument);

  config.sample_rate = 0.0;
  REQUIRE(session.open(&engine, config) == LiveOpenResult::kInvalidArgument);
  config.sample_rate = std::numeric_limits<double>::quiet_NaN();
  REQUIRE(session.open(&engine, config) == LiveOpenResult::kInvalidArgument);
  config.sample_rate = 48000.0;
  config.block_size = 0;
  REQUIRE(session.open(&engine, config) == LiveOpenResult::kInvalidArgument);

  // Nothing opened, so every reading is the closed one rather than a stale
  // figure from a half-built session.
  REQUIRE_FALSE(session.is_running());
  REQUIRE(session.actual_block_size() == 0);
  REQUIRE(session.actual_sample_rate() == 0.0);
  REQUIRE(session.output_latency_ms() == 0.0);
  REQUIRE(session.xrun_count() == 0u);
  REQUIRE(session.render_callback_count() == 0u);
  session.close();
  REQUIRE_FALSE(session.is_running());
}

TEST_CASE("LiveSession refuses an index past the device list", "[host][live]") {
  // The engine outlives the session deliberately: ~LiveSession unbinds the
  // engine's MIDI input, so a session destroyed second would reach a dead one.
  RealtimeEngine engine;
  LiveSession session;
  LiveDeviceInfo info;

  // An index past the list is refused rather than resolved to the first entry,
  // and the caller's buffer is left alone so a false return cannot be read as
  // an empty name.
  const size_t audio_outputs = LiveSession::audio_output_count();
  const size_t midi_inputs = LiveSession::midi_input_count();
  REQUIRE_FALSE(LiveSession::audio_output_info(audio_outputs + 1, &info));
  REQUIRE_FALSE(LiveSession::midi_input_info(midi_inputs + 1, &info));
  REQUIRE_FALSE(LiveSession::audio_output_info(0, nullptr));
  REQUIRE_FALSE(LiveSession::midi_input_info(0, nullptr));

  LiveSession::Config config;
  // MIDI is opened first, so a machine without a source refuses there and the
  // audio index is never reached. Asking for no MIDI leaves the index as the
  // only thing left to refuse, which is the check this case is named after.
  config.use_midi_input = false;
  config.use_default_audio_output = false;
  config.audio_output_index = audio_outputs + 1;
  REQUIRE(session.open(&engine, config) == LiveOpenResult::kInvalidArgument);
  REQUIRE_FALSE(session.is_running());
}

TEST_CASE("LiveSession enumerates the machine's devices", "[host][live][.]") {
  // Enumeration opens nothing and makes no sound, but it does read the live
  // device list, so it stays out of the default run with its siblings.
  const size_t outputs = LiveSession::audio_output_count();
  const size_t inputs = LiveSession::midi_input_count();
  INFO("audio outputs: " << outputs << ", midi inputs: " << inputs);
  REQUIRE(outputs > 0);

  bool any_named = false;
  for (size_t i = 0; i < outputs; ++i) {
    LiveDeviceInfo info;
    if (LiveSession::audio_output_info(i, &info)) {
      INFO("audio output " << i << ": " << info.name);
      REQUIRE(info.name[0] != '\0');
      any_named = true;
    }
  }
  // A list of devices none of which will say what it is has not enumerated
  // anything a host can show.
  REQUIRE(any_named);

  for (size_t i = 0; i < inputs; ++i) {
    LiveDeviceInfo info;
    if (LiveSession::midi_input_info(i, &info)) {
      INFO("midi input " << i << ": " << info.name);
      REQUIRE(info.name[0] != '\0');
    }
  }
}

TEST_CASE("LiveSession opens an audio output with no MIDI input", "[host][live][.]") {
  // The engine outlives the session deliberately: ~LiveSession unbinds the
  // engine's MIDI input, so a session destroyed second would reach a dead one.
  RealtimeEngine engine;
  LiveSession session;
  LiveSession::Config config;
  config.use_midi_input = false;

  const LiveOpenResult result = session.open(&engine, config);
  if (result == LiveOpenResult::kAudioOutputUnavailable) {
    SUCCEED("no audio output device available; skipping");
    return;
  }
  // A machine with no MIDI source at all must still reach a running session,
  // which is the whole point of the configuration.
  REQUIRE(result == LiveOpenResult::kOk);
  REQUIRE(session.is_running());
  REQUIRE(session.actual_block_size() > 0);
  REQUIRE(session.actual_sample_rate() > 0.0);

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const uint64_t callbacks = session.render_callback_count();
  session.close();

  INFO("render callbacks: " << callbacks);
  // Opened is not running: without a callback the session proves only that a
  // device accepted the format.
  REQUIRE(callbacks > 0u);
  REQUIRE_FALSE(session.is_running());
  REQUIRE(session.actual_block_size() == 0);
}

TEST_CASE("LiveSession's reported latency tracks the block size it negotiated", "[host][live][.]") {
  // The reported figure is the driver's device latency plus its safety offset
  // plus the buffer, and only the last of those moves with the block size. Two
  // opens on one device therefore cancel the two constants, leaving a
  // difference this side is responsible for: a dropped buffer term or a broken
  // rate conversion shows up here, where the absolute value cannot show it.
  // What survives the subtraction is not checked -- that needs a loopback.
  struct Reading {
    LiveOpenResult result = LiveOpenResult::kOk;
    int block = 0;
    double rate = 0.0;
    double latency_ms = 0.0;
  };
  const auto measure = [](int block_size) {
    RealtimeEngine engine;
    LiveSession session;
    LiveSession::Config config;
    config.use_midi_input = false;
    config.block_size = block_size;
    Reading reading;
    reading.result = session.open(&engine, config);
    if (reading.result == LiveOpenResult::kOk) {
      reading.block = session.actual_block_size();
      reading.rate = session.actual_sample_rate();
      reading.latency_ms = session.output_latency_ms();
    }
    session.close();
    return reading;
  };

  const Reading small = measure(128);
  const Reading large = measure(512);
  if (small.result == LiveOpenResult::kAudioOutputUnavailable ||
      large.result == LiveOpenResult::kAudioOutputUnavailable) {
    SUCCEED("no audio output device available; skipping");
    return;
  }
  REQUIRE(small.result == LiveOpenResult::kOk);
  REQUIRE(large.result == LiveOpenResult::kOk);
  INFO("small: " << small.block << " frames @ " << small.rate << " Hz -> " << small.latency_ms
                 << " ms");
  INFO("large: " << large.block << " frames @ " << large.rate << " Hz -> " << large.latency_ms
                 << " ms");
  REQUIRE(small.rate == large.rate);
  REQUIRE(small.rate > 0.0);

  if (small.block == large.block) {
    // The driver held one buffer size across both requests, so the term under
    // test never varied. Reporting this as a pass would record a measurement
    // that did not happen.
    WARN("device would not change its block size (" << small.block
                                                    << " frames); difference not measured");
    return;
  }
  const double expected_ms = static_cast<double>(large.block - small.block) * 1000.0 / small.rate;
  REQUIRE(large.latency_ms - small.latency_ms == Catch::Approx(expected_ms).margin(0.05));
}

TEST_CASE("LiveSession runs a minute of 128-frame blocks without a dropout",
          "[host][live][.][slow]") {
  // The engine outlives the session deliberately: ~LiveSession unbinds the
  // engine's MIDI input, so a session destroyed second would reach a dead one.
  RealtimeEngine engine;
  LiveSession session;
  LiveSession::Config config;
  config.sample_rate = 48000.0;
  config.block_size = 128;
  // Takes a MIDI source when the machine has one and runs without it when it
  // does not: a dropout is a property of the render callback, and skipping the
  // run for want of a keyboard measures nothing at all.
  config.use_midi_input = LiveSession::midi_input_count() > 0;

  const LiveOpenResult result = session.open(&engine, config);
  if (result == LiveOpenResult::kAudioOutputUnavailable) {
    SUCCEED("no audio output device available; skipping");
    return;
  }
  REQUIRE(result == LiveOpenResult::kOk);
  REQUIRE(session.is_running());
  // The driver may round the request up; what it settled on is the figure the
  // dropout count below has to be read against.
  INFO("negotiated block size: " << session.actual_block_size());
  INFO("reported output latency: " << session.output_latency_ms() << " ms");
  REQUIRE(session.actual_block_size() > 0);

  const auto started = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(60));
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

  const uint64_t callbacks = session.render_callback_count();
  const uint32_t xruns = session.xrun_count();
  const double expected =
      elapsed * session.actual_sample_rate() / static_cast<double>(session.actual_block_size());
  session.close();

  INFO("render callbacks: " << callbacks << " of " << expected << " expected, xruns: " << xruns);
  // Zero dropouts over zero callbacks measures nothing, and a loose floor would
  // pass a stream running at a fraction of its rate. The device drives the
  // callback off its own clock, so the count the elapsed time predicts is the
  // one to hold it to; the band covers start-up and the final partial block.
  REQUIRE(static_cast<double>(callbacks) > expected * 0.98);
  REQUIRE(static_cast<double>(callbacks) < expected * 1.02);
  REQUIRE(xruns == 0u);
  REQUIRE_FALSE(session.is_running());
}
