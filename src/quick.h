#pragma once

/// @file quick.h
/// @brief Simple function API for quick music analysis.
/// @details Provides stateless functions for common music analysis tasks.
/// These functions are designed for ease of use and WASM interoperability.
///
/// Every entry here resamples to the analysis rate before measuring, which is
/// the property that makes them interchangeable with each other and NOT
/// interchangeable with the core call they wrap. An analysis that is meant to
/// run at the caller's rate therefore has no entry in this file rather than an
/// entry that skips the resample, because a reader deciding whether a pinned
/// route exists reads the file and stops at the first name that matches.

#include <cstddef>
#include <vector>

#include "analysis/key_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/onset_analyzer.h"

namespace sonare {
namespace quick {

/// @brief Detects BPM from audio samples.
/// @param samples Pointer to audio samples (mono, float32)
/// @param size Number of samples
/// @param sample_rate Sample rate in Hz
/// @return Estimated BPM
float detect_bpm(const float* samples, size_t size, int sample_rate);

/// @brief Detects musical key from audio samples.
/// @param samples Pointer to audio samples (mono, float32)
/// @param size Number of samples
/// @param sample_rate Sample rate in Hz
/// @return Detected key
Key detect_key(const float* samples, size_t size, int sample_rate);

/// @brief Detects musical key from audio samples with explicit configuration.
Key detect_key(const float* samples, size_t size, int sample_rate, const KeyConfig& config);

/// @brief Returns ranked musical key candidates from audio samples.
/// @param samples Pointer to audio samples (mono, float32)
/// @param size Number of samples
/// @param sample_rate Sample rate in Hz
/// @param config Key analysis configuration
/// @return Sorted key candidates with raw profile correlation.
std::vector<KeyCandidate> detect_key_candidates(const float* samples, size_t size, int sample_rate,
                                                const KeyConfig& config = KeyConfig());

/// @brief Detects onset times from audio samples.
/// @param samples Pointer to audio samples (mono, float32)
/// @param size Number of samples
/// @param sample_rate Sample rate in Hz
/// @return Vector of onset times in seconds
std::vector<float> detect_onsets(const float* samples, size_t size, int sample_rate);

/// @brief Detects onset times with an explicit detector configuration.
/// @param samples Pointer to audio samples (mono, float32)
/// @param size Number of samples
/// @param sample_rate Sample rate in Hz
/// @param config Detector configuration
/// @return Vector of onset times in seconds
/// @details Resamples above the analysis rate exactly as the form without a
///          configuration does, so the two differ only in the configuration.
///          Every peak-picking field is counted in FRAMES and a frame is
///          hop_length divided by the rate in force, so a form that skipped the
///          resample would silently reinterpret all of them -- a
///          default-filled config would then read as a no-op and return a
///          different answer.
std::vector<float> detect_onsets(const float* samples, size_t size, int sample_rate,
                                 const OnsetDetectConfig& config);

/// @brief Detects beat times from audio samples.
/// @param samples Pointer to audio samples (mono, float32)
/// @param size Number of samples
/// @param sample_rate Sample rate in Hz
/// @return Vector of beat times in seconds
std::vector<float> detect_beats(const float* samples, size_t size, int sample_rate);

/// @brief Detects downbeat times from audio samples.
/// @param samples Pointer to audio samples (mono, float32)
/// @param size Number of samples
/// @param sample_rate Sample rate in Hz
/// @return Vector of downbeat times in seconds
std::vector<float> detect_downbeats(const float* samples, size_t size, int sample_rate);

/// @brief Performs complete music analysis.
/// @param samples Pointer to audio samples (mono, float32)
/// @param size Number of samples
/// @param sample_rate Sample rate in Hz
/// @return Complete analysis result
AnalysisResult analyze(const float* samples, size_t size, int sample_rate);

}  // namespace quick
}  // namespace sonare
