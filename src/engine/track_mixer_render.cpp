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
  acquire_lanes();
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (!lanes || lanes->empty()) return false;
  if (!channels || num_channels <= 0 || num_samples <= 0) return true;
  if (num_channels > kMaxBusChannels || num_samples > max_block_size_ || scratch_.empty()) {
    return false;
  }

  const int render_channels = std::min(num_channels, kMaxLaneChannels);
  const int master_channels = std::min(num_channels, kMaxBusChannels);
  if (lanes != applied_lane_snapshot_) prepare_lanes_from_snapshot(*lanes);
  // Advance the insert-parameter smoothers once for this sub-block before any
  // lane/bus chain runs, so an automated insert param reaches its processor at
  // the same cadence as the lane fader smoother (no double advance: the bus
  // chain runs only inside this call).
  advance_insert_automations(num_samples);
  for (size_t bus_index = 0; bus_index < bus_configs_.size(); ++bus_index) {
    clear_bus(bus_index, bus_render_channels(bus_index, master_channels), num_samples);
  }
  for (size_t lane_index = 0; lane_index < lanes->size(); ++lane_index) {
    active_track_ids_[lane_index] = (*lanes)[lane_index].track_id;
    clear_lane(lane_index, render_channels, num_samples);
    for (int ch = 0; ch < render_channels; ++ch) {
      lane_channel_ptrs_[static_cast<size_t>(ch)] = lane_channel(lane_index, ch);
    }
    player.process_track_at((*lanes)[lane_index].track_id, lane_channel_ptrs_.data(),
                            render_channels, num_samples, timeline_sample);
    process_lane_strip(lane_index, render_channels, num_samples, timeline_sample);
    mix_lane_sends(lane_index, render_channels, num_samples, timeline_sample);
  }
  player.process_excluding_tracks_at(active_track_ids_.data(), lanes->size(), channels,
                                     render_channels, num_samples, timeline_sample);

  const bool any_solo = any_lane_solo(*lanes);
  for (size_t lane_index = 0; lane_index < lanes->size(); ++lane_index) {
    apply_lane_to_mix(lane_index, channels, render_channels, num_samples, any_solo, meter_tap,
                      render_frame, scope_tap, master_channels);
  }
  process_buses(channels, master_channels, num_samples, meter_tap, render_frame, scope_tap);
  return true;
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
  acquire_lanes();
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (!lanes || lanes->empty()) return false;
  if (num_channels <= 0 || num_samples <= 0) return false;
  if (num_channels > kMaxBusChannels || num_samples > max_block_size_ || scratch_.empty()) {
    return false;
  }
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
  return true;
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
    mix_lane_sends(lane_index, render_channels, num_samples, 0);
    apply_lane_to_mix(lane_index, channels, render_channels, num_samples, any_solo, meter_tap,
                      render_frame, scope_tap, master_channels);
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
  snapshot_sidechain_key(lane_index, num_channels, num_samples);
}

void TrackMixerRuntime::mix_lane_sends(size_t lane_index, int num_channels, int num_samples,
                                       int64_t timeline_sample) noexcept {
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (!lanes || lane_index >= lanes->size()) return;
  LaneState& lane = lane_states_[lane_index];
  if (!lane.strip) return;
  const TrackLaneConfig& config = (*lanes)[lane_index];
  for (size_t send_index = 0; send_index < config.sends.size(); ++send_index) {
    const TrackLaneConfig::Send& send = config.sends[send_index];
    BusState* bus = bus_state_for(send.bus_id);
    if (!bus) continue;
    const auto bus_it = std::find_if(bus_states_.begin(), bus_states_.end(),
                                     [bus](const BusState& state) { return &state == bus; });
    if (bus_it == bus_states_.end()) continue;
    const size_t bus_index = static_cast<size_t>(std::distance(bus_states_.begin(), bus_it));
    for (int ch = 0; ch < num_channels; ++ch) {
      lane_channel_ptrs_[static_cast<size_t>(ch)] = bus_channel(bus_index, ch);
    }
    lane.strip->mix_send_at(send_index, lane_channel_ptrs_.data(), num_channels, num_samples,
                            timeline_sample);
  }
}

void TrackMixerRuntime::process_buses(float* const* channels, int master_channels, int num_samples,
                                      MeterTelemetryTap* meter_tap, int64_t render_frame,
                                      ScopeTelemetryTap* scope_tap) noexcept {
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
                                          int num_channels, int num_samples, bool any_solo,
                                          MeterTelemetryTap* meter_tap, int64_t render_frame,
                                          ScopeTelemetryTap* scope_tap,
                                          int master_channels) noexcept {
  LaneState& lane = lane_states_[lane_index];
  const bool audible = !lane.mute && (!any_solo || lane.solo);
  lane.gate.set_target(audible ? 1.0f : 0.0f);
  // Group/folder routing: a lane with an output bus sums its post-fader
  // signal into that bus buffer instead of the master mix; process_buses
  // (which runs after every lane was applied) then carries it to the master
  // through the bus gain and inserts. Sends were already tapped pre-fader.
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
    // already does). Use the same balance normalization as PannerProcessor's
    // Balance mode so a centered lane stays at unity for any law and only the
    // away channel is pulled down — for Linear0dB this is byte-identical to the
    // previous hardcoded balance. A lane with no strip carries no configured
    // law, so it keeps the historical Linear0dB balance.
    const mixing::PanLaw lane_pan_law =
        lane.strip ? lane.strip->pan_law() : mixing::PanLaw::Linear0dB;
    for (int i = 0; i < num_samples; ++i) {
      const float fader = lane.fader_gain.process();
      const float gate = lane.gate.process();
      const float pan = lane.pan.process();
      float left_gain = fader * gate;
      float right_gain = fader * gate;
      // A centered lane is left at unity (no pan processing) so an unpanned lane
      // stays bit-exact regardless of the law; only an off-center pan engages
      // the law-aware, balance-normalized gains.
      if (num_channels >= 2 && pan != 0.0f) {
        const mixing::PanGains g = mixing::compute_pan_gains(pan, lane_pan_law);
        const float norm = std::max(g.left, g.right);
        const float inv_norm = norm > 0.0f ? 1.0f / norm : 0.0f;
        left_gain *= g.left * inv_norm;
        right_gain *= g.right * inv_norm;
      }
      lane_channel(lane_index, 0)[i] *= left_gain;
      if (dest_left) dest_left[i] += lane_channel(lane_index, 0)[i];
      if (num_channels >= 2 && dest_right) {
        lane_channel(lane_index, 1)[i] *= right_gain;
        dest_right[i] += lane_channel(lane_index, 1)[i];
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
  for (int i = 0; i < num_samples; ++i) {
    const float fader = lane.fader_gain.process();
    const float gate = lane.gate.process();
    // Keep the stereo pan smoother advancing so a later stereo render resumes
    // from the right phase; surround placement comes from the panner, not pan.
    (void)lane.pan.process();
    const float fg = fader * gate;
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
      if (dest[p] != nullptr) dest[p][i] += g * src;
    }
  }
  for (int p = 0; p < planes; ++p) {
    lane.surround_gain[static_cast<size_t>(p)] = target.gain[static_cast<size_t>(p)];
  }
}

}  // namespace sonare::engine
