#include "engine/realtime_engine.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "rt/scoped_no_denormals.h"
#include "util/math_utils.h"

namespace sonare::engine {
namespace {

int64_t block_end_frame(int64_t block_start, int num_frames) noexcept {
  return block_start + static_cast<int64_t>(std::max(num_frames, 0));
}

bool command_belongs_to_block(int64_t sample_time, int64_t block_start, int num_frames) noexcept {
  return sample_time >= block_start && sample_time < block_end_frame(block_start, num_frames);
}

}  // namespace

void RealtimeEngine::prepare(double sample_rate, int max_block_size, size_t command_capacity,
                             size_t telemetry_capacity) {
  max_block_size_ = std::max(max_block_size, 1);
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  tempo_map_.prepare(sample_rate);
  transport_.prepare(sample_rate, &tempo_map_);
  clip_player_.prepare(sample_rate, max_block_size_);
  clip_player_.set_tempo_map(&tempo_map_);
  metronome_.prepare(sample_rate, &tempo_map_);
#if defined(SONARE_WITH_MIXING)
  meter_tap_.prepare(sample_rate, max_block_size_, 0, telemetry_capacity);
#endif
  automation_.prepare(sample_rate, &tempo_map_);
#if defined(SONARE_WITH_MIXING)
  mixing_runtime_.prepare(sample_rate_, max_block_size_);
  monitor_runtime_.prepare(sample_rate_, max_block_size_);
  monitor_bus_storage_.assign(static_cast<size_t>(max_block_size_) * monitor_bus_channels_.size(),
                              0.0f);
  for (size_t ch = 0; ch < monitor_bus_channels_.size(); ++ch) {
    monitor_bus_channels_[ch] =
        monitor_bus_storage_.data() + ch * static_cast<size_t>(max_block_size_);
  }
#endif
  commands_.reserve(next_power_of_2(std::max<size_t>(command_capacity, 2)));
  // Telemetry is a single-producer queue with the audio thread as its only
  // producer; reserve it here so process()/enqueue_telemetry never push to an
  // unreserved (capacity 0) queue and silently drop records.
  telemetry_.reserve(next_power_of_2(std::max<size_t>(telemetry_capacity, 2)));
  pending_active_.fill(false);
  // Pre-size the engine-level smoothers so kSetParamSmoothed never allocates on
  // the audio thread; mark all slots inactive.
  applied_param_smoothing_ms_ = param_smoothing_ms_.load(std::memory_order_relaxed);
  for (SmoothedParam& slot : smoothed_params_) {
    slot.active = false;
    slot.target_id = 0;
    slot.smoother.prepare(sample_rate_, applied_param_smoothing_ms_);
    slot.smoother.reset(0.0f);
  }
  telemetry_overflow_count_ = 0;
  automation_bind_overflow_reported_ = automation_.bind_target_overflow_count();
  automation_stale_lane_reported_ = automation_.stale_lane_apply_count();
}

void RealtimeEngine::process(float* const* io, int num_channels, int num_frames) noexcept {
  process_impl(io, nullptr, num_channels, num_frames, true);
}

void RealtimeEngine::process_with_monitor(float* const* io, float* const* monitor_out,
                                          int num_channels, int num_frames) noexcept {
  process_impl(io, monitor_out, num_channels, num_frames, false);
}

void RealtimeEngine::process_impl(float* const* io, float* const* monitor_out, int num_channels,
                                  int num_frames, bool fold_monitor_to_main) noexcept {
  rt::ScopedNoDenormals no_denormals;

  const int frames = std::max(num_frames, 0);
  if (max_block_size_ <= 0) {
    silence(io, num_channels, frames);
    silence(monitor_out, num_channels, frames);
    enqueue_error(TelemetryErrorCode::kNotPrepared, 0, 0, static_cast<uint32_t>(frames));
    return;
  }
  if (frames > max_block_size_) {
    const auto state = transport_.snapshot();
    silence(io, num_channels, frames);
    silence(monitor_out, num_channels, frames);
    transport_.advance(frames);
    enqueue_error(TelemetryErrorCode::kMaxBlockExceeded, state.render_frame, state.sample_position,
                  static_cast<uint32_t>(frames));
    return;
  }

  const auto state = transport_.snapshot();
  // Adopt the latest published clip / automation snapshots exactly once at
  // block start. Every per-sub-block read below then sees a stable set, so a
  // control-thread publish can never swap data mid-block.
  clip_player_.acquire_clips();
  automation_.acquire_lanes();
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
    // Carry the loop length so timeline_at_offset can fold offsets past the
    // first wrap; with a short loop and a large block the playhead can wrap
    // more than once within this block.
    const int64_t loop_end_sample = tempo_map_.ppq_to_sample(state.loop_end_ppq);
    boundary_context.loop_len_samples =
        loop_end_sample - boundary_context.loop_start_timeline_sample;
  }

