#pragma once

/// @file warp.h
/// @brief Audio warping / alignment for the MIR -> editing bridge.
///
/// This is an OFFLINE, control-plane-only component (subsystem sonare::mir).
/// It NEVER runs on the audio thread. It MAY allocate freely. It does NOT
/// re-implement DSP: chroma comes from feature/chroma, the DTW core comes from
/// util/sequence, source separation from effects/hpss, and time-scale
/// modification from effects/phase_vocoder (harmonic, phase-locked) and a
/// reused OLA path (percussive). See @ref mir_warp_tsm.
///
/// The module exposes three capabilities:
///   1. WarpMap: a piecewise-linear, strictly monotonic map between "warp time"
///      (a target/musical timeline expressed in audio samples or PPQ anchors)
///      and "source time" (the original recording's sample index). Built from
///      manual markers and/or detected beat markers.
///   2. chroma_dtw_align: align two signals via chroma + (memory-restricted
///      multiscale) DTW and emit anchor pairs. See @ref mir_warp_mrmsdtw.
///   3. warp_to_length / warp_to_map: HPSS-split component-specific TSM to hit a
///      target length or follow a WarpMap. See @ref mir_warp_tsm.
///
/// @section mir_determinism Determinism
/// Same input -> byte-identical output. No clocks, no std::rand, no
/// floating non-determinism beyond IEEE arithmetic. DTW and the multiscale
/// refinement are deterministic dynamic programs.

#include <cstddef>
#include <utility>
#include <vector>

#include "core/audio.h"

namespace sonare::mir {

// ===========================================================================
// WarpMap: monotonic warp-time <-> source-time mapping
// ===========================================================================

/// @brief A single (warp_sample, source_sample) anchor pair.
///
/// Both coordinates are audio-sample positions (double, to permit sub-sample
/// anchors derived from PPQ). `warp_sample` is the position on the target /
/// musical timeline; `source_sample` is the corresponding position in the
/// original recording.
struct WarpAnchor {
  double warp_sample = 0.0;
  double source_sample = 0.0;
};

/// @brief Piecewise-linear, strictly monotonic warp-time <-> source-time map.
///
/// Anchors are sorted and de-duplicated on construction. Between anchors the
/// map interpolates linearly; outside the anchor span it extrapolates using the
/// nearest segment's slope (clamped to stay strictly increasing). A valid map
/// requires at least two anchors with strictly increasing coordinates on both
/// axes (so both directions are invertible / round-trippable).
class WarpMap {
 public:
  WarpMap() = default;

  /// @brief Builds a WarpMap from arbitrary anchors.
  /// @param anchors Anchor pairs (any order; sorted internally by warp_sample).
  /// @throws SonareException if fewer than two usable, strictly-monotonic
  ///         anchors remain after sorting/de-duplication.
  static WarpMap from_anchors(std::vector<WarpAnchor> anchors);

  /// @brief Builds a WarpMap from paired marker positions.
  /// @details Each manual marker / detected beat marker contributes one anchor:
  ///   the marker's position on the warp timeline paired with its position in
  ///   the source recording. Both vectors must be the same length. This is the
  ///   "convert manual markers AND detected beat markers into anchors" entry
  ///   point; callers concatenate manual + beat markers before calling.
  /// @param warp_samples Marker positions on the warp/target timeline (samples).
  /// @param source_samples Marker positions in the source recording (samples).
  static WarpMap from_markers(const std::vector<double>& warp_samples,
                              const std::vector<double>& source_samples);

  /// @brief True if the map has at least two monotonic anchors.
  bool valid() const { return anchors_.size() >= 2; }

  /// @brief Sorted, de-duplicated anchors.
  const std::vector<WarpAnchor>& anchors() const { return anchors_; }

  /// @brief Maps a warp-timeline sample to the corresponding source sample.
  double warp_to_source(double warp_sample) const;

  /// @brief Maps a source-recording sample to the corresponding warp sample.
  double source_to_warp(double source_sample) const;

 private:
  explicit WarpMap(std::vector<WarpAnchor> anchors) : anchors_(std::move(anchors)) {}

