#pragma once

/// @file warp_detail.h
/// @brief Internal entry points into the chroma-DTW alignment, for tests.
///
/// Deliberately in src/ rather than include/sonare/: nothing here is installed,
/// so the C ABI, the parity map and every binding stay unaware of it. It exists
/// because the banded DP and the column-norm reduction it depends on are
/// otherwise reachable only through a multi-second full alignment, which leaves
/// them with no coverage in the default test run.

#include <utility>
#include <vector>

namespace sonare::mir::detail {

/// @brief Per-column L2 norms of a chroma matrix [n_chroma x frames], row-major.
///
/// Each column sums over c in ascending order. That order is part of the
/// contract: the norms become the denominator of a cosine distance whose value
/// selects a discrete DTW path index, so a reassociation here is observable.
std::vector<double> chroma_column_norms(const std::vector<float>& m, int n_chroma, int frames);

/// @brief Banded DTW over two chroma matrices; the band rules are at the definition.
std::vector<std::pair<int, int>> banded_dtw_path(const std::vector<float>& ref, int n_chroma,
                                                 int ref_frames, const std::vector<float>& tgt,
                                                 int tgt_frames,
                                                 const std::vector<int>& projected_tgt,
                                                 int band_radius);

}  // namespace sonare::mir::detail
