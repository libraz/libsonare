#pragma once

/// @file quick.h
/// @brief Simple function API for quick music analysis.
/// @details Provides stateless functions for common music analysis tasks.
/// These functions are designed for ease of use and WASM interoperability.

#include <cstddef>
#include <vector>

#include "analysis/acoustic_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/music_analyzer.h"

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

/// @brief Detects acoustic parameters from audio samples.
/// @param samples Pointer to audio samples (mono, float32)
/// @param size Number of samples
/// @param sample_rate Sample rate in Hz
/// @return Acoustic analysis result
AcousticParameters detect_acoustic(const float* samples, size_t size, int sample_rate);

/// @brief Computes acoustic parameters from impulse-response samples.
/// @param samples Pointer to IR samples (mono, float32)
/// @param size Number of samples
/// @param sample_rate Sample rate in Hz
/// @return Acoustic analysis result
AcousticParameters analyze_impulse_response(const float* samples, size_t size, int sample_rate);

}  // namespace quick
}  // namespace sonare
