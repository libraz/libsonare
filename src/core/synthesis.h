#pragma once

/// @file synthesis.h
/// @brief Synthetic audio generators (tone, chirp, clicks).

#include <vector>

#include "core/audio.h"

namespace sonare {

/// @brief Generates a pure sinusoidal tone.
/// @details Output samples are `amplitude * sin(2*pi*frequency*t + phi)`
///          where `t = i / sr`. Length is `static_cast<int>(duration * sr)`.
/// @param frequency Tone frequency in Hz.
/// @param sr Sample rate in Hz (default 22050).
/// @param duration Duration in seconds (default 1.0).
/// @param phi Phase offset in radians (default 0).
/// @param amplitude Peak amplitude (default 1.0).
/// @return Mono Audio at the given sample rate.
Audio tone(float frequency, int sr = 22050, float duration = 1.0f, float phi = 0.0f,
           float amplitude = 1.0f);

/// @brief Generates a frequency-sweeping signal (linear or exponential).
/// @details Linear sweep: `f(t) = fmin + (fmax - fmin) * t / duration`.
///          Exponential sweep: `f(t) = fmin * (fmax / fmin) ** (t / duration)`.
///          The phase is the integral of `2*pi*f(t)`, evaluated at the same
///          time grid as `tone` (`t = i / sr`). Output samples are
///          `sin(phase)` so that an exponential sweep with `fmin == fmax`
///          collapses to a pure tone.
/// @param fmin Initial frequency in Hz (must be > 0 for exponential sweep).
/// @param fmax Final frequency in Hz (must share the sign of fmin for exp).
/// @param sr Sample rate (default 22050).
/// @param duration Duration in seconds (default 1.0).
/// @param linear If true (default), use a linear sweep, else exponential.
/// @return Mono Audio.
Audio chirp(float fmin, float fmax, int sr = 22050, float duration = 1.0f, bool linear = true);

/// @brief Generates a click track from a list of event times.
/// @details Each click is a logarithmically-decaying sine burst (matching
///          `librosa.clicks`): envelope `2^[0 .. -10]` modulated by
///          `sin(2*pi*frequency*t)`. Clicks past the end are truncated.
/// @param times Event times in seconds (sample positions = round(t * sr)).
/// @param sr Sample rate (default 22050).
/// @param length Output length in samples. If 0, length is chosen to fit the
///        last click in full.
/// @param frequency Click sine frequency in Hz (default 1000).
/// @param click_duration Click burst duration in seconds (default 0.1).
/// @return Mono Audio.
Audio clicks(const std::vector<float>& times, int sr = 22050, int length = 0,
             float frequency = 1000.0f, float click_duration = 0.1f);

}  // namespace sonare
