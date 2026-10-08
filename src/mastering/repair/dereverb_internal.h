#pragma once

/// @file dereverb_internal.h
/// @brief The subtraction-mask pieces the offline and streaming dereverberators share.

#include "mastering/repair/dereverb_classical.h"

namespace sonare::mastering::repair::detail {

/// @brief Frames between a frame and the late-reverb frame its mask subtracts.
int dereverb_late_delay_frames(const DereverbClassicalConfig& config, int sample_rate);

/// @brief Power a tail keeps across @p delay_frames, `10^(-6 delay / T60)`.
double dereverb_late_decay(const DereverbClassicalConfig& config, int delay_frames,
                           int sample_rate);

/// @brief Gain of one cell from its power and its decayed late power.
/// @return 1 when the late power does not pass `threshold * current`; @p suppressed says whether
///   it did.
double dereverb_subtraction_gain(double current_power, double late_psd,
                                 const DereverbClassicalConfig& config, bool* suppressed);

}  // namespace sonare::mastering::repair::detail
