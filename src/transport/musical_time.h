#pragma once

/// @file musical_time.h
/// @brief Helpers for PPQ, samples, and musical note values.

#include <cstdint>

namespace sonare::transport {

enum class NoteModifier {
  kStraight,
  kDotted,
  kTriplet,
};

double note_length_ppq(int denominator, NoteModifier modifier = NoteModifier::kStraight) noexcept;
int64_t ppq_duration_to_samples(double ppq, double bpm, double sample_rate) noexcept;
double samples_to_ppq_duration(int64_t samples, double bpm, double sample_rate) noexcept;

}  // namespace sonare::transport
