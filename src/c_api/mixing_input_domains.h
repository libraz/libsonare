#pragma once

/// @file mixing_input_domains.h
/// @brief Numeric domains of the C-ABI mixing entries' arguments that no core validator sees,
///        stated once for the checks that refuse them and for numeric_domain_rows.h.

#include "core/audio.h"
#include "mixing/automation_lane.h"
#include "streaming/realtime_limits.h"
#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// @brief What the sample rate a mixing assistant request names for its output accepts: the
///        audio rates, since the suggestion configures a mixer running at it.
inline constexpr sonare::util::check::NumericDomain kMixingAssistantSampleRateDomain =
    sonare::kAudioSampleRateDomain;
/// @brief What the sample rate of one assistant input track accepts.
inline constexpr sonare::util::check::NumericDomain kMixingAssistantTrackSampleRateDomain =
    sonare::kAudioSampleRateDomain;
/// @brief What a mixer's max_block_size accepts; 0 takes the mixer default.
inline constexpr sonare::util::check::NumericDomain kMixerMaxBlockSizeDomain =
    sonare::util::check::with_sentinel(
        sonare::util::check::integer_domain(1, sonare::kMaxRealtimeBlockSize), 0);
/// @brief What the curve of a scheduled automation event accepts.
inline constexpr sonare::util::check::NumericDomain kStripAutomationCurveDomain =
    sonare::util::check::integer_domain(
        0, static_cast<int>(sonare::mixing::AutomationCurveType::SCurve));
/// @brief What the parameter id of a scheduled insert automation event accepts.
inline constexpr sonare::util::check::NumericDomain kStripInsertAutomationParamIdDomain =
    sonare::util::check::integer_domain(0, 65535u);

}  // namespace sonare_c_detail
