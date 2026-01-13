#pragma once

/// @file pitch.h
/// @brief Pitch (fundamental frequency) detection using YIN and pYIN algorithms.

#include <vector>

#include "core/audio.h"

namespace sonare {

/// @brief Pitch detection configuration.
struct PitchConfig {
  int frame_length = 2048;  ///< Frame length in samples
  int hop_length = 512;     ///< Hop length in samples
  float fmin = 65.0f;       ///< Minimum frequency in Hz (C2)
  float fmax = 2093.0f;     ///< Maximum frequency in Hz (C7)
  float threshold = 0.3f;   ///< YIN threshold for voiced/unvoiced
  bool fill_na = false;     ///< Fill unvoiced frames with 0 (otherwise NaN)
};

/// @brief Pitch detection result.
struct PitchResult {
  std::vector<float> f0;           ///< Fundamental frequency for each frame [n_frames]
  std::vector<float> voiced_prob;  ///< Voicing probability [n_frames]
  std::vector<bool> voiced_flag;   ///< Binary voiced flag [n_frames]

  /// @brief Returns number of frames.
  int n_frames() const { return static_cast<int>(f0.size()); }

  /// @brief Returns median F0 of voiced frames.
  float median_f0() const;

  /// @brief Returns mean F0 of voiced frames.
  float mean_f0() const;
};

/// @brief Computes YIN difference function.
/// @param frame Audio frame
/// @param frame_length Length of frame
/// @param max_lag Maximum lag (tau) to compute
/// @return Difference function values [max_lag]
std::vector<float> yin_difference(const float* frame, int frame_length, int max_lag);

/// @brief Computes cumulative mean normalized difference function (CMNDF).
/// @param diff Difference function from yin_difference()
/// @return CMNDF values [same size as diff]
std::vector<float> yin_cmndf(const std::vector<float>& diff);

/// @brief Finds the best pitch period using parabolic interpolation.
/// @param cmndf Cumulative mean normalized difference function
/// @param threshold YIN threshold
/// @param min_period Minimum period in samples
/// @param max_period Maximum period in samples
/// @return Best period in samples (fractional), or 0 if unvoiced
float yin_find_pitch(const std::vector<float>& cmndf, float threshold, int min_period,
                     int max_period);

/// @brief Detects pitch for a single frame using YIN algorithm.
/// @param frame Audio frame
/// @param frame_length Length of frame
/// @param sr Sample rate in Hz
/// @param fmin Minimum frequency in Hz
/// @param fmax Maximum frequency in Hz
/// @param threshold YIN threshold (0.0 to 1.0)
/// @return Detected frequency in Hz (0 if unvoiced)
float yin(const float* frame, int frame_length, int sr, float fmin = 65.0f, float fmax = 2093.0f,
          float threshold = 0.3f);

/// @brief Detects pitch for a single frame with confidence.
/// @param frame Audio frame
/// @param frame_length Length of frame
/// @param sr Sample rate in Hz
/// @param fmin Minimum frequency in Hz
/// @param fmax Maximum frequency in Hz
/// @param threshold YIN threshold
/// @param out_confidence Output: voicing confidence (0 to 1)
/// @return Detected frequency in Hz (0 if unvoiced)
float yin_with_confidence(const float* frame, int frame_length, int sr, float fmin, float fmax,
                          float threshold, float* out_confidence);

/// @brief Detects pitch using YIN algorithm (frame-by-frame).
/// @param audio Input audio
/// @param config Pitch configuration
/// @return Pitch detection result
PitchResult yin_track(const Audio& audio, const PitchConfig& config = PitchConfig());

/// @brief Detects pitch using pYIN algorithm with HMM smoothing.
/// @param audio Input audio
/// @param config Pitch configuration
/// @return Pitch detection result
/// @details pYIN uses probabilistic multiple pitch candidates and Viterbi
/// decoding for more robust pitch tracking than standard YIN.
PitchResult pyin(const Audio& audio, const PitchConfig& config = PitchConfig());

/// @brief Converts frequency to MIDI note number.
/// @param freq Frequency in Hz
/// @return MIDI note number (A4 = 69)
float freq_to_midi(float freq);

/// @brief Converts MIDI note number to frequency.
/// @param midi MIDI note number
/// @return Frequency in Hz
float midi_to_freq(float midi);

}  // namespace sonare
