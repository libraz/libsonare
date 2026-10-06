#include <algorithm>
#include <cmath>

#include "engine/meter_telemetry.h"
#include "engine/scope_telemetry.h"
#include "engine/track_mixer.h"
#include "engine/track_mixer_internal.h"
#include "mixing/downmix.h"

namespace sonare::engine {

bool TrackMixerRuntime::render_clips(ClipPlayer& player, float* const* channels, int num_channels,
                                     int num_samples, int64_t timeline_sample,
                                     MeterTelemetryTap* meter_tap, int64_t render_frame,
                                     ScopeTelemetryTap* scope_tap) noexcept {
  // Self-contained clip-only block: exactly begin / render-into-lanes / finish
  // for a block whose only contributor is the clip player. Retained for callers
  // that have no instrument pass to fold in. RealtimeEngine combines clip and
  // instrument contributors in one opened block, including when PDC is active.
  acquire_lanes();
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (!lanes || lanes->empty()) return false;
  // Degenerate arguments are "handled" (nothing to render), not a fallback.
  if (!channels || num_channels <= 0 || num_samples <= 0) return true;
  if (!begin_block(num_channels, num_samples)) return false;
  if (!render_clips_into_lanes(player, channels, num_channels, num_samples, timeline_sample)) {
    return false;
  }
  finish_block(channels, num_channels, num_samples, timeline_sample, meter_tap, render_frame,
               scope_tap);
  return true;
}

bool TrackMixerRuntime::begin_block(int num_channels, int num_samples) noexcept {
  acquire_lanes();
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (!lanes || lanes->empty()) return false;
  if (num_channels <= 0 || num_samples <= 0) return false;
  if (num_channels > kMaxBusChannels || num_samples > max_block_size_ || scratch_.empty()) {
    return false;
  }
  if (lanes != applied_lane_snapshot_) prepare_lanes_from_snapshot(*lanes);
  // Advance the insert-parameter smoothers once for this block before any
  // lane/bus chain runs, so an automated insert param reaches its processor at
  // the same cadence as the lane fader smoother (no double advance: every lane
  // and bus chain of this block runs inside the matching finish_block()).
  advance_insert_automations(num_samples);
  const int render_channels = std::min(num_channels, kMaxLaneChannels);
  for (size_t lane_index = 0; lane_index < lanes->size(); ++lane_index) {
    clear_lane(lane_index, render_channels, num_samples);
    // An opened block processes every configured lane, including silent ones,
    // so stateful strip/bus tails advance over zero input.
    source_mix_lane_active_[lane_index] = true;
  }
  for (int ch = 0; ch < render_channels; ++ch) {
    float* direct = direct_channel(ch);
    std::fill(direct, direct + num_samples, 0.0f);
  }
  const int master_channels = std::min(num_channels, kMaxBusChannels);
  for (size_t bus_index = 0; bus_index < bus_configs_.size(); ++bus_index) {
    clear_bus(bus_index, bus_render_channels(bus_index, master_channels), num_samples);
  }
  return true;
}

bool TrackMixerRuntime::render_clips_into_lanes(ClipPlayer& player, float* const* channels,
                                                int num_channels, int num_samples,
                                                int64_t timeline_sample,
                                                float* const* direct_output) noexcept {
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (!lanes || lanes->empty()) return false;
  if (!channels || num_channels <= 0 || num_samples <= 0) return true;
  if (num_channels > kMaxBusChannels || num_samples > max_block_size_ || scratch_.empty()) {
    return false;
  }
  const int render_channels = std::min(num_channels, kMaxLaneChannels);
  for (size_t lane_index = 0; lane_index < lanes->size(); ++lane_index) {
    active_track_ids_[lane_index] = (*lanes)[lane_index].track_id;
    for (int ch = 0; ch < render_channels; ++ch) {
      lane_channel_ptrs_[static_cast<size_t>(ch)] = lane_channel(lane_index, ch);
    }
    player.process_track_at((*lanes)[lane_index].track_id, lane_channel_ptrs_.data(),
                            render_channels, num_samples, timeline_sample);
    // Clip compensation is source-local: delay this lane's raw clip before it
    // is combined with hosted-instrument audio, and keep a bank per track so
    // two lanes rendered back-to-back cannot share delay history.
    lane_states_[lane_index].clip_pdc_delay.process(lane_channel_ptrs_.data(), render_channels,
                                                    num_samples);
    // The clip pass touches every lane, including the ones the block leaves
    // silent: a lane strip's inserts are stateful, so skipping a silent lane
    // would freeze a reverb tail or a compressor release mid-decay.
    source_mix_lane_active_[lane_index] = true;
  }
  // Lane-less clips are staged in the direct bank; finish_block() adds them before lane output.
  for (int ch = 0; ch < render_channels; ++ch) {
    lane_channel_ptrs_[static_cast<size_t>(ch)] =
        direct_output != nullptr ? direct_output[static_cast<size_t>(ch)] : direct_channel(ch);
  }
  player.process_excluding_tracks_at(active_track_ids_.data(), lanes->size(),
                                     lane_channel_ptrs_.data(), render_channels, num_samples,
                                     timeline_sample);
  return true;
}

void TrackMixerRuntime::finish_block(float* const* channels, int num_channels, int num_samples,
                                     int64_t timeline_sample, MeterTelemetryTap* meter_tap,
                                     int64_t render_frame, ScopeTelemetryTap* scope_tap) noexcept {
  if (!channels || num_channels <= 0 || num_samples <= 0) return;
  if (num_channels > kMaxBusChannels || num_samples > max_block_size_ || scratch_.empty()) return;
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (!lanes) return;
  const int render_channels = std::min(num_channels, kMaxLaneChannels);
  const int master_channels = std::min(num_channels, kMaxBusChannels);
  add_direct_to_mix(channels, master_channels, num_samples);
  const bool any_solo = any_lane_solo(*lanes);
  // Two passes (all strips + sends, then all lane outputs), not one interleaved
  // pass: this is the accumulation order the clip path has always used, so a
  // clip-only block stays bit-identical. Strips run in lane-key order, so every
  // source lane's key is ready before its destination's strip.
  for (size_t position = 0; position < lanes->size(); ++position) {
    const size_t lane_index = lane_at(position, lanes->size());
    if (!source_mix_lane_active_[lane_index]) continue;
    process_lane_strip(lane_index, render_channels, num_samples, timeline_sample);
    advance_lane_gain(lane_index, num_samples, any_solo);
    mix_lane_sends(lane_index, render_channels, num_samples, timeline_sample);
  }
  for (size_t lane_index = 0; lane_index < lanes->size(); ++lane_index) {
    if (!source_mix_lane_active_[lane_index]) continue;
    apply_lane_to_mix(lane_index, channels, render_channels, num_samples, meter_tap, render_frame,
                      scope_tap, master_channels);
  }
  process_buses(channels, master_channels, num_samples, meter_tap, render_frame, scope_tap);
}

bool TrackMixerRuntime::mix_source(uint32_t track_id, float* const* source, float* const* channels,
                                   int num_channels, int num_samples, MeterTelemetryTap* meter_tap,
                                   int64_t render_frame, ScopeTelemetryTap* scope_tap) noexcept {
  acquire_lanes();
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (!lanes || lanes->empty()) return false;
  if (!source || !channels || num_channels <= 0 || num_samples <= 0) return true;
  if (num_channels > kMaxBusChannels || num_samples > max_block_size_ || scratch_.empty()) {
    return false;
  }

  // Self-contained single-source mix: clear the buses, mix this one source into
  // its lane, then process the buses once -- exactly begin/into-lane/finish for
  // one source.
  if (lanes != applied_lane_snapshot_) prepare_lanes_from_snapshot(*lanes);
  advance_insert_automations(num_samples);
  const int render_channels = std::min(num_channels, kMaxLaneChannels);
  for (size_t lane_index = 0; lane_index < lanes->size(); ++lane_index) {
    clear_lane(lane_index, render_channels, num_samples);
    source_mix_lane_active_[lane_index] = false;
  }
  for (int ch = 0; ch < render_channels; ++ch) {
    float* direct = direct_channel(ch);
    std::fill(direct, direct + num_samples, 0.0f);
  }
  const int master_channels = std::min(num_channels, kMaxBusChannels);
  for (size_t bus_index = 0; bus_index < bus_configs_.size(); ++bus_index) {
    clear_bus(bus_index, bus_render_channels(bus_index, master_channels), num_samples);
  }
  bool routed_through_lane = false;
  mix_source_into_lane(track_id, source, channels, num_channels, num_samples, routed_through_lane,
                       meter_tap, render_frame, scope_tap);
  // Unmatched sources also need finish to advance the direct delay.
  finish_source_mix(channels, num_channels, num_samples, meter_tap, render_frame, scope_tap);
  return true;
}

bool TrackMixerRuntime::begin_source_mix(int num_channels, int num_samples) noexcept {
  return begin_block(num_channels, num_samples);
}

bool TrackMixerRuntime::mix_source_into_lane(uint32_t track_id, float* const* source,
                                             float* const* channels, int num_channels,
                                             int num_samples, bool& routed_through_lane,
                                             MeterTelemetryTap* meter_tap, int64_t render_frame,
                                             ScopeTelemetryTap* scope_tap) noexcept {
  routed_through_lane = false;
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (!lanes || lanes->empty()) return false;
  if (!source || !channels || num_channels <= 0 || num_samples <= 0) return true;
  if (num_channels > kMaxBusChannels || num_samples > max_block_size_ || scratch_.empty()) {
    return false;
  }

  (void)meter_tap;
  (void)render_frame;
  (void)scope_tap;
  const int render_channels = std::min(num_channels, kMaxLaneChannels);
  for (size_t lane_index = 0; lane_index < lanes->size(); ++lane_index) {
    if ((*lanes)[lane_index].track_id != track_id) continue;
    for (int ch = 0; ch < render_channels; ++ch) {
      const float* src = source[static_cast<size_t>(ch)];
      float* lane = lane_channel(lane_index, ch);
      if (src) {
        for (int i = 0; i < num_samples; ++i) {
          lane[i] += src[i];
        }
      }
    }
    source_mix_lane_active_[lane_index] = true;
    routed_through_lane = true;
    return true;
  }

  // Destination 0 and currently-unconfigured destinations stay on the main bus.
  // Stage them so they share the lane-stage timebase before the master delay.
  for (int ch = 0; ch < render_channels; ++ch) {
    lane_channel_ptrs_[static_cast<size_t>(ch)] = direct_channel(ch);
  }
  add_source_to_mix(source, lane_channel_ptrs_.data(), render_channels, num_samples);
  return true;
}

void TrackMixerRuntime::finish_source_mix(float* const* channels, int num_channels, int num_samples,
                                          MeterTelemetryTap* meter_tap, int64_t render_frame,
                                          ScopeTelemetryTap* scope_tap) noexcept {
  if (!channels || num_channels <= 0 || num_samples <= 0) return;
  if (num_channels > kMaxBusChannels || num_samples > max_block_size_ || scratch_.empty()) return;
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (!lanes) return;
  const int render_channels = std::min(num_channels, kMaxLaneChannels);
  const int master_channels = std::min(num_channels, kMaxBusChannels);
  add_direct_to_mix(channels, master_channels, num_samples);
  const bool any_solo = any_lane_solo(*lanes);
  for (size_t position = 0; position < lanes->size(); ++position) {
    const size_t lane_index = lane_at(position, lanes->size());
    if (!source_mix_lane_active_[lane_index]) continue;
    process_lane_strip(lane_index, render_channels, num_samples, 0);
    advance_lane_gain(lane_index, num_samples, any_solo);
    mix_lane_sends(lane_index, render_channels, num_samples, 0);
    apply_lane_to_mix(lane_index, channels, render_channels, num_samples, meter_tap, render_frame,
                      scope_tap, master_channels);
  }
  process_buses(channels, master_channels, num_samples, meter_tap, render_frame, scope_tap);
}

float* TrackMixerRuntime::lane_channel(size_t lane_index, int channel) noexcept {
  const size_t lane_stride = static_cast<size_t>(kMaxLaneChannels) * max_block_size_;
  const size_t offset = lane_index * lane_stride + static_cast<size_t>(channel) * max_block_size_;
  return scratch_.data() + offset;
}

float* TrackMixerRuntime::direct_channel(int channel) noexcept {
  return direct_scratch_.data() +
         static_cast<size_t>(channel) * static_cast<size_t>(max_block_size_);
}

float* TrackMixerRuntime::key_channel(size_t lane_index, int channel) noexcept {
  const size_t lane_stride = static_cast<size_t>(kMaxLaneChannels) * max_block_size_;
  const size_t offset = lane_index * lane_stride + static_cast<size_t>(channel) * max_block_size_;
  return key_scratch_.data() + offset;
}

float* TrackMixerRuntime::lane_gain(size_t lane_index) noexcept {
  return lane_gain_scratch_.data() + lane_index * static_cast<size_t>(max_block_size_);
}

float* TrackMixerRuntime::send_source_channel(int channel) noexcept {
  return send_source_scratch_.data() +
         static_cast<size_t>(channel) * static_cast<size_t>(max_block_size_);
}

float* TrackMixerRuntime::pre_send_source_channel(int channel) noexcept {
  return send_source_scratch_.data() +
         (static_cast<size_t>(kMaxLaneChannels) + static_cast<size_t>(channel)) *
             static_cast<size_t>(max_block_size_);
}

float* TrackMixerRuntime::bus_channel(size_t bus_index, int channel) noexcept {
  const size_t bus_stride = static_cast<size_t>(kMaxBusChannels) * max_block_size_;
  const size_t offset = bus_index * bus_stride + static_cast<size_t>(channel) * max_block_size_;
  return bus_scratch_.data() + offset;
}

int TrackMixerRuntime::bus_render_channels(size_t bus_index, int master_channels) const noexcept {
  const int stereo_width = std::min(master_channels, kMaxLaneChannels);
  if (bus_index >= bus_configs_.size()) return stereo_width;
  const int count = channel_count(bus_configs_[bus_index].layout);
  // Only a surround layout widens a bus; every other bus keeps the historical
  // min(master, 2) width so its summing/processing stays bit-identical.
  return is_surround_channel_count(count) ? std::min(count, kMaxBusChannels) : stereo_width;
}

void TrackMixerRuntime::clear_lane(size_t lane_index, int num_channels, int num_samples) noexcept {
  for (int ch = 0; ch < num_channels; ++ch) {
    float* channel = lane_channel(lane_index, ch);
    std::fill(channel, channel + num_samples, 0.0f);
  }
}

void TrackMixerRuntime::clear_bus(size_t bus_index, int num_channels, int num_samples) noexcept {
  for (int ch = 0; ch < num_channels; ++ch) {
    float* channel = bus_channel(bus_index, ch);
    std::fill(channel, channel + num_samples, 0.0f);
  }
}

void TrackMixerRuntime::add_source_to_mix(float* const* source, float* const* channels,
                                          int num_channels, int num_samples) noexcept {
  for (int ch = 0; ch < num_channels; ++ch) {
    const float* src = source[static_cast<size_t>(ch)];
    float* dst = channels[static_cast<size_t>(ch)];
    if (!src || !dst) continue;
    for (int i = 0; i < num_samples; ++i) {
      dst[i] += src[i];
    }
  }
}

void TrackMixerRuntime::add_direct_to_mix(float* const* channels, int num_channels,
                                          int num_samples) noexcept {
  const int direct_channels = std::min(num_channels, kMaxLaneChannels);
  for (int ch = 0; ch < direct_channels; ++ch) {
    lane_channel_ptrs_[static_cast<size_t>(ch)] = direct_channel(ch);
  }
  direct_pdc_delay_.process(lane_channel_ptrs_.data(), direct_channels, num_samples);
  add_source_to_mix(lane_channel_ptrs_.data(), channels, direct_channels, num_samples);
}

bool TrackMixerRuntime::any_lane_solo(const std::vector<TrackLaneConfig>& lanes) const noexcept {
  bool any_solo = false;
  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    any_solo = any_solo || lane_states_[lane_index].solo;
  }
  return any_solo;
}

