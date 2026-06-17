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

constexpr uint32_t kEngineParamNamespace = 0x4D580000u;
constexpr uint32_t kEngineParamNamespaceMask = 0xFFFF0000u;
constexpr uint32_t kEngineParamLaneMask = 0x0000FF00u;
constexpr uint32_t kEngineParamKindMask = 0x000000FFu;
constexpr uint32_t kEngineParamLaneShift = 8u;
constexpr uint32_t kEngineParamLaneMaster = 0xFFu;
constexpr uint32_t kEngineParamLaneBusBase = 0xFEu;

}  // namespace

void RealtimeEngine::prepare(double sample_rate, int max_block_size, size_t command_capacity,
                             size_t telemetry_capacity) {
  max_block_size_ = std::max(max_block_size, 1);
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  tempo_map_.prepare(sample_rate);
  publish_tempo_map_snapshot();
  tempo_map_snapshot_.acquire();
  active_tempo_map_ = tempo_map_snapshot_.current();
  if (active_tempo_map_ == nullptr) active_tempo_map_ = &tempo_map_;
  transport_.prepare(sample_rate, active_tempo_map_);
  clip_player_.prepare(sample_rate, max_block_size_);
  clip_player_.set_tempo_map(active_tempo_map_);
  clip_player_.set_page_request_sink(this);
#if defined(SONARE_WITH_ARRANGEMENT)
  midi_sequencer_.prepare(sample_rate);
  midi_clock_.prepare(active_tempo_map_);
  // Pre-size the host-instrument render scratch (channel-planar) so the audio
  // path never allocates when an instrument is registered. Re-prepare an
  // already-registered instrument so it matches the new block size.
  midi_instrument_storage_.assign(
      static_cast<size_t>(max_block_size_) * midi_instrument_channels_.size(), 0.0f);
  for (size_t ch = 0; ch < midi_instrument_channels_.size(); ++ch) {
    midi_instrument_channels_[ch] =
        midi_instrument_storage_.data() + ch * static_cast<size_t>(max_block_size_);
  }
  // PDC clip-bus scratch: the clip player renders here first when an instrument
  // reports latency, so the clip bus can be delayed (phase-aligned with the
  // instruments) before being summed into the source layer.
  clip_scratch_storage_.assign(static_cast<size_t>(max_block_size_) * clip_scratch_channels_.size(),
                               0.0f);
  for (size_t ch = 0; ch < clip_scratch_channels_.size(); ++ch) {
    clip_scratch_channels_[ch] =
        clip_scratch_storage_.data() + ch * static_cast<size_t>(max_block_size_);
  }
  // The dispatch tee is the sequencer's permanent sink: it demuxes events to
  // the instrument rack and optionally mirrors them to a live MIDI output seam.
  // Re-prepare every already-registered instrument to the new block size.
  midi_dispatch_sink_.rack = &instrument_rack_;
  midi_sequencer_.set_sink(&midi_dispatch_sink_);
  instrument_rack_.for_each([&](uint32_t, midi::MidiInstrument* instrument) {
    instrument->prepare(sample_rate_, max_block_size_);
  });
  // Size the PDC delays from whatever instruments are already bound (their
  // latency is known now that they have been prepared).
  recompute_pdc();
#endif
  metronome_.prepare(sample_rate, active_tempo_map_);
#if defined(SONARE_WITH_MIXING)
  meter_tap_.prepare(sample_rate, max_block_size_, 0,
                     telemetry_capacity *
                         (TrackMixerRuntime::kMaxTrackLanes + TrackMixerRuntime::kMaxBusLanes + 2));
  // Spectrum/vectorscope snapshots are interval-gated, so a shallower per-target
  // ring depth than the meter tap suffices.
  scope_tap_.prepare(sample_rate, max_block_size_,
                     8 * (TrackMixerRuntime::kMaxTrackLanes + TrackMixerRuntime::kMaxBusLanes + 2),
                     2048, scope_band_count_);
#endif
  automation_.prepare(sample_rate, active_tempo_map_);
#if defined(SONARE_WITH_MIXING)
  // Route reserved engine-namespace automation lanes (mixer fader/pan) straight
  // to the mixer runtimes instead of the bound-processor table.
  automation_.set_engine_param_router(&RealtimeEngine::route_engine_parameter_thunk, this,
                                      kEngineParamNamespaceMask, kEngineParamNamespace);
#endif
  input_capture_storage_.assign(
      static_cast<size_t>(max_block_size_) * input_capture_channels_.size(), 0.0f);
  for (size_t ch = 0; ch < input_capture_channels_.size(); ++ch) {
    input_capture_channels_[ch] =
        input_capture_storage_.data() + ch * static_cast<size_t>(max_block_size_);
  }
#if defined(SONARE_WITH_MIXING)
  mixing_runtime_.prepare(sample_rate_, max_block_size_);
  monitor_runtime_.prepare(sample_rate_, max_block_size_);
  track_mixer_runtime_.prepare(sample_rate_, max_block_size_);
  update_reported_graph_latency();
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
  clip_page_requests_.reserve(next_power_of_2(std::max<size_t>(telemetry_capacity, 2)));
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

void RealtimeEngine::publish_tempo_map_snapshot() {
  auto map = std::make_shared<transport::TempoMap>();
  map->prepare(sample_rate_);
  if (!control_tempo_segments_.empty()) {
    map->set_segments(control_tempo_segments_);
  }
  if (!control_time_signatures_.empty()) {
    map->set_time_signatures(control_time_signatures_);
  }
  tempo_map_snapshot_.publish(std::move(map));
}

void RealtimeEngine::adopt_tempo_map_snapshot() noexcept {
  tempo_map_snapshot_.acquire();
  const transport::TempoMap* map = tempo_map_snapshot_.current();
  if (map == nullptr || map == active_tempo_map_) return;
  active_tempo_map_ = map;
  transport_.set_tempo_map(active_tempo_map_);
  clip_player_.set_tempo_map(active_tempo_map_);
  automation_.set_tempo_map(active_tempo_map_);
  metronome_.set_tempo_map(active_tempo_map_);
#if defined(SONARE_WITH_ARRANGEMENT)
  midi_clock_.prepare(active_tempo_map_);
#endif
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

  adopt_tempo_map_snapshot();
  const transport::TempoMap& tempo_map = *(active_tempo_map_ ? active_tempo_map_ : &tempo_map_);
  const auto state = transport_.snapshot();
  clip_page_underrun_reported_this_block_ = false;
  // Adopt the latest published clip / automation snapshots exactly once at
  // block start. Every per-sub-block read below then sees a stable set, so a
  // control-thread publish can never swap data mid-block.
  clip_player_.acquire_clips();
  automation_.acquire_lanes();
#if defined(SONARE_WITH_ARRANGEMENT)
  midi_sequencer_.acquire_midi_clips();
  host::MidiInputSource* midi_input_source = midi_input_source_.load(std::memory_order_acquire);
  live_midi_input_destination_id_ = midi_input_destination_id_.load(std::memory_order_relaxed);
  live_midi_input_count_ = midi_input_source != nullptr
                               ? midi_input_source->drain_block(live_midi_input_events_.data(),
                                                                live_midi_input_events_.size(),
                                                                state.render_frame, frames)
                               : 0;
  for (size_t i = 1; i < live_midi_input_count_; ++i) {
    midi::MidiEvent value = live_midi_input_events_[i];
    size_t j = i;
    while (j > 0 && live_midi_input_events_[j - 1].render_frame > value.render_frame) {
      live_midi_input_events_[j] = live_midi_input_events_[j - 1];
      --j;
    }
    live_midi_input_events_[j] = value;
  }
#endif
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
    boundary_context.loop_start_timeline_sample = tempo_map.ppq_to_sample(state.loop_start_ppq);
    // Carry the loop length so timeline_at_offset can fold offsets past the
    // first wrap; with a short loop and a large block the playhead can wrap
    // more than once within this block.
    const int64_t loop_end_sample = tempo_map.ppq_to_sample(state.loop_end_ppq);
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
      const int64_t timeline_sample = tempo_map.ppq_to_sample(automation_boundaries.ppq[i]);
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
          tempo_map.sample_to_ppq(boundary_context.loop_start_timeline_sample + tail_frames);
      automation_.collect_boundaries(state.loop_start_ppq, post_wrap_end_ppq,
                                     &automation_boundaries);
      for (size_t i = 0; i < automation_boundaries.size; ++i) {
        const int64_t timeline_sample = tempo_map.ppq_to_sample(automation_boundaries.ppq[i]);
        const int offset =
            wrap_offset +
            static_cast<int>(timeline_sample - boundary_context.loop_start_timeline_sample);
        if (offset >= wrap_offset && offset < frames) {
          boundary_splitter_.add_automation(offset);
        }
      }
    }
  } else {
    const double block_end_ppq = tempo_map.sample_to_ppq(state.sample_position + frames);
    automation_.collect_boundaries(state.ppq_position, block_end_ppq, &automation_boundaries);
    for (size_t i = 0; i < automation_boundaries.size; ++i) {
      const int64_t timeline_sample = tempo_map.ppq_to_sample(automation_boundaries.ppq[i]);
      boundary_splitter_.add_automation(static_cast<int>(timeline_sample - state.sample_position));
    }
  }
  // Insert control-period boundaries so automation lanes and engine-level
  // parameter smoothers are re-evaluated at a bounded cadence within the block.
  // The boundary list is fixed-capacity, so for blocks larger than
  // kControlPeriod * budget we widen the period to spread the boundaries evenly
  // across the whole block instead of packing the first ~budget*64 samples and
  // dropping the rest (which would freeze automation/smoothing in the block's
  // tail and reintroduce zipper artifacts). Smaller blocks keep the nominal
  // 64-sample cadence unchanged.
  if (automation_.lane_count() > 0 || any_smoothed_param_active()) {
    // Reserve headroom for the mandatory boundaries (block start/end, loop,
    // clip, command, marker, automation breakpoints) so control boundaries do
    // not consume the entire list.
    constexpr int kControlBoundaryBudget = static_cast<int>(BoundaryList::kCapacity) - 12;
    int period = kControlPeriod;
    if (frames > kControlPeriod * kControlBoundaryBudget) {
      period = (frames + kControlBoundaryBudget - 1) / kControlBoundaryBudget;
    }
    for (int offset = period; offset < frames; offset += period) {
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

#if defined(SONARE_WITH_ARRANGEMENT)
  // MIDI event edges split sub-blocks at the exact sample a UMP event fires, so
  // the sequencer dispatches each event at its sample-accurate boundary rather
  // than at block granularity. Uses a distinct BoundarySource::kMidi (added via
  // add_midi) so dense-MIDI overflow stays distinguishable in telemetry.
  midi::MidiSequencer::BoundaryOffsets midi_boundaries;
  midi_sequencer_.collect_boundaries(state.sample_position, frames, &midi_boundaries);
  for (size_t i = 0; i < midi_boundaries.size; ++i) {
    boundary_splitter_.add_midi(midi_boundaries.offsets[i]);
  }
  for (size_t i = 0; i < live_midi_input_count_; ++i) {
    const int64_t event_frame = live_midi_input_events_[i].render_frame;
    if (event_frame >= state.render_frame && event_frame < state.render_frame + frames) {
      boundary_splitter_.add_midi(static_cast<int>(event_frame - state.render_frame));
    }
  }
#endif

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
  clip_player_.begin_page_miss_block();
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
#if defined(SONARE_WITH_ARRANGEMENT)
    // Hang-note safety: when the playhead wraps at a loop boundary, release
    // every note still sounding from the pre-wrap region so it does not hang
    // into the looped-back region. The note-offs fire at the wrap's render
    // frame. RT-safe (no alloc).
    if ((boundaries[i].sources & boundary_source_mask(BoundarySource::kLoop)) != 0) {
      midi_sequencer_.all_notes_off(boundaries[i].render_frame);
    }
#endif
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
  clip_player_.end_page_miss_block();

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
  // Clips and sequenced MIDI only render (and the playhead only advances)
  // while the transport is rolling, so roll it for the duration of the render
  // and restore the prior state afterwards.
  const bool was_playing = transport_.playing();
  if (!was_playing) {
    transport_.play();
  }
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
  if (!was_playing) {
    transport_.stop();
  }
#if defined(SONARE_WITH_ARRANGEMENT)
  midi_sequencer_.all_notes_off(transport_.render_frame());
  flush_pdc_delays();
#endif
#if defined(SONARE_WITH_MIXING)
  track_mixer_runtime_.flush_pdc_delays();
#endif
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
    // render_frame()/sample_position() read plain int64_t transport counters
    // written by the audio thread without synchronization. On 64-bit targets
    // these are naturally-aligned aligned loads (no tearing); on a hypothetical
    // 32-bit target a torn read could momentarily report a half-updated value.
    // This is a benign, best-effort diagnostic stamp on an overflow-error
    // telemetry record, not a control value, so the unsynchronized read is
    // intentional and acceptable.
    out.render_frame = transport_.render_frame();
    out.timeline_sample = transport_.sample_position();
    out.audible_timeline_sample = audible_timeline_sample(out.timeline_sample);
    out.graph_latency_samples_q8 = graph_latency_samples_q8_;
    out.value = delta;
    return true;
  }
  return telemetry_.pop(out);
}

