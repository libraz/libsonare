#pragma once

/// @file segment_input_domains.h
/// @brief Numeric domains of the C-ABI segmentation entries' arguments that no core validator
///        sees, stated once for the checks that refuse them and for numeric_domain_rows.h.

#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// @brief What a row or column count of a segmentation matrix argument accepts.
inline constexpr sonare::util::check::NumericDomain kSegmentMatrixExtentDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What the neighbour count of a recurrence or cross-similarity matrix accepts.
inline constexpr sonare::util::check::NumericDomain kSegmentNeighbourCountDomain =
    sonare::util::check::integer_domain_of<int>(0);
/// @brief What the diagonal exclusion width of a recurrence matrix accepts.
inline constexpr sonare::util::check::NumericDomain kSegmentRecurrenceWidthDomain =
    sonare::util::check::integer_domain_of<int>(0);
/// @brief What the subsegment count per segment accepts.
inline constexpr sonare::util::check::NumericDomain kSubsegmentCountDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What the cluster count of agglomerative segmentation accepts.
inline constexpr sonare::util::check::NumericDomain kAgglomerativeClusterCountDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What path enhancement's window length and filter count accept.
inline constexpr sonare::util::check::NumericDomain kPathEnhanceWindowDomain =
    sonare::util::check::integer_domain_of<int>(1);
inline constexpr sonare::util::check::NumericDomain kPathEnhanceFilterCountDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What path enhancement's tempo ratio bounds accept.
inline constexpr sonare::util::check::NumericDomain kPathEnhanceMaxRatioDomain =
    sonare::util::check::integer_domain_of<int>(1);
inline constexpr sonare::util::check::NumericDomain kPathEnhanceMinRatioDomain =
    sonare::util::check::integer_domain_of<int>(0);

}  // namespace sonare_c_detail