  std::vector<WarpAnchor> anchors_;
};

// ===========================================================================
// Chroma-DTW alignment
// ===========================================================================

/// @brief Configuration for chroma_dtw_align.
///
/// @section mir_warp_mrmsdtw Memory-restricted multiscale DTW (MrMsDTW)
/// A full N*M DTW cost matrix is O(N*M) memory, which is prohibitive for long
/// signals and WASM. We therefore use a memory-restricted multiscale DTW in the
/// spirit of MrMsDTW (Pratzlich et al. 2016, "Memory-restricted multiscale
/// dynamic time warping"):
///
///   * The chroma sequences are repeatedly downsampled by `scale_factor`
///     (default 2) along the time axis (mean-pooling adjacent frames) until the
///     longer sequence is at most `coarse_max_frames` (default 256) frames.
///   * A FULL DTW is run on the coarsest level (bounded memory because both
///     sides are <= coarse_max_frames).
///   * The coarse path is projected up to the next finer level and a DTW is run
///     only inside a diagonal BAND of half-width `band_radius` (default 16)
///     frames around the projected path. Each banded level is O(N * band_width)
///     memory rather than O(N*M). This repeats down to the original resolution.
///
/// `max_full_frames` is a guard: if BOTH sequences are already shorter than this
/// (default 512), a single full DTW is used directly (no multiscale needed).
struct ChromaDtwConfig {
  // Chroma front-end (chroma_cqt is used internally; these mirror its knobs).
  int hop_length = 512;  ///< Hop length (samples) -> chroma frame rate.
  int bins_per_octave = 12;
  // Multiscale / band parameters (documented above).
  int coarse_max_frames = 256;  ///< Coarsest-level max frames before full DTW.
  int max_full_frames = 512;    ///< If both seqs <= this, skip multiscale.
  int scale_factor = 2;         ///< Time downsample factor per level (>= 2).
  int band_radius = 16;         ///< Band half-width (frames) at each refine level.
};

/// @brief Result of a chroma-DTW alignment.
struct ChromaDtwResult {
  /// Alignment path as (reference_frame, target_frame) index pairs, in
  /// increasing order. Frame indices are at the ORIGINAL chroma resolution.
  std::vector<std::pair<int, int>> path;
  /// The path converted to WarpAnchors at audio-sample resolution, where
  /// warp_sample is the TARGET signal position and source_sample is the
  /// REFERENCE signal position. Ready for WarpMap::from_anchors.
  std::vector<WarpAnchor> anchors;
  /// Mean absolute frame-index residual of the path around its diagonal trend
  /// (a coarse alignment-quality indicator; not a hard error bound).
  float mean_residual_frames = 0.0f;
  int reference_frames = 0;  ///< Reference chroma frame count.
  int target_frames = 0;     ///< Target chroma frame count.
};

/// @brief Aligns `target` to `reference` via chroma features + MrMsDTW.
/// @details Computes a 12-bin chromagram for each signal, then aligns them with
///   the memory-restricted multiscale DTW described in @ref mir_warp_mrmsdtw.
///   The two signals SHOULD share a sample rate; if not, the caller should
///   resample first (this module does no I/O / resampling).
/// @param reference The reference signal (maps to source_sample in anchors).
/// @param target The signal to be aligned (maps to warp_sample in anchors).
/// @param config Chroma + multiscale-DTW parameters.
/// @throws SonareException if either signal is empty or yields < 2 frames.
ChromaDtwResult chroma_dtw_align(const Audio& reference, const Audio& target,
                                 const ChromaDtwConfig& config = ChromaDtwConfig());

// ===========================================================================
// Time-scale modification (HPSS + component-specific TSM)
// ===========================================================================

/// @brief Configuration for the warping / TSM stage.
///
/// @section mir_warp_tsm HPSS-split component-specific TSM
/// To preserve transients while keeping tonal content phase-coherent we split
/// the input with effects/hpss (median-filter HPSS) and stretch each component
/// with a method matched to its content, then sum the results:
///
///   * HARMONIC component -> phase-vocoder with identity phase-locking
///     (effects/phase_vocoder `phase_vocoder_phaselocked`, Laroche & Dolson
///     1999). Phase-locking keeps tonal partials coherent and avoids phasiness.
///   * PERCUSSIVE component -> a windowed overlap-add (OLA) resynthesis at the
///     stretched hop. libsonare has no standalone WSOLA implementation in
///     effects/; instead of re-deriving WSOLA here (which would duplicate DSP
///     this module is forbidden from owning), we reuse the existing STFT/iSTFT
///     OLA machinery by stretching the percussive spectrogram's FRAME POSITIONS
///     without phase propagation (magnitude/phase carried per source frame),
///     which preserves transient sharpness far better than a phase vocoder.
///     This substitution documents the chosen transient-preserving implementation.
///
/// Both paths share the same `n_fft`/`hop_length` analysis grid so the summed
/// output stays aligned.
struct WarpTsmConfig {
  int n_fft = 2048;
  int hop_length = 512;
  /// HPSS median-filter kernel sizes (odd, >= 3). Defaults match HpssConfig.
  int hpss_kernel_harmonic = 31;
  int hpss_kernel_percussive = 31;
};

/// @brief Time-scales `audio` to exactly `target_length` samples.
/// @details Deterministic: identical input -> byte-identical output. Routes the
///   harmonic and percussive components through the methods documented in
///   @ref mir_warp_tsm and returns audio of exactly `target_length` samples.
/// @param audio Input signal (mono recommended; multi-sample buffer treated as
///   a single channel by the underlying STFT path).
/// @param target_length Desired output length in samples (> 0).
/// @param config Analysis / HPSS parameters.
/// @throws SonareException if audio is empty or target_length == 0.
Audio warp_to_length(const Audio& audio, size_t target_length,
                     const WarpTsmConfig& config = WarpTsmConfig());

/// @brief Time-scales `audio` so its source timeline follows `map`.
/// @details The output length is taken from the warp-axis span of `map`
///   (map.warp_to_source covers [0, output_length)). For the current scope the
///   warp is realized as a single global rate equal to the average
///   source/warp slope of the map; segment-wise variable-rate warping is a
///   future extension and is documented as such. Deterministic.
/// @param audio Input signal whose timeline is the map's SOURCE axis.
/// @param map A valid WarpMap.
/// @param config Analysis / HPSS parameters.
/// @throws SonareException if audio is empty or map is invalid.
Audio warp_to_map(const Audio& audio, const WarpMap& map,
                  const WarpTsmConfig& config = WarpTsmConfig());

}  // namespace sonare::mir
