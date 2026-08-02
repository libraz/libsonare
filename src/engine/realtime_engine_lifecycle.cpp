#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "engine/realtime_engine.h"
#include "engine/realtime_engine_internal.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare::engine {

void RealtimeEngine::prepare(double sample_rate, int max_block_size, size_t command_capacity,
                             size_t telemetry_capacity, int max_channels) {
  max_block_size_ = std::max(max_block_size, 1);
  prepared_channels_ = std::clamp(max_channels, 1, static_cast<int>(kMaxAudioChannels));
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
  size_t instrument_source_outputs = 1;
#if defined(SONARE_WITH_MIXING)
  instrument_source_outputs = kMaxInstrumentSourceOutputs;
#endif
  midi_instrument_storage_.assign(static_cast<size_t>(max_block_size_) *
                                      static_cast<size_t>(prepared_channels_) *
                                      instrument_source_outputs,
                                  0.0f);
  for (int ch = 0; ch < prepared_channels_; ++ch) {
    midi_instrument_channels_[ch] =
        midi_instrument_storage_.data() + ch * static_cast<size_t>(max_block_size_);
  }
#if defined(SONARE_WITH_MIXING)
  const size_t source_stride =
      static_cast<size_t>(prepared_channels_) * static_cast<size_t>(max_block_size_);
  for (size_t source = 0; source < kMaxInstrumentSourceOutputs; ++source) {
    for (int ch = 0; ch < prepared_channels_; ++ch) {
      midi_instrument_source_channels_[source][ch] = midi_instrument_storage_.data() +
                                                     source * source_stride +
                                                     ch * static_cast<size_t>(max_block_size_);
    }
  }
#endif
  // PDC clip-bus scratch: the clip player renders here first when an instrument
  // reports latency, so the clip bus can be delayed (phase-aligned with the
  // instruments) before being summed into the source layer.
  clip_scratch_storage_.assign(
      static_cast<size_t>(max_block_size_) * static_cast<size_t>(prepared_channels_), 0.0f);
  for (int ch = 0; ch < prepared_channels_; ++ch) {
    clip_scratch_channels_[ch] =
        clip_scratch_storage_.data() + ch * static_cast<size_t>(max_block_size_);
  }
  // The dispatch tee is the sequencer's permanent sink: it demuxes events to
  // the instrument rack and optionally mirrors them to a live MIDI output seam.
  // Re-prepare every already-registered instrument to the new block size.
  midi_dispatch_sink_.rack = &instrument_rack_;
  midi_dispatch_sink_.external = &external_midi_queue_;
  external_clock_sync_sink_.queue = &external_midi_queue_;
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
#if defined(SONARE_WITH_GRAPH)
  automation_.set_external_target_resolver(&RealtimeEngine::resolve_graph_parameter_thunk, this);
#endif
#if defined(SONARE_WITH_MIXING)
  // Route reserved engine-namespace automation lanes (mixer fader/pan) straight
  // to the mixer runtimes instead of the bound-processor table.
  automation_.set_engine_param_router(&RealtimeEngine::route_engine_parameter_thunk, this,
                                      kEngineParamNamespaceMask, kEngineParamNamespace);
  // The router now claims two disjoint id namespaces (mixer fader/pan plus
  // insert automation), which a single mask/match cannot express, so gate it on
  // parameter_target_reserved (static and pure: safe as an audio-thread fn ptr).
  automation_.set_engine_param_gate(&RealtimeEngine::parameter_target_reserved);
#endif
  input_capture_storage_.assign(
      static_cast<size_t>(max_block_size_) * static_cast<size_t>(prepared_channels_), 0.0f);
  for (int ch = 0; ch < prepared_channels_; ++ch) {
    input_capture_channels_[ch] =
        input_capture_storage_.data() + ch * static_cast<size_t>(max_block_size_);
  }
#if defined(SONARE_WITH_MIXING)
  mixing_runtime_.prepare(sample_rate_, max_block_size_);
  monitor_runtime_.prepare(sample_rate_, max_block_size_);
  track_mixer_runtime_.prepare(sample_rate_, max_block_size_);
  update_reported_graph_latency();
  monitor_bus_storage_.assign(
      static_cast<size_t>(max_block_size_) * static_cast<size_t>(prepared_channels_), 0.0f);
  for (int ch = 0; ch < prepared_channels_; ++ch) {
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
  clip_page_request_overflow_count_.store(0, std::memory_order_relaxed);
  pending_active_.fill(false);
  // Pre-size the engine-level smoothers so kSetParamSmoothed never allocates on
  // the audio thread; mark all slots inactive.
  applied_param_smoothing_ms_ = param_smoothing_ms_.load(std::memory_order_relaxed);
  for (SmoothedParam& slot : smoothed_params_) {
    slot.active = false;
    slot.assigned = false;
    slot.target_id = 0;
    slot.smoother.prepare(sample_rate_, applied_param_smoothing_ms_);
    slot.smoother.reset(0.0f);
  }
#if defined(SONARE_WITH_MIXING)
  for (MasterInsertAutoSlot& slot : master_insert_auto_slots_) {
    slot.active = false;
    slot.assigned = false;
    slot.insert_index = 0;
    slot.param_id = 0;
    slot.smoother.prepare(sample_rate_, 5.0f);
    slot.smoother.reset(0.0f);
  }
  master_insert_automation_overflow_count_ = 0;
#endif
  insert_automation_overflow_reported_ = 0;
  telemetry_overflow_count_ = 0;
  automation_bind_overflow_reported_ = automation_.bind_target_overflow_count();
  automation_stale_lane_reported_ = automation_.stale_lane_apply_count();
}

void RealtimeEngine::publish_tempo_map_snapshot() {
  auto map = std::make_shared<transport::TempoMap>();
  map->prepare(sample_rate_);
  if (!control_tempo_segments_.empty()) {
    map->set_segments(control_tempo_segments_);
  } else {
    // Empty map: fall back to the last single tempo (default 120 BPM), not a
    // hardcoded default that would discard an earlier set_tempo().
    map->set_segments({{0.0, control_single_tempo_bpm_, 0.0}});
  }
  if (!control_time_signatures_.empty()) {
    map->set_time_signatures(control_time_signatures_);
  } else {
    map->set_time_signatures({{0.0, control_single_time_sig_}});
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
  const MetronomeConfig metronome_config = metronome_.config();
  if (metronome_config.enabled) {
    MetronomeConfig disabled = metronome_config;
    disabled.enabled = false;
    metronome_.set_config(disabled);
  }
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
  if (metronome_config.enabled) {
    metronome_.set_config(metronome_config);
  }
#if defined(SONARE_WITH_ARRANGEMENT)
  midi_sequencer_.all_notes_off(transport_.render_frame());
  flush_pdc_delays();
#endif
#if defined(SONARE_WITH_MIXING)
  track_mixer_runtime_.flush_pdc_delays();
#endif
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
  SONARE_CHECK_MSG(transport::valid_public_tempo(bpm), ErrorCode::InvalidParameter,
                   "tempo must be finite, positive, and at most 100000 BPM");
  control_single_tempo_bpm_ = bpm;
  control_tempo_segments_ = {{0.0, bpm, 0.0}};
  publish_tempo_map_snapshot();
}

void RealtimeEngine::set_tempo_segments(std::vector<transport::TempoSegment> segments) {
  for (const transport::TempoSegment& segment : segments) {
    SONARE_CHECK_MSG(transport::valid_public_tempo_segment(segment), ErrorCode::InvalidParameter,
                     "tempo segments contain invalid timeline or out-of-range BPM values");
  }
  control_tempo_segments_ = std::move(segments);
  publish_tempo_map_snapshot();
}

void RealtimeEngine::set_time_signature(int numerator, int denominator) {
  SONARE_CHECK_MSG(numerator > 0 && denominator > 0, ErrorCode::InvalidParameter,
                   "time signature values must be positive");
  control_single_time_sig_ = {numerator, denominator};
  control_time_signatures_ = {{0.0, {numerator, denominator}}};
  publish_tempo_map_snapshot();
}

void RealtimeEngine::set_time_signature_segments(
    std::vector<transport::TimeSignatureSegment> segments) {
  for (const transport::TimeSignatureSegment& segment : segments) {
    SONARE_CHECK_MSG(transport::valid_public_time_signature_segment(segment),
                     ErrorCode::InvalidParameter,
                     "time signature segments contain invalid timeline or signature values");
  }
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
#if defined(SONARE_WITH_GRAPH)
  latency_q8 += graph_runtime_.latency_samples_q8();
#endif
#if defined(SONARE_WITH_ARRANGEMENT)
  latency_q8 += pdc_total_q8_;
#endif
#if defined(SONARE_WITH_MIXING)
  latency_q8 += track_mixer_runtime_.latency_samples_q8();
  if (mixing_enabled_.load(std::memory_order_relaxed)) {
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

void RealtimeEngine::set_capture_segment(CaptureSegment segment) noexcept {
  capture_sink_.prepare(segment);
}

void RealtimeEngine::set_capture_armed(bool armed) noexcept { capture_sink_.arm(armed); }

void RealtimeEngine::set_capture_punch(int64_t start_sample, int64_t end_sample,
                                       bool enabled) noexcept {
  capture_sink_.set_punch(start_sample, end_sample, enabled);
}

void RealtimeEngine::reset_capture() noexcept { capture_sink_.reset(); }

}  // namespace sonare::engine
