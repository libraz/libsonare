#pragma once

/// @file chord_hmm.h
/// @brief Viterbi HMM smoothing for chord sequences.

#include <utility>
#include <vector>

#include "analysis/chord_templates.h"
#include "util/types.h"

namespace sonare {

/// @brief Per-frame chord candidate list. Pair is (template index, observation score).
struct ChordHmmObservation {
  std::vector<std::pair<int, float>> candidates;
};

/// @brief Configuration for chord HMM smoothing.
struct ChordHmmConfig {
  int beam_width = 24;                  ///< Maximum candidates retained per frame
  float emission_weight = 6.0f;         ///< Correlation-to-log likelihood scale
  float self_transition_logp = -0.05f;  ///< Bias for remaining on same chord
  /// @brief Bias for a cadence whose dominant carries the tritone (V7 -> I, V7 -> i).
  /// @details The strongest harmonic cue there is, and the one a root-motion-only
  /// test cannot see: it scores a natural-minor v -> i identically to the
  /// harmonic-minor V7 -> i that actually resolves.
  float dominant_cadential_transition_logp = -0.9f;
  /// @brief Bias for a cadence spelled with the expected chord qualities.
  float cadential_transition_logp = -1.2f;
  /// @brief Bias for cadential root motion carrying an unexpected quality.
  /// @details A minor v resolving to i in a minor key, or a minor chord standing
  /// where the dominant should be in a major key: the motion is cadential but
  /// the chord does not pull, so it sits between a spelled cadence and an
  /// ordinary related transition.
  float weak_cadential_transition_logp = -1.7f;
  float related_transition_logp = -2.1f;
  float remote_transition_logp = -4.6f;
  bool use_key_context = false;
  PitchClass key_root = PitchClass::C;
  Mode key_mode = Mode::Major;
};

/// @brief Computes a Viterbi-smoothed chord template index sequence.
/// @param observations Per-frame candidate chord emissions
/// @param templates Chord templates referenced by observations
/// @param config HMM configuration
/// @return Best template index for each observation frame
std::vector<int> viterbi_chord_sequence(const std::vector<ChordHmmObservation>& observations,
                                        const std::vector<ChordTemplate>& templates,
                                        const ChordHmmConfig& config = ChordHmmConfig());

}  // namespace sonare
