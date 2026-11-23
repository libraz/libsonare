#pragma once

#include <cstdint>

#include "automation/automation_lane.h"
#include "engine/track_mixer.h"
#include "mixing/panner.h"
#include "mixing/stereo_width.h"
#include "util/constants.h"
#include "util/parameter_checks.h"
#if defined(SONARE_WITH_MIXING)
#include "engine/mixing_runtime.h"
#endif

namespace sonare::arrangement {

/// Refuses a point value outside the domain of the lane's target.
inline void check_automation_lane_value(sonare::automation::AutomationTargetKind kind,
                                        uint32_t param_id, float value, const char* subject) {
  namespace check = sonare::util::check;
  if (kind == sonare::automation::AutomationTargetKind::kTrackPan) {
    check::in_range(value, sonare::mixing::kPanMin, sonare::mixing::kPanMax, subject);
    return;
  }
  if (kind == sonare::automation::AutomationTargetKind::kTrackWidth) {
    check::in_range(value, sonare::mixing::kWidthMin, sonare::mixing::kWidthMax, subject);
    return;
  }
  if (kind == sonare::automation::AutomationTargetKind::kTrackSendDb) {
    check::in_range(value, sonare::constants::kFloorDb,
                    sonare::engine::TrackMixerRuntime::kMaxGainDb, subject);
    return;
  }
#if defined(SONARE_WITH_MIXING)
  if (kind == sonare::automation::AutomationTargetKind::kOpaque) {
    if (param_id == sonare::engine::MixingRuntime::kPan) {
      check::in_range(value, sonare::mixing::kPanMin, sonare::mixing::kPanMax, subject);
      return;
    }
    if (param_id == sonare::engine::MixingRuntime::kWidth) {
      check::in_range(value, sonare::mixing::kWidthMin, sonare::mixing::kWidthMax, subject);
      return;
    }
  }
#else
  (void)param_id;
#endif
  check::finite_float(value, subject);
}

}  // namespace sonare::arrangement
