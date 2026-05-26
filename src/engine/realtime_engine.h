#pragma once

/// @file realtime_engine.h
/// @brief Pass-through realtime engine skeleton.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "automation/automation_engine.h"
#include "engine/boundary_splitter.h"
#include "engine/capture.h"
#include "engine/clip_player.h"
#include "engine/meter_telemetry.h"
#include "engine/metronome.h"
#include "engine/mixing_runtime.h"
#include "engine/monitor_runtime.h"
#include "engine/telemetry.h"
#include "rt/command.h"
#include "rt/param_smoother.h"
#include "rt/spsc_queue.h"
#include "transport/marker.h"
#include "transport/tempo_map.h"
#include "transport/transport.h"

#if defined(SONARE_WITH_GRAPH)
#include "engine/graph_runtime.h"
#endif

namespace sonare::engine {

class RealtimeEngine {
 public:
  static constexpr size_t kMaxCommandsPerBlock = 64;
  static constexpr size_t kMaxPendingCommands = 64;

  void prepare(double sample_rate, int max_block_size, size_t command_capacity = 1024,
               size_t telemetry_capacity = 1024);

  void process(float* const* io, int num_channels, int num_frames) noexcept;
  void render_offline(float* const* out, int num_channels, int64_t total_frames, int block_size);

  bool push_command(const rt::Command& command) noexcept;
  bool pop_telemetry(Telemetry& out) noexcept;
  bool pop_meter_telemetry(MeterTelemetryRecord& out) noexcept { return meter_tap_.pop(out); }
  void set_tempo(double bpm);
  void set_time_signature(int numerator, int denominator);
  void set_loop(double start_ppq, double end_ppq, bool enabled) noexcept;
  void set_markers(std::vector<transport::Marker> markers);
  size_t marker_count() const noexcept { return markers_.marker_count(); }
  bool marker_by_index(size_t index, transport::Marker* out) const noexcept;
  bool marker_by_id(uint32_t id, transport::Marker* out) const noexcept;
  bool seek_marker(uint32_t marker_id) noexcept;
  bool set_loop_from_markers(uint32_t start_marker_id, uint32_t end_marker_id) noexcept;
  void set_metronome_config(MetronomeConfig config) noexcept;
  const MetronomeConfig& metronome_config() const noexcept { return metronome_.config(); }
  int64_t count_in_end_sample(int64_t start_sample, int bars) const noexcept;
  void set_clips(std::vector<ClipSchedule> clips);
  size_t clip_count() const noexcept { return clip_player_.clip_count(); }
  void set_capture_segment(CaptureSegment segment) noexcept;
  void set_capture_armed(bool armed) noexcept;
  void set_capture_punch(int64_t start_sample, int64_t end_sample, bool enabled) noexcept;
  void reset_capture() noexcept;
  int64_t captured_frames() const noexcept { return capture_sink_.captured_frames(); }
  uint32_t capture_overflow_count() const noexcept { return capture_sink_.overflow_count(); }
  bool capture_armed() const noexcept { return capture_sink_.armed(); }
  bool capture_punch_enabled() const noexcept { return capture_sink_.punch_enabled(); }
  automation::AutomationEngine& automation() noexcept { return automation_; }
  const automation::AutomationEngine& automation() const noexcept { return automation_; }

  // Mixing channel-strip insert stage. bind_mixing_strip binds a control-thread
  // ChannelStrip whose process_at runs per sub-block when mixing is enabled.
  bool bind_mixing_strip(mixing::ChannelStrip* strip) noexcept;
  void set_mixing_enabled(bool enabled) noexcept { mixing_enabled_ = enabled; }
  bool mixing_enabled() const noexcept { return mixing_enabled_; }
  MixingRuntime& mixing() noexcept { return mixing_runtime_; }

  // Solo/mute + PFL/AFL monitoring stage applied to a registered set of strips.
  bool add_monitor_strip(mixing::ChannelStrip* strip) noexcept;
  bool remove_monitor_strip(mixing::ChannelStrip* strip) noexcept {
    return monitor_runtime_.remove_strip(strip);
  }
  void set_monitoring_enabled(bool enabled) noexcept { monitoring_enabled_ = enabled; }
  bool monitoring_enabled() const noexcept { return monitoring_enabled_; }
  MonitorRuntime& monitor() noexcept { return monitor_runtime_; }

