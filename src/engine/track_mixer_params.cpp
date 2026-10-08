#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iterator>

#include "engine/track_mixer.h"
#include "util/constants.h"
#include "util/db.h"

namespace sonare::engine {

using sonare::constants::kFloorDb;

namespace {

template <typename Bus>
auto constructed_bus_parameter_value(const Bus& bus, unsigned int insert_index,
                                     unsigned int param_id, float* out_value, int) noexcept
    -> decltype(bus.constructed_insert_parameter_value(insert_index, param_id, out_value)) {
  return bus.constructed_insert_parameter_value(insert_index, param_id, out_value);
}

template <typename Bus>
bool constructed_bus_parameter_value(const Bus&, unsigned int, unsigned int, float*,
                                     long) noexcept {
  return false;
}

using SidechainTable = TrackMixerRuntime::SidechainTable;
using SidechainBinding = TrackMixerRuntime::SidechainBinding;

// @p table with @p next committed the way the setters do: source_id 0 removes
// entry @p found (swap with the last), a found entry takes the new source, and
// otherwise @p next is appended. The caller has checked the entry exists or fits.
SidechainTable with_binding(const SidechainTable& table, int found,
                            const SidechainBinding& next) noexcept {
  SidechainTable candidate = table;
  if (next.source_id == 0) {
    const size_t last = candidate.count - 1;
    candidate.bindings[static_cast<size_t>(found)] = candidate.bindings[last];
    candidate.bindings[last] = SidechainBinding{};
    candidate.count = last;
  } else if (found >= 0) {
    SidechainBinding& binding = candidate.bindings[static_cast<size_t>(found)];
    binding.source_kind = next.source_kind;
    binding.source_id = next.source_id;
  } else {
    candidate.bindings[candidate.count++] = next;
  }
  return candidate;
}

}  // namespace

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
      lane.fader_gain.set_target(db_to_linear(std::clamp(value, kFloorDb, kMaxGainDb)));
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

bool TrackMixerRuntime::set_track_parameter(uint32_t track_id, unsigned int param_id,
                                            float value) noexcept {
  acquire_lanes();
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    if (lanes != applied_lane_snapshot_) prepare_lanes_from_snapshot(*lanes);
  }
  const int lane_index = lane_index_for_track(track_id);
  if (lane_index < 0) return false;
  return set_lane_parameter(static_cast<size_t>(lane_index), param_id, value);
}

bool TrackMixerRuntime::restore_track_parameter(uint32_t track_id, unsigned int param_id) noexcept {
  // The rest values prepare() and reset() seed: unity gain, centre pan.
  if (param_id != kFaderDb && param_id != kPan) return false;
  return set_track_parameter(track_id, param_id, 0.0f);
}

bool TrackMixerRuntime::set_lane_solo_mute(size_t lane_index, bool solo, bool mute) noexcept {
  acquire_lanes();
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    // Unchanged config -> lane_states_ is already arranged; skip the remap (see
    // set_lane_parameter).
    if (lanes != applied_lane_snapshot_) prepare_lanes_from_snapshot(*lanes);
  }
  if (lane_index >= lane_count()) return false;
  // Lane state, not the strip's hard flags: the smoothed lane gate scales the master/bus
  // sum and every send (pre-fader included). PFL and the sidechain key are taken ahead of
  // it; the strip's hard mute flags would silence those too.
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

bool TrackMixerRuntime::apply_track_insert_param_by_name_now(uint32_t track_id,
                                                             unsigned int insert_index,
                                                             const std::string& key,
                                                             float value) noexcept {
  if (!std::isfinite(value)) return false;
  size_t lane_index = 0;
  unsigned int param_id = 0;
  if (!resolve_track_insert_param(track_id, insert_index, key, &lane_index, &param_id)) {
    return false;
  }
  // A direct edit may arrive before the first render has acquired a newly
  // published lane snapshot (offline mirror and worklet both use this path).
  // Arrange the audio-side lane state before claiming the smoother slot.
  acquire_lanes();
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    if (lanes != applied_lane_snapshot_) prepare_lanes_from_snapshot(*lanes);
  }
  return route_lane_insert_param_smoothed(lane_index, insert_index, param_id, value);
}

bool TrackMixerRuntime::restore_track_insert_param_by_name(uint32_t track_id,
                                                           unsigned int insert_index,
                                                           const std::string& key,
                                                           float value) noexcept {
  if (!std::isfinite(value)) return false;
  size_t lane_index = 0;
  unsigned int param_id = 0;
  if (!resolve_track_insert_param(track_id, insert_index, key, &lane_index, &param_id)) {
    return false;
  }
  acquire_lanes();
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.current()) {
    if (lanes != applied_lane_snapshot_) prepare_lanes_from_snapshot(*lanes);
  }
  if (!apply_lane_insert_parameter(lane_index, insert_index, param_id, value)) return false;
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (slot.assigned && !slot.is_bus && slot.index == lane_index &&
        slot.insert_index == insert_index && slot.param_id == param_id) {
      slot.active = false;
      slot.assigned = false;
    }
  }
  return true;
}