void TrackMixerRuntime::process_lane_strip(size_t lane_index, int num_channels, int num_samples,
                                           int64_t timeline_sample) noexcept {
  LaneState& lane = lane_states_[lane_index];
  for (int ch = 0; ch < num_channels; ++ch) {
    lane_channel_ptrs_[static_cast<size_t>(ch)] = lane_channel(lane_index, ch);
  }
  // Capture the source before the strip mutates the lane buffers in place; kept for telemetry.
  capture_input_peak_db(lane_channel_ptrs_.data(), num_channels, num_samples, lane.input_peak_db);
  lane_in_pdc_delays_[lane_index].process(lane_channel_ptrs_.data(), num_channels, num_samples);
  if (lane.strip) {
    deliver_lane_sidechains(lane_index, num_channels, num_samples);
    lane.strip->process_at(lane_channel_ptrs_.data(), num_channels, num_samples, timeline_sample);
    lane_insert_gr_boards_[lane_index].publish(*lane.strip);
  } else {
    lane_insert_gr_boards_[lane_index].clear();
  }
  // The key leaves at p(L) + s(L); each consumer's key edge aligns it from there.
  snapshot_sidechain_key(lane_index, num_channels, num_samples);
  lane_pdc_delays_[lane_index].process(lane_channel_ptrs_.data(), num_channels, num_samples);
  // PFL is deliberately taken after the lane strip (and its PDC) but before
  // the lane fader/gate/pan stage in apply_lane_to_mix(). It therefore remains
  // audible when the lane is muted or solo-gated, matching the cue tap point.
  add_lane_monitor_pfl(lane_index, num_channels, num_samples);
}