  boundary_splitter_.begin(boundary_context);
  if (boundary_context.loop_wrap) {
    // Register EVERY wrap that falls inside this block, not just the first.
    // Each wrap must become a sub-block boundary so the over-wrapped tail of
    // the block renders from the looped position rather than running past
    // loop_end.
    for (size_t i = 0; i < loop_boundaries.size(); ++i) {
      boundary_splitter_.add_loop(loop_boundaries[i].offset);
    }
  }
  for (size_t i = 0; i < pending_.size(); ++i) {
    if (!pending_active_[i]) continue;
    const auto sample_time = pending_[i].sample_time;
    if (command_belongs_to_block(sample_time, state.render_frame, frames)) {
      boundary_splitter_.add_command(static_cast<int>(sample_time - state.render_frame));
    }
  }

  automation::AutomationBoundaryList automation_boundaries;
  if (boundary_context.loop_wrap) {
    // On a loop-wrap block the playhead is NOT a single linear ppq span: it runs
    // from ppq_position up to loop_end_ppq (offsets [0, loop_wrap_offset)), then
    // jumps back and runs from loop_start_ppq (offsets [loop_wrap_offset,
    // frames)). Collecting one span ppq_position..block_end_ppq would overshoot
    // past loop_end and miss every breakpoint in the looped-back region, so
    // those breakpoints never become sub-block boundaries and apply a full block
    // late. Collect each region separately and map breakpoints to their offset
    // using the same fold the boundary splitter applies.
    const int wrap_offset = boundary_context.loop_wrap_offset;
    // Pre-wrap region: timeline runs forward to loop_end.
    automation_.collect_boundaries(state.ppq_position, state.loop_end_ppq, &automation_boundaries);
    for (size_t i = 0; i < automation_boundaries.size; ++i) {
      const int64_t timeline_sample = tempo_map_.ppq_to_sample(automation_boundaries.ppq[i]);
      const int offset = static_cast<int>(timeline_sample - state.sample_position);
      if (offset >= 0 && offset < wrap_offset) {
        boundary_splitter_.add_automation(offset);
      }
    }
    // Post-wrap region: timeline restarts at loop_start. The tail of this block
    // renders (frames - wrap_offset) samples from the loop start; collect that
    // far past loop_start_ppq.
    const int64_t tail_frames = static_cast<int64_t>(frames) - wrap_offset;
    if (tail_frames > 0) {
      const double post_wrap_end_ppq =
          tempo_map_.sample_to_ppq(boundary_context.loop_start_timeline_sample + tail_frames);
      automation_.collect_boundaries(state.loop_start_ppq, post_wrap_end_ppq,
                                     &automation_boundaries);
      for (size_t i = 0; i < automation_boundaries.size; ++i) {
        const int64_t timeline_sample = tempo_map_.ppq_to_sample(automation_boundaries.ppq[i]);
        const int offset =
            wrap_offset +
            static_cast<int>(timeline_sample - boundary_context.loop_start_timeline_sample);
        if (offset >= wrap_offset && offset < frames) {
          boundary_splitter_.add_automation(offset);
        }
      }
    }
  } else {
    const double block_end_ppq = tempo_map_.sample_to_ppq(state.sample_position + frames);
    automation_.collect_boundaries(state.ppq_position, block_end_ppq, &automation_boundaries);
    for (size_t i = 0; i < automation_boundaries.size; ++i) {
      const int64_t timeline_sample = tempo_map_.ppq_to_sample(automation_boundaries.ppq[i]);
      boundary_splitter_.add_automation(static_cast<int>(timeline_sample - state.sample_position));
    }
  }
  // Insert control-period boundaries so automation lanes and engine-level
  // parameter smoothers are re-evaluated at a bounded cadence within the block.
  if (automation_.lane_count() > 0 || any_smoothed_param_active()) {
    for (int offset = kControlPeriod; offset < frames; offset += kControlPeriod) {
      boundary_splitter_.add_automation(offset);
    }
  }

