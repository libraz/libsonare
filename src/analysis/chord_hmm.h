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
  float cadential_transition_logp = -1.2f;
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