void TrackMixerRuntime::add_lane_monitor_pfl(size_t lane_index, int num_channels,
                                             int num_samples) noexcept {
  if (monitor_bus_ == nullptr || lane_states_[lane_index].monitor_mode != TrackMonitorMode::kPfl) {
    return;
  }
  const int channels = std::min({num_channels, kMaxLaneChannels, monitor_bus_channel_count_});
  for (int ch = 0; ch < channels; ++ch) {
    float* dst = monitor_bus_[static_cast<size_t>(ch)];
    const float* src = lane_channel(lane_index, ch);
    if (dst == nullptr || src == nullptr) continue;
    for (int i = 0; i < num_samples; ++i) {
      dst[i] += src[i];
    }
  }
}

void TrackMixerRuntime::advance_lane_gain(size_t lane_index, int num_samples,
                                          bool any_solo) noexcept {
  LaneState& lane = lane_states_[lane_index];
  update_lane_gate_target(lane_index, any_solo);
  float* gain = lane_gain(lane_index);
  for (int i = 0; i < num_samples; ++i) {
    gain[i] = lane.fader_gain.process() * lane.gate.process();
  }
}

void TrackMixerRuntime::update_lane_gate_target(size_t lane_index, bool any_solo) noexcept {
  LaneState& lane = lane_states_[lane_index];
  const bool audible = !lane.mute && (!any_solo || lane.solo);
  lane.gate.set_target(audible ? 1.0f : 0.0f);
}

