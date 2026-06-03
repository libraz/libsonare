#pragma once

/// @file grid_snap.h
/// @brief Pure PPQ grid-snapping helpers (beat / bar / subdivision).
///
/// OFFLINE, control-plane-only (subsystem sonare::mir). These are pure
/// functions over PPQ coordinates and a tempo/time-signature grid. They
/// transform coordinate INPUT that a caller subsequently feeds to a project edit
/// command; they do NOT emit commands and do NOT change command semantics.
///
/// All functions are deterministic and free of side effects.

#include <vector>

#include "transport/tempo_map.h"

namespace sonare::mir {

/// @brief A musical grid derived from a tempo map's time signature.
///
/// The grid is defined purely in PPQ (quarter-note) space, matching
/// transport::TempoMap's convention (1 ppq == 1 quarter note). It needs only
/// the time signature at the region of interest, plus an optional bar origin.
struct SnapGrid {
  /// Active time signature (numerator / denominator).
  transport::TimeSignature time_sig{};
  /// PPQ position of bar 1 / beat 1 (the grid origin). Usually 0.
  double origin_ppq = 0.0;
};

/// @brief Builds a SnapGrid from a TempoMap at a given ppq (reads its meter).
SnapGrid make_grid(const transport::TempoMap& tempo_map, double ppq);

/// @brief Length of one beat (the time-signature's denominator unit) in PPQ.
double beat_length_ppq(const SnapGrid& grid) noexcept;

/// @brief Length of one bar in PPQ (numerator beats).
double bar_length_ppq(const SnapGrid& grid) noexcept;

/// @brief Snaps a PPQ coordinate to the nearest beat line.
/// @param grid Grid definition.
/// @param ppq Input coordinate.
/// @param strength Snap strength in [0, 1]; 0 returns ppq unchanged, 1 returns
///        the exact grid line, intermediate values interpolate linearly.
double snap_to_beat(const SnapGrid& grid, double ppq, double strength = 1.0) noexcept;

/// @brief Snaps a PPQ coordinate to the nearest bar line.
double snap_to_bar(const SnapGrid& grid, double ppq, double strength = 1.0) noexcept;

/// @brief Snaps a PPQ coordinate to a subdivision of the beat.
/// @param division Subdivisions per beat (e.g. 2 = eighth, 4 = sixteenth,
///        3 = triplet eighth). Values <= 0 are treated as 1 (beat grid).
double snap_to_subdivision(const SnapGrid& grid, double ppq, int division,
                           double strength = 1.0) noexcept;

/// @brief Snaps a list of PPQ coordinates to the beat grid (vectorized helper).
std::vector<double> snap_to_beat(const SnapGrid& grid, const std::vector<double>& ppqs,
                                 double strength = 1.0);

/// @brief Snaps a list of PPQ coordinates to a subdivision grid.
std::vector<double> snap_to_subdivision(const SnapGrid& grid, const std::vector<double>& ppqs,
                                        int division, double strength = 1.0);

}  // namespace sonare::mir
