#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "engine/insert_automation_id.h"
#include "engine/realtime_engine.h"
#include "engine/realtime_engine_internal.h"

namespace sonare::engine {

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

bool RealtimeEngine::parameter_target_reserved(uint32_t target_id) noexcept {
  return (target_id & kEngineParamNamespaceMask) == kEngineParamNamespace ||
         is_insert_param_id(target_id);
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
      if (parameter_target_reserved(command.target_id)) {
        if (!route_engine_parameter(command.target_id, command.arg.f)) {
          enqueue_error(TelemetryErrorCode::kUnknownTarget, transport_.render_frame(),
                        transport_.sample_position(), command.target_id);
        }
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
      // Route through the smoothed insert-param slot table so a manual set snaps
      // on first touch and glides on repeats -- identical to an automated
      // breakpoint and to the bus path -- instead of zipping. Control-thread
      // resolution already rejected unknown targets, so a false return is a
      // slot-table overflow, surfaced via kInsertAutomationOverflow telemetry.
      const uint32_t packed = command.target_id;
      const size_t lane_index = (packed >> 16) & 0xFFu;
      const unsigned int insert_index = (packed >> 8) & 0xFFu;
      const unsigned int param_id = packed & 0xFFu;
      track_mixer_runtime_.route_lane_insert_param_smoothed(lane_index, insert_index, param_id,
                                                            command.arg.f);
#else
      enqueue_error(TelemetryErrorCode::kUnknownTarget, transport_.render_frame(),
                    transport_.sample_position(), command.target_id);
#endif
      break;
    }
    case rt::CommandType::kSetMasterInsertParam: {
#if defined(SONARE_WITH_MIXING)
      // target_id = (insert_index << 8) | param_id (no lane field for master).
      // Smoothed like the track and bus paths (snap-then-glide) so a manual
      // master insert tweak does not click. Overflow is surfaced via telemetry.
      const uint32_t packed = command.target_id;
      const unsigned int insert_index = (packed >> 8) & 0xFFu;
      const unsigned int param_id = packed & 0xFFu;
      route_master_insert_param_smoothed(insert_index, param_id, command.arg.f);
#else
      enqueue_error(TelemetryErrorCode::kUnknownTarget, transport_.render_frame(),
                    transport_.sample_position(), command.target_id);
#endif
      break;
    }
    case rt::CommandType::kMidiSysExImmediate: {
#if defined(SONARE_WITH_ARRANGEMENT)
      // Resolve the scalar slot reference (arg.i = (generation << 32) | index)
      // filled by push_midi_sysex. The command queue's acquire on pop already
      // published the slot bytes; the generation guard drops a slot the control
      // thread has since recycled (burst deeper than the ring), so a torn
      // payload never reaches the instrument. The slot bytes stay valid for the
      // synchronous dispatch below (the instrument consumes them inline).
      const uint64_t packed = static_cast<uint64_t>(command.arg.i);
      const uint32_t slot_index = static_cast<uint32_t>(packed & 0xFFFFFFFFu);
      const uint32_t generation = static_cast<uint32_t>(packed >> 32);
      if (slot_index < sysex_payload_slots_.size()) {
        SysExPayloadSlot& slot = sysex_payload_slots_[slot_index];
        if (slot.generation.load(std::memory_order_relaxed) == generation && slot.size > 0) {
          // The UMP carries only a SysEx marker (non-channel-voice message type);
          // the resolved payload view drives the instrument's SysEx handler, the
          // same shape the offline clip path dispatches.
          const midi::Ump ump = midi::make_sysex_handle(0, /*handle=*/0);
          midi_sequencer_.inject_event(command.target_id, command.sample_time, ump,
                                       slot.bytes.data(), slot.size);
        }
      }
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
  settle_master_insert_automations();
#endif
  // Quiesce the engine-side monitor (solo/mute) smoothers and their strips so a
  // bounce that starts muted/soloed opens at the steady-state gain.
  monitor_runtime_.settle();
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
#if defined(SONARE_WITH_MIXING)
  // Advance the master-strip insert-automation smoothers at the same sub-block
  // cadence and push them to the master inserts (lane/bus insert smoothers are
  // advanced inside the track mixer's own render path).
  advance_master_insert_automations(num_steps);
#endif
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
