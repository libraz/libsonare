#include <algorithm>
#include <cmath>

#include "engine/track_mixer.h"
#include "util/constants.h"
#include "util/db.h"

namespace sonare::engine {

using sonare::constants::kFloorDb;

bool TrackMixerRuntime::set_lane_parameter(size_t lane_index, unsigned int param_id,
                                           float value) noexcept {
  acquire_lanes();
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    // Skip the full LaneState remap when the published config is unchanged: this
    // hot automation path only reads/writes lane_states_[lane_index], which is
    // already arranged for the current snapshot.
    if (lanes != applied_lane_snapshot_) prepare_lanes_from_snapshot(*lanes);
  }
  if (lane_index >= lane_count()) return false;
  LaneState& lane = lane_states_[lane_index];
  switch (param_id) {
    case kFaderDb:
      if (!std::isfinite(value)) return false;
      lane.fader_gain.set_target(db_to_linear(std::clamp(value, kFloorDb, 24.0f)));
      return true;
    case kPan:
      if (!std::isfinite(value)) return false;
      lane.pan.set_target(std::clamp(value, -1.0f, 1.0f));
      return true;
    // Width is deliberately not part of the arrangement typed-target
    // contract.  It remains available through the standalone mixer API, but
    // accepting it here would make an id that the compiler must never emit
    // look successfully automated while no lane state owns it.
    case kWidth:
      return false;
    default:
      return false;
  }
}

bool TrackMixerRuntime::set_lane_solo_mute(size_t lane_index, bool solo, bool mute) noexcept {
  acquire_lanes();
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    // Unchanged config -> lane_states_ is already arranged; skip the remap (see
    // set_lane_parameter).
    if (lanes != applied_lane_snapshot_) prepare_lanes_from_snapshot(*lanes);
  }
  if (lane_index >= lane_count()) return false;
  LaneState& lane = lane_states_[lane_index];
  lane.solo = solo;
  lane.mute = mute;
  return true;
}

bool TrackMixerRuntime::set_lane_monitor_mode(size_t lane_index, TrackMonitorMode mode) noexcept {
  acquire_lanes();
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    // The command carries the lane index, while the published snapshot may
    // have changed since enqueue. Arrange the audio state for that snapshot
    // before applying the transition; prepare_lanes_from_snapshot preserves
    // existing modes by track id when lanes reorder.
    if (lanes != applied_lane_snapshot_) prepare_lanes_from_snapshot(*lanes);
  }
  if (lane_index >= lane_count()) return false;
  switch (mode) {
    case TrackMonitorMode::kOff:
    case TrackMonitorMode::kPfl:
    case TrackMonitorMode::kAfl:
      lane_states_[lane_index].monitor_mode = mode;
      return true;
  }
  return false;
}

bool TrackMixerRuntime::monitor_active() const noexcept {
  const std::vector<TrackLaneConfig>* lanes = lanes_.current();
  if (lanes == nullptr) return false;
  const size_t count = std::min(lanes->size(), lane_states_.size());
  for (size_t lane_index = 0; lane_index < count; ++lane_index) {
    if (lane_states_[lane_index].monitor_mode != TrackMonitorMode::kOff) return true;
  }
  return false;
}

bool TrackMixerRuntime::set_track_insert_bypassed(uint32_t track_id, unsigned int insert_index,
                                                  bool bypassed, bool reset_on_bypass) noexcept {
  if (track_id == 0) return false;
  // Resolve the strip from the control-thread binding table: never acquire the
  // lane snapshot (that is the audio thread's single-consumer side) and never
  // touch lane_states_ (rewritten by the audio thread every block).
  mixing::ChannelStrip* strip = bound_strip_for(track_id);
  if (strip == nullptr) return false;
  return strip->set_insert_bypassed(insert_index, bypassed, reset_on_bypass);
}

