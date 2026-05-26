#pragma once

/// @file audio_profile.h
/// @brief Mastering assistant audio profiling.

#include <cstddef>
#include <string>
#include <vector>

#include "core/audio.h"

namespace sonare::mastering::assistant {

struct LoudnessProfile {
  float integrated_lufs = 0.0f;
  float lra_lu = 0.0f;
  float true_peak_db = 0.0f;
  float crest_factor_db = 0.0f;
};

struct SpectralProfile {
  float sub_rms_db = 0.0f;       ///< 20-60 Hz
  float low_rms_db = 0.0f;       ///< 60-250 Hz
  float low_mid_rms_db = 0.0f;   ///< 250-500 Hz
  float mid_rms_db = 0.0f;       ///< 500-2000 Hz
  float high_mid_rms_db = 0.0f;  ///< 2-6 kHz
  float high_rms_db = 0.0f;      ///< 6-12 kHz
  float air_rms_db = 0.0f;       ///< 12 kHz-Nyquist
  float centroid_hz = 0.0f;
  float flatness = 0.0f;
  float rolloff_hz = 0.0f;
};

struct DynamicsProfile {
  float short_term_lufs_std = 0.0f;
  float attack_density = 0.0f;  ///< Onset peaks per second.
  float sustain_ratio = 0.0f;   ///< 0 = transient-heavy, 1 = sustained.
};

struct GenreCandidate {
  std::string name;
  float score = 0.0f;
};

struct AudioProfile {
  float duration_sec = 0.0f;
  float bpm = 0.0f;
  float bpm_confidence = 0.0f;
  LoudnessProfile loudness{};
  SpectralProfile spectral{};
  DynamicsProfile dynamics{};
  std::vector<GenreCandidate> genre_candidates;
};

struct AudioProfileConfig {
  int n_fft = 2048;
  int hop_length = 512;
  int true_peak_oversample = 4;
};

AudioProfile analyze_audio_profile(const float* samples, std::size_t length, int sample_rate,
                                   const AudioProfileConfig& config = {});
AudioProfile analyze_audio_profile(const Audio& audio, const AudioProfileConfig& config = {});
std::string audio_profile_to_json(const AudioProfile& profile);

}  // namespace sonare::mastering::assistant