transport::TransportState RealtimeEngine::transport_state_control() const noexcept {
  transport::TransportState state = transport_.snapshot_control();
  const transport::TempoMap* snapshot = tempo_map_snapshot_.control_current().get();
  const transport::TempoMap& map = *(snapshot ? snapshot : &tempo_map_);
  state.ppq_position = map.sample_to_ppq(state.sample_position);
  state.bpm = map.bpm_at_sample(state.sample_position);
  state.bar_start_ppq = map.bar_start_ppq(state.ppq_position);
  state.bar_count = map.ppq_to_bar_beat(state.ppq_position).bar;
  state.time_sig = map.time_signature_at_ppq(state.ppq_position);
  return state;
}

void RealtimeEngine::set_tempo(double bpm) {
  control_tempo_segments_ = {{0.0, bpm, 0.0}};
  publish_tempo_map_snapshot();
}

void RealtimeEngine::set_tempo_segments(std::vector<transport::TempoSegment> segments) {
  control_tempo_segments_ = std::move(segments);
  publish_tempo_map_snapshot();
}

void RealtimeEngine::set_time_signature(int numerator, int denominator) {
  control_time_signatures_ = {{0.0, {numerator, denominator}}};
  publish_tempo_map_snapshot();
}

void RealtimeEngine::set_time_signature_segments(
    std::vector<transport::TimeSignatureSegment> segments) {
  control_time_signatures_ = std::move(segments);
  publish_tempo_map_snapshot();
}

