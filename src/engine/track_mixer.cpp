#include "engine/track_mixer.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <stdexcept>

#include "engine/meter_telemetry.h"
#include "engine/scope_telemetry.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"

namespace sonare::engine {

namespace {

mixing::PanMode to_pan_mode(int mode) {
  switch (mode) {
    case 0:
      return mixing::PanMode::Balance;
    case 1:
      return mixing::PanMode::StereoPan;
    case 2:
      return mixing::PanMode::DualPan;
    default:
      return mixing::PanMode::Balance;
  }
}

mixing::PanLaw to_pan_law(int law) {
  switch (law) {
    case 1:
      return mixing::PanLaw::Const4p5dB;
    case 2:
      return mixing::PanLaw::Const6dB;
    case 3:
      return mixing::PanLaw::Linear0dB;
    case 0:
    default:
      return mixing::PanLaw::Const3dB;
  }
}

constexpr uint32_t lane_meter_target(size_t lane_index) noexcept {
  return static_cast<uint32_t>(lane_index + 1);
}

constexpr uint32_t bus_meter_target(size_t bus_index) noexcept {
  return static_cast<uint32_t>(33 + bus_index);
}

}  // namespace

std::unique_ptr<mixing::ChannelStrip> make_channel_strip_from_spec(const mixing::api::Strip& spec) {
  auto strip = std::make_unique<mixing::ChannelStrip>(
      mixing::ChannelStripConfig{spec.fader_db, spec.pan, to_pan_law(spec.pan_law), 5.0f,
                                 mixing::EqPosition::PreFader, spec.input_trim_db});
  strip->set_vca_offset_db(spec.vca_offset_db);
  strip->set_width(spec.width);
  strip->set_muted(spec.muted);
  strip->set_soloed(spec.soloed);
  strip->set_solo_safe(spec.solo_safe);
  strip->set_pan_mode(to_pan_mode(spec.pan_mode));
  strip->set_dual_pan(spec.dual_pan_left, spec.dual_pan_right);
  strip->set_polarity_invert(spec.polarity_invert_left, spec.polarity_invert_right);
  strip->set_channel_delay_samples(spec.channel_delay_samples);
  strip->set_surround_pan_params({spec.surround_pan.azimuth, spec.surround_pan.elevation,
                                  spec.surround_pan.divergence, spec.surround_pan.lfe,
                                  spec.surround_pan.distance});
  for (const auto& insert : spec.inserts) {
    auto processor =
        mastering::api::make_insert(insert.processor_name, insert.params_json, nullptr);
    if (!processor) {
      return nullptr;
    }
    const bool spo = mastering::api::channel_policy(insert.processor_name) ==
                     mastering::api::ChannelPolicy::StereoPairOnly;
    if (insert.slot == mixing::api::InsertSlot::PreFader) {
      strip->add_pre_insert(std::move(processor), spo);
    } else {
      strip->add_post_insert(std::move(processor), spo);
    }
  }
  return strip;
}

bool TrackMixerRuntime::set_track_lanes(std::vector<TrackLaneConfig> lanes) {
  if (!lane_config_valid(lanes)) return false;
  const auto snapshot = std::make_shared<const std::vector<TrackLaneConfig>>(std::move(lanes));
  if (!lanes_.publish(snapshot)) return false;
  acquire_lanes();
  prepare_lanes_from_snapshot(*snapshot);
  try {
    configure_lane_sends(*snapshot);
  } catch (...) {
    return false;
  }
  recompute_lane_pdc(*snapshot);
  return true;
}

bool TrackMixerRuntime::set_buses(std::vector<TrackBusConfig> buses) {
  if (!bus_config_valid(buses)) return false;
  bus_configs_ = std::move(buses);
  for (size_t index = 0; index < bus_states_.size(); ++index) {
    BusState& state = bus_states_[index];
    if (index < bus_configs_.size()) {
      state.bus_id = bus_configs_[index].bus_id;
      state.gain_db.prepare(sample_rate_, 5.0f);
      state.gain_db.reset(bus_configs_[index].gain_db);
      if (!state.bus) {
        state.bus = std::make_unique<mixing::FxBus>(static_cast<int>(kMaxTrackLanes));
      }
      if (max_block_size_ > 0) {
        state.bus->prepare(sample_rate_, max_block_size_);
      }
    } else {
      state.bus_id = 0;
      state.gain_db.reset(0.0f);
      state.bus.reset();
    }
  }
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    try {
      configure_lane_sends(*lanes);
    } catch (...) {
      return false;
    }
  }
  return true;
}