void TrackMixerRuntime::prime_lane_controls() noexcept {
  acquire_lanes();
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (!lanes || lanes->empty() || scratch_.empty()) return;
  if (lanes != applied_lane_snapshot_) prepare_lanes_from_snapshot(*lanes);
  const bool any_solo = any_lane_solo(*lanes);
  for (size_t lane_index = 0; lane_index < lanes->size(); ++lane_index) {
    update_lane_gate_target(lane_index, any_solo);
  }
}

void TrackMixerRuntime::mix_lane_sends(size_t lane_index, int num_channels, int num_samples,
                                       int64_t timeline_sample) noexcept {
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (!lanes || lane_index >= lanes->size()) return;
  LaneState& lane = lane_states_[lane_index];
  if (!lane.strip) return;
  const TrackLaneConfig& config = (*lanes)[lane_index];
  if (config.sends.empty()) return;
  // A send is tapped from lane-width buffers, so it is bounded by the lane's own
  // width regardless of how wide the destination bus is.
  const int rows = std::min(num_channels, kMaxLaneChannels);

  bool any_pre_fader = false;
  bool any_post_fader = false;
  for (size_t send_index = 0; send_index < config.sends.size(); ++send_index) {
    if (lane.strip->send_timing(send_index) == mixing::SendTiming::PreFader) {
      any_pre_fader = true;
    } else {
      any_post_fader = true;
    }
  }

  // Post-fader source: the lane buffer as it stands after the strip and the
  // cross-lane PDC alignment, scaled by this block's fader x gate ramp -- the
  // very signal apply_lane_to_mix is about to pan into the mix. Tapping it here
  // is what makes a post-fader send follow the lane's alignment AND its
  // fader/mute/solo, instead of running at full level off an unaligned tap.
  std::array<float*, kMaxLaneChannels> post_source{};
  if (any_post_fader) {
    const float* gain = lane_gain(lane_index);
    for (int ch = 0; ch < rows; ++ch) {
      const float* src = lane_channel(lane_index, ch);
      float* dst = send_source_channel(ch);
      for (int i = 0; i < num_samples; ++i) {
        dst[i] = src[i] * gain[i];
      }
      post_source[static_cast<size_t>(ch)] = dst;
    }
  }

  // Pre-fader source: the strip's own pre-fader tap, re-timed onto the lane
  // timebase. Built once per lane (never per send) so the alignment bank is fed
  // exactly one block of audio per block, and unconditionally whenever the lane
  // declares a pre-fader send, so an unresolvable send bus cannot leave a gap in
  // the bank's history.
  std::array<float*, kMaxLaneChannels> pre_source{};
  if (any_pre_fader) {
    for (int ch = 0; ch < rows; ++ch) {
      pre_source[static_cast<size_t>(ch)] = pre_send_source_channel(ch);
    }
    const int copied = lane.strip->copy_pre_fader_tap(pre_source.data(), rows, num_samples);
    for (int ch = copied; ch < rows; ++ch) {
      float* dst = pre_source[static_cast<size_t>(ch)];
      std::fill(dst, dst + num_samples, 0.0f);
    }
    lane_pre_send_pdc_delays_[lane_index].process(pre_source.data(), rows, num_samples);
  }

  std::array<float*, kMaxLaneChannels> dest{};
  for (size_t send_index = 0; send_index < config.sends.size(); ++send_index) {
    const TrackLaneConfig::Send& send = config.sends[send_index];
    BusState* bus = bus_state_for(send.bus_id);
    if (!bus) continue;
    const auto bus_it = std::find_if(bus_states_.begin(), bus_states_.end(),
                                     [bus](const BusState& state) { return &state == bus; });
    if (bus_it == bus_states_.end()) continue;
    const size_t bus_index = static_cast<size_t>(std::distance(bus_states_.begin(), bus_it));
    for (int ch = 0; ch < rows; ++ch) {
      dest[static_cast<size_t>(ch)] = bus_channel(bus_index, ch);
    }
    const bool pre_fader = lane.strip->send_timing(send_index) == mixing::SendTiming::PreFader;
    const float* const* source = pre_fader ? pre_source.data() : post_source.data();
    lane.strip->mix_send_from_at(send_index, source, dest.data(), rows, num_samples,
                                 timeline_sample);
  }
}

