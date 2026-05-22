#pragma once

/// @file constants.h
/// @brief Universal numeric constants for libsonare.
/// @details Header-only, dependency-free. Provides both float and double variants
///          (suffix `D` for double) where precision matters (e.g., biquad coefficient design,
///          K-weighting filters). Music and dB constants are float-only.

namespace sonare {
namespace constants {

// ============================================================
// Mathematical constants
// ============================================================
inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr double kPiD = 3.14159265358979323846;
inline constexpr float kTwoPi = 6.28318530717958647692f;
inline constexpr double kTwoPiD = 6.28318530717958647692;
inline constexpr float kHalfPi = 1.57079632679489661923f;
inline constexpr double kHalfPiD = 1.57079632679489661923;
inline constexpr float kInvPi = 0.31830988618379067154f;
inline constexpr double kInvPiD = 0.31830988618379067154;

inline constexpr float kSqrt2 = 1.41421356237309504880f;
inline constexpr double kSqrt2D = 1.41421356237309504880;
inline constexpr float kInvSqrt2 = 0.70710678118654752440f;
inline constexpr double kInvSqrt2D = 0.70710678118654752440;

inline constexpr float kLn2 = 0.69314718055994530942f;
inline constexpr double kLn2D = 0.69314718055994530942;
inline constexpr float kLog10Of2 = 0.30102999566398119521f;
inline constexpr double kLog10Of2D = 0.30102999566398119521;

// ============================================================
// Audio / DSP
// ============================================================
inline constexpr float kEpsilon = 1e-10f;         ///< Generic near-zero epsilon
inline constexpr float kSpectrumEpsilon = 1e-8f;  ///< Larger epsilon for log-magnitude operations
inline constexpr float kFloorDb = -120.0f;        ///< Numerical dB floor
inline constexpr double kFloorDbD = -120.0;
inline constexpr float kSilenceDb = -80.0f;  ///< Silence threshold

// ============================================================
// Music
// ============================================================
inline constexpr float kA4Hz = 440.0f;
inline constexpr float kMidiA4 = 69.0f;
inline constexpr float kSemitonesPerOctave = 12.0f;
inline constexpr float kCentsPerOctave = 1200.0f;
inline constexpr float kCentsPerSemitone = 100.0f;
inline constexpr float kButterworthQ =
    0.70710678118654752440f;  ///< 1/sqrt(2), Butterworth response Q
inline constexpr double kButterworthQD = 0.70710678118654752440;

// ============================================================
// librosa-compatible defaults (reference only, not enforced)
// ============================================================
inline constexpr int kDefaultSampleRate = 22050;
inline constexpr int kDefaultNFft = 2048;
inline constexpr int kDefaultHopLength = 512;
inline constexpr int kDefaultNMels = 128;

}  // namespace constants
}  // namespace sonare