bool TrackMixerRuntime::active() const noexcept {
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  return lanes && !lanes->empty();
}

size_t TrackMixerRuntime::lane_count() const noexcept {
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  return lanes ? lanes->size() : 0;
}

bool TrackMixerRuntime::set_lane_parameter(size_t lane_index, unsigned int param_id,
                                           float value) noexcept {
  acquire_lanes();
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    prepare_lanes_from_snapshot(*lanes);
  }
  if (lane_index >= lane_count()) return false;
  LaneState& lane = lane_states_[lane_index];
  switch (param_id) {
    case kFaderDb:
      if (!std::isfinite(value)) return false;
      lane.fader_db.set_target(std::clamp(value, -120.0f, 24.0f));
      return true;
    case kPan:
      if (!std::isfinite(value)) return false;
      lane.pan.set_target(std::clamp(value, -1.0f, 1.0f));
      return true;
    case kWidth:
      return std::isfinite(value);
    default:
      return false;
  }
}

bool TrackMixerRuntime::set_lane_solo_mute(size_t lane_index, bool solo, bool mute) noexcept {
  acquire_lanes();
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    prepare_lanes_from_snapshot(*lanes);
  }
  if (lane_index >= lane_count()) return false;
  LaneState& lane = lane_states_[lane_index];
  lane.solo = solo;
  lane.mute = mute;
  return true;
}

bool TrackMixerRuntime::bind_track_strip(uint32_t track_id, mixing::ChannelStrip* strip) {
  if (track_id == 0) return false;
  acquire_lanes();
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    prepare_lanes_from_snapshot(*lanes);
  }
  for (LaneState& lane : lane_states_) {
    if (lane.track_id != track_id) continue;
    lane.strip = strip;
    if (strip && max_block_size_ > 0) {
      strip->prepare(sample_rate_, max_block_size_);
    }
    if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
      try {
        configure_lane_sends(*lanes);
      } catch (...) {
        return false;
      }
      recompute_lane_pdc(*lanes);
    }
    return true;
  }
  for (LaneState& lane : lane_states_) {
    if (lane.track_id != 0) continue;
    lane.track_id = track_id;
    lane.strip = strip;
    if (strip && max_block_size_ > 0) {
      strip->prepare(sample_rate_, max_block_size_);
    }
    if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
      try {
        configure_lane_sends(*lanes);
      } catch (...) {
        return false;
      }
      recompute_lane_pdc(*lanes);
    }
    return true;
  }
  return false;
}

bool TrackMixerRuntime::set_track_strip(uint32_t track_id, const mixing::api::Strip& spec) {
  if (track_id == 0) return false;
  std::unique_ptr<mixing::ChannelStrip> strip;
  try {
    strip = make_channel_strip_from_spec(spec);
  } catch (...) {
    return false;
  }
  if (!strip) return false;
  if (max_block_size_ > 0) {
    strip->prepare(sample_rate_, max_block_size_);
  }

  mixing::ChannelStrip* raw = strip.get();
  for (OwnedStrip& owned : owned_strips_) {
    if (owned.track_id == track_id) {
      owned.strip = std::move(strip);
      return bind_track_strip(track_id, raw);
    }
  }
  if (owned_strips_.size() >= kMaxTrackLanes) {
    return false;
  }
  owned_strips_.push_back(OwnedStrip{track_id, std::move(strip)});
  return bind_track_strip(track_id, raw);
}

