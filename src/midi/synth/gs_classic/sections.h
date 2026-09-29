#pragma once

/// @file sections.h
/// @brief Section designer of the GS classic realization: one stage as second-order rows.
///
/// A port of the soundings `section_sos` family at 32 kHz in double precision. Shelf
/// (first or second order), peaking, pole (bilinear or one-multiply, with an optional
/// resonance) and loop-free all-pass chain stages, each optionally reached by a mix
/// against a section stored at full scale. `form` changes only a resonance-free pole;
/// the other stages have one design each. A stage with nothing to do is one unity row.

#include <cstddef>

#include "midi/synth/gs_classic/model_format.h"

namespace sonare::midi::synth::gs_classic {

/// Doubles per designed row: `b0, b1, b2, a1, a2`, normalized so that a0 = 1.
inline constexpr std::size_t kGsClassicSectionRowSize = 5;

/// The quantities a stage is designed from at one sample; fields the stage lacks are ignored.
struct GsClassicSectionValues {
  double corner_hz = 0.0;  ///< centre of a peaking stage
  double gain_db = 0.0;
  double q = 0.0;
  bool has_q = false;
  double count = 0.0;  ///< pole cascades and all-pass sections; truncated toward zero
  double mix = 0.0;
};

/// Rows a stage can need when its count never exceeds `largest_count`.
std::size_t gs_classic_section_capacity(const GsClassicSection& section,
                                        double largest_count) noexcept;

/// Designs a stage into `rows` (kGsClassicSectionRowSize doubles each); returns the row
/// count, or 0 when `capacity` rows are too few. `reached` is the stage's `reached_by`, or null.
std::size_t gs_classic_design_section(const GsClassicSection& section,
                                      const GsClassicReachedBy* reached,
                                      const GsClassicSectionValues& values, double* rows,
                                      std::size_t capacity) noexcept;

}  // namespace sonare::midi::synth::gs_classic
