#pragma once

/// @file synthesis.h
/// @brief Synthetic audio generators (tone, chirp, clicks).

#include <vector>

#include "core/audio.h"
#include "util/constants.h"

namespace sonare {

/// Length of a generated signal when a caller gives none, in seconds.
inline constexpr float kDefaultSynthesisDurationSec = 1.0f;
/// tone's phase offset when a caller gives none, in radians.
inline constexpr float kDefaultTonePhase = 0.0f;
/// tone's peak amplitude when a caller gives none.
inline constexpr float kDefaultToneAmplitude = 1.0f;
/// clicks' output length meaning "fit the last click in full".
inline constexpr int kClicksFitLength = 0;
/// Sine frequency of a click burst when a caller gives none, in hertz.
inline constexpr float kDefaultClickFrequencyHz = 1000.0f;
/// Length of a click burst when a caller gives none, in seconds.
inline constexpr float kDefaultClickDurationSec = 0.1f;

/// @brief Generates a pure sinusoidal tone.
/// @details Output samples are `amplitude * sin(2*pi*frequency*t + phi)`
///          where `t = i / sr`. Length is `static_cast<int>(duration * sr)`.
/// @param frequency Tone frequency in Hz.
/// @param sr Sample rate in Hz (default 22050).
/// @param duration Duration in seconds (default 1.0).
/// @param phi Phase offset in radians (default 0).
/// @param amplitude Peak amplitude (default 1.0).
/// @return Mono Audio at the given sample rate.
Audio tone(float frequency, int sr = constants::kDefaultSampleRate,
           float duration = kDefaultSynthesisDurationSec, float phi = kDefaultTonePhase,
           float amplitude = kDefaultToneAmplitude);

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
Audio chirp(float fmin, float fmax, int sr = constants::kDefaultSampleRate,
            float duration = kDefaultSynthesisDurationSec, bool linear = true);

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
Audio clicks(const std::vector<float>& times, int sr = constants::kDefaultSampleRate,
             int length = kClicksFitLength, float frequency = kDefaultClickFrequencyHz,
             float click_duration = kDefaultClickDurationSec);

}  // namespace sonare
