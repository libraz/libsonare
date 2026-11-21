#pragma once

/// @file mastering_input_domains.h
/// @brief Numeric domains of the mastering C-ABI arguments that no core validator sees, stated
///        once for the checks that refuse them and for numeric_domain_rows.h.

#include <sonare/sonare_c.h>

#include <cstddef>
#include <limits>

#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// @brief What the band index of an equalizer edit accepts.
inline constexpr sonare::util::check::NumericDomain kEqBandIndexDomain =
    sonare::util::check::integer_domain(0, static_cast<long long>(SONARE_EQ_MAX_BANDS) - 1);
/// @brief What the band count of a matching equalizer accepts.
inline constexpr sonare::util::check::NumericDomain kEqMatchMaxBandsDomain =
    sonare::util::check::integer_domain(1, static_cast<long long>(SONARE_EQ_MAX_BANDS));
/// @brief What the channel count of a channel-linked repair accepts.
inline constexpr sonare::util::check::NumericDomain kRepairChannelCountDomain =
    sonare::util::check::integer_domain_of<std::size_t>(1);
/// @brief What the FFT size of the noise-band grid accepts, a power of two in this range.
inline constexpr sonare::util::check::NumericDomain kNoiseBandFftSizeDomain =
    sonare::util::check::integer_domain(1, std::numeric_limits<int>::max());
/// @brief What the sample rate of the noise-band grid accepts.
inline constexpr sonare::util::check::NumericDomain kNoiseBandSampleRateDomain =
    sonare::util::check::integer_domain_of<int>(1);

}  // namespace sonare_c_detail