bool TrackMixerRuntime::set_track_insert_bypassed(uint32_t track_id, unsigned int insert_index,
                                                  bool bypassed, bool reset_on_bypass) noexcept {
  if (track_id == 0) return false;
  acquire_lanes();
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    prepare_lanes_from_snapshot(*lanes);
  }
  for (LaneState& lane : lane_states_) {
    if (lane.track_id != track_id || lane.strip == nullptr) continue;
    return lane.strip->set_insert_bypassed(insert_index, bypassed, reset_on_bypass);
  }
  return false;
}

bool TrackMixerRuntime::resolve_track_insert_param(uint32_t track_id, unsigned int insert_index,
                                                   const std::string& key, size_t* out_lane_index,
                                                   unsigned int* out_param_id) noexcept {
  if (track_id == 0 || out_lane_index == nullptr || out_param_id == nullptr) return false;
  acquire_lanes();
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    prepare_lanes_from_snapshot(*lanes);
  }
  for (size_t i = 0; i < lane_states_.size(); ++i) {
    LaneState& lane = lane_states_[i];
    if (lane.track_id != track_id || lane.strip == nullptr) continue;
    const int id = lane.strip->insert_parameter_id_for_key(insert_index, key);
    if (id < 0) return false;
    *out_lane_index = i;
    *out_param_id = static_cast<unsigned int>(id);
    return true;
  }
  return false;
}

bool TrackMixerRuntime::apply_lane_insert_parameter(size_t lane_index, unsigned int insert_index,
                                                    unsigned int param_id, float value) noexcept {
  if (lane_index >= lane_states_.size()) return false;
  LaneState& lane = lane_states_[lane_index];
  if (lane.strip == nullptr) return false;
  return lane.strip->apply_insert_parameter(insert_index, param_id, value);
}

bool TrackMixerRuntime::set_track_eq_band(uint32_t track_id, size_t band_index,
                                          const sonare::mastering::eq::EqBand& band) noexcept {
  if (track_id == 0) return false;
  acquire_lanes();
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    prepare_lanes_from_snapshot(*lanes);
  }
  for (LaneState& lane : lane_states_) {
    if (lane.track_id != track_id || lane.strip == nullptr) continue;
    try {
      lane.strip->set_eq_band(band_index, band);
      if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
        recompute_lane_pdc(*lanes);
      }
      return true;
    } catch (...) {
      return false;
    }
  }
  return false;
}

mixing::ChannelStrip* TrackMixerRuntime::lane_strip_for_track(uint32_t track_id) noexcept {
  if (track_id == 0) return nullptr;
  acquire_lanes();
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    prepare_lanes_from_snapshot(*lanes);
  }
  for (LaneState& lane : lane_states_) {
    if (lane.track_id == track_id && lane.strip != nullptr) {
      return lane.strip;
    }
  }
  return nullptr;
}

bool TrackMixerRuntime::set_track_pan(uint32_t track_id, float pan) noexcept {
  if (!std::isfinite(pan)) return false;
  mixing::ChannelStrip* strip = lane_strip_for_track(track_id);
  if (!strip) return false;
  strip->set_pan(pan);
  return true;
}

bool TrackMixerRuntime::set_track_pan_law(uint32_t track_id, mixing::PanLaw law) noexcept {
  mixing::ChannelStrip* strip = lane_strip_for_track(track_id);
  if (!strip) return false;
  strip->set_pan_law(law);
  return true;
}

bool TrackMixerRuntime::set_track_pan_mode(uint32_t track_id, mixing::PanMode mode) noexcept {
  mixing::ChannelStrip* strip = lane_strip_for_track(track_id);
  if (!strip) return false;
  strip->set_pan_mode(mode);
  return true;
}

