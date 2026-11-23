#pragma once

/// @file playback_domains.h
/// @brief Numeric domains of the playback C-ABI arguments, stated once for the checks that refuse
///        them and for the table in numeric_domain_rows.h.

#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// @brief What an offline render's in_channels accepts; the input's sample cap bounds the product.
inline constexpr sonare::util::check::NumericDomain kPlaybackRenderInChannelsDomain =
    sonare::util::check::integer_domain_of<int>(1);

}  // namespace sonare_c_detail
