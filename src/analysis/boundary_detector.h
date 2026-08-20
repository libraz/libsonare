#pragma once

/// @file boundary_detector.h
/// @brief Section boundary detection using self-similarity analysis.

#include <cstddef>
#include <vector>

#include "core/audio.h"

namespace sonare {

// Forward declarations
class MelSpectrogram;
class Chroma;

/// @brief Memory budget for the stored self-similarity band, in bytes.
/// @details Only the diagonal band of the self-similarity matrix is ever read,
/// so only that band is stored (see @ref boundary_ssm_band_width). The working
/// set is therefore linear in the frame count rather than quadratic, and this
/// budget is the backstop that keeps even a pathological length bounded: past it
/// the feature grid is mean-pooled.
inline constexpr size_t kBoundarySsmBudgetBytes = 256u * 1024u * 1024u;

/// @brief Returns the number of self-similarity diagonals stored per frame.
/// @param kernel_size Checkerboard kernel size in frames.
/// @details The checkerboard kernel centred on frame c reads ssm(row, col) with
/// row and col both in [c - kernel_size/2, c + kernel_size/2), so `row - col`
/// never leaves [-(kernel_size - 1), kernel_size - 1] for an even kernel size.
/// Everything outside that band of the full matrix is unreachable, so the store
/// keeps `2 * (2 * (kernel_size / 2) - 1) + 1` diagonals and nothing else.
size_t boundary_ssm_band_width(int kernel_size);

/// @brief Returns the frame count at which the band budget starts pooling.
/// @param kernel_size Checkerboard kernel size in frames.
/// @return `kBoundarySsmBudgetBytes / (band width * sizeof(float))`, at least 1.
///         With the default 64-frame kernel this is several hours of audio, so
///         pooling is a backstop rather than a normal-path behaviour.
int boundary_pooled_target_frames(int kernel_size);

/// @brief Returns the feature pooling stride for a raw frame count.
/// @param n_frames Raw analysis frame count before pooling.
/// @param kernel_size Checkerboard kernel size in frames.
/// @return 1 when the input fits the band budget, otherwise the number of raw
///         frames averaged into each analysis frame.
int boundary_pooling_stride(int n_frames, int kernel_size);

/// @brief Returns the analysis frame count the band is built on.
/// @param n_frames Raw analysis frame count before pooling.
/// @param kernel_size Checkerboard kernel size in frames.
/// @return The pooled frame count, never above
///         @ref boundary_pooled_target_frames, so
///         `frames * band width * sizeof(float)` never exceeds
///         @ref kBoundarySsmBudgetBytes for any input length.
int boundary_ssm_frames(int n_frames, int kernel_size);

/// @brief Configuration for boundary detection.
struct BoundaryConfig {
  int n_fft = 2048;      ///< FFT size
  int hop_length = 512;  ///< Hop length
  int kernel_size = 64;  ///< Checkerboard kernel size in frames
  /// Relative novelty threshold, applied to the curve after it has been scaled
  /// by its own maximum. Selects how prominent a peak must be *within this
  /// track*; it says nothing about how much the features actually changed.
  float threshold = 0.3f;
  /// Absolute novelty threshold, applied to the raw checkerboard response before
  /// that scaling. A stationary feature sequence still produces a full-scale
  /// normalized curve — self-scaling turns residual fluctuation into peaks of
  /// 1.0 — so the relative threshold alone segments material that never changes,
  /// and a repeated loop shatters into a section every few seconds. This is the
  /// floor that asks whether anything changed at all.
  ///
  /// The default is the midpoint of two measured populations. Stationary material
  /// tops out around 0.003 (a repeated loop: 0.00004 to 0.0012 depending on how
  /// much of it the kernel spans; a steady chord 0.0003; steady white noise, the
  /// worst case, 0.003). A genuine section change runs from 0.008 for a weak one
  /// up to 0.95 where the timbre changes outright. Set to 0 to disable the floor
  /// and gate on the relative threshold alone.
  ///
  /// Two limits worth knowing. A change carried by level alone is not invisible
  /// here — the first MFCC coefficient tracks level, and per-frame normalization
  /// removes the feature vector's length but not that coefficient's ratio to the
  /// rest, so the vector really does turn. It just turns about five times less
  /// than a comparable change of pitch does, which puts its response at 0.0003
  /// sustained (0.001 if the level steps abruptly and the step itself registers
  /// as a transient) — below what steady noise produces. **Lowering this floor
  /// does not recover level-only structure**; it admits noise first, because the
  /// noise is louder than the signal being chased. Segmenting on level needs an
  /// energy-based segmenter, not a smaller threshold here. Second limit: a loop
  /// longer than the kernel span (@ref kernel_size hops, 1.5 s by default) is not
  /// stationary at the scale the kernel measures, so its within-loop contrast
  /// clears the floor and it is segmented.
  float absolute_threshold = 0.005f;
  int n_mfcc = 13;             ///< Number of MFCC coefficients
  int n_chroma = 12;           ///< Number of chroma bins
  float peak_distance = 2.0f;  ///< Minimum distance between peaks in seconds
  bool use_mfcc = true;        ///< Use MFCC features
  bool use_chroma = true;      ///< Use chroma features
};

/// @brief Detected boundary event.
struct Boundary {
  float time;      ///< Boundary time in seconds (authoritative output)
  int frame;       ///< Index into the analysis grid. Equals the STFT frame index
                   ///< at the configured hop_length for normal-length inputs, on
                   ///< the analysis rate's time base rather than the source's --
                   ///< a higher-rate input is resampled first. For long-form
                   ///< inputs the feature grid is additionally mean-pooled, so
                   ///< this is an index into the pooled grid (not the raw STFT
                   ///< frame); use @c time for sample/second mapping either way.
  float strength;  ///< Boundary strength (novelty score)
};

/// @brief Boundary detector for finding section transitions.
/// @details Uses MFCC and chroma features to compute self-similarity matrix,
/// then applies checkerboard kernel convolution to detect structural boundaries.
class BoundaryDetector {
 public:
  /// @brief Constructs boundary detector from audio.
  /// @param audio Input audio; rates above 22.05 kHz are analyzed at 22.05 kHz,
  /// so boundary times are stable across common source rates. The same rendering
  /// delivered at 44.1 kHz and at 48 kHz lands on one time grid, and matches the
  /// section analysis layered on this detector, which resamples the same way.
  /// @param config Boundary detection configuration
  explicit BoundaryDetector(const Audio& audio, const BoundaryConfig& config = BoundaryConfig());

