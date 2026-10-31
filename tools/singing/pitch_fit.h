#pragma once

/// @file pitch_fit.h
/// @brief The measurement behind singing_pitch_compensation.inc: the F0 the
///        singing assembly sounds at a force and a tension, and the tension that
///        sounds a note. Shared by the generator's sweep tool and the drift test,
///        so both measure the one SingingPhonation the engine renders.

namespace sonare::tools::singing {

/// Rate the table is measured at.
inline constexpr double kFitSampleRate = 48000.0;

/// F0 (Hz) of SingingPhonation at the patch defaults voicing @p note_hz, held at
/// @p force and @p tension, over a window after the onset has settled; 0 when
/// it does not phonate.
double assembly_f0(float note_hz, float force, float tension);

/// Tension at which the assembly sounds @p note_hz at @p force, searched from
/// @p guess; negative when no tension in reach phonates there.
float fit_tension(float note_hz, float force, float guess);

}  // namespace sonare::tools::singing