void TrackMixerRuntime::process_buses(float* const* channels, int master_channels, int num_samples,
                                      MeterTelemetryTap* meter_tap, int64_t render_frame,
                                      ScopeTelemetryTap* scope_tap) noexcept {
  // Master input stage: everything already summed into the master mix (lane dry
  // paths, and clips on tracks with no lane) is delayed by in(master) - L. The
  // bank rests at zero -- and process() short-circuits -- whenever no bus path
  // carries latency, which leaves a project without a latent bus insert
  // byte-identical.
  if (master_pdc_delay_.delay_samples_q8() != 0) {
    for (int ch = 0; ch < master_channels; ++ch) {
      lane_channel_ptrs_[static_cast<size_t>(ch)] = channels[static_cast<size_t>(ch)];
    }
    master_pdc_delay_.process(lane_channel_ptrs_.data(), master_channels, num_samples);
  }
  const int lane_channels = std::min(master_channels, kMaxLaneChannels);
  const size_t bus_count = bus_configs_.size();
  bus_key_frames_.fill(0);
  // First phase: re-time every bus input the lanes filled to in(b) - L, before
  // any bus adds into another.
  for (size_t bus_index = 0; bus_index < bus_count; ++bus_index) {
    if (bus_pdc_delays_[bus_index].delay_samples_q8() == 0) continue;
    std::array<float*, kMaxBusChannels> planes{};
    const int width = bus_render_channels(bus_index, master_channels);
    for (int ch = 0; ch < width; ++ch) planes[static_cast<size_t>(ch)] = bus_channel(bus_index, ch);
    bus_pdc_delays_[bus_index].process(planes.data(), width, num_samples);
  }
  // Second phase, in topological order: each bus renders, then feeds its
  // output and sends along their own aligned edges.
  for (size_t order_index = 0; order_index < bus_count; ++order_index) {
    const size_t bus_index = bus_order_[order_index];
    BusState& bus = bus_states_[bus_index];
    if (bus.bus == nullptr) continue;
    // Each bus runs its insert chain and gain at its own declared width; a
    // surround group bus is 6/8 wide, every other bus stays at the historical
    // min(master, 2) so its output is bit-identical.
    const int bus_channels = bus_render_channels(bus_index, master_channels);
    for (int ch = 0; ch < bus_channels; ++ch) {
      lane_channel_ptrs_[static_cast<size_t>(ch)] = bus_channel(bus_index, ch);
    }
    deliver_bus_sidechains(bus_index, lane_channels, num_samples);
    std::array<float, mixing::kMaxMeterChannels> bus_input_peak_db{};
    // Capture after bus PDC but before trim, polarity, EQ, or inserts. The
    // telemetry record can then expose the signal entering the bus controls
    // alongside the post-insert bus meter and gain reduction.
    capture_input_peak_db(lane_channel_ptrs_.data(), bus_channels, num_samples, bus_input_peak_db);
    // Input trim (pre-insert), mirroring a strip. The smoother holds a linear
    // gain (like the strip's GainProcessor), so it rests at unity (1.0) and is
    // skipped there, leaving a never-trimmed bus bit-identical.
    if (bus.input_trim_gain.current() != 1.0f || bus.input_trim_gain.target() != 1.0f) {
      for (int i = 0; i < num_samples; ++i) {
        const float trim = bus.input_trim_gain.process();
        for (int ch = 0; ch < bus_channels; ++ch) {
          float* plane = lane_channel_ptrs_[static_cast<size_t>(ch)];
          if (plane) plane[i] *= trim;
        }
      }
    }
    // Polarity invert on the front pair (pre-insert), mirroring a strip.
    const float polarity_l = bus.polarity_left.load(std::memory_order_relaxed);
    const float polarity_r = bus.polarity_right.load(std::memory_order_relaxed);
    if (polarity_l < 0.0f && bus_channels >= 1) {
      if (float* plane = lane_channel_ptrs_[0]) {
        for (int i = 0; i < num_samples; ++i) plane[i] *= polarity_l;
      }
    }
    if (polarity_r < 0.0f && bus_channels >= 2) {
      if (float* plane = lane_channel_ptrs_[1]) {
        for (int i = 0; i < num_samples; ++i) plane[i] *= polarity_r;
      }
    }
    // Dedicated EQ (pre-insert), every plane alike. Skipped with no enabled band,
    // and before prepare(), which is what sizes its state for every plane.
    if (bus.eq_enabled.load(std::memory_order_relaxed) &&
        bus.eq_active.load(std::memory_order_relaxed) && max_block_size_ > 0) {
      bus.eq.process(lane_channel_ptrs_.data(), bus_channels, num_samples);
    }
    bus.bus->process(lane_channel_ptrs_.data(), bus_channels, num_samples);
    bus_insert_gr_boards_[bus_index].publish(bus.bus->bus());
    // Output pan (post-insert, pre-width), stereo buses only. Skipped at rest:
    // the centred panner is not an exact identity under every law.
    if (bus_channels == 2 && !bus.panner.at_rest_identity()) {
      bus.panner.process(lane_channel_ptrs_.data(), 2, num_samples);
    }
    // Stereo width on the front pair (post-insert), mirroring a strip. Skipped
    // only while both the target and the in-flight smoothed width rest at 1: the
    // mid/side round-trip is not guaranteed bit-exact, so a never-widened bus
    // stays bit-identical, yet an in-flight ramp back toward 1 is not cut off
    // mid-glide (which would jump the side component and click).
    if ((bus.width.width() != 1.0f || bus.width.current_width() != 1.0f) && bus_channels >= 2 &&
        lane_channel_ptrs_[0] && lane_channel_ptrs_[1]) {
      bus.width.process(lane_channel_ptrs_.data(), 2, num_samples);
    }
    const BusRoute& route = bus_routes_[bus_index];
    std::array<const float*, kMaxBusChannels> post{};
    for (int ch = 0; ch < bus_channels; ++ch)
      post[static_cast<size_t>(ch)] = bus_channel(bus_index, ch);
    // Pre-gain taps: pre-fader sends and this bus's sidechain key.
    std::array<const float*, kMaxBusChannels> pre{};
    if (route.any_pre_send) {
      for (int ch = 0; ch < bus_channels; ++ch) {
        float* dst = bus_pre_tap_channel(ch);
        std::copy(post[static_cast<size_t>(ch)], post[static_cast<size_t>(ch)] + num_samples, dst);
        pre[static_cast<size_t>(ch)] = dst;
      }
    }
    if (route.key_source) {
      std::array<float*, kMaxLaneChannels> key{};
      for (int ch = 0; ch < kMaxLaneChannels; ++ch) {
        key[static_cast<size_t>(ch)] = bus_key_channel(bus_index, ch);
      }
      int key_channels = std::min(bus_channels, kMaxLaneChannels);
      if (is_surround_channel_count(bus_channels)) {
        mixing::downmix(layout_from_channel_count(bus_channels), ChannelLayout::Stereo, post.data(),
                        key.data(), static_cast<size_t>(num_samples));
        key_channels = kMaxLaneChannels;
      } else {
        for (int ch = 0; ch < key_channels; ++ch) {
          std::copy(post[static_cast<size_t>(ch)], post[static_cast<size_t>(ch)] + num_samples,
                    key[static_cast<size_t>(ch)]);
        }
      }
      bus_key_frames_[bus_index] = num_samples;
      bus_key_channels_[bus_index] = key_channels;
    }
    for (int i = 0; i < num_samples; ++i) {
      const float gain = bus.gain.process();
      for (int ch = 0; ch < bus_channels; ++ch) {
        float* bus_channel_ptr = bus_channel(bus_index, ch);
        if (bus_channel_ptr) bus_channel_ptr[i] *= gain;
      }
    }
    // Meter the full bus width so a surround group bus publishes per-plane
    // telemetry (drained via the wide meter drain); the goniometer scope stays a
    // stereo metric on the front pair.
    if (meter_tap) {
      meter_tap->process_lightweight(lane_channel_ptrs_.data(), bus_channels, num_samples,
                                     render_frame, bus_meter_target(bus_index),
                                     bus_input_peak_db.data(),
                                     bus.bus->bus().meter_snapshot().gain_reduction_db);
    }
    if (scope_tap) {
      scope_tap->process(lane_channel_ptrs_.data(), std::min(bus_channels, kMaxLaneChannels),
                         num_samples, render_frame, bus_meter_target(bus_index));
    }
    // Each edge re-times its own copy by in(destination) - out(this bus), then
    // lands on the destination by the width rule. Applied after metering so the
    // bus meters keep reporting the bus's own output.
    const auto feed_edge = [&](size_t edge, const float* const* source, int destination,
                               mixing::SendProcessor* send) noexcept {
      float* const* dest = channels;
      int dest_channels = master_channels;
      std::array<float*, kMaxBusChannels> dest_planes{};
      if (destination >= 0) {
        const size_t dest_index = static_cast<size_t>(destination);
        dest_channels = bus_render_channels(dest_index, master_channels);
        for (int ch = 0; ch < dest_channels; ++ch) {
          dest_planes[static_cast<size_t>(ch)] = bus_channel(dest_index, ch);
        }
        dest = dest_planes.data();
      }
      mixing::AlignmentDelay& delay = bus_edge_delays_[bus_index * kBusEdgesPerBus + edge];
      if (delay.delay_samples_q8() == 0 && send == nullptr) {
        add_with_width_rule(source, bus_channels, dest, dest_channels, num_samples);
        return;
      }
      std::array<float*, kMaxBusChannels> scratch{};
      std::array<const float*, kMaxBusChannels> feed{};
      for (int ch = 0; ch < bus_channels; ++ch) {
        scratch[static_cast<size_t>(ch)] = bus_edge_channel(ch);
        feed[static_cast<size_t>(ch)] = scratch[static_cast<size_t>(ch)];
        std::copy(source[ch], source[ch] + num_samples, scratch[static_cast<size_t>(ch)]);
      }
      delay.process(scratch.data(), bus_channels, num_samples);
      if (send != nullptr) send->process(scratch.data(), bus_channels, num_samples);
      add_with_width_rule(feed.data(), bus_channels, dest, dest_channels, num_samples);
    };
    feed_edge(0, post.data(), route.output_index, nullptr);
    const TrackBusConfig& config = bus_configs_[bus_index];
    for (size_t send_index = 0; send_index < route.send_count; ++send_index) {
      mixing::SendProcessor* send = bus_sends_[bus_index][send_index].get();
      const int destination = route.send_index[send_index];
      if (send == nullptr || destination < 0) continue;
      const bool pre_fader = config.sends[send_index].timing == mixing::SendTiming::PreFader;
      feed_edge(1 + send_index, pre_fader ? pre.data() : post.data(), destination, send);
    }
  }
  // Master keys, aligned to the master input, for deliver_master_sidechains().
  for (size_t i = 0; i < audio_sidechains_.count; ++i) {
    const SidechainBinding& binding = audio_sidechains_.bindings[i];
    if (binding.target_kind != static_cast<uint8_t>(SidechainTargetKind::Master)) continue;
    const size_t slot = binding.key_slot;
    std::array<const float*, kMaxLaneChannels> planes{};
    const int key_channels = build_keyed_input(i, lane_channels, num_samples, planes, true);
    master_key_frames_[slot] = key_channels > 0 ? num_samples : 0;
    master_key_channels_[slot] = key_channels;
  }
}

