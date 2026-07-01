#include "engine/track_mixer.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <stdexcept>

#include "engine/track_mixer_internal.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "util/db.h"

namespace sonare::engine {

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

namespace {

// True when two strip specs carry the same insert chain (count + each slot,
// processor name, and params). Only then can a strip be updated in place; any
// insert-topology or param change must rebuild the processor chain.
bool strip_inserts_equal(const mixing::api::Strip& a, const mixing::api::Strip& b) {
  if (a.inserts.size() != b.inserts.size()) return false;
  for (size_t i = 0; i < a.inserts.size(); ++i) {
    if (a.inserts[i].slot != b.inserts[i].slot ||
        a.inserts[i].processor_name != b.inserts[i].processor_name ||
        a.inserts[i].params_json != b.inserts[i].params_json) {
      return false;
    }
  }
  return true;
}

// Apply every smoothable / scalar strip parameter to an already-constructed
// strip via its setters (mirrors make_channel_strip_from_spec minus the insert
// chain). The fader/pan/trim setters retarget the strip's existing smoothers, so
// the change ramps from the current value instead of snapping.
void apply_strip_scalars(mixing::ChannelStrip& strip, const mixing::api::Strip& spec) {
  strip.set_fader_db(spec.fader_db);
  strip.set_pan(spec.pan);
  strip.set_pan_law(to_pan_law(spec.pan_law));
  strip.set_input_trim_db(spec.input_trim_db);
  strip.set_vca_offset_db(spec.vca_offset_db);
  strip.set_width(spec.width);
  strip.set_muted(spec.muted);
  strip.set_soloed(spec.soloed);
  strip.set_solo_safe(spec.solo_safe);
  strip.set_pan_mode(to_pan_mode(spec.pan_mode));
  strip.set_dual_pan(spec.dual_pan_left, spec.dual_pan_right);
  strip.set_polarity_invert(spec.polarity_invert_left, spec.polarity_invert_right);
  strip.set_channel_delay_samples(spec.channel_delay_samples);
  strip.set_surround_pan_params({spec.surround_pan.azimuth, spec.surround_pan.elevation,
                                 spec.surround_pan.divergence, spec.surround_pan.lfe,
                                 spec.surround_pan.distance});
}

}  // namespace

bool TrackMixerRuntime::set_track_lanes(std::vector<TrackLaneConfig> lanes) {
  if (!lane_config_valid(lanes)) return false;
  const auto snapshot = std::make_shared<const std::vector<TrackLaneConfig>>(std::move(lanes));
  if (!lanes_.publish(snapshot)) return false;
  clear_lane_insert_automations();
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
  clear_bus_insert_automations();
  for (size_t index = 0; index < bus_states_.size(); ++index) {
    BusState& state = bus_states_[index];
    if (index < bus_configs_.size()) {
      state.bus_id = bus_configs_[index].bus_id;
      state.gain_db.prepare(sample_rate_, 5.0f);
      state.gain_db.reset(bus_configs_[index].gain_db);
      // Re-prepare the trim/width smoothers for the current rate without
      // disturbing any value a prior set_bus_strip already applied.
      state.input_trim_gain.prepare(sample_rate_, 5.0f);
      if (max_block_size_ > 0) {
        state.width.prepare(sample_rate_, max_block_size_);
      }
      if (!state.bus) {
        state.bus = std::make_unique<mixing::FxBus>(static_cast<int>(kMaxTrackLanes));
      }
      if (max_block_size_ > 0) {
        state.bus->prepare(sample_rate_, max_block_size_);
      }
    } else {
      state.bus_id = 0;
      state.gain_db.reset(0.0f);
      state.input_trim_gain.reset(1.0f);
      state.width.set_width(1.0f);
      state.polarity_left.store(1.0f, std::memory_order_relaxed);
      state.polarity_right.store(1.0f, std::memory_order_relaxed);
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

bool TrackMixerRuntime::bind_track_strip(uint32_t track_id, mixing::ChannelStrip* strip) {
  if (track_id == 0) return false;
  acquire_lanes();
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    prepare_lanes_from_snapshot(*lanes);
  }
  for (LaneState& lane : lane_states_) {
    if (lane.track_id != track_id) continue;
    const size_t lane_index = static_cast<size_t>(&lane - lane_states_.data());
    clear_insert_automation_for_lane(lane_index);
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
    const size_t lane_index = static_cast<size_t>(&lane - lane_states_.data());
    clear_insert_automation_for_lane(lane_index);
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

  // In-place fast path: when a strip already exists for this track and only its
  // smoothable scalars changed (identical insert topology), retarget the existing
  // strip's parameters instead of rebuilding it. A rebuild constructs a fresh
  // strip whose fader/pan/trim smoothers settle straight to the new value, so a
  // live gain/pan edit would jump (an audible click); an in-place update keeps
  // the smoother state so the change ramps. PDC is recomputed in case the channel
  // delay changed; the strip pointer is unchanged so the lane binding stays valid.
  for (OwnedStrip& owned : owned_strips_) {
    if (owned.track_id == track_id && owned.strip && strip_inserts_equal(owned.spec, spec)) {
      apply_strip_scalars(*owned.strip, spec);
      owned.spec = spec;
      if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
        recompute_lane_pdc(*lanes);
      }
      return true;
    }
  }

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
      owned.spec = spec;
      return bind_track_strip(track_id, raw);
    }
  }
  if (owned_strips_.size() >= kMaxTrackLanes) {
    return false;
  }
  owned_strips_.push_back(OwnedStrip{track_id, std::move(strip), spec});
  return bind_track_strip(track_id, raw);
}

bool TrackMixerRuntime::set_bus_strip(uint32_t bus_id, const mixing::api::Bus& bus) {
  BusState* state = bus_state_for(bus_id);
  if (!state) return false;
  const size_t bus_index = static_cast<size_t>(state - bus_states_.data());
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
  state->input_trim_gain.set_target(db_to_linear(bus.input_trim_db));
  state->width.set_width(bus.width);
  state->polarity_left.store(bus.polarity_invert_left ? -1.0f : 1.0f, std::memory_order_relaxed);
  state->polarity_right.store(bus.polarity_invert_right ? -1.0f : 1.0f, std::memory_order_relaxed);
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (slot.active && slot.is_bus && slot.index == bus_index) {
      slot.active = false;
    }
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
    bus.input_trim_gain.prepare(sample_rate_, 5.0f);
    bus.width.prepare(sample_rate_, max_block_size_);
    if (bus.bus) {
      bus.bus->prepare(sample_rate_, max_block_size_);
    }
  }
  for (mixing::AlignmentDelay& delay : lane_pdc_delays_) {
    delay.set_prepared_channels(kMaxLaneChannels);
    delay.prepare(sample_rate_, max_block_size_);
  }
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    slot.smoother.prepare(sample_rate_, 5.0f);
    slot.smoother.reset(0.0f);
    slot.active = false;
    slot.is_bus = false;
    slot.index = 0;
    slot.insert_index = 0;
    slot.param_id = 0;
  }
  insert_automation_overflow_count_ = 0;
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
    bus.input_trim_gain.reset(bus.input_trim_gain.target());
    // Seed the width smoother from its target too; set_width() only stores the
    // target, so without this an offline pre-roll glides width from 1.0 over the
    // first audible block instead of opening at the configured width.
    bus.width.reset();
  }
  // Snap each automated insert parameter to its target and push it once, so an
  // offline pre-roll opens at the steady-state value (same determinism as the
  // fader/pan settle above).
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (!slot.active) continue;
    const float target = slot.smoother.target();
    slot.smoother.reset(target);
    if (slot.is_bus) {
      apply_bus_insert_parameter(slot.index, slot.insert_index, slot.param_id, target);
    } else {
      apply_lane_insert_parameter(slot.index, slot.insert_index, slot.param_id, target);
    }
  }
}

void TrackMixerRuntime::flush_pdc_delays() noexcept {
  for (mixing::AlignmentDelay& delay : lane_pdc_delays_) {
    delay.reset();
  }
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
  // Automation-seeded strip: no spec applied yet, so leave spec default (a later
  // set_track_strip will see a differing insert topology and rebuild).
  owned_strips_.push_back(OwnedStrip{track_id, std::move(strip), {}});
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
  applied_lane_snapshot_ = &lanes;
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

}  // namespace sonare::engine
