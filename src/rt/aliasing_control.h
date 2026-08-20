#pragma once

/// @file aliasing_control.h
/// @brief Shared nonlinear antialiasing mode selection.

namespace sonare::rt {

/// @brief How a nonlinear stage suppresses the aliasing its own harmonic
///   generation would otherwise fold below Nyquist.
/// @details Not every mode is implemented by every processor that accepts
///   this enum: a processor rejects a value it cannot honor from its config
///   validation rather than silently behaving as @c None. Consult the
///   processor's own header for which values it supports.
enum class AliasingControl {
  /// No antialiasing; the nonlinearity runs at the host sample rate.
  None,
  /// First-order antiderivative antialiasing (adds no more than half a
  /// sample of latency).
  Adaa1,
  /// Second-order antiderivative antialiasing (adds one sample of latency).
  Adaa2,
  /// Runs the nonlinearity at 4x the host sample rate through a polyphase
  /// interpolation/decimation pair, adding the pair's streaming round-trip
  /// latency (see @c rt::Oversampler::streaming_round_trip_latency_samples).
  Oversample4x,
};

}  // namespace sonare::rt
