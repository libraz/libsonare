#include "engine/track_mixer.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <stdexcept>

#include "engine/meter_telemetry.h"
#include "mastering/api/insert_factory.h"

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
  for (const auto& insert : spec.inserts) {
    auto processor =
        mastering::api::make_insert(insert.processor_name, insert.params_json, nullptr);
    if (!processor) {
      return nullptr;
    }
    if (insert.slot == mixing::api::InsertSlot::PreFader) {
      strip->add_pre_insert(std::move(processor));
    } else {
      strip->add_post_insert(std::move(processor));
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
      fx->add_insert(std::move(processor));
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
  bus_scratch_.assign(kMaxBusLanes * kMaxLaneChannels * static_cast<size_t>(max_block_size_), 0.0f);
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

void TrackMixerRuntime::flush_pdc_delays() noexcept {
  for (mixing::AlignmentDelay& delay : lane_pdc_delays_) {
    delay.reset();
  }
}

bool TrackMixerRuntime::render_clips(ClipPlayer& player, float* const* channels, int num_channels,
                                     int num_samples, int64_t timeline_sample,
                                     MeterTelemetryTap* meter_tap, int64_t render_frame) noexcept {
  acquire_lanes();
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (!lanes || lanes->empty()) return false;
  if (!channels || num_channels <= 0 || num_samples <= 0) return true;
  if (num_channels > kMaxLaneChannels || num_samples > max_block_size_ || scratch_.empty()) {
    return false;
  }

  const int render_channels = std::min(num_channels, kMaxLaneChannels);
  prepare_lanes_from_snapshot(*lanes);
  for (size_t bus_index = 0; bus_index < bus_configs_.size(); ++bus_index) {
    clear_bus(bus_index, render_channels, num_samples);
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
                      render_frame);
  }
  process_buses(channels, render_channels, num_samples, meter_tap, render_frame);
  return true;
}

bool TrackMixerRuntime::mix_source(uint32_t track_id, float* const* source, float* const* channels,
                                   int num_channels, int num_samples, MeterTelemetryTap* meter_tap,
                                   int64_t render_frame) noexcept {
  acquire_lanes();
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (!lanes || lanes->empty()) return false;
  if (!source || !channels || num_channels <= 0 || num_samples <= 0) return true;
  if (num_channels > kMaxLaneChannels || num_samples > max_block_size_ || scratch_.empty()) {
    return false;
  }

  prepare_lanes_from_snapshot(*lanes);
  const int render_channels = std::min(num_channels, kMaxLaneChannels);
  for (size_t bus_index = 0; bus_index < bus_configs_.size(); ++bus_index) {
    clear_bus(bus_index, render_channels, num_samples);
  }
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
                      meter_tap, render_frame);
    process_buses(channels, render_channels, num_samples, meter_tap, render_frame);
    return true;
  }

  // Destination 0 and currently-unconfigured destinations stay on the main bus.
  add_source_to_mix(source, channels, render_channels, num_samples);
  return true;
}

bool TrackMixerRuntime::lane_config_valid(
    const std::vector<TrackLaneConfig>& lanes) const noexcept {
  if (lanes.size() > kMaxTrackLanes) return false;
  for (size_t i = 0; i < lanes.size(); ++i) {
    if (lanes[i].track_id == 0) return false;
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

float* TrackMixerRuntime::bus_channel(size_t bus_index, int channel) noexcept {
  const size_t bus_stride = static_cast<size_t>(kMaxLaneChannels) * max_block_size_;
  const size_t offset = bus_index * bus_stride + static_cast<size_t>(channel) * max_block_size_;
  return bus_scratch_.data() + offset;
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
      strip->add_send(mixing::SendConfig{send.enabled ? send.level_db : -120.0f,
                                         mixing::SendTiming::PostFader, 5.0f});
    }
  }
}

void TrackMixerRuntime::process_lane_strip(size_t lane_index, int num_channels, int num_samples,
                                           int64_t timeline_sample) noexcept {
  LaneState& lane = lane_states_[lane_index];
  for (int ch = 0; ch < num_channels; ++ch) {
    lane_channel_ptrs_[static_cast<size_t>(ch)] = lane_channel(lane_index, ch);
  }
  if (lane.strip) {
    lane.strip->process_at(lane_channel_ptrs_.data(), num_channels, num_samples, timeline_sample);
  }
  lane_pdc_delays_[lane_index].process(lane_channel_ptrs_.data(), num_channels, num_samples);
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

void TrackMixerRuntime::process_buses(float* const* channels, int num_channels, int num_samples,
                                      MeterTelemetryTap* meter_tap, int64_t render_frame) noexcept {
  for (size_t bus_index = 0; bus_index < bus_configs_.size(); ++bus_index) {
    BusState& bus = bus_states_[bus_index];
    if (bus.bus == nullptr) continue;
    for (int ch = 0; ch < num_channels; ++ch) {
      lane_channel_ptrs_[static_cast<size_t>(ch)] = bus_channel(bus_index, ch);
    }
    bus.bus->process(lane_channel_ptrs_.data(), num_channels, num_samples);
    for (int i = 0; i < num_samples; ++i) {
      const float gain = std::pow(10.0f, bus.gain_db.process() / 20.0f);
      for (int ch = 0; ch < num_channels; ++ch) {
        float* bus_channel_ptr = bus_channel(bus_index, ch);
        if (bus_channel_ptr) bus_channel_ptr[i] *= gain;
      }
    }
    if (meter_tap) {
      meter_tap->process_lightweight(lane_channel_ptrs_.data(), num_channels, num_samples,
                                     render_frame, bus_meter_target(bus_index));
    }
    for (int ch = 0; ch < num_channels; ++ch) {
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
                                          MeterTelemetryTap* meter_tap,
                                          int64_t render_frame) noexcept {
  LaneState& lane = lane_states_[lane_index];
  const bool audible = !lane.mute && (!any_solo || lane.solo);
  lane.gate.set_target(audible ? 1.0f : 0.0f);
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
    if (channels[0]) channels[0][i] += lane_channel(lane_index, 0)[i];
    if (num_channels >= 2 && channels[1]) {
      lane_channel(lane_index, 1)[i] *= right_gain;
      channels[1][i] += lane_channel(lane_index, 1)[i];
    }
  }
  if (meter_tap) {
    for (int ch = 0; ch < num_channels; ++ch) {
      lane_channel_ptrs_[static_cast<size_t>(ch)] = lane_channel(lane_index, ch);
    }
    meter_tap->process_lightweight(lane_channel_ptrs_.data(), num_channels, num_samples,
                                   render_frame, lane_meter_target(lane_index));
  }
}

}  // namespace sonare::engine