  /// @brief Constructs boundary detector from pre-computed features.
  /// @param mel Pre-computed mel spectrogram
  /// @param chroma Pre-computed chroma
  /// @param sr Sample rate
  /// @param config Boundary detection configuration
  /// @pre @p mel and @p chroma MUST share the same time base — i.e. both
  ///      computed with the same hop_length as @p config.hop_length. The two
  ///      feature streams are combined frame-for-frame (truncated to the
  ///      shorter), so a hop mismatch silently misaligns MFCC and chroma frames
  ///      and corrupts the novelty curve. There is no hop metadata on the inputs
  ///      to verify this, so the caller is responsible for the precondition.
  BoundaryDetector(const MelSpectrogram& mel, const Chroma& chroma, int sr,
                   const BoundaryConfig& config = BoundaryConfig());

  /// @brief Returns detected boundaries with timing.
  const std::vector<Boundary>& boundaries() const { return boundaries_; }

  /// @brief Returns boundary times in seconds.
  std::vector<float> boundary_times() const;

  /// @brief Returns the novelty curve, scaled by its own maximum.
  /// @details Values are in [0, 1] and are what @ref BoundaryConfig::threshold is
  /// compared against. The scaling is per-track, so a peak of 1.0 means "the most
  /// novel frame here", not "a large change" — multiply by @ref novelty_peak to
  /// recover the raw checkerboard response the absolute threshold reads.
  const std::vector<float>& novelty_curve() const { return novelty_curve_; }

  /// @brief Returns the largest raw checkerboard response, before normalization.
  /// @details The factor that maps @ref novelty_curve back onto the absolute
  /// scale @ref BoundaryConfig::absolute_threshold lives on. Zero when the curve
  /// was left unnormalized because nothing rose above the numerical floor.
  float novelty_peak() const { return novelty_peak_; }

  /// @brief Returns number of detected boundaries.
  size_t count() const { return boundaries_.size(); }

  /// @brief Returns the rate the analysis ran at, not the input's rate.
  /// @details Equals the input rate at or below 22.05 kHz, and 22.05 kHz above
  /// it, since a higher-rate input is resampled before any feature is computed.
  int sample_rate() const { return sr_; }

  /// @brief Returns hop length.
  int hop_length() const { return hop_length_; }

  /// @brief Returns the analysis frame count the similarity band was built on.
  /// @details Never exceeds @ref boundary_pooled_target_frames: for an input past
  /// the band budget this is the pooled frame count, not the raw STFT frame count.
  int n_frames() const { return n_frames_; }

  /// @brief Returns the feature pooling stride.
  /// @details 1 when the input fit the band budget without pooling — which is the
  /// case for every realistic input length — otherwise the number of raw STFT
  /// frames averaged into each analysis frame, which is also the factor by which
  /// the effective hop duration was multiplied.
  int frame_stride() const { return frame_stride_; }

 private:
  void compute_features();
  /// @brief Combines and L2-normalizes flattened MFCC/chroma features.
  /// @details Shared by both constructors. Sets n_frames_, n_features_, features_.
  void combine_features(const std::vector<float>& mfcc_features, int mfcc_frames,
                        const std::vector<float>& chroma_features, int chroma_frames);
  /// @brief Fills ssm_band_ with the diagonal band the checkerboard kernel reads.
  void compute_self_similarity();
  /// @brief Mean-pools the feature grid so the band stays within its memory budget.
  /// @details No-op (stride 1) unless n_frames_ exceeds
  /// @ref boundary_pooled_target_frames; otherwise it averages consecutive frames
  /// into pooled, re-normalized feature vectors and records the pooling stride in
  /// frame_stride_ so boundary times stay correct.
  void downsample_features();
  void compute_novelty_curve();
  void detect_boundaries();
  float compute_checkerboard_kernel(int center) const;

  std::vector<Boundary> boundaries_;
  std::vector<float> novelty_curve_;
  std::vector<float> features_;  // Combined feature matrix
  std::vector<float> ssm_band_;  // Diagonal band of the self-similarity matrix,
                                 // n_frames_ rows of (2 * band_radius_ + 1) diagonals
  int n_frames_;
  int n_features_;
  int band_radius_ = 0;        // stored half-bandwidth: only |row - col| <= this is kept
  int frame_stride_ = 1;       // feature pooling factor (>1 only past the band budget)
  float novelty_peak_ = 0.0f;  // raw novelty maximum the curve was divided by
  // Declared before sr_ because sr_ is initialized from it: the audio the
  // detector analyses may have been resampled away from the caller's rate.
  Audio audio_;
  int sr_;
  int hop_length_;
  BoundaryConfig config_;
};

/// @brief Quick boundary detection function.
/// @param audio Input audio
/// @param config Boundary detection configuration
/// @return Vector of boundary times in seconds
std::vector<float> detect_boundaries(const Audio& audio,
                                     const BoundaryConfig& config = BoundaryConfig());

}  // namespace sonare