int64_t RealtimeEngine::sample_at_ppq(double ppq) const noexcept {
  const transport::TempoMap* snapshot = tempo_map_snapshot_.control_current().get();
  const transport::TempoMap& map = *(snapshot ? snapshot : &tempo_map_);
  return map.ppq_to_sample(ppq);
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

void RealtimeEngine::update_reported_graph_latency() noexcept {
  int latency_q8 = 0;
#if defined(SONARE_WITH_ARRANGEMENT)
  latency_q8 += pdc_total_q8_;
#endif
#if defined(SONARE_WITH_MIXING)
  latency_q8 += track_mixer_runtime_.latency_samples_q8();
  if (mixing_enabled_) {
    latency_q8 += mixing_runtime_.latency_samples_q8();
  }
#endif
  set_graph_latency_samples_q8(latency_q8);
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
  if (bars <= 0) return start_sample;
  const transport::TempoMap* snapshot = tempo_map_snapshot_.control_current().get();
  const transport::TempoMap& map = *(snapshot ? snapshot : &tempo_map_);
  const double start_ppq = map.sample_to_ppq(start_sample);
  const double bar_start = map.bar_start_ppq(start_ppq);
  const transport::TimeSignature sig = map.time_signature_at_ppq(start_ppq);
  const double bar_len = static_cast<double>(std::max(sig.numerator, 1)) * 4.0 /
                         static_cast<double>(std::max(sig.denominator, 1));
  return map.ppq_to_sample(bar_start + bar_len * static_cast<double>(bars));
}

void RealtimeEngine::set_clips(std::vector<ClipSchedule> clips) {
  const transport::TempoMap* map = tempo_map_snapshot_.control_current().get();
  clip_player_.set_clips(std::move(clips), map ? map : &tempo_map_);
}

#if defined(SONARE_WITH_ARRANGEMENT)
void RealtimeEngine::set_midi_clips(std::vector<midi::MidiClipSchedule> clips) {
  midi_sequencer_.set_midi_clips(std::move(clips));
}

bool RealtimeEngine::set_midi_fx(uint32_t destination_id, const midi::MidiFxChain& chain) noexcept {
  return midi_sequencer_.set_midi_fx(destination_id, chain, transport_.render_frame());
}

void RealtimeEngine::clear_midi_fx(uint32_t destination_id) noexcept {
  midi_sequencer_.clear_midi_fx(destination_id);
}

void RealtimeEngine::emit_midi_transport_command(uint8_t status, int64_t render_frame) noexcept {
  MidiSyncSink* sync_sink = midi_sync_sink_.load(std::memory_order_acquire);
  if (sync_sink == nullptr) return;
  uint8_t byte = 0;
  if (midi::encode_transport_command(status, &byte, 1) != 1) return;
  sync_sink->on_midi_sync_byte(render_frame, byte);
}

void RealtimeEngine::emit_midi_clock_block(int64_t timeline_start_sample,
                                           int64_t render_start_frame, int num_frames) noexcept {
  MidiSyncSink* sync_sink = midi_sync_sink_.load(std::memory_order_acquire);
  if (sync_sink == nullptr || num_frames <= 0) return;
  const int64_t block_end_sample = timeline_start_sample + num_frames;
  for (int64_t tick = midi_clock_.first_tick_at_or_after(timeline_start_sample);
       midi_clock_.frame_of_tick(tick) < block_end_sample; ++tick) {
    const int64_t timeline_tick_frame = midi_clock_.frame_of_tick(tick);
    if (timeline_tick_frame < timeline_start_sample) continue;
    const int64_t render_frame = render_start_frame + (timeline_tick_frame - timeline_start_sample);
    sync_sink->on_midi_sync_byte(render_frame, midi::kStatusClock);
  }
}

void RealtimeEngine::dispatch_live_midi_input(int64_t render_start_frame, int num_frames) noexcept {
  if (num_frames <= 0) return;
  const int64_t render_end_frame = render_start_frame + num_frames;
  for (size_t i = 0; i < live_midi_input_count_; ++i) {
    const midi::MidiEvent& event = live_midi_input_events_[i];
    if (event.render_frame < render_start_frame) continue;
    if (event.render_frame >= render_end_frame) break;
    uint8_t cc = 0;
    float norm = 0.0f;
    uint32_t param_id = 0;
    float mapped_value = 0.0f;
    if (midi::cc_number_of(event.ump, &cc) && midi::cc_normalized_value(event.ump, &norm) &&
        midi_cc_map_.lookup_param(cc, event.ump.channel(), &param_id) &&
        midi_cc_map_.value_to_unit(cc, event.ump.channel(), norm, &mapped_value)) {
      automation_.set_parameter(param_id, mapped_value);
    }
    midi_sequencer_.inject_event(live_midi_input_destination_id_, event.render_frame, event.ump);
  }
}

void RealtimeEngine::set_midi_instrument(midi::MidiInstrument* instrument) noexcept {
  set_midi_instrument(0, instrument);
}

bool RealtimeEngine::set_midi_instrument(uint32_t destination_id,
                                         midi::MidiInstrument* instrument) noexcept {
  // Hang-note safety on swap/clear: if this destination currently has a bound
  // instrument that is about to be replaced or removed, release every note
  // sounding on it first. The note-offs route through the rack to the OUTGOING
  // instrument (still bound at this point) before the binding changes, so it
  // does not leave a hanging note. set_midi_instrument is control-thread only
  // and called between blocks, matching the sequencer's mutation contract.
  midi::MidiInstrument* const previous = instrument_rack_.get(destination_id);
  if (previous != nullptr && previous != instrument) {
    midi_sequencer_.all_notes_off_for_destination(destination_id, transport_.render_frame());
  }
  if (!instrument_rack_.set(destination_id, instrument)) {
    return false;  // rack full: leave existing bindings untouched
  }
  // The sequencer's sink is the rack itself (set in prepare); no per-instrument
  // sink wiring is needed. Prepare the freshly-registered instrument to the
  // engine's sample rate / block size. prepare() may allocate, so this stays a
  // control-thread operation.
  if (instrument != nullptr && max_block_size_ > 0) {
    instrument->prepare(sample_rate_, max_block_size_);
  }
  // The bound set (and thus the maximum instrument latency) changed: refresh the
  // PDC delays so clip + instrument audio stays phase-aligned. Control-thread
  // only, matching the delay lines' reallocation contract.
  recompute_pdc();
  return true;
}

void RealtimeEngine::recompute_pdc() noexcept {
  // The whole project's reported latency is the slowest bound instrument: every
  // source must be delayed to meet it. Clip audio (zero latency) is delayed by
  // the full total; an instrument that already self-delays by L_i needs only the
  // remaining (total - L_i). After both, all sources coincide at +total. Tracked
  // in Q8.8 so an instrument's sub-sample latency is compensated too (M-45).
  pdc_total_q8_ = instrument_rack_.max_latency_samples_q8();
  clip_pdc_delay_.set_delay_q8(pdc_total_q8_);
  pdc_instrument_count_ = 0;
  instrument_rack_.for_each([&](uint32_t destination_id, midi::MidiInstrument* instrument) {
    if (pdc_instrument_count_ >= instrument_pdc_delays_.size()) return;
    const size_t slot = pdc_instrument_count_++;
    instrument_pdc_dest_[slot] = destination_id;
    instrument_pdc_delays_[slot].set_delay_q8(pdc_total_q8_ - instrument->latency_samples_q8());
  });
  // Surface the applied compensation as the engine's graph latency so transport
  // telemetry (audible_timeline_sample) reflects the real output delay.
  update_reported_graph_latency();
}

void RealtimeEngine::flush_pdc_delays() noexcept {
  if (pdc_total_q8_ > 0) {
    clip_pdc_delay_.reset();
    for (size_t i = 0; i < pdc_instrument_count_; ++i) {
      instrument_pdc_delays_[i].reset();
    }
  }
#if defined(SONARE_WITH_MIXING)
  track_mixer_runtime_.flush_pdc_delays();
#endif
}
#endif

void RealtimeEngine::set_capture_segment(CaptureSegment segment) noexcept {
  capture_sink_.prepare(segment);
}

void RealtimeEngine::set_capture_armed(bool armed) noexcept { capture_sink_.arm(armed); }

void RealtimeEngine::set_capture_punch(int64_t start_sample, int64_t end_sample,
                                       bool enabled) noexcept {
  capture_sink_.set_punch(start_sample, end_sample, enabled);
}

void RealtimeEngine::reset_capture() noexcept { capture_sink_.reset(); }

bool RealtimeEngine::parameter_target_reserved(uint32_t target_id) noexcept {
  return (target_id & kEngineParamNamespaceMask) == kEngineParamNamespace;
}

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
  if (parameter_target_reserved(param_id)) {
    return false;
  }
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
#if defined(SONARE_WITH_MIXING)
      if (parameter_target_reserved(command.target_id)) {
        if (!route_engine_parameter(command.target_id, command.arg.f)) {
          enqueue_error(TelemetryErrorCode::kUnknownTarget, transport_.render_frame(),
                        transport_.sample_position(), command.target_id);
        }
        break;
      }
#endif
      // Failures (unknown target / non-RT-safe) bump automation_ counters,
      // which process() converts to telemetry after the sub-block loop. Do
      // not emit an error here or the rejection would be double-reported.
      automation_.set_parameter(command.target_id, command.arg.f);
      break;
    case rt::CommandType::kSetParamSmoothed:
#if defined(SONARE_WITH_MIXING)
      if (parameter_target_reserved(command.target_id) &&
          !route_engine_parameter(command.target_id, command.arg.f)) {
        enqueue_error(TelemetryErrorCode::kUnknownTarget, transport_.render_frame(),
                      transport_.sample_position(), command.target_id);
        break;
      }
#endif
      // Engine-level smoothing: start (or retarget) a one-pole ramp toward the
      // requested value. The ramp is ticked once per control period in
      // process() and pushed to the bound parameter, avoiding the zipper noise
      // of an immediate jump for targets that do not smooth internally.
      start_smoothed_param(command.target_id, command.arg.f);
      break;
    case rt::CommandType::kTransportPlay:
      emit_midi_transport_command(
          transport_.sample_position() <= 0 ? midi::kStatusStart : midi::kStatusContinue,
          command.sample_time);
      transport_.play();
      break;
    case rt::CommandType::kTransportStop:
      transport_.stop();
      emit_midi_transport_command(midi::kStatusStop, command.sample_time);
      // Hang-note safety: stopping is a playback discontinuity. Release every
      // sounding note at the stop frame so a sustained note does not hang (the
      // playhead freezes on stop, so a scheduled note-off would never arrive),
      // and the active-note table is cleared. RT-safe (no alloc). The note-offs
      // reach the instrument even though sub-block dispatch/render is gated off
      // while stopped, so the instrument falls silent on the next render.
#if defined(SONARE_WITH_ARRANGEMENT)
      midi_sequencer_.all_notes_off(command.sample_time);
      // Flush PDC delay tails: their buffered audio belongs to the pre-stop
      // position and must not ring out across the discontinuity.
      flush_pdc_delays();
#endif
#if defined(SONARE_WITH_MIXING)
      track_mixer_runtime_.flush_pdc_delays();
#endif
      break;
    case rt::CommandType::kTransportSeekSample:
      transport_.seek_sample(command.arg.i);
      // Hang-note safety: a seek jumps the playhead, so notes sounding before
      // the jump must be released at the seek frame rather than left to a
      // note-off that the new position will never reach.
#if defined(SONARE_WITH_ARRANGEMENT)
      midi_sequencer_.all_notes_off(command.sample_time);
      flush_pdc_delays();
#endif
#if defined(SONARE_WITH_MIXING)
      track_mixer_runtime_.flush_pdc_delays();
#endif
      break;
    case rt::CommandType::kTransportSeekPpq:
      transport_.seek_ppq(command.arg.d);
#if defined(SONARE_WITH_ARRANGEMENT)
      midi_sequencer_.all_notes_off(command.sample_time);
      flush_pdc_delays();
#endif
#if defined(SONARE_WITH_MIXING)
      track_mixer_runtime_.flush_pdc_delays();
#endif
      break;
    case rt::CommandType::kSeekMarker:
      if (!seek_marker(command.target_id)) {
        enqueue_error(TelemetryErrorCode::kUnknownTarget, transport_.render_frame(),
                      transport_.sample_position(), command.target_id);
      } else {
        // Successful marker seek is a playhead jump: same hang-note release.
#if defined(SONARE_WITH_ARRANGEMENT)
        midi_sequencer_.all_notes_off(command.sample_time);
        flush_pdc_delays();
#endif
#if defined(SONARE_WITH_MIXING)
        track_mixer_runtime_.flush_pdc_delays();
#endif
      }
      break;
    case rt::CommandType::kMidiNoteOnImmediate:
    case rt::CommandType::kMidiNoteOffImmediate: {
#if defined(SONARE_WITH_ARRANGEMENT)
      const uint64_t packed = static_cast<uint64_t>(command.arg.i);
      const uint8_t velocity = static_cast<uint8_t>(packed & 0x7Fu);
      const uint8_t note = static_cast<uint8_t>((packed >> 8) & 0x7Fu);
      const uint8_t channel = static_cast<uint8_t>((packed >> 16) & 0x0Fu);
      const uint8_t group = static_cast<uint8_t>((packed >> 24) & 0x0Fu);
      const midi::Ump ump = command.type == rt::CommandType::kMidiNoteOnImmediate
                                ? midi::make_midi1_note_on(group, channel, note, velocity)
                                : midi::make_midi1_note_off(group, channel, note, velocity);
      midi_sequencer_.inject_event(command.target_id, command.sample_time, ump);
#endif
      break;
    }
    case rt::CommandType::kMidiCcImmediate: {
      // Group (1) queueable scalar MIDI: synthesize a MIDI 1.0 CC UMP from the
      // packed scalar fields and route it through the sequencer's host-injection
      // path so it reaches the instrument exactly like a clip-scheduled event.
      // RT-safe: no allocation. command.sample_time has already been clamped to
      // a concrete render frame by drain_commands (>= 0 here).
#if defined(SONARE_WITH_ARRANGEMENT)
      const uint64_t packed = static_cast<uint64_t>(command.arg.i);
      const uint8_t value7 = static_cast<uint8_t>(packed & 0x7Fu);
      const uint8_t controller = static_cast<uint8_t>((packed >> 8) & 0x7Fu);
      const uint8_t channel = static_cast<uint8_t>((packed >> 16) & 0x0Fu);
      const uint8_t group = static_cast<uint8_t>((packed >> 24) & 0x0Fu);
      const midi::Ump ump = midi::make_midi1_control_change(group, channel, controller, value7);
      uint32_t param_id = 0;
      float mapped_value = 0.0f;
      if (midi_cc_map_.lookup_param(controller, channel, &param_id) &&
          midi_cc_map_.value_to_unit(controller, channel, static_cast<float>(value7) / 127.0f,
                                     &mapped_value)) {
        automation_.set_parameter(param_id, mapped_value);
      }
      midi_sequencer_.inject_event(command.target_id, command.sample_time, ump);
#endif
      break;
    }
    case rt::CommandType::kMidiAllNotesOff:
      // MIDI panic: release every sounding note tracked by the sequencer at this
      // command's render frame. RT-safe, no allocation.
#if defined(SONARE_WITH_ARRANGEMENT)
      midi_sequencer_.all_notes_off(command.sample_time);
#endif
      break;
    case rt::CommandType::kSetSoloMute: {
#if defined(SONARE_WITH_MIXING)
      const uint64_t packed = static_cast<uint64_t>(command.arg.i);
      const bool mute = (packed & 0x1u) != 0u;
      const bool solo = (packed & 0x2u) != 0u;
      if (!track_mixer_runtime_.set_lane_solo_mute(static_cast<size_t>(command.target_id), solo,
                                                   mute)) {
        enqueue_error(TelemetryErrorCode::kUnknownTarget, transport_.render_frame(),
                      transport_.sample_position(), command.target_id);
      }
#else
      enqueue_error(TelemetryErrorCode::kUnknownTarget, transport_.render_frame(),
                    transport_.sample_position(), command.target_id);
#endif
      break;
    }
    case rt::CommandType::kSetTrackInsertParam: {
#if defined(SONARE_WITH_MIXING)
      // target_id = (lane_index << 16) | (insert_index << 8) | param_id; the
      // control thread resolved the JSON-key name to param_id before enqueuing.
      const uint32_t packed = command.target_id;
      const size_t lane_index = (packed >> 16) & 0xFFu;
      const unsigned int insert_index = (packed >> 8) & 0xFFu;
      const unsigned int param_id = packed & 0xFFu;
      if (!track_mixer_runtime_.apply_lane_insert_parameter(lane_index, insert_index, param_id,
                                                            command.arg.f)) {
        enqueue_error(TelemetryErrorCode::kUnknownTarget, transport_.render_frame(),
                      transport_.sample_position(), command.target_id);
      }
#else
      enqueue_error(TelemetryErrorCode::kUnknownTarget, transport_.render_frame(),
                    transport_.sample_position(), command.target_id);
#endif
      break;
    }
    case rt::CommandType::kSetMasterInsertParam: {
#if defined(SONARE_WITH_MIXING)
      // target_id = (insert_index << 8) | param_id (no lane field for master).
      const uint32_t packed = command.target_id;
      const unsigned int insert_index = (packed >> 8) & 0xFFu;
      const unsigned int param_id = packed & 0xFFu;
      if (owned_master_strip_ == nullptr ||
          !owned_master_strip_->apply_insert_parameter(insert_index, param_id, command.arg.f)) {
        enqueue_error(TelemetryErrorCode::kUnknownTarget, transport_.render_frame(),
                      transport_.sample_position(), command.target_id);
      }
#else
      enqueue_error(TelemetryErrorCode::kUnknownTarget, transport_.render_frame(),
                    transport_.sample_position(), command.target_id);
#endif
      break;
    }
    case rt::CommandType::kSetTempoMap:
    case rt::CommandType::kSetLoop:
    case rt::CommandType::kSwapGraph:
    case rt::CommandType::kSwapAutomation:
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
void RealtimeEngine::set_mixing_enabled(bool enabled) noexcept {
  mixing_enabled_ = enabled;
  update_reported_graph_latency();
}

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
  if (bound) {
    update_reported_graph_latency();
  }
  return bound;
}