void TrackMixerRuntime::add_with_width_rule(const float* const* source, int from_channels,
                                            float* const* dest, int to_channels,
                                            int num_samples) noexcept {
  const auto standard = [](int count) {
    return count == 1 || count == 2 || is_surround_channel_count(count);
  };
  // Outlives the branch: source points into it for the accumulation below.
  std::array<float*, kMaxBusChannels> fold{};
  if (from_channels > to_channels && standard(from_channels) && standard(to_channels)) {
    for (int ch = 0; ch < to_channels; ++ch) fold[static_cast<size_t>(ch)] = bus_fold_channel(ch);
    mixing::downmix(layout_from_channel_count(from_channels),
                    layout_from_channel_count(to_channels), source, fold.data(),
                    static_cast<size_t>(num_samples));
    source = fold.data();
  }
  const int planes = std::min(from_channels, to_channels);
  for (int ch = 0; ch < planes; ++ch) {
    float* dst = dest[static_cast<size_t>(ch)];
    const float* src = source[static_cast<size_t>(ch)];
    if (!dst || !src) continue;
    for (int i = 0; i < num_samples; ++i) {
      dst[i] += src[i];
    }
  }
}

float* TrackMixerRuntime::bus_edge_channel(int channel) noexcept {
  return bus_edge_scratch_.data() +
         static_cast<size_t>(channel) * static_cast<size_t>(max_block_size_);
}