bool TrackMixerRuntime::track_insert_constructed_parameter_value(uint32_t track_id,
                                                                 unsigned int insert_index,
                                                                 unsigned int param_id,
                                                                 float* out_value) const noexcept {
  if (out_value == nullptr) return false;
  const mixing::ChannelStrip* strip = bound_strip_for(track_id);
  return strip != nullptr &&
         strip->constructed_insert_parameter_value(insert_index, param_id, out_value);
}

bool TrackMixerRuntime::lane_insert_constructed_parameter_value(uint32_t track_id,
                                                                unsigned int insert_index,
                                                                unsigned int param_id,
                                                                float* out_value) const noexcept {
  const int lane_index = lane_index_for_track(track_id);
  if (out_value == nullptr || lane_index < 0) return false;
  // release_parameter_base() runs from the audio thread. Read the already
  // prepared lane state instead of the control-side binding/snapshot tables.
  const LaneState& lane = lane_states_[static_cast<size_t>(lane_index)];
  return lane.strip != nullptr &&
         lane.strip->constructed_insert_parameter_value(insert_index, param_id, out_value);
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

bool TrackMixerRuntime::apply_bus_insert_param_by_name_now(uint32_t bus_id,
                                                           unsigned int insert_index,
                                                           const std::string& key,
                                                           float value) noexcept {
  if (!std::isfinite(value)) return false;
  size_t bus_index = 0;
  unsigned int param_id = 0;
  if (!resolve_bus_insert_param(bus_id, insert_index, key, &bus_index, &param_id)) return false;
  return route_bus_insert_param_smoothed_by_id(bus_id, insert_index, param_id, value);
}

bool TrackMixerRuntime::restore_bus_insert_param_by_name(uint32_t bus_id, unsigned int insert_index,
                                                         const std::string& key,
                                                         float value) noexcept {
  if (!std::isfinite(value)) return false;
  size_t bus_index = 0;
  unsigned int param_id = 0;
  if (!resolve_bus_insert_param(bus_id, insert_index, key, &bus_index, &param_id)) return false;
  if (!apply_bus_insert_parameter(bus_index, insert_index, param_id, value)) return false;
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (slot.assigned && slot.is_bus && slot.bus_id == bus_id &&
        slot.insert_index == insert_index && slot.param_id == param_id) {
      slot.active = false;
      slot.assigned = false;
    }
  }
  return true;
}

bool TrackMixerRuntime::bus_insert_constructed_parameter_value(uint32_t bus_id,
                                                               unsigned int insert_index,
                                                               unsigned int param_id,
                                                               float* out_value) const noexcept {
  if (out_value == nullptr) return false;
  const BusState* state = bus_state_for(bus_id);
  return state != nullptr && state->bus != nullptr &&
         constructed_bus_parameter_value(state->bus->bus(), insert_index, param_id, out_value, 0);
}

bool TrackMixerRuntime::apply_bus_insert_parameter(size_t bus_index, unsigned int insert_index,
                                                   unsigned int param_id, float value) noexcept {
  if (bus_index >= bus_states_.size()) return false;
  mixing::FxBus* bus = bus_states_[bus_index].bus.get();
  if (bus == nullptr) return false;
  return bus->apply_insert_parameter(insert_index, param_id, value);
}

TrackMixerRuntime::InsertAutoSlot* TrackMixerRuntime::find_or_claim_insert_slot(
    bool is_bus, size_t index, uint32_t bus_id, unsigned int insert_index, unsigned int param_id,
    float value) noexcept {
  InsertAutoSlot* free_slot = nullptr;
  InsertAutoSlot* settled_match = nullptr;
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (slot.assigned && slot.is_bus == is_bus &&
        (is_bus ? slot.bus_id == bus_id : slot.index == index) &&
        slot.insert_index == insert_index && slot.param_id == param_id) {
      if (slot.active) return &slot;
      settled_match = &slot;
    }
    if (!slot.active && free_slot == nullptr) {
      free_slot = &slot;
    }
  }
  if (settled_match != nullptr) free_slot = settled_match;
  if (free_slot == nullptr) {
    ++insert_automation_overflow_count_;
    return nullptr;
  }
  free_slot->active = true;
  free_slot->assigned = true;
  free_slot->is_bus = is_bus;
  free_slot->index = index;
  free_slot->bus_id = bus_id;
  free_slot->insert_index = insert_index;
  free_slot->param_id = param_id;
  // A newly claimed or settled slot starts from the last successful processor
  // value, including scene restores. A retained custom insert without metadata
  // keeps its settled smoother value; a newly claimed one falls back to target.
  float baseline = settled_match != nullptr ? settled_match->smoother.current() : value;
  bool captured = false;
  if (is_bus && index < bus_states_.size() && bus_states_[index].bus != nullptr) {
    const auto& bus = bus_states_[index].bus->bus();
    captured = bus.last_applied_insert_parameter_value(insert_index, param_id, &baseline) ||
               constructed_bus_parameter_value(bus, insert_index, param_id, &baseline, 0);
  } else if (!is_bus && index < lane_states_.size() && lane_states_[index].strip != nullptr) {
    const auto* strip = lane_states_[index].strip;
    captured = strip->last_applied_insert_parameter_value(insert_index, param_id, &baseline) ||
               strip->constructed_insert_parameter_value(insert_index, param_id, &baseline);
  }
  free_slot->smoother.reset(
      (captured || settled_match != nullptr) && std::isfinite(baseline) ? baseline : value);
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
      find_or_claim_insert_slot(false, lane_index, 0, insert_index, param_id, value);
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
  const uint32_t bus_id = bus_states_[bus_index].bus_id;
  InsertAutoSlot* slot =
      find_or_claim_insert_slot(true, bus_index, bus_id, insert_index, param_id, value);
  if (slot == nullptr) return false;
  slot->smoother.set_target(value);
  return true;
}

