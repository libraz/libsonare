#pragma once

/// @file ppq_domains.h
/// @brief Numeric domains of every quarter-note (PPQ) argument a C entry takes, stated once for
///        the engine and project checks that refuse them and for numeric_domain_rows.h.

#include "transport/tempo_map.h"
#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// @brief What a timeline position, or a PPQ span that may be empty, accepts.
inline constexpr sonare::util::check::NumericDomain kPublicPpqDomain =
    sonare::util::check::real_domain(0.0, sonare::transport::kMaxPublicPpq);
/// @brief What a PPQ length that must cover some time accepts.
inline constexpr sonare::util::check::NumericDomain kPublicPpqLengthDomain =
    sonare::util::check::real_domain(0.0, false, sonare::transport::kMaxPublicPpq, true);

}  // namespace sonare_c_detail