  // Clip edges must split sub-blocks at the exact sample where a clip starts or
  // ends, so automation/fades evaluated per sub-block do not lag up to a full
  // block at clip boundaries. collect_boundaries returns offsets relative to
  // the block's timeline sample position, matching add_clip's convention.
  ClipBoundaryList clip_boundaries;
  clip_player_.collect_boundaries(state.sample_position, frames, &clip_boundaries);
  for (size_t i = 0; i < clip_boundaries.size; ++i) {
    boundary_splitter_.add_clip(clip_boundaries.offsets[i]);
  }

  // Punch in/out transitions must split sub-blocks at the exact sample so the
  // capture sink starts/stops on a sub-block boundary rather than at block
  // granularity. Register each punch edge that falls inside this block.
  if (capture_sink_.armed() && capture_sink_.punch_enabled()) {
    CaptureBoundaryList capture_boundaries;
    collect_capture_boundaries(state.sample_position, frames, capture_sink_.punch_start_sample(),
                               capture_sink_.punch_end_sample(), &capture_boundaries);
    for (size_t i = 0; i < capture_boundaries.size; ++i) {
      boundary_splitter_.add_marker(capture_boundaries.offsets[i]);
    }
  }

  const uint32_t capture_overflow_before = capture_sink_.overflow_count();
  const BoundaryList& boundaries = boundary_splitter_.finish();
  int previous_offset = 0;
  for (size_t i = 0; i < boundaries.size(); ++i) {
    const int offset = boundaries[i].offset;
    if (offset > previous_offset) {
      process_subblock(io, monitor_out, num_channels, previous_offset, offset - previous_offset,
                       fold_monitor_to_main);
      transport_.advance(offset - previous_offset);
      previous_offset = offset;
    }
    // Dispatch commands due at this boundary's render frame. A boundary at the
    // exclusive block end belongs to the next process() call, so leave those
    // commands pending.
    if (offset < frames) {
      apply_due_commands(boundaries[i].render_frame);
    }
    const int next_offset = (i + 1 < boundaries.size()) ? boundaries[i + 1].offset : frames;
    const int sub_block_len = next_offset - offset;
    // Evaluate automation at this sub-block's start using the advanced
    // transport snapshot, so breakpoints that fell mid-block (and were added as
    // boundary points above) are honored at their exact sub-block boundary.
    automation_.apply(transport_.snapshot(), 0, sub_block_len);
    // Advance engine-level smoothing ramps by this sub-block's length and push
    // the interpolated values to their bound parameters at the same cadence.
    tick_smoothed_params(sub_block_len);
  }
  if (frames > previous_offset) {
    process_subblock(io, monitor_out, num_channels, previous_offset, frames - previous_offset,
                     fold_monitor_to_main);
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
  const uint32_t bind_overflow_total = automation_.bind_target_overflow_count();
  if (bind_overflow_total != automation_bind_overflow_reported_) {
    const uint32_t delta = bind_overflow_total - automation_bind_overflow_reported_;
    automation_bind_overflow_reported_ = bind_overflow_total;
    enqueue_error(TelemetryErrorCode::kAutomationBindTargetOverflow, state.render_frame,
                  state.sample_position, delta);
  }
  const uint32_t stale_lane_total = automation_.stale_lane_apply_count();
  if (stale_lane_total != automation_stale_lane_reported_) {
    const uint32_t delta = stale_lane_total - automation_stale_lane_reported_;
    automation_stale_lane_reported_ = stale_lane_total;
    enqueue_error(TelemetryErrorCode::kStaleAutomationLanes, state.render_frame,
                  state.sample_position, delta);
  }
  if (boundaries.overflowed()) {
    enqueue_error(TelemetryErrorCode::kBoundaryOverflow, state.render_frame, state.sample_position,
                  boundaries.dropped_count());
  }
  // Surface capture overflow on the telemetry channel (not only via the polled
  // capture_overflow_count() accessor) so the two stay consistent. The sink
  // increments its counter when the capture segment is full; report the delta
  // accrued during this block.
  const uint32_t capture_overflow_delta = capture_sink_.overflow_count() - capture_overflow_before;
  if (capture_overflow_delta > 0) {
    enqueue_error(TelemetryErrorCode::kCaptureOverflow, state.render_frame, state.sample_position,
                  capture_overflow_delta);
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
  // Not prepared: max_block_size_ is still 0. Emit a single kNotPrepared record
  // instead of looping one frame at a time and flooding telemetry per frame.
  if (max_block_size_ <= 0) {
    enqueue_error(TelemetryErrorCode::kNotPrepared, transport_.render_frame(),
                  transport_.sample_position(), 0);
    return;
  }

  const int frames_per_block = std::max(1, std::min(block_size, max_block_size_));
  // Reuse the member scratch: size it once here (offline path), then the
  // per-block loop only rewrites pointers and never reallocates.
  render_block_channels_.assign(static_cast<size_t>(num_channels), nullptr);
  for (int64_t frame = 0; frame < total_frames; frame += frames_per_block) {
    const int frames = static_cast<int>(std::min<int64_t>(frames_per_block, total_frames - frame));
    for (int ch = 0; ch < num_channels; ++ch) {
      render_block_channels_[static_cast<size_t>(ch)] = out[ch] ? out[ch] + frame : nullptr;
    }
    process(render_block_channels_.data(), num_channels, frames);
  }
}

bool RealtimeEngine::push_command(const rt::Command& command) noexcept {
  // Runs on the CONTROL thread. The telemetry_ SPSC queue's sole producer is
  // the audio thread, so the control thread must NOT push to it. On a full
  // command queue we bump an atomic overflow counter (control thread is its
  // only writer) and report failure via the return value. pop_telemetry then
  // synthesizes a kCommandQueueOverflow record from this counter, so dropped
  // commands surface even without a process() call -- and without any
  // control-thread write to the audio-thread-owned telemetry_ queue.
  if (commands_.push(command)) {
    return true;
  }
  command_overflow_count_.fetch_add(1, std::memory_order_relaxed);
  return false;
}

bool RealtimeEngine::pop_telemetry(Telemetry& out) noexcept {
  // Consumer/control-thread side. Before draining the audio-thread telemetry_
  // queue, surface any command-queue overflows accrued by push_command since
  // the last drain. This keeps the control thread off telemetry_ as a producer
  // while still reporting dropped commands.
  const uint32_t total = command_overflow_count_.load(std::memory_order_relaxed);
  if (total != command_overflow_reported_) {
    const uint32_t delta = total - command_overflow_reported_;
    command_overflow_reported_ = total;
    out = Telemetry{};
    out.type = TelemetryType::kError;
    out.error = TelemetryErrorCode::kCommandQueueOverflow;
    out.render_frame = transport_.render_frame();
    out.timeline_sample = transport_.sample_position();
    out.audible_timeline_sample = audible_timeline_sample(out.timeline_sample);
    out.graph_latency_samples_q8 = graph_latency_samples_q8_;
    out.value = delta;
    return true;
  }
  return telemetry_.pop(out);
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
  return automation_.bind_target(param_id, &node->processor());
}
#endif

void RealtimeEngine::drain_commands(int64_t block_render_frame, int num_frames) noexcept {
  rt::Command command{};
  for (size_t i = 0; i < kMaxCommandsPerBlock && commands_.pop(command); ++i) {
    // A command due now or in the past is clamped to the block head and treated
    // as current-block. Current-block commands take priority over far-future
    // ones when the pending bank is full, so a backlog of future commands can
    // never drop a command that must fire this block.
    bool current_block;
    if (command.sample_time < 0 || command.sample_time <= block_render_frame) {
      command.sample_time = block_render_frame;
      current_block = true;
    } else {
      current_block = command_belongs_to_block(command.sample_time, block_render_frame, num_frames);
    }
    store_pending(command, current_block);
  }
  // Commands beyond the per-block cap stay queued for future blocks; surface
  // the deferred backlog so hosts can observe the resulting temporal drift.
  // This is distinct from kCommandQueueOverflow (commands dropped at push):
  // here nothing is lost and the value is the remaining queued count.
  if (!commands_.empty()) {
    enqueue_error(TelemetryErrorCode::kCommandBacklogDeferred, block_render_frame,
                  transport_.sample_position(), static_cast<uint32_t>(commands_.size_approx()));
  }
}

void RealtimeEngine::store_pending(const rt::Command& command, bool prefer_current) noexcept {
  for (size_t i = 0; i < pending_.size(); ++i) {
    if (!pending_active_[i]) {
      pending_[i] = command;
      pending_active_[i] = true;
      return;
    }
  }
  // Bank is full. If this command must fire in the current block, evict the
  // furthest-future pending entry to make room rather than dropping the
  // current-block command. The evicted future command is the one whose loss is
  // least disruptive (it would have fired latest, if at all).
  if (prefer_current) {
    size_t furthest = pending_.size();
    int64_t furthest_time = command.sample_time;
    for (size_t i = 0; i < pending_.size(); ++i) {
      if (pending_[i].sample_time > furthest_time) {
        furthest_time = pending_[i].sample_time;
        furthest = i;
      }
    }
    if (furthest < pending_.size()) {
      pending_[furthest] = command;
      pending_active_[furthest] = true;
      // The displaced far-future command is dropped; report it so hosts can
      // observe the lost command rather than have it vanish silently.
      enqueue_error(TelemetryErrorCode::kPendingCommandOverflow, transport_.render_frame(),
                    transport_.sample_position(), 1);
      return;
    }
  }
  enqueue_error(TelemetryErrorCode::kPendingCommandOverflow, transport_.render_frame(),
                transport_.sample_position(), 1);
}

void RealtimeEngine::apply_due_commands(int64_t boundary_render_frame) noexcept {
  // Fire every pending command whose sample_time falls at or before the current
  // sub-block boundary's render frame. The boundary splitter registers each
  // pending command's offset as a sub-block boundary, so a command with
  // sample_time T fires precisely at the sub-block whose render-frame range
  // begins at T -- intra-block sample accuracy, not all-at-once at block head.
  for (size_t i = 0; i < pending_.size(); ++i) {
    if (!pending_active_[i]) continue;
    if (pending_[i].sample_time <= boundary_render_frame) {
      apply_command(pending_[i]);
      pending_active_[i] = false;
    }
  }
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
      // Engine-level smoothing: start (or retarget) a one-pole ramp toward the
      // requested value. The ramp is ticked once per control period in
      // process() and pushed to the bound parameter, avoiding the zipper noise
      // of an immediate jump for targets that do not smooth internally.
      start_smoothed_param(command.target_id, command.arg.f);
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
      transport_.seek_ppq(command.arg.d);
      break;
    case rt::CommandType::kSeekMarker:
      if (!seek_marker(command.target_id)) {
        enqueue_error(TelemetryErrorCode::kUnknownTarget, transport_.render_frame(),
                      transport_.sample_position(), command.target_id);
      }
      break;
    case rt::CommandType::kSetTempoMap:
    case rt::CommandType::kSetLoop:
    case rt::CommandType::kSwapGraph:
    case rt::CommandType::kSwapAutomation:
    case rt::CommandType::kSetSoloMute:
    case rt::CommandType::kAddClip:
    case rt::CommandType::kRemoveClip:
    case rt::CommandType::kArmRecord:
    case rt::CommandType::kPunch:
    case rt::CommandType::kSetMetronome:
    case rt::CommandType::kSetMarker:
      // These are part of the binding control vocabulary but are NOT applied
      // through the realtime command queue: they own data that must be swapped
      // via the RtPublisher pattern on direct engine setters (set_tempo,
      // set_loop, swap_graph, set_clips, set_capture_*, set_metronome_config,
      // set_markers, ...). Surfacing a dedicated reason (rather than the
      // misleading kUnknownTarget) tells the host exactly why the command was
      // dropped: it was enqueued through the wrong channel.
      enqueue_error(TelemetryErrorCode::kNonQueueableCommand, transport_.render_frame(),
                    transport_.sample_position(), static_cast<uint32_t>(command.type));
      break;
  }
}

#if defined(SONARE_WITH_MIXING)
bool RealtimeEngine::bind_mixing_strip(mixing::ChannelStrip* strip) {
  if (strip != nullptr && monitor_runtime_.contains(strip)) {
    return false;
  }
  const bool bound = mixing_runtime_.bind(strip);
  if (bound && max_block_size_ > 0) {
    // Re-prepare so the freshly bound strip sees the engine's sample rate and
    // block size. bind() runs on the control thread, so allocation is allowed.
    mixing_runtime_.prepare(sample_rate_, max_block_size_);
  }
  return bound;
}

bool RealtimeEngine::add_monitor_strip(mixing::ChannelStrip* strip) noexcept {
  if (strip != nullptr && mixing_runtime_.strip() == strip) {
    return false;
  }
  return monitor_runtime_.add_strip(strip);
}
#endif

void RealtimeEngine::set_param_smoothing_ms(float smoothing_ms) noexcept {
  param_smoothing_ms_.store(std::max(smoothing_ms, 0.0f), std::memory_order_relaxed);
}

void RealtimeEngine::start_smoothed_param(uint32_t target_id, float value) noexcept {
  if (target_id == 0) {
    // 0 is the reserved invalid target id; treat as an unbound target so the
    // failure surfaces through the same counter path as kSetParam.
    automation_.set_parameter(target_id, value);
    return;
  }
  // Reuse an existing slot for this target, or claim a free one. The ramp
  // starts from the slot's current value (its last applied output) so repeated
  // retargets remain continuous.
  SmoothedParam* free_slot = nullptr;
  for (SmoothedParam& slot : smoothed_params_) {
    if (slot.active && slot.target_id == target_id) {
      slot.smoother.set_target(value);
      return;
    }
    if (!slot.active && free_slot == nullptr) {
      free_slot = &slot;
    }
  }
  if (free_slot == nullptr) {
    enqueue_error(TelemetryErrorCode::kSmoothedParameterCapacity, transport_.render_frame(),
                  transport_.sample_position(), target_id);
    // Preserve the command instead of dropping it. Under saturation we lose
    // smoothing continuity, but the target still reaches the requested value.
    automation_.set_parameter(target_id, value);
    return;
  }
  free_slot->active = true;
  free_slot->target_id = target_id;
  free_slot->smoother.set_target(value);
}

bool RealtimeEngine::any_smoothed_param_active() const noexcept {
  for (const SmoothedParam& slot : smoothed_params_) {
    if (slot.active) return true;
  }
  return false;
}

void RealtimeEngine::tick_smoothed_params(int num_steps) noexcept {
  if (num_steps <= 0) return;
  const float requested_smoothing_ms = param_smoothing_ms_.load(std::memory_order_relaxed);
  if (requested_smoothing_ms != applied_param_smoothing_ms_) {
    applied_param_smoothing_ms_ = requested_smoothing_ms;
    for (SmoothedParam& slot : smoothed_params_) {
      const float current = slot.smoother.current();
      const float target = slot.smoother.target();
      slot.smoother.prepare(sample_rate_, applied_param_smoothing_ms_);
      slot.smoother.reset(current);
      slot.smoother.set_target(target);
    }
  }
  constexpr float kSettleEpsilon = 1.0e-6f;
  for (SmoothedParam& slot : smoothed_params_) {
    if (!slot.active) continue;
    const float current = slot.smoother.advance(num_steps);
    automation_.set_parameter(slot.target_id, current);
    // Retire the slot once the ramp has effectively settled at its target so
    // the bank does not stay saturated with finished ramps.
    if (std::abs(slot.smoother.target() - current) <= kSettleEpsilon) {
      slot.smoother.reset(slot.smoother.target());
      slot.active = false;
      slot.target_id = 0;
    }
  }
}

void RealtimeEngine::process_subblock(float* const* io, float* const* monitor_out, int num_channels,
                                      int offset, int num_frames,
                                      bool fold_monitor_to_main) noexcept {
#if !defined(SONARE_WITH_MIXING)
  (void)fold_monitor_to_main;
#endif
  std::array<float*, kMaxAudioChannels> sub_channels{};
  int channels = 0;
  const int scratch_channels =
      std::min<int>(std::max(num_channels, 0), static_cast<int>(sub_channels.size()));
  if (monitor_out && num_frames > 0 && offset >= 0) {
    for (int ch = 0; ch < scratch_channels; ++ch) {
      if (monitor_out[ch]) {
        std::fill(monitor_out[ch] + offset, monitor_out[ch] + offset + num_frames, 0.0f);
      }
    }
  }
  if (io && num_channels > 0 && num_frames > 0 && offset >= 0) {
    channels = scratch_channels;
    for (int ch = 0; ch < channels; ++ch) {
      sub_channels[static_cast<size_t>(ch)] = io[ch] ? io[ch] + offset : nullptr;
    }
    clip_player_.process_at(sub_channels.data(), channels, num_frames,
                            transport_.sample_position());
    metronome_.process(sub_channels.data(), channels, num_frames, transport_.sample_position());
#if defined(SONARE_WITH_MIXING)
    // Mixing channel-strip insert stage (fader/pan/width/EQ/inserts) runs
    // sample-accurately at the sub-block's timeline position when enabled.
    if (mixing_enabled_) {
      mixing_runtime_.process_at(sub_channels.data(), channels, num_frames,
                                 transport_.sample_position());
    }
    // Solo/mute + PFL/AFL monitoring stage for any registered strips. Existing
    // process() callers keep foldback compatibility; process_with_monitor()
    // receives the cue bus separately without contaminating the main output.
    if (monitoring_enabled_) {
      for (int ch = 0; ch < channels; ++ch) {
        std::fill(monitor_bus_channels_[static_cast<size_t>(ch)],
                  monitor_bus_channels_[static_cast<size_t>(ch)] + num_frames, 0.0f);
      }
      const size_t strip_count = monitor_runtime_.size();
      for (size_t s = 0; s < strip_count; ++s) {
        monitor_runtime_.process_strip(s, sub_channels.data(), channels, num_frames,
                                       transport_.sample_position(), monitor_bus_channels_.data());
      }
      for (int ch = 0; ch < channels; ++ch) {
        float* out = sub_channels[static_cast<size_t>(ch)];
        const float* monitor = monitor_bus_channels_[static_cast<size_t>(ch)];
        float* cue = monitor_out && monitor_out[ch] ? monitor_out[ch] + offset : nullptr;
        if (cue) {
          std::copy(monitor, monitor + num_frames, cue);
        }
        if (!fold_monitor_to_main || !out || !monitor) continue;
        for (int i = 0; i < num_frames; ++i) {
          out[i] += monitor[i];
        }
      }
    }
#endif
  }
#if defined(SONARE_WITH_GRAPH)
  graph_runtime_.process(io, num_channels, offset, num_frames);
#else
  (void)io;
  (void)offset;
#endif
  if (channels > 0 && num_frames > 0) {
#if defined(SONARE_WITH_MIXING)
    meter_tap_.process(sub_channels.data(), channels, num_frames, transport_.render_frame());
#endif
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
