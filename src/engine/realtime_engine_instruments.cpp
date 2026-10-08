/// @file realtime_engine_instruments.cpp
/// @brief RealtimeEngine: continuous parameter automation for hosted
///        instruments (the reserved instrument-param id namespace).

#include <cmath>

#include "engine/instrument_automation_id.h"
#include "engine/realtime_engine.h"

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
  constexpr float kSettleEpsilon = 1.0e-6f;
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

}  // namespace sonare::engine

#endif
