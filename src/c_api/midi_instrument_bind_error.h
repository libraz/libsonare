#pragma once

#include "c_api/sonare_c_internal.h"

namespace sonare_c_detail {

#if defined(SONARE_WITH_ARRANGEMENT)
/// Records the C-ABI error for a failed RealtimeEngine instrument bind.
inline SonareError record_midi_instrument_bind_failure(
    const sonare::engine::RealtimeEngine& engine) noexcept {
  using Status = sonare::engine::MidiInstrumentBindStatus;
  switch (engine.last_midi_instrument_bind_status()) {
    case Status::kAlreadyBoundElsewhere:
      return run_validation([] {
        sonare::util::check::refuse("instrument",
                                    "must not already be bound to another destination");
      });
    case Status::kInstrumentPrepareFailed:
      return run_validation([] {
        sonare::util::check::refuse(
            "instrument", "must be preparable at the engine's sample rate and block size");
      });
    case Status::kRackFull:
      set_last_error_static(SONARE_ERROR_INVALID_STATE, "the instrument rack is full");
      return SONARE_ERROR_INVALID_STATE;
    case Status::kPreparationFailed:
      set_last_error_static(SONARE_ERROR_INVALID_STATE,
                            "the instrument could not prepare scheduled or queued SysEx");
      return SONARE_ERROR_INVALID_STATE;
    default:
      set_last_error_static(SONARE_ERROR_OUT_OF_MEMORY,
                            "out of memory while binding the instrument");
      return SONARE_ERROR_OUT_OF_MEMORY;
  }
}
#endif

}  // namespace sonare_c_detail
