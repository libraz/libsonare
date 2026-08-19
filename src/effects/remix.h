#pragma once

/// @file remix.h
/// @brief Time-domain remixing of audio segments
///        (librosa.effects.remix compatible).

#include <cstddef>
#include <utility>
#include <vector>

namespace sonare {

/// @brief Resolves the cut points a remix would use.
/// @details Returns one clamped `(start, end)` pair per input interval, in
///          order. With `align_zeros` false the intervals are returned clamped
///          to `[0, n]` and otherwise unchanged. With `align_zeros` true each
///          boundary is snapped to the nearest zero-crossing of `y`, matching
///          `librosa.effects.remix` — with two guards that keep a slice from
///          disappearing:
///
///          - a signal with NO sign change at all (silence, a DC offset, a
///            constant) is not snapped. librosa's zero-crossing set degenerates
///            to the two padded sentinels {0, n} there, which collapses every
///            slice to nothing.
///          - a slice that survives clamping but collapses to empty after
///            snapping falls back to its unsnapped boundaries. Sparse crossings
///            (an impulse train) otherwise pull both ends onto the same point.
///
///          Snapping is a per-signal decision, so a host that needs the SAME
///          cut points across a multichannel take resolves them once here and
///          applies them to every channel itself; calling @ref remix per
///          channel snaps each channel independently and drifts them apart.
/// @param y Input signal.
/// @param n Number of samples.
/// @param intervals Sequence of (start, end) sample boundaries.
/// @param align_zeros Snap to zero-crossings (default true).
/// @return One resolved (start, end) pair per input interval; a slice that is
///         empty after clamping is returned as `(start, start)`.
/// @throw sonare::SonareException on null input with n > 0.
std::vector<std::pair<int, int>> align_remix_intervals(
    const float* y, std::size_t n, const std::vector<std::pair<int, int>>& intervals,
    bool align_zeros = true);
std::vector<std::pair<int, int>> align_remix_intervals(
    const std::vector<float>& y, const std::vector<std::pair<int, int>>& intervals,
    bool align_zeros = true);

/// @brief Reorders / concatenates a signal by interval slices.
/// @details Each interval `(start, end)` selects samples `y[start..end)`
///          (end-exclusive). The output is the concatenation of all such
///          slices in order. When `align_zeros` is true, both `start` and
///          `end` are snapped as described in @ref align_remix_intervals.
/// @param y Input signal.
/// @param n Number of samples.
/// @param intervals Sequence of (start, end) sample boundaries.
/// @param align_zeros Snap to zero-crossings (default true).
/// @return Remixed signal.
/// @throw sonare::SonareException on null input with n > 0 or invalid intervals.
std::vector<float> remix(const float* y, std::size_t n,
                         const std::vector<std::pair<int, int>>& intervals,
                         bool align_zeros = true);
std::vector<float> remix(const std::vector<float>& y,
                         const std::vector<std::pair<int, int>>& intervals,
                         bool align_zeros = true);

}  // namespace sonare
