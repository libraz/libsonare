/// @file rt_bindings.cpp
/// @brief Minimal C ABI exports for the realtime WASM target.

#ifdef __EMSCRIPTEN__

#include <emscripten/emscripten.h>

#include <cstdint>
#include <new>

#include "engine/realtime_engine.h"
#include "engine/telemetry.h"
#include "rt/command.h"

using sonare::engine::RealtimeEngine;

extern "C" {

EMSCRIPTEN_KEEPALIVE
uint32_t sonare_rt_engine_abi_version() { return sonare::rt::kEngineAbiVersion; }

EMSCRIPTEN_KEEPALIVE
RealtimeEngine* sonare_rt_engine_create() {
  try {
    return new RealtimeEngine();
  } catch (...) {
    return nullptr;
  }
}

EMSCRIPTEN_KEEPALIVE
void sonare_rt_engine_destroy(RealtimeEngine* engine) { delete engine; }

EMSCRIPTEN_KEEPALIVE
int sonare_rt_engine_prepare(RealtimeEngine* engine, double sample_rate, int max_block_size,
                             uint32_t command_capacity, uint32_t telemetry_capacity) {
  if (!engine || sample_rate <= 0.0 || max_block_size <= 0) return 0;
  try {
    engine->prepare(sample_rate, max_block_size, command_capacity, telemetry_capacity);
    return 1;
  } catch (...) {
    return 0;
  }
}

EMSCRIPTEN_KEEPALIVE
int sonare_rt_engine_play(RealtimeEngine* engine, int64_t render_frame) {
  if (!engine) return 0;
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kTransportPlay;
  command.sample_time = render_frame;
  return engine->push_command(command) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int sonare_rt_engine_stop(RealtimeEngine* engine, int64_t render_frame) {
  if (!engine) return 0;
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kTransportStop;
  command.sample_time = render_frame;
  return engine->push_command(command) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int sonare_rt_engine_seek_sample(RealtimeEngine* engine, int64_t timeline_sample,
                                 int64_t render_frame) {
  if (!engine) return 0;
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kTransportSeekSample;
  command.sample_time = render_frame;
  command.arg.i = timeline_sample;
  return engine->push_command(command) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int sonare_rt_engine_seek_ppq(RealtimeEngine* engine, double ppq, int64_t render_frame) {
  if (!engine) return 0;
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kTransportSeekPpq;
  command.sample_time = render_frame;
  // Engine reads the PPQ scalar from the full-precision double slot of the arg
  // union (kTransportSeekPpq -> transport_.seek_ppq(command.arg.d)). Match the
  // C API; writing the float slot would surface as garbage.
  command.arg.d = ppq;
  return engine->push_command(command) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int sonare_rt_engine_set_tempo(RealtimeEngine* engine, double bpm) {
  if (!engine || bpm <= 0.0) return 0;
  engine->set_tempo(bpm);
  return 1;
}

EMSCRIPTEN_KEEPALIVE
int sonare_rt_engine_set_loop(RealtimeEngine* engine, double start_ppq, double end_ppq,
                              int enabled) {
  if (!engine) return 0;
  engine->set_loop(start_ppq, end_ppq, enabled != 0);
  return 1;
}

EMSCRIPTEN_KEEPALIVE
int sonare_rt_engine_seek_marker(RealtimeEngine* engine, uint32_t marker_id, int64_t render_frame) {
  if (!engine) return 0;
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kSeekMarker;
  command.target_id = marker_id;
  command.sample_time = render_frame;
  return engine->push_command(command) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int sonare_rt_engine_set_metronome_enabled(RealtimeEngine* engine, int enabled, double beat_gain,
                                           double accent_gain, int click_samples) {
  if (!engine) return 0;
  sonare::engine::MetronomeConfig config{};
  config.enabled = enabled != 0;
  config.beat_gain = static_cast<float>(beat_gain);
  config.accent_gain = static_cast<float>(accent_gain);
  config.click_samples = click_samples;
  engine->set_metronome_config(config);
  return 1;
}

EMSCRIPTEN_KEEPALIVE
int sonare_rt_engine_set_capture_armed(RealtimeEngine* engine, int armed) {
  if (!engine) return 0;
  engine->set_capture_armed(armed != 0);
  return 1;
}

EMSCRIPTEN_KEEPALIVE
int sonare_rt_engine_set_capture_punch(RealtimeEngine* engine, int64_t start_sample,
                                       int64_t end_sample, int enabled) {
  if (!engine) return 0;
  engine->set_capture_punch(start_sample, end_sample, enabled != 0);
  return 1;
}

EMSCRIPTEN_KEEPALIVE
void sonare_rt_engine_process(RealtimeEngine* engine, float* const* channels, int num_channels,
                              int num_frames) {
  if (!engine) return;
  engine->process(channels, num_channels, num_frames);
}

EMSCRIPTEN_KEEPALIVE
int sonare_rt_engine_drain_telemetry(RealtimeEngine* engine, int32_t* types_errors_values,
                                     double* frame_values, int max_records) {
  if (!engine || !types_errors_values || !frame_values || max_records <= 0) return 0;
  int count = 0;
  sonare::engine::Telemetry telemetry{};
  while (count < max_records && engine->pop_telemetry(telemetry)) {
    const int type_offset = count * 4;
    types_errors_values[type_offset] = static_cast<int32_t>(telemetry.type);
    types_errors_values[type_offset + 1] = static_cast<int32_t>(telemetry.error);
    types_errors_values[type_offset + 2] = telemetry.graph_latency_samples_q8;
    types_errors_values[type_offset + 3] = static_cast<int32_t>(telemetry.value);
    const int frame_offset = count * 3;
    frame_values[frame_offset] = static_cast<double>(telemetry.render_frame);
    frame_values[frame_offset + 1] = static_cast<double>(telemetry.timeline_sample);
    frame_values[frame_offset + 2] = static_cast<double>(telemetry.audible_timeline_sample);
    ++count;
  }
  return count;
}

}  // extern "C"

#endif  // __EMSCRIPTEN__