bool TrackMixerRuntime::set_bus_insert_bypassed(uint32_t bus_id, unsigned int insert_index,
                                                bool bypassed, bool reset_on_bypass) noexcept {
  if (bus_id == 0) return false;
  for (size_t i = 0; i < bus_configs_.size() && i < bus_states_.size(); ++i) {
    if (bus_states_[i].bus_id != bus_id || bus_states_[i].bus == nullptr) continue;
    return bus_states_[i].bus->set_insert_bypassed(insert_index, bypassed, reset_on_bypass);
  }
  return false;
}

bool TrackMixerRuntime::resolve_track_insert_param(uint32_t track_id, unsigned int insert_index,
                                                   const std::string& key, size_t* out_lane_index,
                                                   unsigned int* out_param_id) noexcept {
  if (track_id == 0 || out_lane_index == nullptr || out_param_id == nullptr) return false;
  // Read-only resolution, safe during playback: the lane index is the track's
  // position in the control-side snapshot (prepare_lanes_from_snapshot arranges
  // lane_states_[i] for lanes[i], so the index the audio thread applies against
  // matches), and the strip comes from the control-thread binding table.
  // insert_parameter_id_for_key reads the processor's static descriptor table
  // only. No acquire_lanes(), no lane_states_ access.
  const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get();
  if (lanes == nullptr) return false;
  for (size_t i = 0; i < lanes->size(); ++i) {
    if ((*lanes)[i].track_id != track_id) continue;
    mixing::ChannelStrip* strip = bound_strip_for(track_id);
    if (strip == nullptr) return false;
    const int id = strip->insert_parameter_id_for_key(insert_index, key);
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

bool TrackMixerRuntime::resolve_bus_insert_param(uint32_t bus_id, unsigned int insert_index,
                                                 const std::string& key, size_t* out_bus_index,
                                                 unsigned int* out_param_id) noexcept {
  if (bus_id == 0 || out_bus_index == nullptr || out_param_id == nullptr) return false;
  for (size_t i = 0; i < bus_configs_.size() && i < bus_states_.size(); ++i) {
    if (bus_states_[i].bus_id != bus_id || bus_states_[i].bus == nullptr) continue;
    const int id = bus_states_[i].bus->insert_parameter_id_for_key(insert_index, key);
    if (id < 0) return false;
    *out_bus_index = i;
    *out_param_id = static_cast<unsigned int>(id);
    return true;
  }
  return false;
}

bool TrackMixerRuntime::apply_bus_insert_parameter(size_t bus_index, unsigned int insert_index,
                                                   unsigned int param_id, float value) noexcept {
  if (bus_index >= bus_states_.size()) return false;
  mixing::FxBus* bus = bus_states_[bus_index].bus.get();
  if (bus == nullptr) return false;
  return bus->apply_insert_parameter(insert_index, param_id, value);
}

TrackMixerRuntime::InsertAutoSlot* TrackMixerRuntime::find_or_claim_insert_slot(
    bool is_bus, size_t index, unsigned int insert_index, unsigned int param_id,
    float value) noexcept {
  InsertAutoSlot* free_slot = nullptr;
  InsertAutoSlot* settled_match = nullptr;
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (slot.assigned && slot.is_bus == is_bus && slot.index == index &&
        slot.insert_index == insert_index && slot.param_id == param_id) {
      if (slot.active) return &slot;
      settled_match = &slot;
    }
    if (!slot.active && free_slot == nullptr) {
      free_slot = &slot;
    }
  }
  if (settled_match != nullptr) {
    settled_match->active = true;
    return settled_match;
  }
  if (free_slot == nullptr) {
    ++insert_automation_overflow_count_;
    return nullptr;
  }
  free_slot->active = true;
  free_slot->assigned = true;
  free_slot->is_bus = is_bus;
  free_slot->index = index;
  free_slot->insert_index = insert_index;
  free_slot->param_id = param_id;
  // Snap to the first observed value so the smoother does not glide up from the
  // reset 0 the first time this target is automated.
  free_slot->smoother.reset(value);
  return free_slot;
}

bool TrackMixerRuntime::route_lane_insert_param_smoothed(size_t lane_index,
                                                         unsigned int insert_index,
                                                         unsigned int param_id,
                                                         float value) noexcept {
  if (!std::isfinite(value)) return false;
  if (lane_index >= lane_states_.size() || lane_states_[lane_index].strip == nullptr) {
    return false;
  }
  InsertAutoSlot* slot =
      find_or_claim_insert_slot(false, lane_index, insert_index, param_id, value);
  if (slot == nullptr) return false;
  slot->smoother.set_target(value);
  return true;
}

bool TrackMixerRuntime::route_bus_insert_param_smoothed(size_t bus_index, unsigned int insert_index,
                                                        unsigned int param_id,
                                                        float value) noexcept {
  if (!std::isfinite(value)) return false;
  if (bus_index >= bus_states_.size() || bus_states_[bus_index].bus == nullptr) {
    return false;
  }
  InsertAutoSlot* slot = find_or_claim_insert_slot(true, bus_index, insert_index, param_id, value);
  if (slot == nullptr) return false;
  slot->smoother.set_target(value);
  return true;
}

void TrackMixerRuntime::advance_insert_automations(int num_samples) noexcept {
  if (num_samples <= 0) return;
  constexpr float kSettleEpsilon = 1.0e-6f;
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (!slot.active) continue;
    const float value = slot.smoother.advance(num_samples);
    if (slot.is_bus) {
      apply_bus_insert_parameter(slot.index, slot.insert_index, slot.param_id, value);
    } else {
      apply_lane_insert_parameter(slot.index, slot.insert_index, slot.param_id, value);
    }
    if (std::abs(slot.smoother.target() - value) <= kSettleEpsilon) {
      slot.smoother.reset(slot.smoother.target());
      slot.active = false;
    }
  }
}

