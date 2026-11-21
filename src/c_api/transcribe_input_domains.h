#pragma once

/// @file transcribe_input_domains.h
/// @brief Numeric domains of the C-ABI transcription and polyphonic-extraction config fields,
///        stated once for the checks that refuse them and for numeric_domain_rows.h. A NaN float
///        or an UNSET integer is absent and never reaches these checks.

#include "editing/polyphony/multi_f0.h"
#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// @brief What a frequency of the transcription config (reference, F0 bounds) accepts, in Hz.
inline constexpr sonare::util::check::NumericDomain kTranscribeFrequencyHzDomain =
    sonare::util::check::real_domain_above(0.0, false);
/// @brief What the shortest kept note accepts, in milliseconds.
inline constexpr sonare::util::check::NumericDomain kTranscribeMinNoteMsDomain =
    sonare::util::check::real_domain_above(0.0, false);
/// @brief What the pitch distance that splits a note accepts, in cents.
inline constexpr sonare::util::check::NumericDomain kTranscribeSegmentationCentsDomain =
    sonare::util::check::real_domain_above(0.0, false);
/// @brief What the level mapped to the softest velocity accepts, in dB below full scale.
inline constexpr sonare::util::check::NumericDomain kTranscribeVelocityFloorDbDomain =
    sonare::util::check::real_domain_below(0.0, false);
/// @brief What a fixed note velocity accepts.
inline constexpr sonare::util::check::NumericDomain kTranscribeFixedVelocityDomain =
    sonare::util::check::integer_domain(1, 127);
/// @brief What the voice count of polyphonic transcription accepts.
inline constexpr sonare::util::check::NumericDomain kTranscribeMaxPolyphonyDomain =
    sonare::util::check::integer_domain(1, sonare::editing::polyphony::kMaxPolyphonyVoices);
/// @brief What a peak ratio (frame or ridge) of polyphonic transcription accepts.
inline constexpr sonare::util::check::NumericDomain kTranscribePeakRatioDomain =
    sonare::util::check::real_domain(0.0, 1.0);
/// @brief What the shortest note as a division of a whole note accepts.
inline constexpr sonare::util::check::NumericDomain kTranscribeMinNoteDivisionDomain =
    sonare::util::check::integer_domain(1, 128);
/// @brief What the level rise that splits a sustained ridge into a new note accepts.
inline constexpr sonare::util::check::NumericDomain kReattackRatioDomain =
    sonare::util::check::real_domain_above(1.0, false);
/// @brief What the frequency refinement limit of polyphonic extraction accepts, in Hz.
inline constexpr sonare::util::check::NumericDomain kPolyphonicMaxRefineHzDomain =
    sonare::util::check::real_domain_above(0.0, false);

}  // namespace sonare_c_detail