bool TrackMixerRuntime::set_track_dual_pan(uint32_t track_id, float left_pan,
                                           float right_pan) noexcept {
  if (!std::isfinite(left_pan) || !std::isfinite(right_pan)) return false;
  mixing::ChannelStrip* strip = lane_strip_for_track(track_id);
  if (!strip) return false;
  strip->set_dual_pan(left_pan, right_pan);
  return true;
}

bool TrackMixerRuntime::set_track_channel_delay_samples(uint32_t track_id,
                                                        int delay_samples) noexcept {
  if (delay_samples < 0) return false;
  mixing::ChannelStrip* strip = lane_strip_for_track(track_id);
  if (!strip) return false;
  try {
    strip->set_channel_delay_samples(delay_samples);
  } catch (...) {
    return false;
  }
  // Channel delay contributes to strip latency, so refresh PDC alignment.
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    recompute_lane_pdc(*lanes);
  }
  return true;
}

bool TrackMixerRuntime::set_bus_gain_db(uint32_t bus_id, float gain_db) noexcept {
  if (!std::isfinite(gain_db)) return false;
  BusState* state = bus_state_for(bus_id);
  if (!state) return false;
  state->gain_db.set_target(std::clamp(gain_db, -120.0f, 24.0f));
  return true;
}

bool TrackMixerRuntime::set_bus_gain_db_by_index(size_t bus_index, float gain_db) noexcept {
  if (!std::isfinite(gain_db) || bus_index >= bus_configs_.size()) return false;
  bus_states_[bus_index].gain_db.set_target(std::clamp(gain_db, -120.0f, 24.0f));
  return true;
}

bool TrackMixerRuntime::set_bus_strip(uint32_t bus_id, const mixing::api::Bus& bus) {
  BusState* state = bus_state_for(bus_id);
  if (!state) return false;
  auto fx = std::make_unique<mixing::FxBus>(static_cast<int>(kMaxTrackLanes));
  try {
    for (const auto& insert : bus.inserts) {
      auto processor =
          mastering::api::make_insert(insert.processor_name, insert.params_json, nullptr);
      if (!processor) return false;
      const bool spo = mastering::api::channel_policy(insert.processor_name) ==
                       mastering::api::ChannelPolicy::StereoPairOnly;
      fx->add_insert(std::move(processor), spo);
    }
  } catch (...) {
    return false;
  }
  if (max_block_size_ > 0) {
    fx->prepare(sample_rate_, max_block_size_);
  }
  state->bus = std::move(fx);
  return true;
}

void TrackMixerRuntime::prepare(double sample_rate, int max_block_size) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  max_block_size_ = std::max(max_block_size, 1);
  scratch_.assign(kMaxTrackLanes * kMaxLaneChannels * static_cast<size_t>(max_block_size_), 0.0f);
  bus_scratch_.assign(kMaxBusLanes * kMaxBusChannels * static_cast<size_t>(max_block_size_), 0.0f);
  key_scratch_.assign(kMaxTrackLanes * kMaxLaneChannels * static_cast<size_t>(max_block_size_),
                      0.0f);
  for (LaneState& lane : lane_states_) {
    lane.fader_db.prepare(sample_rate_, 5.0f);
    lane.pan.prepare(sample_rate_, 5.0f);
    lane.gate.prepare(sample_rate_, 10.0f);
    lane.fader_db.reset(0.0f);
    lane.pan.reset(0.0f);
    lane.gate.reset(1.0f);
    lane.solo = false;
    lane.mute = false;
    if (lane.strip) {
      lane.strip->prepare(sample_rate_, max_block_size_);
    }
  }
  for (BusState& bus : bus_states_) {
    bus.gain_db.prepare(sample_rate_, 5.0f);
    if (bus.bus) {
      bus.bus->prepare(sample_rate_, max_block_size_);
    }
  }
  for (mixing::AlignmentDelay& delay : lane_pdc_delays_) {
    delay.set_prepared_channels(kMaxLaneChannels);
    delay.prepare(sample_rate_, max_block_size_);
  }
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    prepare_lanes_from_snapshot(*lanes);
    try {
      configure_lane_sends(*lanes);
    } catch (...) {
    }
    recompute_lane_pdc(*lanes);
  }
}