void TrackMixerRuntime::clear_insert_automation_for_lane(size_t lane_index) noexcept {
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (slot.assigned && !slot.is_bus && slot.index == lane_index) {
      slot.active = false;
      slot.assigned = false;
    }
  }
}

void TrackMixerRuntime::clear_lane_insert_automations() noexcept {
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (slot.assigned && !slot.is_bus) {
      slot.active = false;
      slot.assigned = false;
    }
  }
}

void TrackMixerRuntime::clear_bus_insert_automations() noexcept {
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (slot.assigned && slot.is_bus) {
      slot.active = false;
      slot.assigned = false;
    }
  }
}

void TrackMixerRuntime::clear_insert_automations() noexcept {
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    slot.active = false;
    slot.assigned = false;
  }
}

bool TrackMixerRuntime::set_track_eq_band(uint32_t track_id, size_t band_index,
                                          const sonare::mastering::eq::EqBand& band) noexcept {
  if (track_id == 0) return false;
  mixing::ChannelStrip* strip = bound_strip_for(track_id);
  if (strip == nullptr) return false;
  try {
    strip->set_eq_band(band_index, band);
  } catch (...) {
    return false;
  }
  // An EQ band change can shift the strip's latency, so refresh the PDC
  // alignment. recompute_lane_pdc reads the lane strips' latency through
  // lane_states_, which is why this setter keeps the control-thread contract
  // (not concurrent with process()); the strip resolution above is read-only
  // regardless.
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
    recompute_lane_pdc(*lanes);
  }
  return true;
}

mixing::ChannelStrip* TrackMixerRuntime::lane_strip_for_track(uint32_t track_id) noexcept {
  if (track_id == 0) return nullptr;
  return bound_strip_for(track_id);
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
  // Channel delay contributes to strip latency, so refresh PDC alignment. Use
  // the control-side snapshot: this is a control-thread structural change (not
  // concurrent with process()), and lanes_.current() belongs to the audio
  // thread.
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
    recompute_lane_pdc(*lanes);
  }
  return true;
}

bool TrackMixerRuntime::set_bus_gain_db(uint32_t bus_id, float gain_db) noexcept {
  if (!std::isfinite(gain_db)) return false;
  BusState* state = bus_state_for(bus_id);
  if (!state) return false;
  state->gain.set_target(db_to_linear(std::clamp(gain_db, kFloorDb, 24.0f)));
  return true;
}