  // Default ramp time for engine-level kSetParamSmoothed commands, in ms.
  void set_param_smoothing_ms(float smoothing_ms) noexcept;
  float param_smoothing_ms() const noexcept {
    return param_smoothing_ms_.load(std::memory_order_relaxed);
  }
  void set_graph_latency_samples_q8(int latency_q8) noexcept;
  int graph_latency_samples_q8() const noexcept { return graph_latency_samples_q8_; }
  int64_t audible_timeline_sample(int64_t timeline_sample) const noexcept;
#if defined(SONARE_WITH_GRAPH)
  // Control-thread graph hot-swap. Allocates a new binding internally, so this
  // is intentionally NOT noexcept (a throwing allocation propagates).
  bool swap_graph(std::unique_ptr<graph::Graph> graph, const char* input_node_id,
                  const char* output_node_id, int num_channels);
  bool has_graph() const noexcept { return graph_runtime_.active_graph() != nullptr; }
  size_t graph_node_count() const noexcept;
  size_t graph_connection_count() const noexcept;
  bool bind_graph_parameter(uint32_t param_id, const char* node_id) noexcept;
#endif

  const transport::Transport& transport() const noexcept { return transport_; }
  int max_block_size() const noexcept { return max_block_size_; }

 private:
  void drain_commands(int64_t block_render_frame, int num_frames) noexcept;
  void store_pending(const rt::Command& command) noexcept;
  void apply_due_commands(int64_t boundary_render_frame) noexcept;
  void apply_command(const rt::Command& command) noexcept;
  void process_subblock(float* const* io, int num_channels, int offset, int num_frames) noexcept;
  void silence(float* const* io, int num_channels, int num_frames) noexcept;
  void start_smoothed_param(uint32_t target_id, float value) noexcept;
  void tick_smoothed_params(int num_steps) noexcept;
  bool any_smoothed_param_active() const noexcept;
  void enqueue_telemetry(Telemetry telemetry) noexcept;
  void enqueue_error(TelemetryErrorCode code, int64_t render_frame, int64_t timeline_sample,
                     uint32_t value) noexcept;
  void compact_pending() noexcept;

  transport::TempoMap tempo_map_{};
  transport::Transport transport_{};
  transport::MarkerMap markers_{};
  ClipPlayer clip_player_{};
  CaptureSink capture_sink_{};
  Metronome metronome_{};
  MeterTelemetryTap meter_tap_{};
  automation::AutomationEngine automation_{};
  MixingRuntime mixing_runtime_{};
  MonitorRuntime monitor_runtime_{};
  rt::SpscQueue<rt::Command> commands_{};
  rt::SpscQueue<Telemetry> telemetry_{};
  BoundarySplitter boundary_splitter_{};
  std::array<rt::Command, kMaxPendingCommands> pending_{};
  std::array<bool, kMaxPendingCommands> pending_active_{};
#if defined(SONARE_WITH_GRAPH)
  GraphRuntime graph_runtime_{};
#endif

  // Engine-level parameter smoothing for kSetParamSmoothed. Each active slot
  // ramps a bound parameter toward its target over param_smoothing_ms_ and is
  // ticked once per control period. Fixed-size: no audio-thread allocation.
  static constexpr size_t kMaxSmoothedParams = 64;
  static constexpr int kControlPeriod = 64;
  struct SmoothedParam {
    uint32_t target_id = 0;
    bool active = false;
    rt::ParamSmoother smoother{};
  };
  std::array<SmoothedParam, kMaxSmoothedParams> smoothed_params_{};

  // Pre-allocated channel pointer scratch reused by render_offline so the
  // per-block loop performs no heap allocation.
  std::vector<float*> render_block_channels_{};

  bool mixing_enabled_ = false;
  bool monitoring_enabled_ = false;
  std::atomic<float> param_smoothing_ms_{20.0f};
  float applied_param_smoothing_ms_ = 20.0f;  // audio thread only
  double sample_rate_ = 48000.0;
  uint32_t telemetry_overflow_count_ = 0;
  int graph_latency_samples_q8_ = 0;
  int max_block_size_ = 0;

  // Command-queue overflow accounting. push_command (control thread) is the
  // sole writer of command_overflow_count_; pop_telemetry (consumer thread)
  // tracks how many it has reported and synthesizes a kCommandQueueOverflow
  // record for any unreported delta. This keeps the control thread off the
  // audio-thread-owned telemetry_ SPSC queue (no producer-side race) while
  // still surfacing dropped commands without requiring a process() call.
  std::atomic<uint32_t> command_overflow_count_{0};
  uint32_t command_overflow_reported_ = 0;
};

}  // namespace sonare::engine
