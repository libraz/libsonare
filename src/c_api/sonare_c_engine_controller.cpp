/// @file sonare_c_engine_controller.cpp
/// @brief The controller-profile C ABI: how a device spells a gesture, and
///        which expression axis that gesture means.
///
/// Every entry here reads the destination's current profile, changes one thing,
/// and installs the result through the engine, which hands it to the instrument
/// on the audio thread at the next block. Installing drops the channels'
/// accumulated axis values however the profile was reached. An instrument with
/// nowhere to put a profile answers NOT_SUPPORTED rather than succeeding quietly:
/// the caller cannot tell a silently discarded mapping from one that took until
/// a note sounds.

#include <sonare/sonare_c.h>

#include <cmath>
#include <string>

#include "engine/realtime_engine.h"
#include "sonare_c_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "midi/controller_profile.h"
#include "midi/instrument.h"
#include "midi/mpe.h"
#endif

using namespace sonare;
using namespace sonare_c_detail;

#if defined(SONARE_WITH_ARRANGEMENT)
static_assert(SONARE_NOTE_TRACKING_COUNT == sonare::midi::kNoteTrackingCount,
              "note tracking ordinals out of step with midi::NoteTracking");
static_assert(SONARE_MPE_DIMENSION_COUNT == sonare::midi::kMpeDimensionCount,
              "MPE dimension ordinals out of step with midi::MpeDimension");

namespace {

/// Maps an engine instrument-control outcome to its C status. Tells the two
/// failures apart: INVALID_PARAMETER for a destination_id nothing is bound to
/// (the spelling sonare_engine_resolve_instrument_automation_id already uses for
/// it), and NOT_SUPPORTED for a bound instrument that holds no profile.
SonareError profile_status(
    sonare::engine::RealtimeEngine::InstrumentControlStatus status) noexcept {
  using Status = sonare::engine::RealtimeEngine::InstrumentControlStatus;
  switch (status) {
    case Status::kOk:
      return SONARE_OK;
    case Status::kNoInstrument:
      return SONARE_ERROR_INVALID_PARAMETER;
    case Status::kNotSupported:
      break;
  }
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "destination instrument holds no controller profile");
  return SONARE_ERROR_NOT_SUPPORTED;
}

/// Reads the profile last installed on the destination into @p out.
SonareError profile_of(SonareRealtimeEngine* engine, uint32_t destination_id,
                       sonare::midi::ControllerProfile* out) noexcept {
  return profile_status(engine->engine.instrument_controller_profile(destination_id, out));
}

/// Hands @p profile to the destination's instrument, adopted on the audio thread
/// at the next block; the control thread never writes the instrument itself.
SonareError install(SonareRealtimeEngine* engine, uint32_t destination_id,
                    const sonare::midi::ControllerProfile& profile) noexcept {
  return profile_status(engine->engine.set_instrument_controller_profile(destination_id, profile));
}

/// The tracking field @p dimension names. A pointer rather than a copy so the
/// setter and the getter share one mapping: three fields spelled out twice is
/// where the two would drift apart. @p dimension has already been range-checked
/// by the entry point, which is where an out-of-domain value is refused.
sonare::midi::NoteTracking* note_tracking_field(sonare::midi::ControllerProfile* profile,
                                                int dimension) noexcept {
  switch (dimension) {
    case SONARE_MPE_DIMENSION_PRESSURE:
      return &profile->pressure_tracking;
    case SONARE_MPE_DIMENSION_TIMBRE:
      return &profile->timbre_tracking;
    default:
      return &profile->bend_tracking;
  }
}

}  // namespace
#endif

const char* sonare_controller_profile_names(void) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
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
  SONARE_C_CATCH_RETURN("")
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
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "unknown controller profile preset");
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
  // works and listens to the wrong thing. Named one at a time because the field
  // is the whole diagnosis, and because the code alone cannot carry it -- it is
  // the same INVALID_PARAMETER an unbound destination returns.
  sonare::midi::ControllerInput input{};
  if (!ordinal_to_enum(binding->input, 0, SONARE_CONTROLLER_INPUT_COUNT - 1, &input)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "controller binding: input is not one of SonareControllerInput");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  sonare::midi::ControllerAxis axis{};
  if (!ordinal_to_enum(binding->axis, 0, SONARE_CONTROLLER_AXIS_COUNT - 1, &axis)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "controller binding: axis is not one of SonareControllerAxis");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (binding->index > 127) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "controller binding: index must be a MIDI controller number in [0, 127]");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  sonare::midi::ControllerProfile profile;
  const SonareError status = profile_of(engine, destination_id, &profile);
  if (status != SONARE_OK) return status;

  sonare::midi::ControllerBinding entry;
  entry.input = input;
  entry.index = binding->index;
  entry.axis = axis;
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

  // The binding itself is checked before the table, so a full table never hides a bad binding.
  if (axis == sonare::midi::ControllerAxis::kNone) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER, "controller binding: the axis is none");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (input == sonare::midi::ControllerInput::kPolyPressure &&
      !sonare::midi::controller_axis_is_excitation(axis)) {
    set_last_error(SONARE_ERROR_INVALID_PARAMETER,
                   "controller binding: a poly-pressure binding named a channel-level axis");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!profile.bind(entry)) {
    set_last_error(SONARE_ERROR_INVALID_STATE, "controller binding table is full");
    return SONARE_ERROR_INVALID_STATE;
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

SonareError sonare_engine_set_controller_note_tracking(SonareRealtimeEngine* engine,
                                                       uint32_t destination_id, int dimension,
                                                       int tracking) {
  SONARE_C_API_ENTRY;
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
  if (dimension < 0 || dimension >= SONARE_MPE_DIMENSION_COUNT) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Refused in every build, so garbage is not reported as a missing feature.
  if (!valid_c_enum(tracking, SONARE_NOTE_TRACKING_COUNT)) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  sonare::midi::ControllerProfile profile;
  const SonareError status = profile_of(engine, destination_id, &profile);
  if (status != SONARE_OK) return status;
  *note_tracking_field(&profile, dimension) =
      admitted_ordinal_to_enum<sonare::midi::NoteTracking>(tracking);
  return install(engine, destination_id, profile);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_controller_note_tracking(SonareRealtimeEngine* engine,
                                                   uint32_t destination_id, int dimension,
                                                   int* out_tracking) {
  SONARE_C_API_ENTRY;
  if (!engine || !out_tracking) return SONARE_ERROR_INVALID_PARAMETER;
  *out_tracking = SONARE_NOTE_TRACKING_LAST;
  if (dimension < 0 || dimension >= SONARE_MPE_DIMENSION_COUNT) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_ARRANGEMENT)
  (void)destination_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  sonare::midi::ControllerProfile profile;
  const SonareError status = profile_of(engine, destination_id, &profile);
  if (status != SONARE_OK) return status;
  *out_tracking = static_cast<int>(*note_tracking_field(&profile, dimension));
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}