bool TrackMixerRuntime::set_bus_gain_db_by_index(size_t bus_index, float gain_db) noexcept {
  if (!std::isfinite(gain_db) || bus_index >= bus_configs_.size()) return false;
  bus_states_[bus_index].gain.set_target(db_to_linear(std::clamp(gain_db, kFloorDb, 24.0f)));
  return true;
}

bool TrackMixerRuntime::set_lane_sidechain(uint32_t track_id, unsigned int insert_index,
                                           uint32_t source_track_id) noexcept {
  if (track_id == 0) return false;
  // Control-thread single writer: work against a local count, then publish the
  // new value with release only after the binding array is settled.
  const size_t count = sidechain_binding_count_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < count; ++i) {
    SidechainBinding& binding = sidechain_bindings_[i];
    if (binding.track_id.load(std::memory_order_acquire) != track_id ||
        binding.insert_index.load(std::memory_order_acquire) != insert_index) {
      continue;
    }
    if (source_track_id == 0) {
      // Drop the binding. The audio thread clears stale keys before delivering
      // the current table, so this control path never touches lane_states_.
      SidechainBinding& last = sidechain_bindings_[count - 1];
      binding.track_id.store(last.track_id.load(std::memory_order_acquire),
                             std::memory_order_release);
      binding.insert_index.store(last.insert_index.load(std::memory_order_acquire),
                                 std::memory_order_release);
      binding.source_track_id.store(last.source_track_id.load(std::memory_order_acquire),
                                    std::memory_order_release);
      last.track_id.store(0, std::memory_order_release);
      last.insert_index.store(0, std::memory_order_release);
      last.source_track_id.store(0, std::memory_order_release);
      sidechain_binding_count_.store(count - 1, std::memory_order_release);
    } else {
      binding.source_track_id.store(source_track_id, std::memory_order_release);
    }
    return true;
  }
  if (source_track_id == 0) return true;
  if (count >= kMaxSidechainBindings) return false;
  SidechainBinding& binding = sidechain_bindings_[count];
  binding.track_id.store(track_id, std::memory_order_relaxed);
  binding.insert_index.store(insert_index, std::memory_order_relaxed);
  binding.source_track_id.store(source_track_id, std::memory_order_relaxed);
  sidechain_binding_count_.store(count + 1, std::memory_order_release);
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
  LaneState& lane = lane_states_[lane_index];
  if (!lane.strip || lane.track_id == 0) return;
  // Clear any binding removed by the control thread without touching the
  // audio-owned lane state there. Current bindings are restored below.
  lane.strip->clear_insert_sidechains();
  const size_t count = sidechain_binding_count_.load(std::memory_order_acquire);
  if (count == 0) return;
  std::array<const float*, kMaxLaneChannels> key{};
  for (size_t i = 0; i < count; ++i) {
    const SidechainBinding& binding = sidechain_bindings_[i];
    if (binding.track_id.load(std::memory_order_acquire) != lane.track_id) continue;
    const int source_index =
        lane_index_for_track(binding.source_track_id.load(std::memory_order_acquire));
    if (source_index < 0) continue;
    // The source lane's key snapshot holds its most recent post-strip,
    // pre-fader audio: the current block when the source renders before this
    // lane, the previous block otherwise (one block of key latency).
    for (int ch = 0; ch < num_channels && ch < kMaxLaneChannels; ++ch) {
      key[static_cast<size_t>(ch)] = key_channel(static_cast<size_t>(source_index), ch);
    }
    lane.strip->set_insert_sidechain(binding.insert_index.load(std::memory_order_acquire),
                                     key.data(), std::min(num_channels, kMaxLaneChannels),
                                     num_samples);
  }
}

void TrackMixerRuntime::snapshot_sidechain_key(size_t lane_index, int num_channels,
                                               int num_samples) noexcept {
  const size_t count = sidechain_binding_count_.load(std::memory_order_acquire);
  if (count == 0) return;
  const uint32_t track_id = lane_states_[lane_index].track_id;
  if (track_id == 0) return;
  bool is_source = false;
  for (size_t i = 0; i < count; ++i) {
    if (sidechain_bindings_[i].source_track_id.load(std::memory_order_acquire) == track_id) {
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

}  // namespace sonare::engine
