#pragma once

/// @file feature_input_domains.h
/// @brief Numeric domains of the C-ABI feature and audio entries' input shapes that no core
///        validator sees, stated once for the checks that refuse them and for the table in
///        numeric_domain_rows.h.

#include <limits>

#include "core/audio.h"
#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// @brief What the chord count given to the roman-numeral and function labelling accepts.
inline constexpr sonare::util::check::NumericDomain kChordLabelCountDomain =
    sonare::util::check::integer_domain(0, static_cast<long long>(sonare::kMaxAudioBufferSize));
/// @brief What the frame count of a planar downmix accepts.
inline constexpr sonare::util::check::NumericDomain kDownmixFramesDomain =
    sonare::util::check::integer_domain_at_least(1);
/// @brief What the channel count of a planar downmix accepts.
inline constexpr sonare::util::check::NumericDomain kDownmixChannelCountDomain =
    sonare::util::check::integer_domain(1, std::numeric_limits<int>::max());
/// @brief What the channel index of a single-channel file load accepts.
inline constexpr sonare::util::check::NumericDomain kAudioChannelIndexDomain =
    sonare::util::check::integer_domain_of<int>(0);
/// @brief What mel_delta's feature and frame counts accept.
inline constexpr sonare::util::check::NumericDomain kMelDeltaShapeDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What the frame count of note segmentation accepts.
inline constexpr sonare::util::check::NumericDomain kNoteSegmentFrameCountDomain =
    sonare::util::check::integer_domain(1, std::numeric_limits<int>::max());

}  // namespace sonare_c_detail
