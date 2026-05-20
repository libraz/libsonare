#pragma once

/// @file downbeat_analyzer.h
/// @brief Downbeat estimation from beat sequence and meter phase.

#include <vector>

#include "analysis/beat_analyzer.h"

namespace sonare {

struct Chord;

/// @brief Result from downbeat estimation.
struct DownbeatResult {
  std::vector<int> beat_indices;
  std::vector<Beat> downbeats;
  TimeSignature time_signature{4, 4, 0.0f};
  float confidence = 0.0f;
};

/// @brief Beat-level observations for downbeat Viterbi scoring.
struct DownbeatObservations {
  std::vector<float> beat_strengths;
  std::vector<float> low_frequency_energy;
  std::vector<float> chord_changes;
  float beat_strength_weight = 1.0f;
  float low_frequency_weight = 0.75f;
  float chord_change_weight = 0.75f;
  float phase_prior_weight = 0.2f;
  float tempo_observation_weight = 0.25f;
  float tempo_transition_weight = 0.15f;
  int tempo_state_count = 5;
};

/// @brief Estimates downbeats from beat-level meter phase.
DownbeatResult estimate_downbeats(const std::vector<Beat>& beats,
                                  const TimeSignature& time_signature, int downbeat_phase);

/// @brief Estimates downbeats using beat-level observations and a meter-state Viterbi pass.
DownbeatResult estimate_downbeats(const std::vector<Beat>& beats,
                                  const TimeSignature& time_signature, int downbeat_phase,
                                  const DownbeatObservations& observations);

/// @brief Converts chord segment starts to beat-level chord-change observations.
std::vector<float> chord_change_observations(const std::vector<Beat>& beats,
                                             const std::vector<Chord>& chords,
                                             float tolerance_seconds = 0.08f);

/// @brief Extracts beat-level low-frequency energy observations from audio.
std::vector<float> low_frequency_energy_observations(const std::vector<Beat>& beats,
                                                     const Audio& audio, float cutoff_hz = 200.0f,
                                                     float window_seconds = 0.08f);

/// @brief Aggregates frame-level onset strength around each beat for downbeat scoring.
std::vector<float> onset_strength_observations(const std::vector<Beat>& beats,
                                               const std::vector<float>& onset_strength, int sr,
                                               int hop_length, float window_seconds = 0.08f);

}  // namespace sonare
