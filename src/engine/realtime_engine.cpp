#include "engine/realtime_engine.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/insert_automation_id.h"
#include "engine/realtime_engine_internal.h"
#include "rt/scoped_no_denormals.h"
#include "util/math_utils.h"

namespace sonare::engine {

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

  adopt_tempo_map_snapshot();
  const transport::TempoMap& tempo_map = *(active_tempo_map_ ? active_tempo_map_ : &tempo_map_);
  const auto state = transport_.snapshot();
  clip_page_underrun_reported_this_block_ = false;
  // Adopt the latest published clip / automation snapshots exactly once at
  // block start. Every per-sub-block read below then sees a stable set, so a
  // control-thread publish can never swap data mid-block.
  clip_player_.acquire_clips();
#if defined(SONARE_WITH_GRAPH)
  // Graph topology and its automation target table share one immutable
  // snapshot, adopted before automation so both remain aligned for this block.
  graph_runtime_.acquire();
#endif
  automation_.acquire_lanes();
#if defined(SONARE_WITH_ARRANGEMENT)
  midi_sequencer_.acquire_midi_clips();
  midi_sequencer_.acquire_midi_fx(state.render_frame);
  midi_cc_maps_.acquire();
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
  const CaptureSink::PunchState punch = capture_sink_.punch_state_rt();
  if (punch.armed && punch.punch_enabled) {
    CaptureBoundaryList capture_boundaries;
    collect_capture_boundaries(state.sample_position, frames, punch.punch_start_sample,
                               punch.punch_end_sample, &capture_boundaries);
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
#if defined(SONARE_WITH_MIXING)
  // Insert-parameter automation that could not claim a smoother slot (master or
  // per-lane/bus table full) is dropped silently in the audio path; surface the
  // per-block delta so the host can see automation targets going unheard.
  const uint32_t insert_overflow_total = insert_automation_overflow_count();
  if (insert_overflow_total != insert_automation_overflow_reported_) {
    const uint32_t delta = insert_overflow_total - insert_automation_overflow_reported_;
    insert_automation_overflow_reported_ = insert_overflow_total;
    enqueue_error(TelemetryErrorCode::kInsertAutomationOverflow, state.render_frame,
                  state.sample_position, delta);
  }
#endif
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
  scope_tap_.begin_block(scope_interval_frames_.load(std::memory_order_relaxed), num_frames);
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
      // The sequencer stamps events in TIMELINE samples; translate them to the
      // monotonic DEVICE render frame as they enter the external output queue so
      // a loop wrap (timeline jumps backward, device keeps rising) cannot invert
      // their order. Restored to 0 afterwards so the device-framed all-notes-off
      // / command dispatch paths pass through untranslated.
      midi_dispatch_sink_.timeline_to_device_offset =
          transport_.render_frame() - transport_.sample_position();
      midi_sequencer_.process_block(transport_.sample_position(), num_frames);
      midi_dispatch_sink_.timeline_to_device_offset = 0;
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
    if (mixing_enabled_.load(std::memory_order_relaxed)) {
      mixing_runtime_.process_at(sub_channels.data(), channels, num_frames,
                                 transport_.sample_position());
    }
    // Solo/mute + PFL/AFL monitoring stage for any registered strips. Existing
    // process() callers keep foldback compatibility; process_with_monitor()
    // receives the cue bus separately without contaminating the main output.
    if (monitoring_enabled_.load(std::memory_order_relaxed)) {
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
    if (!capture_sink_.punch_state_rt().punch_enabled || transport_.playing()) {
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

}  // namespace sonare::engine
