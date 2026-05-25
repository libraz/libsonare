#include "engine/realtime_engine.h"

#include <algorithm>
#include <vector>

#include "rt/scoped_no_denormals.h"

namespace sonare::engine {
namespace {

size_t next_power_of_two(size_t value) {
  size_t out = 1;
  while (out < value) {
    out <<= 1u;
  }
  return out;
}

}  // namespace

void RealtimeEngine::prepare(double sample_rate, int max_block_size, size_t command_capacity,
                             size_t telemetry_capacity) {
  max_block_size_ = std::max(max_block_size, 1);
  tempo_map_.prepare(sample_rate);
  transport_.prepare(sample_rate, &tempo_map_);
  clip_player_.prepare(sample_rate, max_block_size_);
  clip_player_.set_tempo_map(&tempo_map_);
  metronome_.prepare(sample_rate, &tempo_map_);
  meter_tap_.prepare(sample_rate, max_block_size_, 0, telemetry_capacity);
  automation_.prepare(sample_rate, &tempo_map_);
  commands_.reserve(next_power_of_two(std::max<size_t>(command_capacity, 2)));
  telemetry_.reserve(next_power_of_two(std::max<size_t>(telemetry_capacity, 2)));
  pending_active_.fill(false);
  telemetry_overflow_count_ = 0;
}

void RealtimeEngine::process(float* const* io, int num_channels, int num_frames) noexcept {
  rt::ScopedNoDenormals no_denormals;

  const int frames = std::max(num_frames, 0);
  if (max_block_size_ <= 0) {
    silence(io, num_channels, frames);
    enqueue_error(TelemetryErrorCode::kNotPrepared, 0, 0, static_cast<uint32_t>(frames));
    return;
  }
  if (frames > max_block_size_) {
    const auto state = transport_.snapshot();
    silence(io, num_channels, frames);
    transport_.advance(frames);
    enqueue_error(TelemetryErrorCode::kMaxBlockExceeded, state.render_frame, state.sample_position,
                  static_cast<uint32_t>(frames));
    return;
  }

  const auto state = transport_.snapshot();
  drain_commands(state.render_frame, frames);
  const uint32_t unknown_target_count_before = automation_.unknown_target_count();
  const uint32_t non_rt_rejection_count_before = automation_.non_realtime_safe_rejection_count();

  BoundaryBuildContext boundary_context{};
  boundary_context.block_render_frame = state.render_frame;
  boundary_context.block_timeline_sample = state.sample_position;
  boundary_context.num_frames = frames;

  transport::BoundaryList loop_boundaries;
  if (transport_.collect_loop_boundaries(frames, &loop_boundaries) && loop_boundaries.size() > 0) {
    boundary_context.loop_wrap = true;
    boundary_context.loop_wrap_offset = loop_boundaries[0].offset;
    boundary_context.loop_start_timeline_sample = tempo_map_.ppq_to_sample(state.loop_start_ppq);
  }

  boundary_splitter_.begin(boundary_context);
  if (boundary_context.loop_wrap) {
    boundary_splitter_.add_loop(boundary_context.loop_wrap_offset);
  }
  for (size_t i = 0; i < pending_.size(); ++i) {
    if (!pending_active_[i]) continue;
    const auto sample_time = pending_[i].sample_time;
    if (sample_time >= state.render_frame &&
        sample_time <= state.render_frame + static_cast<int64_t>(frames)) {
      boundary_splitter_.add_command(static_cast<int>(sample_time - state.render_frame));
    }
  }

  automation::AutomationBoundaryList automation_boundaries;
  const double block_end_ppq = tempo_map_.sample_to_ppq(state.sample_position + frames);
  automation_.collect_boundaries(state.ppq_position, block_end_ppq, &automation_boundaries);
  for (size_t i = 0; i < automation_boundaries.size; ++i) {
    const int64_t timeline_sample = tempo_map_.ppq_to_sample(automation_boundaries.ppq[i]);
    boundary_splitter_.add_automation(static_cast<int>(timeline_sample - state.sample_position));
  }
  if (automation_.lane_count() > 0) {
    constexpr int kAutomationControlPeriod = 64;
    for (int offset = kAutomationControlPeriod; offset < frames;
         offset += kAutomationControlPeriod) {
      boundary_splitter_.add_automation(offset);
    }
  }

  const BoundaryList& boundaries = boundary_splitter_.finish();
  int previous_offset = 0;
  for (size_t i = 0; i < boundaries.size(); ++i) {
    const int offset = boundaries[i].offset;
    if (offset > previous_offset) {
      process_subblock(io, num_channels, previous_offset, offset - previous_offset);
      transport_.advance(offset - previous_offset);
      previous_offset = offset;
    }
    apply_due_commands(offset);
    const int next_offset = (i + 1 < boundaries.size()) ? boundaries[i + 1].offset : frames;
    automation_.apply(transport_.snapshot(), 0, next_offset - offset);
  }
  if (frames > previous_offset) {
    process_subblock(io, num_channels, previous_offset, frames - previous_offset);
    transport_.advance(frames - previous_offset);
  }

  const auto end_state = transport_.snapshot();
  const uint32_t unknown_target_delta =
      automation_.unknown_target_count() - unknown_target_count_before;
  const uint32_t non_rt_rejection_delta =
      automation_.non_realtime_safe_rejection_count() - non_rt_rejection_count_before;
  if (unknown_target_delta > 0) {
    enqueue_error(TelemetryErrorCode::kUnknownTarget, state.render_frame, state.sample_position,
                  unknown_target_delta);
  }
  if (non_rt_rejection_delta > 0) {
    enqueue_error(TelemetryErrorCode::kNonRealtimeSafeParameter, state.render_frame,
                  state.sample_position, non_rt_rejection_delta);
  }
  if (boundaries.overflowed()) {
    enqueue_error(TelemetryErrorCode::kBoundaryOverflow, state.render_frame, state.sample_position,
                  boundaries.dropped_count());
  }
  enqueue_telemetry({TelemetryType::kProcessBlock, TelemetryErrorCode::kNone, state.render_frame,
                     end_state.sample_position, audible_timeline_sample(end_state.sample_position),
                     graph_latency_samples_q8_, static_cast<uint32_t>(frames)});
}

void RealtimeEngine::render_offline(float* const* out, int num_channels, int64_t total_frames,
                                    int block_size) {
  if (out == nullptr || num_channels <= 0 || total_frames <= 0) {
    return;
  }

  const int frames_per_block = std::max(1, std::min(block_size, max_block_size_));
  std::vector<float*> block_channels(static_cast<size_t>(num_channels), nullptr);
  for (int64_t frame = 0; frame < total_frames; frame += frames_per_block) {
    const int frames = static_cast<int>(std::min<int64_t>(frames_per_block, total_frames - frame));
    for (int ch = 0; ch < num_channels; ++ch) {
      block_channels[static_cast<size_t>(ch)] = out[ch] ? out[ch] + frame : nullptr;
    }
    process(block_channels.data(), num_channels, frames);
  }
}

bool RealtimeEngine::push_command(const rt::Command& command) noexcept {
  if (commands_.push(command)) {
    return true;
  }
  enqueue_error(TelemetryErrorCode::kCommandQueueOverflow, transport_.render_frame(),
                transport_.sample_position(), 1);
  return false;
}

void RealtimeEngine::set_tempo(double bpm) { tempo_map_.set_segments({{0.0, bpm, 0.0}}); }

void RealtimeEngine::set_time_signature(int numerator, int denominator) {
  tempo_map_.set_time_signatures({{0.0, {numerator, denominator}}});
}

void RealtimeEngine::set_loop(double start_ppq, double end_ppq, bool enabled) noexcept {
  transport_.set_loop(start_ppq, end_ppq, enabled);
}

void RealtimeEngine::set_markers(std::vector<transport::Marker> markers) {
  markers_.set_markers(std::move(markers));
}

bool RealtimeEngine::marker_by_index(size_t index, transport::Marker* out) const noexcept {
  return markers_.marker_by_index(index, out);
}

bool RealtimeEngine::marker_by_id(uint32_t id, transport::Marker* out) const noexcept {
  return markers_.marker_by_id(id, out);
}

void RealtimeEngine::set_graph_latency_samples_q8(int latency_q8) noexcept {
  graph_latency_samples_q8_ = std::max(latency_q8, 0);
}

int64_t RealtimeEngine::audible_timeline_sample(int64_t timeline_sample) const noexcept {
  return timeline_sample - (graph_latency_samples_q8_ >> 8);
}

bool RealtimeEngine::seek_marker(uint32_t marker_id) noexcept {
  return transport_.seek_marker(marker_id, markers_);
}

bool RealtimeEngine::set_loop_from_markers(uint32_t start_marker_id,
                                           uint32_t end_marker_id) noexcept {
  return transport_.set_loop_from_markers(start_marker_id, end_marker_id, markers_);
}

void RealtimeEngine::set_metronome_config(MetronomeConfig config) noexcept {
  metronome_.set_config(config);
}

int64_t RealtimeEngine::count_in_end_sample(int64_t start_sample, int bars) const noexcept {
  return metronome_.count_in_end_sample(start_sample, bars);
}

void RealtimeEngine::set_clips(std::vector<ClipSchedule> clips) {
  clip_player_.set_clips(std::move(clips));
}

void RealtimeEngine::set_capture_segment(CaptureSegment segment) noexcept {
  capture_sink_.prepare(segment);
}

void RealtimeEngine::set_capture_armed(bool armed) noexcept { capture_sink_.arm(armed); }

void RealtimeEngine::set_capture_punch(int64_t start_sample, int64_t end_sample,
                                       bool enabled) noexcept {
  capture_sink_.set_punch(start_sample, end_sample, enabled);
}

void RealtimeEngine::reset_capture() noexcept { capture_sink_.reset(); }

#if defined(SONARE_WITH_GRAPH)
bool RealtimeEngine::swap_graph(std::unique_ptr<graph::Graph> graph, const char* input_node_id,
                                const char* output_node_id, int num_channels) {
  if (!graph || !input_node_id || !output_node_id || num_channels <= 0) {
    return false;
  }
  return graph_runtime_.swap(std::shared_ptr<graph::Graph>(std::move(graph)), input_node_id,
                             output_node_id, num_channels);
}

size_t RealtimeEngine::graph_node_count() const noexcept {
  const graph::Graph* graph = graph_runtime_.active_graph();
  return graph ? graph->node_count() : 0;
}

size_t RealtimeEngine::graph_connection_count() const noexcept {
  const graph::Graph* graph = graph_runtime_.active_graph();
  return graph ? graph->connection_count() : 0;
}

bool RealtimeEngine::bind_graph_parameter(uint32_t param_id, const char* node_id) noexcept {
  graph::Graph* graph = graph_runtime_.active_graph();
  if (!graph || !node_id) {
    return false;
  }
  graph::Node* node = graph->node(node_id);
  if (!node) {
    return false;
  }
  automation_.bind_target(param_id, &node->processor());
  return true;
}
#endif

void RealtimeEngine::drain_commands(int64_t block_render_frame, int num_frames) noexcept {
  rt::Command command{};
  for (size_t i = 0; i < kMaxCommandsPerBlock && commands_.pop(command); ++i) {
    if (command.sample_time < 0 || command.sample_time <= block_render_frame) {
      command.sample_time = block_render_frame;
    } else if (command.sample_time >
               block_render_frame + static_cast<int64_t>(std::max(num_frames, 0))) {
      store_pending(command);
      continue;
    }
    store_pending(command);
  }
  // Commands beyond the per-block cap stay queued for future blocks; surface
  // the backlog so hosts can observe the resulting temporal drift.
  if (!commands_.empty()) {
    enqueue_error(TelemetryErrorCode::kCommandQueueOverflow, block_render_frame,
                  transport_.sample_position(), static_cast<uint32_t>(commands_.size_approx()));
  }
}

void RealtimeEngine::store_pending(const rt::Command& command) noexcept {
  for (size_t i = 0; i < pending_.size(); ++i) {
    if (!pending_active_[i]) {
      pending_[i] = command;
      pending_active_[i] = true;
      return;
    }
  }
  enqueue_error(TelemetryErrorCode::kPendingCommandOverflow, transport_.render_frame(),
                transport_.sample_position(), 1);
}

void RealtimeEngine::apply_due_commands(int offset) noexcept {
  const int64_t render_time = transport_.render_frame();
  for (size_t i = 0; i < pending_.size(); ++i) {
    if (!pending_active_[i]) continue;
    if (pending_[i].sample_time <= render_time) {
      apply_command(pending_[i]);
      pending_active_[i] = false;
    }
  }
  (void)offset;
  compact_pending();
}

void RealtimeEngine::apply_command(const rt::Command& command) noexcept {
  switch (command.type) {
    case rt::CommandType::kSetParam:
      // Failures (unknown target / non-RT-safe) bump automation_ counters,
      // which process() converts to telemetry after the sub-block loop. Do
      // not emit an error here or the rejection would be double-reported.
      automation_.set_parameter(command.target_id, command.arg.f);
      break;
    case rt::CommandType::kSetParamSmoothed:
      // Engine-level smoothing ramps are not yet implemented, so a smoothed
      // set currently resolves to an immediate RT-safe update. Target
      // processors that smooth internally via ParamSmoother still ramp; this
      // is intentional, not a stub error.
      automation_.set_parameter(command.target_id, command.arg.f);
      break;
    case rt::CommandType::kTransportPlay:
      transport_.play();
      break;
    case rt::CommandType::kTransportStop:
      transport_.stop();
      break;
    case rt::CommandType::kTransportSeekSample:
      transport_.seek_sample(command.arg.i);
      break;
    case rt::CommandType::kTransportSeekPpq:
      transport_.seek_ppq(static_cast<double>(command.arg.f));
      break;
    case rt::CommandType::kSeekMarker:
      if (!seek_marker(command.target_id)) {
        enqueue_error(TelemetryErrorCode::kUnknownTarget, transport_.render_frame(),
                      transport_.sample_position(), command.target_id);
      }
      break;
    default:
      // Reaching here means a command-vocabulary value that is not meant to
      // flow through the realtime queue (tempo/loop/capture/metronome/etc. are
      // applied via direct engine setters), so kUnknownTarget is the correct
      // defensive response.
      enqueue_error(TelemetryErrorCode::kUnknownTarget, transport_.render_frame(),
                    transport_.sample_position(), static_cast<uint32_t>(command.type));
      break;
  }
}

void RealtimeEngine::process_subblock(float* const* io, int num_channels, int offset,
                                      int num_frames) noexcept {
  std::array<float*, 64> sub_channels{};
  int channels = 0;
  if (io && num_channels > 0 && num_frames > 0 && offset >= 0) {
    channels = std::min<int>(num_channels, static_cast<int>(sub_channels.size()));
    for (int ch = 0; ch < channels; ++ch) {
      sub_channels[static_cast<size_t>(ch)] = io[ch] ? io[ch] + offset : nullptr;
    }
    clip_player_.process_at(sub_channels.data(), channels, num_frames,
                            transport_.sample_position());
    metronome_.process(sub_channels.data(), channels, num_frames, transport_.sample_position());
  }
#if defined(SONARE_WITH_GRAPH)
  graph_runtime_.process(io, num_channels, offset, num_frames);
#else
  (void)io;
  (void)offset;
#endif
  if (channels > 0 && num_frames > 0) {
    meter_tap_.process(sub_channels.data(), channels, num_frames, transport_.render_frame());
    const float* const* capture_channels =
        reinterpret_cast<const float* const*>(sub_channels.data());
    capture_sink_.process(capture_channels, channels, num_frames, transport_.sample_position());
  }
}

void RealtimeEngine::silence(float* const* io, int num_channels, int num_frames) noexcept {
  if (!io || num_channels <= 0 || num_frames <= 0) return;
  for (int ch = 0; ch < num_channels; ++ch) {
    if (!io[ch]) continue;
    std::fill(io[ch], io[ch] + num_frames, 0.0f);
  }
}

void RealtimeEngine::enqueue_telemetry(Telemetry telemetry) noexcept {
  if (telemetry_overflow_count_ > 0 && telemetry.type != TelemetryType::kError) {
    Telemetry overflow{};
    overflow.type = TelemetryType::kError;
    overflow.error = TelemetryErrorCode::kTelemetryOverflow;
    overflow.render_frame = telemetry.render_frame;
    overflow.timeline_sample = telemetry.timeline_sample;
    overflow.audible_timeline_sample = telemetry.audible_timeline_sample;
    overflow.graph_latency_samples_q8 = graph_latency_samples_q8_;
    overflow.value = telemetry_overflow_count_;
    if (telemetry_.push(overflow)) {
      telemetry_overflow_count_ = 0;
    }
  }
  if (!telemetry_.push(telemetry)) {
    ++telemetry_overflow_count_;
  }
}

void RealtimeEngine::enqueue_error(TelemetryErrorCode code, int64_t render_frame,
                                   int64_t timeline_sample, uint32_t value) noexcept {
  enqueue_telemetry({TelemetryType::kError, code, render_frame, timeline_sample,
                     audible_timeline_sample(timeline_sample), graph_latency_samples_q8_, value});
}

void RealtimeEngine::compact_pending() noexcept {
  size_t out = 0;
  for (size_t i = 0; i < pending_.size(); ++i) {
    if (!pending_active_[i]) continue;
    if (out != i) {
      pending_[out] = pending_[i];
      pending_active_[out] = true;
      pending_active_[i] = false;
    }
    ++out;
  }
}

}  // namespace sonare::engine
