#pragma once

/// @file boundary_detector.h
/// @brief Section boundary detection using self-similarity analysis.

#include <vector>

#include "core/audio.h"

namespace sonare {

// Forward declarations
class MelSpectrogram;
class Chroma;

/// @brief Configuration for boundary detection.
struct BoundaryConfig {
  int n_fft = 2048;            ///< FFT size
  int hop_length = 512;        ///< Hop length
  int kernel_size = 64;        ///< Checkerboard kernel size in frames
  float threshold = 0.3f;      ///< Novelty threshold for boundary detection
  int n_mfcc = 13;             ///< Number of MFCC coefficients
  int n_chroma = 12;           ///< Number of chroma bins
  float peak_distance = 2.0f;  ///< Minimum distance between peaks in seconds
  bool use_mfcc = true;        ///< Use MFCC features
  bool use_chroma = true;      ///< Use chroma features
};

/// @brief Detected boundary event.
struct Boundary {
  float time;      ///< Boundary time in seconds
  int frame;       ///< Boundary frame index
  float strength;  ///< Boundary strength (novelty score)
};

/// @brief Boundary detector for finding section transitions.
/// @details Uses MFCC and chroma features to compute self-similarity matrix,
/// then applies checkerboard kernel convolution to detect structural boundaries.
class BoundaryDetector {
 public:
  /// @brief Constructs boundary detector from audio.
  /// @param audio Input audio
  /// @param config Boundary detection configuration
  explicit BoundaryDetector(const Audio& audio, const BoundaryConfig& config = BoundaryConfig());

  /// @brief Constructs boundary detector from pre-computed features.
  /// @param mel Pre-computed mel spectrogram
  /// @param chroma Pre-computed chroma
  /// @param sr Sample rate
  /// @param config Boundary detection configuration
  BoundaryDetector(const MelSpectrogram& mel, const Chroma& chroma, int sr,
                   const BoundaryConfig& config = BoundaryConfig());

  /// @brief Returns detected boundaries with timing.
  const std::vector<Boundary>& boundaries() const { return boundaries_; }

  /// @brief Returns boundary times in seconds.
  std::vector<float> boundary_times() const;

  /// @brief Returns the novelty curve.
  const std::vector<float>& novelty_curve() const { return novelty_curve_; }

  /// @brief Returns number of detected boundaries.
  size_t count() const { return boundaries_.size(); }

  /// @brief Returns sample rate.
  int sample_rate() const { return sr_; }

  /// @brief Returns hop length.
  int hop_length() const { return hop_length_; }

 private:
  void compute_features();
  void compute_self_similarity();
  void compute_novelty_curve();
  void detect_boundaries();
  float compute_checkerboard_kernel(int center) const;

  std::vector<Boundary> boundaries_;
  std::vector<float> novelty_curve_;
  std::vector<float> features_;  // Combined feature matrix
  std::vector<float> ssm_;       // Self-similarity matrix
  int n_frames_;
  int n_features_;
  int sr_;
  int hop_length_;
  BoundaryConfig config_;
  Audio audio_;
};

/// @brief Quick boundary detection function.
/// @param audio Input audio
/// @param config Boundary detection configuration
/// @return Vector of boundary times in seconds
std::vector<float> detect_boundaries(const Audio& audio,
                                     const BoundaryConfig& config = BoundaryConfig());

}  // namespace sonare