void TrackMixerRuntime::process(float* const* channels, int num_channels, int num_samples) {
  (void)channels;
  (void)num_channels;
  (void)num_samples;
}

void TrackMixerRuntime::reset() {
  for (LaneState& lane : lane_states_) {
    lane.fader_db.reset(0.0f);
    lane.pan.reset(0.0f);
    lane.gate.reset(1.0f);
    lane.solo = false;
    lane.mute = false;
  }
  flush_pdc_delays();
}

void TrackMixerRuntime::settle_smoothers() noexcept {
  for (LaneState& lane : lane_states_) {
    lane.fader_db.reset(lane.fader_db.target());
    lane.pan.reset(lane.pan.target());
    lane.gate.reset(lane.gate.target());
    // Quiesce the lane's channel-strip gain stages too so the first rendered
    // block opens without an insert/fader ramp-in.
    if (lane.strip != nullptr) lane.strip->settle();
  }
  for (BusState& bus : bus_states_) {
    bus.gain_db.reset(bus.gain_db.target());
  }
}

void TrackMixerRuntime::flush_pdc_delays() noexcept {
  for (mixing::AlignmentDelay& delay : lane_pdc_delays_) {
    delay.reset();
  }
}

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
  prepare_lanes_from_snapshot(*lanes);
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
  prepare_lanes_from_snapshot(*lanes);
  const int master_channels = std::min(num_channels, kMaxBusChannels);
  for (size_t bus_index = 0; bus_index < bus_configs_.size(); ++bus_index) {
    clear_bus(bus_index, bus_render_channels(bus_index, master_channels), num_samples);
  }
  bool routed_through_lane = false;
  mix_source_into_lane(track_id, source, channels, num_channels, num_samples, routed_through_lane,
                       meter_tap, render_frame, scope_tap);
  if (routed_through_lane) {
    process_buses(channels, master_channels, num_samples, meter_tap, render_frame, scope_tap);
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
  prepare_lanes_from_snapshot(*lanes);
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

  const int render_channels = std::min(num_channels, kMaxLaneChannels);
  const int master_channels = std::min(num_channels, kMaxBusChannels);
  for (size_t lane_index = 0; lane_index < lanes->size(); ++lane_index) {
    if ((*lanes)[lane_index].track_id != track_id) continue;
    clear_lane(lane_index, render_channels, num_samples);
    for (int ch = 0; ch < render_channels; ++ch) {
      const float* src = source[static_cast<size_t>(ch)];
      float* lane = lane_channel(lane_index, ch);
      if (src) {
        std::copy(src, src + num_samples, lane);
      } else {
        std::fill(lane, lane + num_samples, 0.0f);
      }
    }
    process_lane_strip(lane_index, render_channels, num_samples, 0);
    mix_lane_sends(lane_index, render_channels, num_samples, 0);
    apply_lane_to_mix(lane_index, channels, render_channels, num_samples, any_lane_solo(*lanes),
                      meter_tap, render_frame, scope_tap, master_channels);
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
  const int master_channels = std::min(num_channels, kMaxBusChannels);
  process_buses(channels, master_channels, num_samples, meter_tap, render_frame, scope_tap);
}

bool TrackMixerRuntime::lane_config_valid(
    const std::vector<TrackLaneConfig>& lanes) const noexcept {
  if (lanes.size() > kMaxTrackLanes) return false;
  for (size_t i = 0; i < lanes.size(); ++i) {
    if (lanes[i].track_id == 0) return false;
    if (lanes[i].output_bus_id != 0 && bus_state_for(lanes[i].output_bus_id) == nullptr) {
      return false;
    }
    if (lanes[i].sends.size() > mixing::ChannelStrip::kMaxSends) return false;
    for (size_t send_index = 0; send_index < lanes[i].sends.size(); ++send_index) {
      const TrackLaneConfig::Send& send = lanes[i].sends[send_index];
      if (send.bus_id == 0 || !std::isfinite(send.level_db) || send.level_db < -120.0f ||
          send.level_db > 24.0f || bus_state_for(send.bus_id) == nullptr) {
        return false;
      }
      for (size_t other = send_index + 1; other < lanes[i].sends.size(); ++other) {
        if (send.bus_id == lanes[i].sends[other].bus_id) return false;
      }
    }
    for (size_t j = i + 1; j < lanes.size(); ++j) {
      if (lanes[i].track_id == lanes[j].track_id) return false;
    }
  }
  return true;
}

bool TrackMixerRuntime::bus_config_valid(const std::vector<TrackBusConfig>& buses) const noexcept {
  if (buses.size() > kMaxBusLanes) return false;
  for (size_t i = 0; i < buses.size(); ++i) {
    if (buses[i].bus_id == 0 || !std::isfinite(buses[i].gain_db) || buses[i].gain_db < -120.0f ||
        buses[i].gain_db > 24.0f) {
      return false;
    }
    for (size_t j = i + 1; j < buses.size(); ++j) {
      if (buses[i].bus_id == buses[j].bus_id) return false;
    }
  }
  return true;
}

mixing::ChannelStrip* TrackMixerRuntime::owned_strip_for(uint32_t track_id) noexcept {
  for (OwnedStrip& owned : owned_strips_) {
    if (owned.track_id == track_id) {
      return owned.strip.get();
    }
  }
  return nullptr;
}

mixing::ChannelStrip* TrackMixerRuntime::ensure_owned_strip_for(uint32_t track_id) {
  if (mixing::ChannelStrip* strip = owned_strip_for(track_id)) {
    return strip;
  }
  if (owned_strips_.size() >= kMaxTrackLanes) {
    return nullptr;
  }
  auto strip = std::make_unique<mixing::ChannelStrip>(
      mixing::ChannelStripConfig{0.0f, 0.0f, mixing::PanLaw::Linear0dB, 5.0f});
  if (max_block_size_ > 0) {
    strip->prepare(sample_rate_, max_block_size_);
  }
  mixing::ChannelStrip* raw = strip.get();
  owned_strips_.push_back(OwnedStrip{track_id, std::move(strip)});
  return raw;
}

TrackMixerRuntime::BusState* TrackMixerRuntime::bus_state_for(uint32_t bus_id) noexcept {
  for (BusState& state : bus_states_) {
    if (state.bus_id == bus_id) return &state;
  }
  return nullptr;
}

const TrackMixerRuntime::BusState* TrackMixerRuntime::bus_state_for(
    uint32_t bus_id) const noexcept {
  for (const BusState& state : bus_states_) {
    if (state.bus_id == bus_id) return &state;
  }
  return nullptr;
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

void TrackMixerRuntime::prepare_lanes_from_snapshot(
    const std::vector<TrackLaneConfig>& lanes) noexcept {
  const std::array<LaneState, kMaxTrackLanes> previous = lane_states_;
  std::array<LaneState, kMaxTrackLanes> next = lane_states_;
  std::array<bool, kMaxTrackLanes> used_previous{};

  const auto reset_state = [this](LaneState& lane, uint32_t track_id) noexcept {
    lane.track_id = track_id;
    lane.fader_db.prepare(sample_rate_, 5.0f);
    lane.pan.prepare(sample_rate_, 5.0f);
    lane.gate.prepare(sample_rate_, 10.0f);
    lane.fader_db.reset(0.0f);
    lane.pan.reset(0.0f);
    lane.gate.reset(1.0f);
    lane.solo = false;
    lane.mute = false;
    lane.strip = nullptr;
    lane.surround_gain.fill(0.0f);
    lane.surround_primed = false;
  };

  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    const uint32_t track_id = lanes[lane_index].track_id;
    const auto previous_state =
        std::find_if(previous.begin(), previous.end(),
                     [track_id](const LaneState& state) { return state.track_id == track_id; });
    if (previous_state != previous.end()) {
      next[lane_index] = *previous_state;
      used_previous[static_cast<size_t>(std::distance(previous.begin(), previous_state))] = true;
      continue;
    }

    reset_state(next[lane_index], track_id);
  }

  size_t inactive_index = lanes.size();
  for (size_t previous_index = 0; previous_index < previous.size(); ++previous_index) {
    if (used_previous[previous_index] || previous[previous_index].track_id == 0) continue;
    if (inactive_index >= next.size()) break;
    next[inactive_index++] = previous[previous_index];
  }

  for (; inactive_index < next.size(); ++inactive_index) {
    reset_state(next[inactive_index], 0);
  }

  lane_states_ = next;
}

void TrackMixerRuntime::recompute_lane_pdc(const std::vector<TrackLaneConfig>& lanes) noexcept {
  int max_latency_q8 = 0;
  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    const mixing::ChannelStrip* strip = lane_states_[lane_index].strip;
    if (strip != nullptr) {
      max_latency_q8 = std::max(max_latency_q8, strip->latency_samples_q8());
    }
  }

  latency_samples_q8_ = max_latency_q8;
  for (size_t lane_index = 0; lane_index < lane_pdc_delays_.size(); ++lane_index) {
    int delay_q8 = 0;
    if (lane_index < lanes.size()) {
      const mixing::ChannelStrip* strip = lane_states_[lane_index].strip;
      const int lane_latency_q8 = strip != nullptr ? strip->latency_samples_q8() : 0;
      delay_q8 = max_latency_q8 - lane_latency_q8;
    }
    lane_pdc_delays_[lane_index].set_delay_samples_q8(delay_q8);
  }
}