bool RealtimeEngine::set_master_strip(const mixing::api::Strip& strip_spec) {
  std::unique_ptr<mixing::ChannelStrip> strip;
  try {
    strip = make_channel_strip_from_spec(strip_spec);
  } catch (...) {
    return false;
  }
  if (!strip) return false;
  owned_master_strip_ = std::move(strip);
  const bool bound = bind_mixing_strip(owned_master_strip_.get());
  if (bound) {
    set_mixing_enabled(true);
  }
  return bound;
}

bool RealtimeEngine::set_track_lanes(std::vector<TrackLaneConfig> lanes) {
  const bool ok = track_mixer_runtime_.set_track_lanes(std::move(lanes));
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_track_buses(std::vector<TrackBusConfig> buses) {
  const bool ok = track_mixer_runtime_.set_buses(std::move(buses));
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::bind_track_strip(uint32_t track_id, mixing::ChannelStrip* strip) {
  const bool ok = track_mixer_runtime_.bind_track_strip(track_id, strip);
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_track_strip(uint32_t track_id, const mixing::api::Strip& strip) {
  const bool ok = track_mixer_runtime_.set_track_strip(track_id, strip);
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_bus_strip(uint32_t bus_id, const mixing::api::Bus& bus) {
  const bool ok = track_mixer_runtime_.set_bus_strip(bus_id, bus);
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_track_insert_bypassed(uint32_t track_id, unsigned int insert_index,
                                               bool bypassed, bool reset_on_bypass) noexcept {
  return track_mixer_runtime_.set_track_insert_bypassed(track_id, insert_index, bypassed,
                                                        reset_on_bypass);
}

bool RealtimeEngine::set_master_insert_bypassed(unsigned int insert_index, bool bypassed,
                                                bool reset_on_bypass) noexcept {
  return owned_master_strip_ != nullptr &&
         owned_master_strip_->set_insert_bypassed(insert_index, bypassed, reset_on_bypass);
}

bool RealtimeEngine::set_track_insert_param(uint32_t track_id, unsigned int insert_index,
                                            const std::string& key, float value) noexcept {
  size_t lane_index = 0;
  unsigned int param_id = 0;
  if (!track_mixer_runtime_.resolve_track_insert_param(track_id, insert_index, key, &lane_index,
                                                       &param_id)) {
    return false;
  }
  if (lane_index > 0xFFu || insert_index > 0xFFu || param_id > 0xFFu) {
    return false;
  }
  rt::Command command;
  command.type = rt::CommandType::kSetTrackInsertParam;
  command.target_id = (static_cast<uint32_t>(lane_index) << 16) | ((insert_index & 0xFFu) << 8) |
                      (param_id & 0xFFu);
  command.sample_time = -1;  // block head / immediate
  command.arg.f = value;
  return push_command(command);
}

bool RealtimeEngine::set_master_insert_param(unsigned int insert_index, const std::string& key,
                                             float value) noexcept {
  if (owned_master_strip_ == nullptr) {
    return false;
  }
  const int id = owned_master_strip_->insert_parameter_id_for_key(insert_index, key);
  if (id < 0) {
    return false;
  }
  const unsigned int param_id = static_cast<unsigned int>(id);
  if (insert_index > 0xFFu || param_id > 0xFFu) {
    return false;
  }
  rt::Command command;
  command.type = rt::CommandType::kSetMasterInsertParam;
  command.target_id = ((insert_index & 0xFFu) << 8) | (param_id & 0xFFu);
  command.sample_time = -1;  // block head / immediate
  command.arg.f = value;
  return push_command(command);
}

bool RealtimeEngine::set_track_eq_band(uint32_t track_id, size_t band_index,
                                       const mastering::eq::EqBand& band) noexcept {
  const bool ok = track_mixer_runtime_.set_track_eq_band(track_id, band_index, band);
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_track_pan(uint32_t track_id, float pan) noexcept {
  return track_mixer_runtime_.set_track_pan(track_id, pan);
}

bool RealtimeEngine::set_track_pan_law(uint32_t track_id, mixing::PanLaw law) noexcept {
  return track_mixer_runtime_.set_track_pan_law(track_id, law);
}

bool RealtimeEngine::set_track_pan_mode(uint32_t track_id, mixing::PanMode mode) noexcept {
  return track_mixer_runtime_.set_track_pan_mode(track_id, mode);
}

bool RealtimeEngine::set_track_dual_pan(uint32_t track_id, float left_pan,
                                        float right_pan) noexcept {
  return track_mixer_runtime_.set_track_dual_pan(track_id, left_pan, right_pan);
}

bool RealtimeEngine::set_track_channel_delay_samples(uint32_t track_id,
                                                     int delay_samples) noexcept {
  const bool ok = track_mixer_runtime_.set_track_channel_delay_samples(track_id, delay_samples);
  if (ok) {
    update_reported_graph_latency();
  }
  return ok;
}

bool RealtimeEngine::set_master_eq_band(size_t band_index,
                                        const mastering::eq::EqBand& band) noexcept {
  if (owned_master_strip_ == nullptr) return false;
  try {
    owned_master_strip_->set_eq_band(band_index, band);
    update_reported_graph_latency();
    return true;
  } catch (...) {
    return false;
  }
}

uint32_t RealtimeEngine::configure_scope_telemetry(int interval_frames,
                                                   uint32_t band_count) noexcept {
  scope_interval_frames_ = std::max(0, interval_frames);
  const uint32_t clamped = std::clamp<uint32_t>(band_count, 1, ScopeTelemetryRecord::kMaxBands);
  if (clamped != scope_band_count_) {
    scope_band_count_ = clamped;
    if (max_block_size_ > 0) {
      // Re-prepare the tap with the new band resolution. Control-thread only,
      // not concurrent with process() (same contract as prepare()).
      scope_tap_.prepare(
          sample_rate_, max_block_size_,
          8 * (TrackMixerRuntime::kMaxTrackLanes + TrackMixerRuntime::kMaxBusLanes + 2), 2048,
          scope_band_count_);
    }
  }
  return scope_tap_.band_count();
}

bool RealtimeEngine::route_engine_parameter(uint32_t target_id, float value) noexcept {
  if (!parameter_target_reserved(target_id)) return false;
  const uint32_t lane = (target_id & kEngineParamLaneMask) >> kEngineParamLaneShift;
  const uint32_t kind = target_id & kEngineParamKindMask;
  if (lane == kEngineParamLaneMaster) {
    return mixing_runtime_.set_parameter(kind, value);
  }
  if (lane <= kEngineParamLaneBusBase &&
      lane > kEngineParamLaneBusBase - TrackMixerRuntime::kMaxBusLanes) {
    if (kind != TrackMixerRuntime::kFaderDb) return false;
    const uint32_t bus_index = kEngineParamLaneBusBase - lane;
    return track_mixer_runtime_.set_bus_gain_db_by_index(bus_index, value);
  }
  return track_mixer_runtime_.set_lane_parameter(static_cast<size_t>(lane), kind, value);
}

bool RealtimeEngine::route_engine_parameter_thunk(void* context, uint32_t param_id,
                                                  float value) noexcept {
  return static_cast<RealtimeEngine*>(context)->route_engine_parameter(param_id, value);
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

void RealtimeEngine::settle_parameters() noexcept {
  for (SmoothedParam& slot : smoothed_params_) {
    if (!slot.active) continue;
    const float target = slot.smoother.target();
    slot.smoother.reset(target);
#if defined(SONARE_WITH_MIXING)
    if (parameter_target_reserved(slot.target_id)) {
      route_engine_parameter(slot.target_id, target);
    } else {
      automation_.set_parameter(slot.target_id, target);
    }
#else
    automation_.set_parameter(slot.target_id, target);
#endif
    slot.active = false;
    slot.target_id = 0;
  }
#if defined(SONARE_WITH_MIXING)
  track_mixer_runtime_.settle_smoothers();
#endif
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
#if defined(SONARE_WITH_MIXING)
    if (parameter_target_reserved(slot.target_id)) {
      if (!route_engine_parameter(slot.target_id, current)) {
        enqueue_error(TelemetryErrorCode::kUnknownTarget, transport_.render_frame(),
                      transport_.sample_position(), slot.target_id);
        slot.active = false;
        slot.target_id = 0;
        continue;
      }
    } else {
      automation_.set_parameter(slot.target_id, current);
    }
#else
    automation_.set_parameter(slot.target_id, current);
#endif
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
  const bool capture_input = capture_source() == CaptureSource::kInput;
  const int scratch_channels =
      std::min<int>(std::max(num_channels, 0), static_cast<int>(sub_channels.size()));
#if defined(SONARE_WITH_MIXING)
  // Gate spectrum/vectorscope capture for this block; the per-target taps inside
  // the mixer + the master tap below self-skip when this block is not due.
  scope_tap_.begin_block(scope_interval_frames_, num_frames);
#endif
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
    if (capture_input) {
      for (int ch = 0; ch < channels; ++ch) {
        float* dst = input_capture_channels_[static_cast<size_t>(ch)];
        const float* src = sub_channels[static_cast<size_t>(ch)];
        if (!dst) continue;
        if (src) {
          std::copy(src, src + num_frames, dst);
        } else {
          std::fill(dst, dst + num_frames, 0.0f);
        }
      }
#if defined(SONARE_WITH_MIXING)
      meter_tap_.process_lightweight(input_capture_channels_.data(), channels, num_frames,
                                     transport_.render_frame(), 0xFFFFu);
#endif
    }
    const InputMonitorState monitor = input_monitor_.try_load();
    if (!monitor.enabled || monitor.gain != 1.0f) {
      for (int ch = 0; ch < channels; ++ch) {
        float* channel = sub_channels[static_cast<size_t>(ch)];
        if (!channel) continue;
        if (!monitor.enabled) {
          std::fill(channel, channel + num_frames, 0.0f);
        } else {
          for (int i = 0; i < num_frames; ++i) {
            channel[i] *= monitor.gain;
          }
        }
      }
    }
    // Clip audio and sequenced MIDI are both gated on the transport rolling.
    // While stopped, advance() freezes sample_position, so rendering clips
    // would replay the same clip window every block as a sustained buzz.
    const bool transport_rolling = transport_.playing();
#if defined(SONARE_WITH_ARRANGEMENT)
    if (pdc_total_q8_ > 0) {
      // PDC active: render the clip bus into scratch, delay it by the project's
      // total instrument latency so it lands phase-aligned with the
      // (internally-delayed) instruments, then sum it into the source layer.
      // Mirrors the additive-into-io contract of the direct path below. While
      // stopped, the delay keeps running on silence so its tail drains instead
      // of re-emerging stale on the next play.
      for (int ch = 0; ch < channels; ++ch) {
        if (clip_scratch_channels_[static_cast<size_t>(ch)]) {
          std::fill(clip_scratch_channels_[static_cast<size_t>(ch)],
                    clip_scratch_channels_[static_cast<size_t>(ch)] + num_frames, 0.0f);
        }
      }
      if (transport_rolling) {
#if defined(SONARE_WITH_MIXING)
        if (!track_mixer_runtime_.render_clips(clip_player_, clip_scratch_channels_.data(),
                                               channels, num_frames, transport_.sample_position(),
                                               &meter_tap_, transport_.render_frame(),
                                               &scope_tap_)) {
          clip_player_.process_at(clip_scratch_channels_.data(), channels, num_frames,
                                  transport_.sample_position());
        }
#else
        clip_player_.process_at(clip_scratch_channels_.data(), channels, num_frames,
                                transport_.sample_position());
#endif
      }
      clip_pdc_delay_.process(clip_scratch_channels_.data(), channels, num_frames);
      for (int ch = 0; ch < channels; ++ch) {
        float* out = sub_channels[static_cast<size_t>(ch)];
        const float* clip = clip_scratch_channels_[static_cast<size_t>(ch)];
        if (!out) continue;
        for (int i = 0; i < num_frames; ++i) out[i] += clip[i];
      }
    } else if (transport_rolling) {
#if defined(SONARE_WITH_MIXING)
      if (!track_mixer_runtime_.render_clips(clip_player_, sub_channels.data(), channels,
                                             num_frames, transport_.sample_position(), &meter_tap_,
                                             transport_.render_frame(), &scope_tap_)) {
        clip_player_.process_at(sub_channels.data(), channels, num_frames,
                                transport_.sample_position());
      }
#else
      clip_player_.process_at(sub_channels.data(), channels, num_frames,
                              transport_.sample_position());
#endif
    }
#else
    if (transport_rolling) {
#if defined(SONARE_WITH_MIXING)
      if (!track_mixer_runtime_.render_clips(clip_player_, sub_channels.data(), channels,
                                             num_frames, transport_.sample_position(), &meter_tap_,
                                             transport_.render_frame(), &scope_tap_)) {
        clip_player_.process_at(sub_channels.data(), channels, num_frames,
                                transport_.sample_position());
      }
#else
      clip_player_.process_at(sub_channels.data(), channels, num_frames,
                              transport_.sample_position());
#endif
    }
#endif
#if defined(SONARE_WITH_ARRANGEMENT)
    // While stopped, scanning the same window every block would also
    // re-dispatch the same note-ons (saturating the active-note table and
    // re-triggering the instrument) and capture a sustained note with no choke.
    // A stopped transport therefore dispatches nothing and renders no instrument
    // audio; kTransportStop already released sounding notes via all_notes_off.
    // Dispatch the MIDI events whose render frame falls in this sub-block. The
    // sequencer scans [block_start, block_start + num_frames); using the
    // sub-block's timeline sample position keeps dispatch sample-accurate and
    // aligned with the kMidi boundaries inserted above. No allocation.
    //
    // When an instrument is registered it IS the sequencer's sink, so this call
    // feeds the block's events to the instrument at their sample-accurate render
    // frames (event.render_frame relative to this sub-block's first frame). The
    // instrument buffers them; rendering happens immediately below so the events
    // and the audio they drive stay in the same sub-block.
    if (transport_rolling) {
      emit_midi_clock_block(transport_.sample_position(), transport_.render_frame(), num_frames);
      midi_sequencer_.process_block(transport_.sample_position(), num_frames);
    }
    dispatch_live_midi_input(transport_.render_frame(), num_frames);
    // Host-instrument audio injection: sum the instrument's render into the
    // SAME source layer as the clip player, AFTER clip playback + MIDI dispatch
    // and BEFORE the metronome / mixing-strip / monitor / graph stages. This is
    // the PINNED clip/source-merge injection point: instrument output therefore
    // flows through channel strips + monitoring + the graph exactly like clip
    // audio, and PDC/latency matches clips. Opt-in: nullptr leaves the chain and
    // the output bit-identical to the no-instrument path. RT-safe: the scratch
    // is sized in prepare(); the audio thread only zero-fills and sums it.
    if (!instrument_rack_.empty() &&
        (transport_rolling || midi_sequencer_.active_note_count() > 0)) {
      // Per-block transport snapshot pushed to each instrument before it renders
      // (H-4): a tempo-synced delay / arpeggiator / LFO follows the host
      // transport instead of free-running. Each instrument renders into the
      // shared scratch (zero, set_transport, process) and is summed into the
      // sub-block, so multitrack MIDI routed to distinct destinations mixes here.
      const transport::TransportState inst_state = transport_.snapshot();
#if defined(SONARE_WITH_MIXING)
      // Clear the shared buses once before mixing the whole rack so a stateful
      // bus insert chain (e.g. a shared reverb tail or compressor envelope)
      // advances exactly once per block, not once per instrument. Each instrument
      // accumulates into its lane/sends via mix_source_into_lane (no bus
      // processing); finish_source_mix runs the bus chains once afterwards. When
      // no lanes are configured this stays false and each source is summed
      // directly, exactly as before.
      const bool lane_mix_ready = track_mixer_runtime_.begin_source_mix(channels, num_frames);
      bool any_lane_routed = false;
#endif
      instrument_rack_.for_each(
          [&](uint32_t destination_id, midi::MidiInstrument* instrument) noexcept {
            for (int ch = 0; ch < channels; ++ch) {
              std::fill(midi_instrument_channels_[static_cast<size_t>(ch)],
                        midi_instrument_channels_[static_cast<size_t>(ch)] + num_frames, 0.0f);
            }
            instrument->set_transport(inst_state);
            instrument->process(midi_instrument_channels_.data(), channels, num_frames);
            // PDC: an instrument faster than the project's slowest is delayed by the
            // remainder (total - its own latency) so it stays aligned with the clip
            // bus and the other instruments. The slowest instrument's delay is 0.
            if (pdc_total_q8_ > 0) {
              for (size_t k = 0; k < pdc_instrument_count_; ++k) {
                if (instrument_pdc_dest_[k] == destination_id) {
                  instrument_pdc_delays_[k].process(midi_instrument_channels_.data(), channels,
                                                    num_frames);
                  break;
                }
              }
            }
#if defined(SONARE_WITH_MIXING)
            if (lane_mix_ready) {
              bool routed_through_lane = false;
              if (track_mixer_runtime_.mix_source_into_lane(
                      destination_id, midi_instrument_channels_.data(), sub_channels.data(),
                      channels, num_frames, routed_through_lane, &meter_tap_,
                      transport_.render_frame(), &scope_tap_)) {
                any_lane_routed = any_lane_routed || routed_through_lane;
                return;
              }
            }
#endif
            for (int ch = 0; ch < channels; ++ch) {
              float* out = sub_channels[static_cast<size_t>(ch)];
              const float* inst = midi_instrument_channels_[static_cast<size_t>(ch)];
              if (!out) continue;
              for (int i = 0; i < num_frames; ++i) {
                out[i] += inst[i];
              }
            }
          });
#if defined(SONARE_WITH_MIXING)
      // Process the shared bus chains once and sum them into the sub-block. Only
      // when at least one instrument routed through a lane -- mirrors the historic
      // behaviour where the rack stage touched the buses only on a lane match.
      if (lane_mix_ready && any_lane_routed) {
        track_mixer_runtime_.finish_source_mix(sub_channels.data(), channels, num_frames,
                                               &meter_tap_, transport_.render_frame(), &scope_tap_);
      }
#endif
    }
#endif
    if (transport_rolling) {
      metronome_.process(sub_channels.data(), channels, num_frames, transport_.sample_position());
    }
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
    // Master target_id 0 mirrors the master meter target so the host can pair a
    // master spectrum/vectorscope snapshot with its meter record.
    scope_tap_.process(sub_channels.data(), channels, num_frames, transport_.render_frame(), 0);
#endif
    const float* const* capture_channels =
        capture_input ? reinterpret_cast<const float* const*>(input_capture_channels_.data())
                      : reinterpret_cast<const float* const*>(sub_channels.data());
    if (!capture_sink_.punch_enabled() || transport_.playing()) {
      capture_sink_.process(capture_channels, channels, num_frames, transport_.sample_position());
    }
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
  if (telemetry_overflow_count_ > 0 && (telemetry.type != TelemetryType::kError ||
                                        telemetry.error == TelemetryErrorCode::kClipPageUnderrun)) {
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

void RealtimeEngine::on_clip_page_miss(const ClipPageRequest& request) noexcept {
  (void)clip_page_requests_.push(request);
  if (!clip_page_underrun_reported_this_block_) {
    clip_page_underrun_reported_this_block_ = true;
    enqueue_error(TelemetryErrorCode::kClipPageUnderrun, transport_.render_frame(),
                  transport_.sample_position(), request.clip_id);
  }
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