float* TrackMixerRuntime::bus_pre_tap_channel(int channel) noexcept {
  return bus_edge_channel(kMaxBusChannels + channel);
}

float* TrackMixerRuntime::bus_fold_channel(int channel) noexcept {
  return bus_edge_channel(2 * kMaxBusChannels + channel);
}

float* TrackMixerRuntime::bus_key_channel(size_t bus_index, int channel) noexcept {
  return bus_key_scratch_.data() + (bus_index * kMaxLaneChannels + static_cast<size_t>(channel)) *
                                       static_cast<size_t>(max_block_size_);
}

float* TrackMixerRuntime::keyed_input_channel(size_t slot, int channel) noexcept {
  return keyed_input_scratch_.data() + (slot * kMaxLaneChannels + static_cast<size_t>(channel)) *
                                           static_cast<size_t>(max_block_size_);
}

void TrackMixerRuntime::apply_lane_to_mix(size_t lane_index, float* const* channels,
                                          int num_channels, int num_samples,
                                          MeterTelemetryTap* meter_tap, int64_t render_frame,
                                          ScopeTelemetryTap* scope_tap,
                                          int master_channels) noexcept {
  LaneState& lane = lane_states_[lane_index];
  // Group/folder routing: a lane with an output bus sums its post-fader
  // signal into that bus buffer instead of the master mix; process_buses
  // (which runs after every lane was applied) then carries it to the master
  // through the bus gain and inserts. The lane's sends were tapped from the same
  // fader x gate ramp this stage applies, so muting or soloing reaches them too.
  float* dest_left = channels[0];
  float* dest_right = num_channels >= 2 ? channels[1] : nullptr;
  int dest_channels = master_channels;
  bool routed_to_bus = false;
  std::array<float*, kMaxBusChannels> bus_planes{};
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (lanes && lane_index < lanes->size() && (*lanes)[lane_index].output_bus_id != 0) {
    const uint32_t output_bus_id = (*lanes)[lane_index].output_bus_id;
    for (size_t bus_index = 0; bus_index < bus_configs_.size(); ++bus_index) {
      if (bus_configs_[bus_index].bus_id != output_bus_id) continue;
      dest_channels = bus_render_channels(bus_index, master_channels);
      dest_left = bus_channel(bus_index, 0);
      dest_right = dest_channels >= 2 ? bus_channel(bus_index, 1) : nullptr;
      for (int ch = 0; ch < dest_channels; ++ch) {
        bus_planes[static_cast<size_t>(ch)] = bus_channel(bus_index, ch);
      }
      routed_to_bus = true;
      break;
    }
  }
  // Surround destination: a lane summing into a >2-channel master mix or a
  // surround group bus is scattered by the surround panner. Stereo/mono
  // destinations take the byte-identical legacy stereo path below.
  if (is_surround_channel_count(dest_channels)) {
    float* const* dest = routed_to_bus ? bus_planes.data() : channels;
    apply_lane_to_mix_surround(lane_index, dest, num_channels, dest_channels, num_samples);
  } else {
    // A stereo/mono block is a width too: recording it means the next surround
    // block starts from placement rather than from a scatter position left
    // behind an arbitrarily long interlude.
    lane.surround_primed_channels = dest_channels;
    // Honor the strip's configured pan law (the offline/set_track_pan path
    // already does), evaluated with the same NearUnity balance normalization as
    // PannerProcessor's Balance mode so a centered lane stays at unity for any
    // law and only the away channel is pulled down. A lane with no strip carries
    // no configured law, so it takes the plain linear balance.
    const mixing::PanLaw lane_pan_law =
        lane.strip ? lane.strip->pan_law() : mixing::PanLaw::Linear0dB;
    // The AFL tap's eligibility is fixed for the whole block, so resolve the
    // destination planes once rather than re-testing the mode and the bus
    // pointers on every sample of the mix loop.
    const bool afl = lane.monitor_mode == TrackMonitorMode::kAfl && monitor_bus_ != nullptr;
    float* afl_left = afl && monitor_bus_channel_count_ >= 1 ? monitor_bus_[0] : nullptr;
    float* afl_right = afl && monitor_bus_channel_count_ >= 2 ? monitor_bus_[1] : nullptr;
    // The fader x gate product was advanced (once) by advance_lane_gain; reading
    // it back here rather than re-advancing the smoothers is what keeps this
    // stage and the lane's sends on exactly the same per-sample gain.
    const float* lane_fader_gate = lane_gain(lane_index);
    for (int i = 0; i < num_samples; ++i) {
      const float pan = lane.pan.process();
      float left_gain = lane_fader_gate[i];
      float right_gain = left_gain;
      // A centered lane is left at unity (no pan processing) so an unpanned lane
      // stays bit-exact regardless of the law; only an off-center pan engages
      // the law-aware, balance-normalized gains.
      if (num_channels >= 2 && pan != 0.0f) {
        const mixing::PanGains g =
            mixing::compute_pan_gains(pan, lane_pan_law, mixing::PanNormalization::NearUnity);
        left_gain *= g.left;
        right_gain *= g.right;
      }
      lane_channel(lane_index, 0)[i] *= left_gain;
      if (dest_left) dest_left[i] += lane_channel(lane_index, 0)[i];
      if (afl_left) afl_left[i] += lane_channel(lane_index, 0)[i];
      if (num_channels >= 2 && dest_right) {
        lane_channel(lane_index, 1)[i] *= right_gain;
        dest_right[i] += lane_channel(lane_index, 1)[i];
        if (afl_right) afl_right[i] += lane_channel(lane_index, 1)[i];
      }
    }
  }
  if (meter_tap) {
    for (int ch = 0; ch < num_channels; ++ch) {
      lane_channel_ptrs_[static_cast<size_t>(ch)] = lane_channel(lane_index, ch);
    }
    meter_tap->process_lightweight(
        lane_channel_ptrs_.data(), num_channels, num_samples, render_frame,
        lane_meter_target(lane_index), lane.input_peak_db.data(),
        lane.strip != nullptr ? lane.strip->last_gain_reduction_db() : 0.0f);
  }
  if (scope_tap) {
    for (int ch = 0; ch < num_channels; ++ch) {
      lane_channel_ptrs_[static_cast<size_t>(ch)] = lane_channel(lane_index, ch);
    }
    scope_tap->process(lane_channel_ptrs_.data(), num_channels, num_samples, render_frame,
                       lane_meter_target(lane_index));
  }
}

