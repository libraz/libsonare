#pragma once

/// @file room_estimator.h
/// @brief Blind statistical estimation of an equivalent room from a recording.
///
/// Given a recording (an impulse response, or any signal the blind acoustic
/// analyzer can extract a free decay from), this estimates the *equivalent*
/// shoebox room whose statistical reverberation matches the measured decay:
/// an equivalent volume, representative dimensions, per-octave-band mean
/// absorption, and the direct-to-reverberant ratio. It is the inverse of the
/// geometry -> RIR synthesis path and feeds the analysis<->synthesis
/// round-trip (estimate -> material assignment -> re-synthesis).
///
/// Identifiability: the per-band reverberation time RT60(f) together with a
/// fixed room shape determines the *product* volume x absorption, but not how
/// that product splits between a large lightly-absorptive room and a small
/// heavily-absorptive one -- the inverse Sabine/Eyring problem is rank
/// deficient by exactly one degree of freedom. We close it with a configurable
/// mean-absorption prior (`reference_absorption`) that anchors the volume
/// scale, plus a room-shape prior (`aspect_hint_*`). The returned `confidence`
/// reports how well the data actually support the estimate -- excitation
/// adequacy, per-band consistency, and the physical plausibility of the
/// recovered absorptions. This is a creative-FX estimate, not a metrological
/// room measurement; treat the volume as an order-of-magnitude equivalent.

#include <vector>

#include "acoustic/room_types.h"         // sonare::RoomDimensions
#include "analysis/acoustic_analyzer.h"  // AcousticConfig
#include "core/audio.h"

namespace sonare {

/// @brief Equivalent-room parameters recovered from a recording.
struct RoomEstimate {
  float volume = 0.0f;                  ///< equivalent interior volume V (m^3)
  RoomDimensions dims;                  ///< representative shoebox dimensions (m)
  std::vector<float> absorption_bands;  ///< per-octave-band mean absorption alpha-bar(f), [0,1)
  float drr_db = 0.0f;                  ///< direct-to-reverberant ratio (dB)
  std::vector<float> rt60_bands;        ///< per-band RT60 (s), straight from the analyzer
  float confidence = 0.0f;              ///< honest [0,1] support for the estimate
};

/// @brief Priors and analysis settings for `estimate_room`.
struct RoomEstimateConfig {
  /// Delegated to the underlying blind/IR acoustic analyzer.
  AcousticConfig acoustic{};

  /// Room-shape prior: length/width and length/height ratios. Defaults to a
  /// cube. The recovered shape is exactly this aspect with the scale solved
  /// from RT60; shape itself is not identifiable from a single decay.
  float aspect_hint_lw = 1.0f;
  float aspect_hint_lh = 1.0f;

  /// Mean-absorption prior that anchors the otherwise rank-deficient volume
  /// scale (see the file header). Clamped to [0.01, 0.99). A value matching the
  /// true room recovers the true volume; mismatch scales the volume estimate.
  float reference_absorption = 0.15f;

  /// Use Eyring instead of Sabine for bands whose mean absorption exceeds ~0.2,
  /// where Sabine over-predicts RT60.
  bool prefer_eyring = true;
};

/// @brief Estimate the equivalent room from a recording.
///
/// Runs the acoustic analyzer, inverts Sabine/Eyring under the configured
/// priors to recover an equivalent volume and dimensions, solves the per-band
/// mean absorption at that geometry, and measures the direct-to-reverberant
/// ratio from the direct-sound window. A silent or unanalyzable input yields a
/// zeroed estimate with `confidence == 0`.
RoomEstimate estimate_room(const Audio& recording, const RoomEstimateConfig& config = {});

}  // namespace sonare