void TrackMixerRuntime::configure_lane_sends(const std::vector<TrackLaneConfig>& lanes) {
  for (size_t lane_index = 0; lane_index < lanes.size(); ++lane_index) {
    const TrackLaneConfig& config = lanes[lane_index];
    mixing::ChannelStrip* strip = lane_states_[lane_index].strip;
    if (!strip && !config.sends.empty()) {
      strip = ensure_owned_strip_for(config.track_id);
      lane_states_[lane_index].strip = strip;
    }
    if (!strip) continue;

    strip->clear_sends();
    for (const TrackLaneConfig::Send& send : config.sends) {
      if (bus_state_for(send.bus_id) == nullptr) {
        throw std::invalid_argument("track send references an unknown bus");
      }
      strip->add_send(
          mixing::SendConfig{send.enabled ? send.level_db : -120.0f, send.timing, 5.0f});
    }
  }
}

bool TrackMixerRuntime::set_lane_sidechain(uint32_t track_id, unsigned int insert_index,
                                           uint32_t source_track_id) noexcept {
  if (track_id == 0) return false;
  for (size_t i = 0; i < sidechain_binding_count_; ++i) {
    SidechainBinding& binding = sidechain_bindings_[i];
    if (binding.track_id != track_id || binding.insert_index != insert_index) continue;
    if (source_track_id == 0) {
      // Drop the binding; the strip's key state clears so the insert falls
      // back to its internal key until (and unless) another binding feeds it.
      const int lane_index = lane_index_for_track(track_id);
      if (lane_index >= 0 && lane_states_[static_cast<size_t>(lane_index)].strip) {
        lane_states_[static_cast<size_t>(lane_index)].strip->clear_insert_sidechains();
      }
      sidechain_bindings_[i] = sidechain_bindings_[sidechain_binding_count_ - 1];
      sidechain_bindings_[--sidechain_binding_count_] = SidechainBinding{};
    } else {
      binding.source_track_id = source_track_id;
    }
    return true;
  }
  if (source_track_id == 0) return true;
  if (sidechain_binding_count_ >= kMaxSidechainBindings) return false;
  sidechain_bindings_[sidechain_binding_count_++] =
      SidechainBinding{track_id, insert_index, source_track_id};
  return true;
}

