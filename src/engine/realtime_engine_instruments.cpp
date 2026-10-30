/// @file realtime_engine_instruments.cpp
/// @brief RealtimeEngine: continuous parameter automation for hosted
///        instruments (the reserved instrument-param id namespace), and the
///        controller-profile / articulation hand-off to the audio thread.

#include <cmath>

#include "engine/instrument_automation_id.h"
#include "engine/realtime_engine.h"
#include "engine/realtime_engine_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)

namespace sonare::engine {

int64_t RealtimeEngine::resolve_instrument_automation_id(uint32_t destination_id,
                                                         const std::string& key) noexcept {
  midi::MidiInstrument* instrument = instrument_rack_.get(destination_id);
  if (instrument == nullptr) return -1;
  const int id = instrument->parameter_id_for_key(key);
  if (id < 0) return -1;
  const unsigned int param_id = static_cast<unsigned int>(id);
  if (param_id > kInstrumentParamFieldMask) return -1;
  // The bind minted the destination's slot; resolving only looks it up.
  const int64_t slot = instrument_auto_destinations_.find(destination_id);
  if (slot < 0) return -1;
  return make_instrument_param_id(static_cast<uint32_t>(slot), param_id);
}

bool RealtimeEngine::route_instrument_param_smoothed(uint32_t destination_id, unsigned int param_id,
                                                     float value) noexcept {
  if (!std::isfinite(value)) return false;
  InstrumentAutoSlot* free_slot = nullptr;
  InstrumentAutoSlot* settled_match = nullptr;
  for (InstrumentAutoSlot& slot : instrument_auto_slots_) {
    if (slot.assigned && slot.destination_id == destination_id && slot.param_id == param_id) {
      if (slot.active) {
        slot.smoother.set_target(value);
        return true;
      }
      settled_match = &slot;
    }
    if (!slot.active && free_slot == nullptr) {
      free_slot = &slot;
    }
  }
  if (settled_match != nullptr) {
    settled_match->active = true;
    settled_match->smoother.set_target(value);
    return true;
  }
  if (free_slot == nullptr) {
    instrument_automation_overflow_count_.bump();
    return false;
  }
  free_slot->active = true;
  free_slot->assigned = true;
  free_slot->destination_id = destination_id;
  free_slot->param_id = param_id;
  // Snap to the first observed value so the smoother does not glide up from 0
  // (a cutoff lane starting at 8 kHz must not sweep from 0 Hz on its first
  // block). Mirrors the master-insert slot table.
  free_slot->smoother.prepare(sample_rate_, 5.0f);
  free_slot->smoother.reset(value);
  free_slot->smoother.set_target(value);
  return true;
}

void RealtimeEngine::advance_instrument_automations(int num_steps) noexcept {
  if (num_steps <= 0) return;
  for (InstrumentAutoSlot& slot : instrument_auto_slots_) {
    if (!slot.active) continue;
    const float value = slot.smoother.advance(num_steps);
    // An unbound destination applies nothing: a lane resolved against an
    // instrument that has since been unbound is inert rather than dangling.
    if (midi::MidiInstrument* instrument = instrument_rack_.get(slot.destination_id)) {
      instrument->apply_parameter(slot.param_id, value);
    }
    if (std::abs(slot.smoother.target() - value) <= kSettleEpsilon) {
      slot.smoother.reset(slot.smoother.target());
      slot.active = false;
    }
  }
}

void RealtimeEngine::settle_instrument_automations() noexcept {
  for (InstrumentAutoSlot& slot : instrument_auto_slots_) {
    if (!slot.active) continue;
    const float target = slot.smoother.target();
    slot.smoother.reset(target);
    if (midi::MidiInstrument* instrument = instrument_rack_.get(slot.destination_id)) {
      instrument->apply_parameter(slot.param_id, target);
    }
    slot.active = false;
  }
}

void RealtimeEngine::release_instrument_automations(uint32_t destination_id) noexcept {
  for (InstrumentAutoSlot& slot : instrument_auto_slots_) {
    if (slot.assigned && slot.destination_id == destination_id) {
      slot.active = false;
      slot.assigned = false;
    }
  }
}

void RealtimeEngine::clear_instrument_automations() noexcept {
  for (InstrumentAutoSlot& slot : instrument_auto_slots_) {
    slot.active = false;
    slot.assigned = false;
  }
}

bool RealtimeEngine::route_instrument_parameter(uint32_t target_id, float value) noexcept {
  uint32_t destination_id = 0;
  if (!instrument_auto_destinations_.destination(instrument_param_slot(target_id),
                                                 &destination_id)) {
    return false;
  }
  return route_instrument_param_smoothed(destination_id, instrument_param_param(target_id), value);
}

bool RealtimeEngine::describe_instrument_reserved_parameter(
    uint32_t target_id, automation::ParameterDescription* out) const {
  if (out == nullptr) return false;
  uint32_t destination_id = 0;
  if (!instrument_auto_destinations_.destination(instrument_param_slot(target_id),
                                                 &destination_id)) {
    return false;
  }
  const midi::MidiInstrument* instrument = instrument_rack_.get(destination_id);
  if (instrument == nullptr) return false;
  return instrument->describe_parameter(instrument_param_param(target_id), out);
}

#if !defined(SONARE_WITH_MIXING)
// Without the mixing library the reserved-id router serves only the instrument
// namespace. The mixing build defines the full router (mixer fader/pan, strip
// inserts, and this delegation) in realtime_engine_mixing.cpp.
bool RealtimeEngine::route_engine_parameter(uint32_t target_id, float value) noexcept {
  if (!is_instrument_param_id(target_id)) return false;
  return route_instrument_parameter(target_id, value);
}

bool RealtimeEngine::route_engine_parameter_thunk(void* context, uint32_t param_id,
                                                  float value) noexcept {
  return static_cast<RealtimeEngine*>(context)->route_engine_parameter(param_id, value);
}

// Mirrors route_engine_parameter's gate split: without the mixing library the
// reserved-id descriptor serves only the instrument namespace, and the mixing
// build's full descriptor (realtime_engine_mixing.cpp) delegates here for it.
bool RealtimeEngine::describe_reserved_parameter(uint32_t id,
                                                 automation::ParameterDescription* out) const {
  if (!is_instrument_param_id(id)) return false;
  return describe_instrument_reserved_parameter(id, out);
}
#endif

RealtimeEngine::InstrumentControlSlot* RealtimeEngine::find_instrument_control(
    uint32_t destination_id) noexcept {
  for (InstrumentControlSlot& slot : instrument_controls_) {
    if (slot.desired.instrument != nullptr && slot.destination_id == destination_id) return &slot;
  }
  return nullptr;
}

const RealtimeEngine::InstrumentControlSlot* RealtimeEngine::find_instrument_control(
    uint32_t destination_id) const noexcept {
  for (const InstrumentControlSlot& slot : instrument_controls_) {
    if (slot.desired.instrument != nullptr && slot.destination_id == destination_id) return &slot;
  }
  return nullptr;
}

void RealtimeEngine::publish_instrument_control(InstrumentControlSlot& slot) noexcept {
  slot.published.store(slot.desired);
  slot.published_generation.fetch_add(1, std::memory_order_release);
}

void RealtimeEngine::sync_instrument_control(uint32_t destination_id,
                                             midi::MidiInstrument* instrument) noexcept {
  InstrumentControlSlot* slot = find_instrument_control(destination_id);
  for (size_t i = 0; slot == nullptr && i < instrument_controls_.size(); ++i) {
    if (instrument_controls_[i].desired.instrument == nullptr) slot = &instrument_controls_[i];
  }
  // The table has the rack's capacity, so a bound instrument always finds a slot.
  if (slot == nullptr) return;
  InstrumentControlState state{};
  state.instrument = instrument;
  slot->destination_id = destination_id;
  slot->has_profile = false;
  slot->has_articulation = instrument != nullptr;
  if (instrument != nullptr) {
    if (const midi::ControllerProfile* profile = instrument->controller_profile()) {
      state.profile = *profile;
      slot->has_profile = true;
    }
    for (uint8_t channel = 0; slot->has_articulation && channel < 16; ++channel) {
      slot->has_articulation = instrument->articulation(channel, &state.articulation[channel]);
    }
  }
  slot->desired = state;
  publish_instrument_control(*slot);
  // The instrument already holds this state, so the audio thread adopts it without applying.
  slot->adopted_generation = slot->published_generation.load(std::memory_order_relaxed);
  slot->applied_profile_serial = state.profile_serial;
  slot->applied_articulation_serial = state.articulation_serial;
}

RealtimeEngine::InstrumentControlStatus RealtimeEngine::set_instrument_controller_profile(
    uint32_t destination_id, const midi::ControllerProfile& profile) noexcept {
  InstrumentControlSlot* slot = find_instrument_control(destination_id);
  if (slot == nullptr) return InstrumentControlStatus::kNoInstrument;
  if (!slot->has_profile) return InstrumentControlStatus::kNotSupported;
  slot->desired.profile = profile;
  ++slot->desired.profile_serial;
  publish_instrument_control(*slot);
  return InstrumentControlStatus::kOk;
}

RealtimeEngine::InstrumentControlStatus RealtimeEngine::instrument_controller_profile(
    uint32_t destination_id, midi::ControllerProfile* out) const noexcept {
  const InstrumentControlSlot* slot = find_instrument_control(destination_id);
  if (slot == nullptr) return InstrumentControlStatus::kNoInstrument;
  if (!slot->has_profile) return InstrumentControlStatus::kNotSupported;
  *out = slot->desired.profile;
  return InstrumentControlStatus::kOk;
}

RealtimeEngine::InstrumentControlStatus RealtimeEngine::set_instrument_articulation(
    uint32_t destination_id, uint8_t channel, midi::ArticulationMode mode) noexcept {
  InstrumentControlSlot* slot = find_instrument_control(destination_id);
  if (slot == nullptr) return InstrumentControlStatus::kNoInstrument;
  if (!slot->has_articulation) return InstrumentControlStatus::kNotSupported;
  slot->desired.articulation[channel & 0x0Fu] = mode;
  ++slot->desired.articulation_serial;
  publish_instrument_control(*slot);
  return InstrumentControlStatus::kOk;
}

RealtimeEngine::InstrumentControlStatus RealtimeEngine::instrument_articulation(
    uint32_t destination_id, uint8_t channel, midi::ArticulationMode* out) const noexcept {
  const InstrumentControlSlot* slot = find_instrument_control(destination_id);
  if (slot == nullptr) return InstrumentControlStatus::kNoInstrument;
  if (!slot->has_articulation) return InstrumentControlStatus::kNotSupported;
  *out = slot->desired.articulation[channel & 0x0Fu];
  return InstrumentControlStatus::kOk;
}

void RealtimeEngine::adopt_instrument_controls() noexcept {
  for (InstrumentControlSlot& slot : instrument_controls_) {
    const uint32_t generation = slot.published_generation.load(std::memory_order_acquire);
    if (generation == slot.adopted_generation) continue;
    InstrumentControlState state{};
    // A store in progress is adopted at the next block.
    if (!slot.reader.try_load_into(&state)) continue;
    slot.adopted_generation = generation;
    if (state.instrument == nullptr) continue;
    if (state.profile_serial != slot.applied_profile_serial) {
      slot.applied_profile_serial = state.profile_serial;
      state.instrument->set_controller_profile(state.profile);
    }
    if (state.articulation_serial != slot.applied_articulation_serial) {
      slot.applied_articulation_serial = state.articulation_serial;
      for (uint8_t channel = 0; channel < 16; ++channel) {
        state.instrument->set_articulation(channel, state.articulation[channel]);
      }
    }
  }
}

}  // namespace sonare::engine

#endif
