/// @file sonare_c_engine_controller.cpp
/// @brief The controller-profile C ABI: how a device spells a gesture, and
///        which expression axis that gesture means.
///
/// Every entry here reads the destination's current profile, changes one thing,
/// and installs the result through the instrument's own setter, so the rule
/// that installing a profile drops the channels' accumulated axis values holds
/// however the profile was reached. An instrument with nowhere to put a profile
/// answers NOT_SUPPORTED rather than succeeding quietly: the caller cannot tell
/// a silently discarded mapping from one that took until a note sounds.

#include <sonare/sonare_c.h>

#include <cmath>
#include <string>

#include "engine/realtime_engine.h"
#include "sonare_c_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "midi/controller_profile.h"
#include "midi/instrument.h"
#endif

using namespace sonare;
using namespace sonare_c_detail;

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {

/// The instrument bound to @p destination_id, or nullptr. Split out because
/// every entry below needs the same two failures told apart: no instrument at
/// all, and an instrument that declines a profile.
sonare::midi::MidiInstrument* instrument_of(SonareRealtimeEngine* engine,
                                            uint32_t destination_id) noexcept {
  return engine->engine.midi_instrument(destination_id);
}

/// Reads the destination's profile into @p out. Tells the two failures apart:
/// INVALID_PARAMETER for a destination_id nothing is bound to (the spelling
/// sonare_engine_resolve_instrument_automation_id already uses for it), and
/// NOT_SUPPORTED for a bound instrument that holds no profile.
SonareError profile_of(SonareRealtimeEngine* engine, uint32_t destination_id,
                       sonare::midi::ControllerProfile* out) noexcept {
  sonare::midi::MidiInstrument* instrument = instrument_of(engine, destination_id);
  if (instrument == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  const sonare::midi::ControllerProfile* profile = instrument->controller_profile();
  if (profile == nullptr) return SONARE_ERROR_NOT_SUPPORTED;
  *out = *profile;
  return SONARE_OK;
}

/// Installs @p profile back onto the destination. Only called after profile_of
/// has already succeeded, so a refusal here means the instrument's setter and
/// its getter disagree, which is a defect in that instrument rather than in the
/// call.
SonareError install(SonareRealtimeEngine* engine, uint32_t destination_id,
                    const sonare::midi::ControllerProfile& profile) noexcept {
  sonare::midi::MidiInstrument* instrument = instrument_of(engine, destination_id);
  if (instrument == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  return instrument->set_controller_profile(profile) ? SONARE_OK : SONARE_ERROR_NOT_SUPPORTED;
}

}  // namespace
#endif

const char* sonare_controller_profile_names(void) {
#if defined(SONARE_WITH_ARRANGEMENT)
  static const std::string kNames = [] {
    std::string names;
    for (size_t i = 0; i < sonare::midi::ControllerProfile::preset_count(); ++i) {
      const char* name = sonare::midi::ControllerProfile::preset_name_at(i);
      if (name == nullptr) break;
      if (!names.empty()) names += '\n';
      names += name;
    }
    return names;
  }();
  return kNames.c_str();
#else
  return "";
#endif
}

SonareError sonare_engine_set_controller_profile(SonareRealtimeEngine* engine,
                                                 uint32_t destination_id, const char* preset_name) {
  SONARE_C_API_ENTRY;
  if (!engine || !preset_name || preset_name[0] == '\0') return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  // The name is validated before the engine is touched, so an unknown preset
  // reads as a bad argument whether or not the destination has an instrument.
  sonare::midi::ControllerProfile profile;
  if (!sonare::midi::ControllerProfile::preset(preset_name, &profile)) {
    set_last_error("unknown controller profile preset");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return install(engine, destination_id, profile);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_bind_controller(SonareRealtimeEngine* engine, uint32_t destination_id,
                                          const SonareControllerBinding* binding) {
  SONARE_C_API_ENTRY;
  if (!engine || !binding) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  // Ordinals out of range are refused rather than clamped: a value the caller
  // meant as "poly pressure" arriving as "control change" is a binding that
  // works and listens to the wrong thing.
  if (binding->input >= SONARE_CONTROLLER_INPUT_COUNT ||
      binding->axis >= SONARE_CONTROLLER_AXIS_COUNT || binding->index > 127) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  sonare::midi::ControllerProfile profile;
  const SonareError status = profile_of(engine, destination_id, &profile);
  if (status != SONARE_OK) return status;

  sonare::midi::ControllerBinding entry;
  entry.input = static_cast<sonare::midi::ControllerInput>(binding->input);
  entry.index = binding->index;
  entry.axis = static_cast<sonare::midi::ControllerAxis>(binding->axis);
  // A non-finite range or curve would reach the audio thread and stay there, so
  // it is refused here rather than substituted: a mapping silently replaced by
  // a default is a mapping the caller believes it installed.
  SONARE_CHECK_MSG(std::isfinite(binding->lo) && std::isfinite(binding->hi),
                   sonare::ErrorCode::InvalidParameter, "binding range must be finite");
  SONARE_CHECK_MSG(std::isfinite(binding->curve) && binding->curve > 0.0f,
                   sonare::ErrorCode::InvalidParameter,
                   "binding curve must be finite and positive");
  entry.lo = binding->lo;
  entry.hi = binding->hi;
  entry.curve = binding->curve;

  if (!profile.bind(entry)) {
    set_last_error(
        "controller binding refused: the table is full, the axis is none, or a poly-pressure "
        "binding named a channel-level axis");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return install(engine, destination_id, profile);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_clear_controller_bindings(SonareRealtimeEngine* engine,
                                                    uint32_t destination_id) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  sonare::midi::ControllerProfile profile;
  const SonareError status = profile_of(engine, destination_id, &profile);
  if (status != SONARE_OK) return status;
  profile.clear();
  return install(engine, destination_id, profile);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_controller_binding_count(SonareRealtimeEngine* engine,
                                                   uint32_t destination_id, size_t* out_count) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  // Defined on every exit path, so a feature-disabled build and an unbound
  // destination both leave the caller a readable count rather than whatever the
  // stack held.
  *out_count = 0;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  sonare::midi::ControllerProfile profile;
  const SonareError status = profile_of(engine, destination_id, &profile);
  if (status != SONARE_OK) return status;
  *out_count = profile.binding_count();
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_controller_velocity_meaningful(SonareRealtimeEngine* engine,
                                                             uint32_t destination_id,
                                                             int meaningful) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  (void)meaningful;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  sonare::midi::ControllerProfile profile;
  const SonareError status = profile_of(engine, destination_id, &profile);
  if (status != SONARE_OK) return status;
  profile.velocity_meaningful = meaningful != 0;
  return install(engine, destination_id, profile);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_controller_velocity_meaningful(SonareRealtimeEngine* engine,
                                                         uint32_t destination_id,
                                                         int* out_meaningful) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_meaningful) return SONARE_ERROR_INVALID_PARAMETER;
  *out_meaningful = 0;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  sonare::midi::ControllerProfile profile;
  const SonareError status = profile_of(engine, destination_id, &profile);
  if (status != SONARE_OK) return status;
  *out_meaningful = profile.velocity_meaningful ? 1 : 0;
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}
