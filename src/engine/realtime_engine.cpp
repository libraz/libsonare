#include "engine/realtime_engine.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/insert_automation_id.h"
#include "engine/realtime_engine_internal.h"
#include "midi/midi_clip_envelope.h"
#include "midi/ump.h"
#include "rt/scoped_no_denormals.h"
#include "util/math_utils.h"
#include "util/numeric_validation.h"

namespace sonare::engine {

bool RealtimeEngine::is_pushable_midi_ump(const uint32_t* words, size_t count) noexcept {
  if (words == nullptr || count == 0 || count > 4) return false;
  if (count != midi::ump_word_count_for_word0(words[0])) return false;
  const auto type = static_cast<midi::UmpMessageType>((words[0] >> 28) & 0x0Fu);
  return type != midi::UmpMessageType::kData64 && type != midi::UmpMessageType::kData128;
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
  if (num_channels > prepared_channels_) {
    const auto state = transport_.snapshot();
    silence(io, num_channels, frames);
    silence(monitor_out, num_channels, frames);
    transport_.advance(frames);
    enqueue_error(TelemetryErrorCode::kMaxChannelsExceeded, state.render_frame,
                  state.sample_position, static_cast<uint32_t>(num_channels));
    return;
  }

  adopt_tempo_map_snapshot();
  const transport::TempoMap& tempo_map = *(active_tempo_map_ ? active_tempo_map_ : &tempo_map_);
  // A playhead left at or past loop_end (a loop set behind it, a seek or a new
  // tempo map) wraps before anything reads it, not after the first sub-block.
  [[maybe_unused]] const bool wrapped_at_block_start = transport_.fold_into_loop();
  const auto state = transport_.snapshot();
  clip_page_underrun_reported_this_block_ = false;
#if defined(SONARE_WITH_MIXING)
  meter_tap_.begin_block();
  master_insert_gr_board_.begin_block();
  track_mixer_runtime_.begin_insert_gain_reduction_block();
  // Gate scope capture once for the host block. Automation may split this block
  // into many sub-blocks, but those splits must not accelerate the interval
  // counter or change whether the block is due.
  scope_tap_.begin_block(scope_interval_frames_.load(std::memory_order_relaxed), frames);
#endif
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
  midi_sequencer_.acquire_midi_fx(midi::DeviceFrame{state.render_frame});
  // Adopt routes after MIDI-FX cleanup (old route) and before any event dispatches this block.
  adopt_midi_destination_routes(state.render_frame);
  midi_cc_maps_.acquire();
  host::MidiInputSource* midi_input_source = midi_input_source_.load(std::memory_order_acquire);
  const uint64_t source_generation = midi_input_source_generation_.load(std::memory_order_relaxed);
  if (source_generation != adopted_midi_input_source_generation_) {
    // A new input stream never completes a controller gesture the old one began.
    adopted_midi_input_source_generation_ = source_generation;
    if (const midi::CcMap* cc_map = midi_cc_maps_.current()) cc_map->reset_live_decode();
  }
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

  // Device-framed edges are known up front: queued commands, live input and the
  // control-period cadence. Edges derived from the timeline (loop wrap, clip and
  // punch edges, automation breakpoints, MIDI events) are found span by span from
  // the transport state in effect, so an in-block Play, seek or wrap moves them.
  BoundaryBuildContext boundary_context{};
  boundary_context.block_render_frame = state.render_frame;
  boundary_context.block_timeline_sample = state.sample_position;
  boundary_context.num_frames = frames;
  boundary_splitter_.begin(boundary_context);
  for (const rt::Command& command : pending_) {
    const auto sample_time = command.sample_time;
    if (command_belongs_to_block(sample_time, state.render_frame, frames)) {
      boundary_splitter_.add_command(static_cast<int>(sample_time - state.render_frame));
    }
  }
#if defined(SONARE_WITH_ARRANGEMENT)
  live_midi_input_cursor_ = 0;
  for (size_t i = 0; i < live_midi_input_count_; ++i) {
    const int64_t event_frame = live_midi_input_events_[i].render_frame;
    if (command_belongs_to_block(event_frame, state.render_frame, frames)) {
      boundary_splitter_.add_midi(static_cast<int>(event_frame - state.render_frame));
    }
  }
#endif
  // Control-period edges re-evaluate automation lanes and engine-level smoothers
  // at a bounded cadence. Boundary storage covers every offset of the prepared
  // block (prepare_impl), so the cadence cannot evict command or MIDI edges.
  if (automation_.lane_count() > 0 || any_smoothed_param_active()) {
    for (int offset = kControlPeriod; offset < frames; offset += kControlPeriod) {
      boundary_splitter_.add_automation(offset);
    }
  }

  // The first frame on the timeline that changes what the next span renders,
  // capped at @p max_frames: loop end, clip and punch edges, automation breakpoints.
  const auto timeline_span = [&](int max_frames) noexcept {
    if (!transport_.playing()) return max_frames;
    const auto now = transport_.snapshot();
    const int64_t position = now.sample_position;
    int span = max_frames;
    const auto cap = [&](int64_t frame) noexcept {
      const int64_t distance = numeric::saturating_sub(frame, position);
      if (distance > 0 && distance < span) span = static_cast<int>(distance);
    };
    if (now.looping && now.loop_end_ppq > now.loop_start_ppq) {
      cap(tempo_map.ppq_to_sample(now.loop_end_ppq));
    }
    ClipBoundaryList clip_boundaries;
    clip_player_.collect_boundaries(position, span, &clip_boundaries);
    for (size_t i = 0; i < clip_boundaries.size; ++i) {
      cap(numeric::saturating_add(position, static_cast<int64_t>(clip_boundaries.offsets[i])));
    }
    const CaptureSink::PunchState punch = capture_sink_.punch_state_rt();
    if (punch.armed && punch.punch_enabled) {
      const int64_t record_offset = record_offset_samples_.load(std::memory_order_acquire);
      cap(numeric::saturating_add(punch.punch_start_sample, record_offset));
      cap(numeric::saturating_add(punch.punch_end_sample, record_offset));
    }
    if (automation_.lane_count() > 0) {
      automation::AutomationBoundaryList automation_boundaries;
      automation_.collect_boundaries(
          now.ppq_position,
          tempo_map.sample_to_ppq(numeric::saturating_add(position, static_cast<int64_t>(span))),
          &automation_boundaries);
      for (size_t i = 0; i < automation_boundaries.size; ++i) {
        cap(tempo_map.ppq_to_sample(automation_boundaries.ppq[i]));
      }
    }
    return span;
  };

  const uint32_t capture_overflow_before = capture_sink_.overflow_count();
  const BoundaryList& boundaries = boundary_splitter_.finish();
  clip_player_.begin_page_miss_block();
  size_t next_boundary = 0;
  bool wrapped = wrapped_at_block_start;
  int offset = 0;
  while (offset < frames) {
    while (next_boundary < boundaries.size() && boundaries[next_boundary].offset <= offset) {
      ++next_boundary;
    }
    const int64_t render_frame = transport_.render_frame();
#if defined(SONARE_WITH_ARRANGEMENT)
    // Hang-note safety: notes of the iteration a wrap left are released before
    // this frame's commands, so a note-on queued for the wrap frame belongs to
    // the new iteration and is not released with them.
    if (wrapped) {
      publish_instrument_transport();
      midi_sequencer_.all_notes_off(midi::DeviceFrame{render_frame});
    }
    // Commands of this span must see its transport first.
    publish_instrument_transport();
#endif
    apply_due_commands(render_frame);
    int span = next_boundary < boundaries.size() ? boundaries[next_boundary].offset - offset
                                                 : frames - offset;
    span = timeline_span(span);
    // Automation is evaluated at the span start with the transport the commands left.
    automation_.apply(transport_.snapshot(), 0, span);
#if defined(SONARE_WITH_ARRANGEMENT)
    publish_instrument_transport();
    span = dispatch_midi_span(render_frame, state.render_frame, span);
#endif
    // Advance engine-level smoothing ramps by this span and push the
    // interpolated values to their bound parameters at the same cadence.
    tick_smoothed_params(span);
    const bool rolling = transport_.playing();
    const int64_t position = transport_.sample_position();
#if defined(SONARE_WITH_ARRANGEMENT)
    // Clock bytes follow this span's dispatches, so the external queue stays in time order.
    if (rolling) emit_midi_clock_block(position, render_frame, span);
#endif
    process_subblock(io, monitor_out, num_channels, offset, span, fold_monitor_to_main);
    transport_.advance(span);
    // advance() folds a playhead reaching loop_end, which is the wrap.
    wrapped =
        rolling && transport_.sample_position() != numeric::saturating_add(position, int64_t{span});
    offset += span;
  }
#if defined(SONARE_WITH_ARRANGEMENT)
  if (wrapped) {
    publish_instrument_transport();
    midi_sequencer_.all_notes_off(midi::DeviceFrame{transport_.render_frame()});
  }
#else
  (void)wrapped;
#endif
  clip_player_.end_page_miss_block();
#if defined(SONARE_WITH_MIXING)
  meter_tap_.end_block();
  master_insert_gr_board_.end_block();
  track_mixer_runtime_.end_insert_gain_reduction_block();
  scope_tap_.end_block();
#endif

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
  // Base-value table record drops: surface the per-block delta the same
  // way as the other audio-thread counters above. Unconditional, like the
  // table itself.
  if (parameter_base_overflow_count_ != parameter_base_overflow_reported_) {
    const uint32_t delta = parameter_base_overflow_count_ - parameter_base_overflow_reported_;
    parameter_base_overflow_reported_ = parameter_base_overflow_count_;
    enqueue_error(TelemetryErrorCode::kParameterBaseOverflow, state.render_frame,
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
  const int latency_q8 = graph_latency_samples_q8_.load(std::memory_order_relaxed);
  enqueue_telemetry({TelemetryType::kProcessBlock, TelemetryErrorCode::kNone, state.render_frame,
                     end_state.sample_position,
                     audible_timeline_sample(end_state.sample_position, latency_q8), latency_q8,
                     static_cast<uint32_t>(frames)});
}

#if defined(SONARE_WITH_ARRANGEMENT)
void RealtimeEngine::publish_instrument_transport() noexcept {
  if (instrument_rack_.empty()) return;
  const transport::TransportState state = transport_.snapshot();
  instrument_rack_.for_each([&](uint32_t, midi::MidiInstrument* instrument) noexcept {
    instrument->set_transport(state);
  });
}
#endif

void RealtimeEngine::process_subblock(float* const* io, float* const* monitor_out, int num_channels,
                                      int offset, int num_frames,
                                      bool fold_monitor_to_main) noexcept {
#if !defined(SONARE_WITH_MIXING)
  (void)fold_monitor_to_main;
#endif
  std::array<float*, kMaxAudioChannels> sub_channels{};
  int channels = 0;
  const bool capture_input = capture_source() == CaptureSource::kInput;
  const int scratch_channels = std::min(
      {std::max(num_channels, 0), prepared_channels_, static_cast<int>(sub_channels.size())});
#if defined(SONARE_WITH_MIXING)
  std::array<float, mixing::kMaxMeterChannels> master_input_peak_db =
      mixing::detail::meter_floor_array();
  float master_gain_reduction_db = 0.0f;
  bool master_gr_published = false;
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
    const InputMonitorState monitor = input_monitor_reader_.try_load();
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
#if defined(SONARE_WITH_MIXING)
    // Lane-owned PFL/AFL taps and the legacy raw-strip monitor path share the
    // one prepared monitor bus. Clear it before source/lane rendering so the
    // TrackMixer can accumulate PFL (post-strip) and AFL (post lane fader/gate/
    // pan/scatter) samples without a second per-block buffer or allocation.
    const bool lane_monitor_active = track_mixer_runtime_.monitor_active();
    const bool legacy_monitor_active = monitoring_enabled_.load(std::memory_order_relaxed);
    const bool monitor_active = lane_monitor_active || legacy_monitor_active;
    if (monitor_active) {
      for (int ch = 0; ch < channels; ++ch) {
        std::fill(monitor_bus_channels_[static_cast<size_t>(ch)],
                  monitor_bus_channels_[static_cast<size_t>(ch)] + num_frames, 0.0f);
      }
    }
    track_mixer_runtime_.set_monitor_bus(
        lane_monitor_active ? monitor_bus_channels_.data() : nullptr, channels);
#endif
    // Clip audio and sequenced MIDI are both gated on the transport rolling.
    // While stopped, advance() freezes sample_position, so rendering clips
    // would replay the same clip window every block as a sustained buzz.
    const bool transport_rolling = transport_.playing();
#if defined(SONARE_WITH_ARRANGEMENT)
#if defined(SONARE_WITH_MIXING)
    // Block-level bus aggregation. Clip audio and hosted-instrument audio are
    // two contributors to the same buses, so they share ONE begin/finish pair:
    // otherwise each bus insert chain runs twice per block, once over the clip
    // contribution and once over the instrument contribution, and a non-linear
    // insert (compressor, saturation) acts on two partial signals instead of the
    // summed bus. Every strip/lane smoother would also advance twice.
    //
    // The decision has to be made BEFORE the clip pass, and whether the rack
    // actually renders is only known after this block's MIDI dispatch (a live
    // note-on arriving on a stopped transport starts an instrument mid-block).
    // A configured mixer opens every block, even stopped or with an empty rack, so tails advance.
    const bool block_open = track_mixer_runtime_.begin_block(channels, num_frames);
#endif
    if (pdc_total_q8_ > 0) {
      // PDC: direct clips are delayed by total instrument latency; lane clips get a per-lane delay.
      for (int ch = 0; ch < channels; ++ch) {
        if (clip_scratch_channels_[static_cast<size_t>(ch)]) {
          std::fill(clip_scratch_channels_[static_cast<size_t>(ch)],
                    clip_scratch_channels_[static_cast<size_t>(ch)] + num_frames, 0.0f);
        }
      }
#if defined(SONARE_WITH_MIXING)
      if (block_open) {
        if (transport_rolling) {
          track_mixer_runtime_.render_clips_into_lanes(
              clip_player_, clip_scratch_channels_.data(), channels, num_frames,
              transport_.sample_position(), clip_scratch_channels_.data());
        } else {
          // A stopped transport does not scan clips, but every opened lane
          // still advances its raw clip delay with zeros so stale clip audio
          // cannot reappear after a stop/resume transition.
          track_mixer_runtime_.drain_clip_pdc_delays(channels, num_frames);
        }
      } else if (transport_rolling && !track_mixer_runtime_.render_clips(
                                          clip_player_, clip_scratch_channels_.data(), channels,
                                          num_frames, transport_.sample_position(), &meter_tap_,
                                          transport_.render_frame(), &scope_tap_)) {
        clip_player_.process_at(clip_scratch_channels_.data(), channels, num_frames,
                                transport_.sample_position());
      }
#else
      if (transport_rolling) {
        clip_player_.process_at(clip_scratch_channels_.data(), channels, num_frames,
                                transport_.sample_position());
      }
#endif
      clip_pdc_delay_.process(clip_scratch_channels_.data(), channels, num_frames);
#if defined(SONARE_WITH_MIXING)
      if (block_open) {
        // Stage the PDC-aligned unmatched clips into the mixer's direct bank.
        bool routed_through_lane = false;
        track_mixer_runtime_.mix_source_into_lane(
            0, clip_scratch_channels_.data(), sub_channels.data(), channels, num_frames,
            routed_through_lane, &meter_tap_, transport_.render_frame(), &scope_tap_);
      } else
#endif
      {
        for (int ch = 0; ch < channels; ++ch) {
          float* out = sub_channels[static_cast<size_t>(ch)];
          const float* clip = clip_scratch_channels_[static_cast<size_t>(ch)];
          if (!out) continue;
          for (int i = 0; i < num_frames; ++i) out[i] += clip[i];
        }
      }
    } else if (transport_rolling) {
#if defined(SONARE_WITH_MIXING)
      if (block_open) {
        // Aggregated block: accumulate the clip audio into the lanes now; the
        // strips, sends and bus chains run once in finish_block() after the
        // instrument rack has accumulated too.
        track_mixer_runtime_.render_clips_into_lanes(clip_player_, sub_channels.data(), channels,
                                                     num_frames, transport_.sample_position());
      } else if (!track_mixer_runtime_.render_clips(clip_player_, sub_channels.data(), channels,
                                                    num_frames, transport_.sample_position(),
                                                    &meter_tap_, transport_.render_frame(),
                                                    &scope_tap_)) {
        clip_player_.process_at(sub_channels.data(), channels, num_frames,
                                transport_.sample_position());
      }
#else
      clip_player_.process_at(sub_channels.data(), channels, num_frames,
                              transport_.sample_position());
#endif
#if defined(SONARE_WITH_MIXING)
    } else if (block_open) {
      // Keep configured lane strips and their tails advancing over a stopped
      // block. The lane buffers are silent because begin_block() cleared them;
      // drain_clip_pdc_delays() also marks each lane for the single finish pass.
      track_mixer_runtime_.drain_clip_pdc_delays(channels, num_frames);
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
    // Host-instrument audio injection: sum the instrument's render into the
    // SAME source layer as the clip player, AFTER clip playback + MIDI dispatch
    // and BEFORE the metronome / mixing-strip / monitor / graph stages. This is
    // the PINNED clip/source-merge injection point: instrument output therefore
    // flows through channel strips + monitoring + the graph exactly like clip
    // audio, and PDC/latency matches clips. Opt-in: nullptr leaves the chain and
    // the output bit-identical to the no-instrument path. RT-safe: the scratch
    // is sized in prepare(); the audio thread only zero-fills and sums it.
    // Instruments render whenever bound, rolling or not: a release, a held pedal or
    // a reverb tail outlives the last note-off and the transport alike.
    if (!instrument_rack_.empty()) {
      // A tempo-synced delay / arpeggiator / LFO follows the transport snapshot
      // published above instead of free-running. Each instrument renders into the
      // shared scratch (zero, process) and is summed into the sub-block, so
      // multitrack MIDI routed to distinct destinations mixes here.
#if defined(SONARE_WITH_MIXING)
      // The lane accumulators and buses were cleared once for the whole block
      // (begin_block above, shared with the clip pass when it ran). Each
      // instrument accumulates into its lane/sends via mix_source_into_lane (no
      // bus processing); finish_block runs every strip and bus chain once
      // afterwards. With PDC, raw clips are delayed within those same lane
      // accumulators before instruments join them.
      const bool lane_mix_ready =
          block_open || track_mixer_runtime_.begin_source_mix(channels, num_frames);
      bool any_lane_routed = false;
      std::array<uint32_t, TrackMixerRuntime::kMaxTrackLanes> source_track_ids{};
      const size_t source_track_count = lane_mix_ready
                                            ? track_mixer_runtime_.copy_lane_track_ids(
                                                  source_track_ids.data(), source_track_ids.size())
                                            : 0;
#endif
      instrument_rack_.for_each([&](uint32_t destination_id,
                                    midi::MidiInstrument* instrument) noexcept {
        // MIDI clip gain/fade, resolved once and applied to every buffer this
        // destination writes in either branch below.
        const std::vector<midi::MidiClipSchedule>* clip_schedule = midi_sequencer_.current_clips();
        const bool has_clip_envelope = clip_schedule != nullptr && !clip_schedule->empty();
        midi::MidiClipEnvelopeBlock envelope_block;
        if (has_clip_envelope) {
          midi::resolve_midi_clip_envelope(*clip_schedule, destination_id,
                                           transport_.sample_position(), num_frames,
                                           &envelope_block);
        }
#if defined(SONARE_WITH_MIXING)
        // Source-aware render is valid only before PDC: a destination has
        // one legacy delay line, while each source needs independent delay
        // history. Latency-bearing instruments keep the established
        // destination path until the source-PDC bank is configured.
        if (lane_mix_ready && pdc_total_q8_ == 0) {
          std::array<midi::MidiInstrumentSourceOutput, kMaxInstrumentSourceOutputs> outputs{};
          for (size_t source = 0; source <= source_track_count; ++source) {
            const uint32_t track_id = source == 0 ? 0 : source_track_ids[source - 1];
            outputs[source] = {track_id, midi_instrument_source_channels_[source].data()};
            for (int ch = 0; ch < channels; ++ch) {
              std::fill(
                  midi_instrument_source_channels_[source][static_cast<size_t>(ch)],
                  midi_instrument_source_channels_[source][static_cast<size_t>(ch)] + num_frames,
                  0.0f);
            }
          }
          if (instrument->process_source_tracks(outputs.data(), source_track_count + 1, channels,
                                                num_frames)) {
            if (has_clip_envelope) {
              for (size_t source = 0; source <= source_track_count; ++source) {
                midi::apply_midi_clip_envelope(envelope_block, *clip_schedule, destination_id,
                                               transport_.sample_position(),
                                               outputs[source].channels, channels, num_frames);
              }
            }
            if (instrument_source_render_sink_ != nullptr) {
              for (size_t source = 0; source <= source_track_count; ++source) {
                instrument_source_render_sink_->on_instrument_source_audio(
                    destination_id, outputs[source].source_track_id, outputs[source].channels,
                    channels, num_frames, transport_.sample_position());
              }
              return;
            }
            for (size_t source = 0; source <= source_track_count; ++source) {
              bool routed_through_lane = false;
              if (track_mixer_runtime_.mix_source_into_lane(
                      outputs[source].source_track_id, outputs[source].channels,
                      sub_channels.data(), channels, num_frames, routed_through_lane, &meter_tap_,
                      transport_.render_frame(), &scope_tap_)) {
                any_lane_routed = any_lane_routed || routed_through_lane;
                continue;
              }
              for (int ch = 0; ch < channels; ++ch) {
                float* out = sub_channels[static_cast<size_t>(ch)];
                const float* source_audio = outputs[source].channels[static_cast<size_t>(ch)];
                if (!out || !source_audio) continue;
                for (int i = 0; i < num_frames; ++i) out[i] += source_audio[i];
              }
            }
            return;
          }
        }
#endif
        for (int ch = 0; ch < channels; ++ch) {
          std::fill(midi_instrument_channels_[static_cast<size_t>(ch)],
                    midi_instrument_channels_[static_cast<size_t>(ch)] + num_frames, 0.0f);
        }
        instrument->process(midi_instrument_channels_.data(), channels, num_frames);
        if (has_clip_envelope) {
          midi::apply_midi_clip_envelope(envelope_block, *clip_schedule, destination_id,
                                         transport_.sample_position(),
                                         midi_instrument_channels_.data(), channels, num_frames);
        }
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
                  destination_id, midi_instrument_channels_.data(), sub_channels.data(), channels,
                  num_frames, routed_through_lane, &meter_tap_, transport_.render_frame(),
                  &scope_tap_)) {
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
      if (!block_open && lane_mix_ready && any_lane_routed) {
        // No block pass opened, so the rack-only source mix is closed here.
        track_mixer_runtime_.finish_source_mix(sub_channels.data(), channels, num_frames,
                                               &meter_tap_, transport_.render_frame(), &scope_tap_);
      }
#endif
    }
#if defined(SONARE_WITH_MIXING)
    if (block_open) {
      // Aggregated block: run every lane that received audio through its strip /
      // sends / fader, and every bus chain, exactly once over the summed clip +
      // instrument contributions. Runs outside the rack branch so a block whose
      // rack ended up silent still closes the pass the clips opened.
      track_mixer_runtime_.finish_block(sub_channels.data(), channels, num_frames,
                                        transport_.sample_position(), &meter_tap_,
                                        transport_.render_frame(), &scope_tap_);
    }
#endif
#endif
#if defined(SONARE_WITH_MIXING)
    // The input meter remains available even when no master strip is bound.
    capture_input_peak_db(sub_channels.data(), channels, num_frames, master_input_peak_db);
    // Mixing channel-strip insert stage (fader/pan/width/EQ/inserts) runs
    // sample-accurately at the sub-block's timeline position when enabled.
    if (mixing_enabled_.load(std::memory_order_relaxed)) {
      // Master insert keys the track mixer aligned to the master input this block.
      if (owned_master_strip_ != nullptr && mixing_runtime_.strip() == owned_master_strip_.get()) {
        track_mixer_runtime_.deliver_master_sidechains(owned_master_strip_.get(), num_frames);
      }
      mixing_runtime_.process_at(sub_channels.data(), channels, num_frames,
                                 transport_.sample_position());
      if (const auto* strip = mixing_runtime_.strip()) {
        master_gain_reduction_db = strip->last_gain_reduction_db();
        master_insert_gr_board_.publish(*strip);
        master_gr_published = true;
      }
    }
    if (!master_gr_published) master_insert_gr_board_.clear();
    // Legacy raw-strip solo/mute + PFL/AFL remains an independent producer of
    // the same monitor bus. Lane-owned monitor modes were accumulated above;
    // neither path is registered in the other, so each source is summed once.
    if (legacy_monitor_active) {
      const size_t strip_count = monitor_runtime_.size();
      for (size_t s = 0; s < strip_count; ++s) {
        monitor_runtime_.process_strip(s, sub_channels.data(), channels, num_frames,
                                       transport_.sample_position(), monitor_bus_channels_.data());
      }
    }
    // process() folds the complete cue bus into the main output for historical
    // compatibility. process_with_monitor() copies it only to monitor_out, so
    // the program output stays unchanged while the host receives a separate cue
    // signal. Lane mode itself enables this stage even when the legacy
    // monitoring flag is off.
    if (monitor_active) {
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
    meter_tap_.process(sub_channels.data(), channels, num_frames, transport_.render_frame(),
                       master_input_peak_db.data(), master_gain_reduction_db);
#endif
    const float* const* capture_channels =
        capture_input ? reinterpret_cast<const float* const*>(input_capture_channels_.data())
                      : reinterpret_cast<const float* const*>(sub_channels.data());
    if (!capture_sink_.punch_state_rt().punch_enabled || transport_.playing()) {
      const int64_t record_offset = record_offset_samples_.load(std::memory_order_acquire);
      capture_sink_.process(capture_channels, channels, num_frames,
                            numeric::saturating_sub(transport_.sample_position(), record_offset));
    }
#if defined(SONARE_WITH_MIXING)
    scope_tap_.append_master_pre_metronome(sub_channels.data(), channels, num_frames,
                                           transport_.render_frame());
#endif
    // The metronome is a monitor/cue signal: render it after master metering,
    // scope capture, graph processing, and output capture so it remains audible
    // to the player without being committed to recorded program audio.
    if (transport_.playing() && !metronome_.process(sub_channels.data(), channels, num_frames,
                                                    transport_.sample_position())) {
      enqueue_error(TelemetryErrorCode::kMetronomeOverflow, transport_.render_frame(),
                    transport_.sample_position(), 1);
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
