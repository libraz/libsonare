#include <algorithm>
#include <cmath>

#include "engine/meter_telemetry.h"
#include "engine/scope_telemetry.h"
#include "engine/track_mixer.h"
#include "engine/track_mixer_internal.h"

namespace sonare::engine {

bool TrackMixerRuntime::render_clips(ClipPlayer& player, float* const* channels, int num_channels,
                                     int num_samples, int64_t timeline_sample,
                                     MeterTelemetryTap* meter_tap, int64_t render_frame,
                                     ScopeTelemetryTap* scope_tap) noexcept {
  // Self-contained clip-only block: exactly begin / render-into-lanes / finish
  // for a block whose only contributor is the clip player. Retained for callers
  // that have no instrument pass to fold in (the PDC path renders the clip bus
  // into its own delayed scratch, so its buses genuinely belong to that pass).
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
    source_mix_lane_active_[lane_index] = false;
  }
  const int master_channels = std::min(num_channels, kMaxBusChannels);
  for (size_t bus_index = 0; bus_index < bus_configs_.size(); ++bus_index) {
    clear_bus(bus_index, bus_render_channels(bus_index, master_channels), num_samples);
  }
  return true;
}

bool TrackMixerRuntime::render_clips_into_lanes(ClipPlayer& player, float* const* channels,
                                                int num_channels, int num_samples,
                                                int64_t timeline_sample) noexcept {
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
    // The clip pass touches every lane, including the ones the block leaves
    // silent: a lane strip's inserts are stateful, so skipping a silent lane
    // would freeze a reverb tail or a compressor release mid-decay.
    source_mix_lane_active_[lane_index] = true;
  }
  // Clips on tracks without a lane sum straight into the master mix; this must
  // land before any lane output, matching the pre-aggregation order.
  player.process_excluding_tracks_at(active_track_ids_.data(), lanes->size(), channels,
                                     render_channels, num_samples, timeline_sample);
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
  const bool any_solo = any_lane_solo(*lanes);
  // Two passes (all strips + sends, then all lane outputs), not one interleaved
  // pass: this is the accumulation order the clip path has always used, so a
  // clip-only block stays bit-identical.
  for (size_t lane_index = 0; lane_index < lanes->size(); ++lane_index) {
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
  // one source (kept bit-identical to the historical inline implementation).
  if (lanes != applied_lane_snapshot_) prepare_lanes_from_snapshot(*lanes);
  advance_insert_automations(num_samples);
  const int render_channels = std::min(num_channels, kMaxLaneChannels);
  for (size_t lane_index = 0; lane_index < lanes->size(); ++lane_index) {
    clear_lane(lane_index, render_channels, num_samples);
    source_mix_lane_active_[lane_index] = false;
  }
  const int master_channels = std::min(num_channels, kMaxBusChannels);
  for (size_t bus_index = 0; bus_index < bus_configs_.size(); ++bus_index) {
    clear_bus(bus_index, bus_render_channels(bus_index, master_channels), num_samples);
  }
  bool routed_through_lane = false;
  mix_source_into_lane(track_id, source, channels, num_channels, num_samples, routed_through_lane,
                       meter_tap, render_frame, scope_tap);
  if (routed_through_lane) {
    finish_source_mix(channels, num_channels, num_samples, meter_tap, render_frame, scope_tap);
  }
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
  add_source_to_mix(source, channels, render_channels, num_samples);
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
  const bool any_solo = any_lane_solo(*lanes);
  for (size_t lane_index = 0; lane_index < lanes->size(); ++lane_index) {
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
  if (lane.strip) {
    deliver_lane_sidechains(lane_index, num_channels, num_samples);
    lane.strip->process_at(lane_channel_ptrs_.data(), num_channels, num_samples, timeline_sample);
  }
  lane_pdc_delays_[lane_index].process(lane_channel_ptrs_.data(), num_channels, num_samples);
  // PFL is deliberately taken after the lane strip (and its PDC) but before
  // the lane fader/gate/pan stage in apply_lane_to_mix(). It therefore remains
  // audible when the lane is muted or solo-gated, matching the cue tap point.
  add_lane_monitor_pfl(lane_index, num_channels, num_samples);
  snapshot_sidechain_key(lane_index, num_channels, num_samples);
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
  const bool audible = !lane.mute && (!any_solo || lane.solo);
  lane.gate.set_target(audible ? 1.0f : 0.0f);
  float* gain = lane_gain(lane_index);
  for (int i = 0; i < num_samples; ++i) {
    gain[i] = lane.fader_gain.process() * lane.gate.process();
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
  // Bus-stage PDC, first half: everything already summed into the master mix
  // (lane dry paths, and clips on tracks with no lane) reached it without
  // passing through any bus insert chain, so it is delayed by the widest bus
  // latency. The bank rests at zero -- and process() short-circuits -- whenever
  // no bus carries latency, which leaves a project without a latent bus insert
  // byte-identical.
  if (master_pdc_delay_.delay_samples_q8() != 0) {
    for (int ch = 0; ch < master_channels; ++ch) {
      lane_channel_ptrs_[static_cast<size_t>(ch)] = channels[static_cast<size_t>(ch)];
    }
    master_pdc_delay_.process(lane_channel_ptrs_.data(), master_channels, num_samples);
  }
  for (size_t bus_index = 0; bus_index < bus_configs_.size(); ++bus_index) {
    BusState& bus = bus_states_[bus_index];
    if (bus.bus == nullptr) continue;
    // Each bus runs its insert chain and gain at its own declared width; a
    // surround group bus is 6/8 wide, every other bus stays at the historical
    // min(master, 2) so its output is bit-identical.
    const int bus_channels = bus_render_channels(bus_index, master_channels);
    for (int ch = 0; ch < bus_channels; ++ch) {
      lane_channel_ptrs_[static_cast<size_t>(ch)] = bus_channel(bus_index, ch);
    }
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
    bus.bus->process(lane_channel_ptrs_.data(), bus_channels, num_samples);
    // Stereo width on the front pair (post-insert), mirroring a strip. Skipped
    // only while both the target and the in-flight smoothed width rest at 1: the
    // mid/side round-trip is not guaranteed bit-exact, so a never-widened bus
    // stays bit-identical, yet an in-flight ramp back toward 1 is not cut off
    // mid-glide (which would jump the side component and click).
    if ((bus.width.width() != 1.0f || bus.width.current_width() != 1.0f) && bus_channels >= 2 &&
        lane_channel_ptrs_[0] && lane_channel_ptrs_[1]) {
      bus.width.process(lane_channel_ptrs_.data(), 2, num_samples);
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
                                     render_frame, bus_meter_target(bus_index));
    }
    if (scope_tap) {
      scope_tap->process(lane_channel_ptrs_.data(), std::min(bus_channels, kMaxLaneChannels),
                         num_samples, render_frame, bus_meter_target(bus_index));
    }
    // Bus-stage PDC, second half: this bus carries (widest bus latency - its
    // own), so its output lands on the master sum at the same instant as the
    // delayed dry mix and as every other bus. Applied after metering so the bus
    // meters keep reporting the bus's own output, and skipped entirely when the
    // bank rests at zero.
    bus_pdc_delays_[bus_index].process(lane_channel_ptrs_.data(), bus_channels, num_samples);
    // Sum the bus into the master plane-by-plane, up to the planes both share.
    const int sum_channels = std::min(bus_channels, master_channels);
    for (int ch = 0; ch < sum_channels; ++ch) {
      float* dst = channels[static_cast<size_t>(ch)];
      const float* src = bus_channel(bus_index, ch);
      if (!dst || !src) continue;
      for (int i = 0; i < num_samples; ++i) {
        dst[i] += src[i];
      }
    }
  }
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
    meter_tap->process_lightweight(lane_channel_ptrs_.data(), num_channels, num_samples,
                                   render_frame, lane_meter_target(lane_index));
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
  // First surround block for this lane: snap the carried scatter gains to the
  // target so the block starts at full placement instead of fading in from
  // silence. This makes an offline bounce deterministic (no dependence on a
  // pre-roll settle pass) and avoids a first-block click live.
  if (!lane.surround_primed) {
    for (int p = 0; p < planes; ++p) {
      lane.surround_gain[static_cast<size_t>(p)] = target.gain[static_cast<size_t>(p)];
    }
    lane.surround_primed = true;
  }
  const float inv_n = num_samples > 0 ? 1.0f / static_cast<float>(num_samples) : 0.0f;
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
    // Linearly ramp each plane's gain from last block's value to this block's
    // target so a moving pan does not step.
    const float t = static_cast<float>(i + 1) * inv_n;
    for (int p = 0; p < planes; ++p) {
      const float start = lane.surround_gain[static_cast<size_t>(p)];
      const float g = start + (target.gain[static_cast<size_t>(p)] - start) * t;
      const float sample = g * src;
      if (dest[p] != nullptr) dest[p][i] += sample;
      if (p < afl_planes && monitor_bus_[p] != nullptr) {
        monitor_bus_[p][i] += sample;
      }
    }
  }
  for (int p = 0; p < planes; ++p) {
    lane.surround_gain[static_cast<size_t>(p)] = target.gain[static_cast<size_t>(p)];
  }
}

}  // namespace sonare::engine