int TrackMixerRuntime::lane_index_for_track(uint32_t track_id) const noexcept {
  if (track_id == 0) return -1;
  for (size_t i = 0; i < lane_states_.size(); ++i) {
    if (lane_states_[i].track_id == track_id) return static_cast<int>(i);
  }
  return -1;
}

void TrackMixerRuntime::deliver_lane_sidechains(size_t lane_index, int num_channels,
                                                int num_samples) noexcept {
  if (sidechain_binding_count_ == 0) return;
  LaneState& lane = lane_states_[lane_index];
  if (!lane.strip || lane.track_id == 0) return;
  std::array<const float*, kMaxLaneChannels> key{};
  for (size_t i = 0; i < sidechain_binding_count_; ++i) {
    const SidechainBinding& binding = sidechain_bindings_[i];
    if (binding.track_id != lane.track_id) continue;
    const int source_index = lane_index_for_track(binding.source_track_id);
    if (source_index < 0) continue;
    // The source lane's key snapshot holds its most recent post-strip,
    // pre-fader audio: the current block when the source renders before this
    // lane, the previous block otherwise (one block of key latency).
    for (int ch = 0; ch < num_channels && ch < kMaxLaneChannels; ++ch) {
      key[static_cast<size_t>(ch)] = key_channel(static_cast<size_t>(source_index), ch);
    }
    lane.strip->set_insert_sidechain(binding.insert_index, key.data(),
                                     std::min(num_channels, kMaxLaneChannels), num_samples);
  }
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

void TrackMixerRuntime::snapshot_sidechain_key(size_t lane_index, int num_channels,
                                               int num_samples) noexcept {
  if (sidechain_binding_count_ == 0) return;
  const uint32_t track_id = lane_states_[lane_index].track_id;
  if (track_id == 0) return;
  bool is_source = false;
  for (size_t i = 0; i < sidechain_binding_count_; ++i) {
    if (sidechain_bindings_[i].source_track_id == track_id) {
      is_source = true;
      break;
    }
  }
  if (!is_source) return;
  // Copy the post-strip output before the fader/gate/pan stage mutates the
  // lane buffer in place, so keyed inserts see the source's pre-fader signal.
  for (int ch = 0; ch < num_channels && ch < kMaxLaneChannels; ++ch) {
    const float* src = lane_channel(lane_index, ch);
    std::copy(src, src + num_samples, key_channel(lane_index, ch));
  }
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
    bus.bus->process(lane_channel_ptrs_.data(), bus_channels, num_samples);
    for (int i = 0; i < num_samples; ++i) {
      const float gain = std::pow(10.0f, bus.gain_db.process() / 20.0f);
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
    for (int i = 0; i < num_samples; ++i) {
      const float fader = std::pow(10.0f, lane.fader_db.process() / 20.0f);
      const float gate = lane.gate.process();
      const float pan = lane.pan.process();
      float left_gain = fader * gate;
      float right_gain = fader * gate;
      if (num_channels >= 2) {
        left_gain *= pan <= 0.0f ? 1.0f : 1.0f - pan;
        right_gain *= pan >= 0.0f ? 1.0f : 1.0f + pan;
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
  try {
    target = mixing::compute_surround_pan_gains(params, dest_layout);
  } catch (...) {
    return;
  }
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
    const float fader = std::pow(10.0f, lane.fader_db.process() / 20.0f);
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