bool TrackMixerRuntime::route_bus_insert_param_smoothed_by_id(uint32_t bus_id,
                                                              unsigned int insert_index,
                                                              unsigned int param_id,
                                                              float value) noexcept {
  const int bus_index = configured_bus_index(bus_id);
  if (bus_index < 0) return false;
  return route_bus_insert_param_smoothed(static_cast<size_t>(bus_index), insert_index, param_id,
                                         value);
}

bool TrackMixerRuntime::route_track_insert_param_smoothed_by_id(uint32_t track_id,
                                                                unsigned int insert_index,
                                                                unsigned int param_id,
                                                                float value) noexcept {
  const int lane_index = lane_index_for_track(track_id);
  if (lane_index < 0) return false;
  return route_lane_insert_param_smoothed(static_cast<size_t>(lane_index), insert_index, param_id,
                                          value);
}

void TrackMixerRuntime::advance_insert_automations(int num_samples) noexcept {
  if (num_samples <= 0) return;
  constexpr float kSettleEpsilon = 1.0e-6f;
  for (InsertAutoSlot& slot : insert_auto_slots_) {
    if (!slot.active) continue;
    const float value = slot.smoother.advance(num_samples);
    if (slot.is_bus) {
      const int bus_index = configured_bus_index(slot.bus_id);
      if (bus_index < 0) {
        slot.active = false;
        slot.assigned = false;
        continue;
      }
      slot.index = static_cast<size_t>(bus_index);
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
  // An externally bound strip has no retained spec; only an owned one records the band.
  mixing::api::StripEq* spec_eq = nullptr;
  for (OwnedStrip& owned : owned_strips_) {
    if (owned.track_id == track_id && owned.strip.get() == strip) {
      spec_eq = &owned.spec.eq;
      break;
    }
  }
  sonare::mastering::eq::EqBand previous_band;
  sonare::mastering::eq::EqBand previous_spec_band;
  const size_t previous_spec_size = spec_eq != nullptr ? spec_eq->bands.size() : 0;
  const bool had_spec_band = previous_spec_size > band_index;
  try {
    previous_band = strip->eq().band(band_index);
    if (had_spec_band) previous_spec_band = spec_eq->bands[band_index];
  } catch (...) {
    return false;
  }
  // Puts the stage and the retained spec back exactly as they were before the call.
  const auto restore = [&]() noexcept {
    try {
      strip->set_eq_band(band_index, previous_band);
    } catch (...) {
    }
    if (spec_eq == nullptr) return;
    if (had_spec_band) {
      spec_eq->bands[band_index] = previous_spec_band;
    } else {
      spec_eq->bands.resize(previous_spec_size);
    }
  };
  try {
    strip->set_eq_band(band_index, band);
    if (spec_eq != nullptr) store_eq_band(*spec_eq, band_index, band);
  } catch (...) {
    restore();
    return false;
  }
  // An EQ band change can shift the strip's latency, so refresh the PDC
  // alignment. recompute_lane_pdc reads the lane strips' latency through
  // lane_states_, which is why this setter keeps the control-thread contract
  // (not concurrent with process()); the strip resolution above is read-only
  // regardless. It is noexcept and reports an allocation failure as false, in
  // which case the band is taken back so a false return means nothing changed.
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
    if (!recompute_lane_pdc(*lanes)) {
      restore();
      recompute_lane_pdc(*lanes);
      return false;
    }
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

bool TrackMixerRuntime::set_track_surround_pan(uint32_t track_id,
                                               const mixing::SurroundPanParams& params) noexcept {
  if (!std::isfinite(params.azimuth) || !std::isfinite(params.elevation) ||
      !std::isfinite(params.divergence) || !std::isfinite(params.lfe) ||
      !std::isfinite(params.distance)) {
    return false;
  }
  mixing::ChannelStrip* strip = lane_strip_for_track(track_id);
  if (!strip) return false;
  mixing::SurroundPanParams stored = params;
  // Same "keep default" sentinel as sonare_strip_set_surround_pan, applied here so
  // the WASM facade, which calls this method directly, agrees with the C ABI.
  if (stored.distance <= 0.0f) stored.distance = 1.0f;
  strip->set_surround_pan_params(stored);
  return true;
}

bool TrackMixerRuntime::set_track_channel_delay_samples(uint32_t track_id,
                                                        int delay_samples) noexcept {
  // Both bounds are enforced here rather than only at the C ABI. The WASM
  // facade reaches this method directly instead of going through the C entry
  // point, so an upper bound checked only there would leave one surface
  // silently substituting kMaxAlignmentDelaySamples for the value the caller
  // asked for -- a samples/milliseconds mix-up would "succeed" at four seconds
  // -- while the other three report InvalidParameter for the same request.
  // With the rejection in the core, all four surfaces either apply exactly the
  // requested delay or report a failure.
  if (delay_samples < 0 || delay_samples > mixing::kMaxAlignmentDelaySamples) return false;
  mixing::ChannelStrip* strip = lane_strip_for_track(track_id);
  if (!strip) return false;
  // The channel delay moves this lane relative to the others and is not strip
  // latency, so PDC and the reported latency are left alone.
  try {
    strip->set_channel_delay_samples(delay_samples);
  } catch (...) {
    return false;
  }
  return true;
}

bool TrackMixerRuntime::set_bus_pan(uint32_t bus_id, float pan) noexcept {
  if (!std::isfinite(pan)) return false;
  BusState* state = pannable_bus_state_for(bus_id);
  if (!state) return false;
  const float clamped = mixing::clamp_pan(pan);
  state->panner.set_pan(clamped);
  state->spec.pan = clamped;
  return true;
}

bool TrackMixerRuntime::set_bus_pan_law(uint32_t bus_id, mixing::PanLaw law) noexcept {
  BusState* state = pannable_bus_state_for(bus_id);
  if (!state) return false;
  state->panner.set_pan_law(law);
  state->spec.pan_law = static_cast<int>(law);
  return true;
}

bool TrackMixerRuntime::set_bus_pan_mode(uint32_t bus_id, mixing::PanMode mode) noexcept {
  BusState* state = pannable_bus_state_for(bus_id);
  if (!state) return false;
  state->panner.set_pan_mode(mode);
  state->spec.pan_mode = static_cast<int>(mode);
  return true;
}

bool TrackMixerRuntime::set_bus_dual_pan(uint32_t bus_id, float left_pan,
                                         float right_pan) noexcept {
  if (!std::isfinite(left_pan) || !std::isfinite(right_pan)) return false;
  BusState* state = pannable_bus_state_for(bus_id);
  if (!state) return false;
  const float left = mixing::clamp_pan(left_pan);
  const float right = mixing::clamp_pan(right_pan);
  state->panner.set_dual_pan(left, right);
  state->spec.dual_pan_left = left;
  state->spec.dual_pan_right = right;
  return true;
}

bool TrackMixerRuntime::set_bus_eq_band(uint32_t bus_id, size_t band_index,
                                        const sonare::mastering::eq::EqBand& band) noexcept {
  const int index = configured_bus_index(bus_id);
  if (index < 0 || band_index >= mastering::eq::ParametricEq::kMaxBands) return false;
  BusState& state = bus_states_[static_cast<size_t>(index)];
  try {
    // Checked first so a refused band leaves both the stage and the spec untouched.
    if (!strip_eq_acceptable(mixing::api::StripEq{true, {band}}, state.eq.sample_rate())) {
      return false;
    }
    // The spec grows first: it is the only step that allocates, and the stage cannot refuse a
    // band the check above accepted.
    store_eq_band(state.spec.eq, band_index, band);
    state.eq.set_band(band_index, band);
  } catch (...) {
    return false;
  }
  refresh_bus_eq_active(state);
  return true;
}

bool TrackMixerRuntime::set_bus_gain_db(uint32_t bus_id, float gain_db) noexcept {
  if (!std::isfinite(gain_db)) return false;
  BusState* state = bus_state_for(bus_id);
  if (!state) return false;
  state->gain.set_target(db_to_linear(std::clamp(gain_db, kFloorDb, kMaxGainDb)));
  return true;
}

bool TrackMixerRuntime::set_lane_sidechain(uint32_t track_id, unsigned int insert_index,
                                           uint32_t source_track_id) noexcept {
  if (can_set_lane_sidechain(track_id, insert_index, source_track_id) != SidechainRefusal::kNone) {
    return false;
  }
  const int found = find_sidechain_binding(SidechainTargetKind::Lane, track_id, insert_index);
  if (source_track_id == 0 && found < 0) return true;
  const SidechainTable previous = sidechains_;
  if (source_track_id == 0) {
    remove_sidechain_binding(static_cast<size_t>(found));
  } else if (found >= 0) {
    sidechains_.bindings[static_cast<size_t>(found)].source_id = source_track_id;
    publish_sidechains();
  } else {
    const size_t slot = free_key_slot();
    key_edge_delays_[slot].reset();
    sidechains_.bindings[sidechains_.count] =
        SidechainBinding{track_id,
                         insert_index,
                         source_track_id,
                         static_cast<uint8_t>(SidechainTargetKind::Lane),
                         static_cast<uint8_t>(SidechainSourceKind::Track),
                         static_cast<uint8_t>(slot)};
    ++sidechains_.count;
    publish_sidechains();
  }
  // The query already planned the delays; only a failed delay-bank allocation
  // can refuse here, and it puts the previous table back.
  const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get();
  if (lanes == nullptr || recompute_lane_pdc(*lanes)) return true;
  sidechains_ = previous;
  publish_sidechains();
  recompute_lane_pdc(*lanes);
  return false;
}

bool TrackMixerRuntime::lane_key_closes_cycle(uint32_t track_id, uint32_t source_track_id,
                                              size_t skip) const noexcept {
  if (source_track_id == track_id) return true;
  // Walk forward from the destination along source -> destination lane keys;
  // reaching the new source means the new edge closes a cycle.
  std::array<uint32_t, kMaxSidechainBindings + 1> frontier{};
  std::array<bool, kMaxSidechainBindings> walked{};
  size_t head = 0;
  size_t tail = 0;
  frontier[tail++] = track_id;
  while (head < tail) {
    const uint32_t from = frontier[head++];
    for (size_t i = 0; i < sidechains_.count; ++i) {
      const SidechainBinding& binding = sidechains_.bindings[i];
      if (i == skip || walked[i] ||
          binding.target_kind != static_cast<uint8_t>(SidechainTargetKind::Lane) ||
          binding.source_kind != static_cast<uint8_t>(SidechainSourceKind::Track) ||
          binding.source_id != from) {
        continue;
      }
      if (binding.target_id == source_track_id) return true;
      walked[i] = true;
      frontier[tail++] = binding.target_id;
    }
  }
  return false;
}

size_t TrackMixerRuntime::free_key_slot() const noexcept {
  std::array<bool, kMaxSidechainBindings> used{};
  for (size_t i = 0; i < sidechains_.count; ++i) used[sidechains_.bindings[i].key_slot] = true;
  return static_cast<size_t>(
      std::distance(used.begin(), std::find(used.begin(), used.end(), false)));
}

int TrackMixerRuntime::find_sidechain_binding(SidechainTargetKind target_kind, uint32_t target_id,
                                              unsigned int insert_index) const noexcept {
  for (size_t i = 0; i < sidechains_.count; ++i) {
    const SidechainBinding& binding = sidechains_.bindings[i];
    if (binding.target_kind == static_cast<uint8_t>(target_kind) &&
        binding.target_id == target_id && binding.insert_index == insert_index) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void TrackMixerRuntime::prune_lane_sidechains(uint32_t track_id, size_t insert_count) noexcept {
  bool dropped = false;
  for (size_t i = sidechains_.count; i > 0; --i) {
    const SidechainBinding& binding = sidechains_.bindings[i - 1];
    if (static_cast<SidechainTargetKind>(binding.target_kind) == SidechainTargetKind::Lane &&
        binding.target_id == track_id && binding.insert_index >= insert_count) {
      remove_sidechain_binding(i - 1);
      dropped = true;
    }
  }
  if (!dropped) return;
  if (const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get()) {
    recompute_lane_pdc(*lanes);
  }
}

void TrackMixerRuntime::remove_sidechain_binding(size_t index) noexcept {
  const size_t count = sidechains_.count;
  if (index >= count) return;
  sidechains_.bindings[index] = sidechains_.bindings[count - 1];
  sidechains_.bindings[count - 1] = SidechainBinding{};
  sidechains_.count = count - 1;
  publish_sidechains();
}

bool TrackMixerRuntime::sidechain_source_declared(SidechainSourceKind kind,
                                                  uint32_t source_id) const noexcept {
  if (source_id == 0) return false;
  if (kind == SidechainSourceKind::Bus) return configured_bus_index(source_id) >= 0;
  const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get();
  if (lanes == nullptr) return false;
  return std::any_of(lanes->begin(), lanes->end(), [source_id](const TrackLaneConfig& lane) {
    return lane.track_id == source_id;
  });
}

bool TrackMixerRuntime::store_keyed_binding(SidechainTargetKind target_kind, uint32_t target_id,
                                            unsigned int insert_index, SidechainSourceKind kind,
                                            uint32_t source_id) noexcept {
  const int found = find_sidechain_binding(target_kind, target_id, insert_index);
  if (source_id == 0) {
    if (found >= 0) remove_sidechain_binding(static_cast<size_t>(found));
    return true;
  }
  if (found >= 0) {
    SidechainBinding& binding = sidechains_.bindings[static_cast<size_t>(found)];
    binding.source_kind = static_cast<uint8_t>(kind);
    binding.source_id = source_id;
    publish_sidechains();
    return true;
  }
  const size_t count = sidechains_.count;
  if (count >= kMaxSidechainBindings) return false;
  // A key slot owns a delay line and a buffer, so it is claimed from the slots
  // no other binding holds.
  const size_t slot = free_key_slot();
  key_edge_delays_[slot].reset();
  sidechains_.bindings[count] = SidechainBinding{target_id,
                                                 insert_index,
                                                 source_id,
                                                 static_cast<uint8_t>(target_kind),
                                                 static_cast<uint8_t>(kind),
                                                 static_cast<uint8_t>(slot)};
  sidechains_.count = count + 1;
  publish_sidechains();
  return true;
}

bool TrackMixerRuntime::set_bus_sidechain(uint32_t bus_id, unsigned int insert_index,
                                          SidechainSourceKind kind, uint32_t source_id) noexcept {
  if (can_set_bus_sidechain(bus_id, insert_index, kind, source_id) != SidechainRefusal::kNone) {
    return false;
  }
  return commit_keyed_binding(SidechainTargetKind::Bus, bus_id, insert_index, kind, source_id);
}

bool TrackMixerRuntime::set_master_sidechain(unsigned int insert_index, SidechainSourceKind kind,
                                             uint32_t source_id) noexcept {
  if (can_set_master_sidechain(insert_index, kind, source_id) != SidechainRefusal::kNone) {
    return false;
  }
  return commit_keyed_binding(SidechainTargetKind::Master, 0, insert_index, kind, source_id);
}

bool TrackMixerRuntime::commit_keyed_binding(SidechainTargetKind target_kind, uint32_t target_id,
                                             unsigned int insert_index, SidechainSourceKind kind,
                                             uint32_t source_id) noexcept {
  const int found = find_sidechain_binding(target_kind, target_id, insert_index);
  const auto old_kind = found >= 0
                            ? static_cast<SidechainSourceKind>(
                                  sidechains_.bindings[static_cast<size_t>(found)].source_kind)
                            : SidechainSourceKind::Track;
  const uint32_t old_source =
      found >= 0 ? sidechains_.bindings[static_cast<size_t>(found)].source_id : 0;
  // The matching can_set_* query has accepted this binding, so storing it cannot refuse.
  [[maybe_unused]] const bool stored =
      store_keyed_binding(target_kind, target_id, insert_index, kind, source_id);
  assert(stored);
  refresh_bus_graph();
  // Only a failed delay-bank allocation can refuse the planned delays; it puts
  // the previous binding back.
  const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get();
  if (lanes == nullptr || recompute_lane_pdc(*lanes)) return true;
  store_keyed_binding(target_kind, target_id, insert_index, old_kind, old_source);
  refresh_bus_graph();
  recompute_lane_pdc(*lanes);
  return false;
}

int TrackMixerRuntime::build_keyed_input(size_t binding_index, int lane_channels, int num_samples,
                                         std::array<const float*, kMaxLaneChannels>& planes,
                                         bool into_slot) noexcept {
  const SidechainBinding& binding = audio_sidechains_.bindings[binding_index];
  const uint32_t source_id = binding.source_id;
  int channels = 0;
  if (binding.source_kind == static_cast<uint8_t>(SidechainSourceKind::Bus)) {
    const int source_index = configured_bus_index(source_id);
    if (source_index < 0) return 0;
    const size_t index = static_cast<size_t>(source_index);
    // A bus key is snapshot in this pass; a source that did not render leaves 0 frames.
    const int frames = bus_key_frames_[index];
    channels = frames > 0 ? bus_key_channels_[index] : lane_channels;
    for (int ch = 0; ch < channels; ++ch) {
      float* plane = bus_key_channel(index, ch);
      if (frames < num_samples) std::fill(plane + std::max(frames, 0), plane + num_samples, 0.0f);
      planes[static_cast<size_t>(ch)] = plane;
    }
  } else {
    const int source_index = lane_index_for_track(source_id);
    if (source_index < 0) return 0;
    // The source's snapshot was taken at ITS sub-block's length, which may be
    // shorter than this one (a source that did not render this block). Silence
    // the shortfall so the key never carries an older, longer sub-block's audio.
    int& source_frames = key_frames_[static_cast<size_t>(source_index)];
    const int stale = num_samples - source_frames;
    channels = std::min(lane_channels, kMaxLaneChannels);
    for (int ch = 0; ch < channels; ++ch) {
      float* plane = key_channel(static_cast<size_t>(source_index), ch);
      if (stale > 0) std::fill(plane + source_frames, plane + num_samples, 0.0f);
      planes[static_cast<size_t>(ch)] = plane;
    }
    if (stale > 0) source_frames = num_samples;
  }
  const size_t slot = binding.key_slot;
  mixing::AlignmentDelay& delay = key_edge_delays_[slot];
  if (delay.delay_samples_q8() == 0 && !into_slot) return channels;
  std::array<float*, kMaxLaneChannels> aligned{};
  for (int ch = 0; ch < channels; ++ch) {
    aligned[static_cast<size_t>(ch)] = keyed_input_channel(slot, ch);
    std::copy(planes[static_cast<size_t>(ch)], planes[static_cast<size_t>(ch)] + num_samples,
              aligned[static_cast<size_t>(ch)]);
    planes[static_cast<size_t>(ch)] = aligned[static_cast<size_t>(ch)];
  }
  delay.process(aligned.data(), channels, num_samples);
  return channels;
}

void TrackMixerRuntime::deliver_bus_sidechains(size_t bus_index, int lane_channels,
                                               int num_samples) noexcept {
  mixing::FxBus* fx = bus_states_[bus_index].bus.get();
  if (fx == nullptr) return;
  fx->clear_insert_sidechains();
  const uint32_t bus_id = bus_configs_[bus_index].bus_id;
  for (size_t i = 0; i < audio_sidechains_.count; ++i) {
    const SidechainBinding& binding = audio_sidechains_.bindings[i];
    if (binding.target_kind != static_cast<uint8_t>(SidechainTargetKind::Bus) ||
        binding.target_id != bus_id) {
      continue;
    }
    std::array<const float*, kMaxLaneChannels> planes{};
    const int channels = build_keyed_input(i, lane_channels, num_samples, planes, false);
    if (channels <= 0) continue;
    fx->set_insert_sidechain(binding.insert_index, planes.data(), channels, num_samples);
  }
}

void TrackMixerRuntime::deliver_master_sidechains(mixing::ChannelStrip* strip,
                                                  int num_samples) noexcept {
  if (strip == nullptr || num_samples <= 0 || num_samples > max_block_size_) return;
  strip->clear_insert_sidechains();
  for (size_t i = 0; i < audio_sidechains_.count; ++i) {
    const SidechainBinding& binding = audio_sidechains_.bindings[i];
    if (binding.target_kind != static_cast<uint8_t>(SidechainTargetKind::Master)) continue;
    const size_t slot = binding.key_slot;
    const int frames = master_key_frames_[slot];
    if (frames <= 0) continue;
    const int channels = master_key_channels_[slot];
    std::array<const float*, kMaxLaneChannels> planes{};
    for (int ch = 0; ch < channels; ++ch) {
      float* plane = keyed_input_channel(slot, ch);
      if (frames < num_samples) std::fill(plane + frames, plane + num_samples, 0.0f);
      planes[static_cast<size_t>(ch)] = plane;
    }
    strip->set_insert_sidechain(binding.insert_index, planes.data(), channels, num_samples);
    // Consumed: a block the mixer does not render leaves the insert unkeyed.
    master_key_frames_[slot] = 0;
  }
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
  const size_t count = audio_sidechains_.count;
  if (count == 0) return;
  for (size_t i = 0; i < count; ++i) {
    const SidechainBinding& binding = audio_sidechains_.bindings[i];
    if (binding.target_kind != static_cast<uint8_t>(SidechainTargetKind::Lane) ||
        binding.target_id != lane.track_id) {
      continue;
    }
    // Lanes render in key order, so the source snapshot is this block's audio;
    // the key edge re-times it to this lane's strip input.
    std::array<const float*, kMaxLaneChannels> key{};
    const int channels = build_keyed_input(i, num_channels, num_samples, key, false);
    if (channels <= 0) continue;
    lane.strip->set_insert_sidechain(binding.insert_index, key.data(), channels, num_samples);
  }
}

void TrackMixerRuntime::snapshot_sidechain_key(size_t lane_index, int num_channels,
                                               int num_samples) noexcept {
  const size_t count = audio_sidechains_.count;
  if (count == 0) return;
  const uint32_t track_id = lane_states_[lane_index].track_id;
  if (track_id == 0) return;
  bool is_source = false;
  for (size_t i = 0; i < count; ++i) {
    const SidechainBinding& binding = audio_sidechains_.bindings[i];
    if (binding.source_kind == static_cast<uint8_t>(SidechainSourceKind::Track) &&
        binding.source_id == track_id) {
      is_source = true;
      break;
    }
  }
  if (!is_source) return;
  // Copy the post-strip output before lane PDC and the fader/gate/pan stage
  // mutate the lane buffer in place, so keyed inserts see the pre-fader signal.
  for (int ch = 0; ch < num_channels && ch < kMaxLaneChannels; ++ch) {
    const float* src = lane_channel(lane_index, ch);
    std::copy(src, src + num_samples, key_channel(lane_index, ch));
  }
  key_frames_[lane_index] = num_samples;
}

SidechainRefusal TrackMixerRuntime::can_set_lane_sidechain(
    uint32_t track_id, unsigned int insert_index, uint32_t source_track_id) const noexcept {
  if (track_id == 0) return SidechainRefusal::kInvalidTarget;
  const int found = find_sidechain_binding(SidechainTargetKind::Lane, track_id, insert_index);
  if (source_track_id == 0 && found < 0) return SidechainRefusal::kNone;
  if (source_track_id != 0) {
    if (source_track_id == track_id) return SidechainRefusal::kSelfKey;
    if (lane_key_closes_cycle(track_id, source_track_id,
                              found >= 0 ? static_cast<size_t>(found) : kMaxSidechainBindings)) {
      return SidechainRefusal::kCycle;
    }
    if (found < 0 && sidechains_.count >= kMaxSidechainBindings) {
      return SidechainRefusal::kTableFull;
    }
  }
  const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get();
  if (lanes == nullptr) return SidechainRefusal::kNone;
  // Lane keys are track-sourced, so the bus graph is the live one.
  const SidechainTable candidate =
      with_binding(sidechains_, found,
                   SidechainBinding{track_id, insert_index, source_track_id,
                                    static_cast<uint8_t>(SidechainTargetKind::Lane),
                                    static_cast<uint8_t>(SidechainSourceKind::Track),
                                    static_cast<uint8_t>(found < 0 ? free_key_slot() : 0)});
  PdcPlan plan;
  return plan_pdc(*lanes, current_bus_graph_view(), &plan, nullptr, nullptr, false, &candidate)
             ? SidechainRefusal::kNone
             : SidechainRefusal::kPlanRefused;
}

SidechainRefusal TrackMixerRuntime::can_set_bus_sidechain(uint32_t bus_id,
                                                          unsigned int insert_index,
                                                          SidechainSourceKind kind,
                                                          uint32_t source_id) const noexcept {
  const int bus_index = configured_bus_index(bus_id);
  if (bus_index < 0) return SidechainRefusal::kInvalidTarget;
  if (static_cast<uint8_t>(kind) > static_cast<uint8_t>(SidechainSourceKind::Bus)) {
    return SidechainRefusal::kInvalidSourceKind;
  }
  const mixing::FxBus* fx = bus_states_[static_cast<size_t>(bus_index)].bus.get();
  if (fx == nullptr) return SidechainRefusal::kInvalidTarget;
  if (insert_index >= fx->num_inserts()) return SidechainRefusal::kInsertOutOfRange;
  const int found = find_sidechain_binding(SidechainTargetKind::Bus, bus_id, insert_index);
  const bool bus_key = source_id != 0 && kind == SidechainSourceKind::Bus;
  if (source_id != 0) {
    if (!sidechain_source_declared(kind, source_id)) return SidechainRefusal::kUndeclaredSource;
    if (bus_key && source_id == bus_id) return SidechainRefusal::kSelfKey;
  }
  if (source_id == 0 && found < 0) return SidechainRefusal::kNone;
  // The candidate key edges order the buses; a bus key that closes a cycle with
  // the outputs and sends is refused.
  std::array<KeyEdge, kMaxSidechainBindings + 1> edges{};
  std::array<KeyEdge, kMaxSidechainBindings> current{};
  const size_t current_count =
      collect_key_edges(current, found >= 0 ? static_cast<size_t>(found) : kMaxSidechainBindings);
  std::copy(current.begin(), current.begin() + static_cast<std::ptrdiff_t>(current_count),
            edges.begin());
  size_t edge_count = current_count;
  if (bus_key) edges[edge_count++] = KeyEdge{source_id, bus_id};
  BusGraphView view = current_bus_graph_view();
  if (!validate_bus_graph(bus_configs_, edges.data(), edge_count, &view.order)) {
    return SidechainRefusal::kCycle;
  }
  if (source_id != 0 && found < 0 && sidechains_.count >= kMaxSidechainBindings) {
    return SidechainRefusal::kTableFull;
  }
  const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get();
  if (lanes == nullptr) return SidechainRefusal::kNone;
  const SidechainTable candidate = with_binding(
      sidechains_, found,
      SidechainBinding{bus_id, insert_index, source_id,
                       static_cast<uint8_t>(SidechainTargetKind::Bus), static_cast<uint8_t>(kind),
                       static_cast<uint8_t>(found < 0 ? free_key_slot() : 0)});
  build_routes(bus_configs_, view.skip_binding, &view.routes, &candidate);
  PdcPlan plan;
  return plan_pdc(*lanes, view, &plan, nullptr, nullptr, false, &candidate)
             ? SidechainRefusal::kNone
             : SidechainRefusal::kPlanRefused;
}

SidechainRefusal TrackMixerRuntime::can_set_master_sidechain(unsigned int insert_index,
                                                             SidechainSourceKind kind,
                                                             uint32_t source_id) const noexcept {
  if (insert_index >= master_insert_count_) return SidechainRefusal::kInsertOutOfRange;
  if (static_cast<uint8_t>(kind) > static_cast<uint8_t>(SidechainSourceKind::Bus)) {
    return SidechainRefusal::kInvalidSourceKind;
  }
  if (source_id != 0 && !sidechain_source_declared(kind, source_id)) {
    return SidechainRefusal::kUndeclaredSource;
  }
  const int found = find_sidechain_binding(SidechainTargetKind::Master, 0, insert_index);
  if (source_id == 0 && found < 0) return SidechainRefusal::kNone;
  if (source_id != 0 && found < 0 && sidechains_.count >= kMaxSidechainBindings) {
    return SidechainRefusal::kTableFull;
  }
  const std::vector<TrackLaneConfig>* lanes = lanes_.control_current().get();
  if (lanes == nullptr) return SidechainRefusal::kNone;
  const SidechainTable candidate = with_binding(
      sidechains_, found,
      SidechainBinding{
          0, insert_index, source_id, static_cast<uint8_t>(SidechainTargetKind::Master),
          static_cast<uint8_t>(kind), static_cast<uint8_t>(found < 0 ? free_key_slot() : 0)});
  // A master key is no bus-graph edge, so the bus order stands; only the routes'
  // key-source flags follow the candidate table.
  BusGraphView view = current_bus_graph_view();
  build_routes(bus_configs_, view.skip_binding, &view.routes, &candidate);
  PdcPlan plan;
  return plan_pdc(*lanes, view, &plan, nullptr, nullptr, false, &candidate)
             ? SidechainRefusal::kNone
             : SidechainRefusal::kPlanRefused;
}

}  // namespace sonare::engine