void TrackMixerRuntime::apply_lane_to_mix_surround(size_t lane_index, float* const* dest,
                                                   int lane_channels, int dest_channels,
                                                   int num_samples) noexcept {
  LaneState& lane = lane_states_[lane_index];
  const ChannelLayout dest_layout = layout_from_channel_count(dest_channels);
  mixing::SurroundPanParams params;
  if (lane.strip != nullptr) {
    params = lane.strip->surround_pan_params();
  }
  mixing::SurroundPanGains target;
  if (!mixing::try_compute_surround_pan_gains(params, dest_layout, &target)) return;
  const int planes = std::min(dest_channels, mixing::kMaxSurroundPlanes);
  // First surround block at this destination width: snap the carried scatter
  // gains to the target so the block starts at full placement instead of fading
  // in from silence, or from gains a different layout computed. This makes an
  // offline bounce deterministic (no dependence on a pre-roll settle pass) and
  // avoids a first-block click live.
  if (lane.surround_primed_channels != dest_channels) {
    for (int p = 0; p < planes; ++p) {
      lane.surround_gain[static_cast<size_t>(p)].reset(target.gain[static_cast<size_t>(p)]);
    }
    lane.surround_primed_channels = dest_channels;
  }
  for (int p = 0; p < planes; ++p) {
    lane.surround_gain[static_cast<size_t>(p)].set_target(target.gain[static_cast<size_t>(p)]);
  }
  // Block-invariant, so it is resolved once instead of per sample per plane.
  const bool afl = lane.monitor_mode == TrackMonitorMode::kAfl && monitor_bus_ != nullptr;
  const int afl_planes = afl ? std::min(planes, monitor_bus_channel_count_) : 0;
  const float* lane_fader_gate = lane_gain(lane_index);
  for (int i = 0; i < num_samples; ++i) {
    // Keep the stereo pan smoother advancing so a later stereo render resumes
    // from the right phase; surround placement comes from the panner, not pan.
    (void)lane.pan.process();
    const float fg = lane_fader_gate[i];
    float left = lane_channel(lane_index, 0)[i] * fg;
    lane_channel(lane_index, 0)[i] = left;
    float src = left;
    if (lane_channels >= 2) {
      const float right = lane_channel(lane_index, 1)[i] * fg;
      lane_channel(lane_index, 1)[i] = right;
      // -6 dB stereo fold to a point source keeps a correlated centre at unity.
      src = 0.5f * (left + right);
    }
    // One smoother step per plane per sample: the glide is a sample-rate-derived
    // time constant, so it is identical however process() split the block.
    for (int p = 0; p < planes; ++p) {
      const float g = lane.surround_gain[static_cast<size_t>(p)].process();
      const float sample = g * src;
      if (dest[p] != nullptr) dest[p][i] += sample;
      if (p < afl_planes && monitor_bus_[p] != nullptr) {
        monitor_bus_[p][i] += sample;
      }
    }
  }
}

}  // namespace sonare::engine
