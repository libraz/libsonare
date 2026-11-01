#pragma once

/// @file downbeat_analyzer.h
/// @brief Downbeat estimation from beat sequence and bar-start evidence.

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

/// @brief Per-beat bar-start evidence the downbeat phase is selected from.
/// @details Each vector is empty or holds one value per beat.
struct DownbeatObservations {
  std::vector<float> lf;               ///< Power below 150 Hz around each beat's attack
  std::vector<float> harmonic_change;  ///< 1 - cosine of this beat span's chroma to the previous
};

/// @brief Estimates downbeats from beat-level meter phase.
DownbeatResult estimate_downbeats(const std::vector<Beat>& beats,
                                  const TimeSignature& time_signature, int downbeat_phase);

/// @brief True at the secondary accent of a bar: position 2 in 4/4, position 3 in 6/8.
/// @details Position 0 (the downbeat) is not included; 3/4 has no secondary accent.
constexpr bool is_secondary_strong_position(int beats_per_bar, int position) {
  return (beats_per_bar == 4 && position == 2) || (beats_per_bar == 6 && position == 3);
}

/// @brief Measures bar-start evidence for each beat from a centred power spectrogram.
/// @param power Power spectrogram [n_bins x n_frames], row-major; frame k centred at k * hop / sr.
/// @param n_bins Frequency bins, n_fft / 2 + 1.
/// @param n_frames Frames.
/// @param sr Sample rate of the spectrogram.
/// @param n_fft FFT length the bins come from.
/// @param hop Hop between frames, in samples.
/// @param beats Beats; each is measured at its event time.
/// @details `lf` averages the power below 150 Hz over frames centred in [t - 0.02, t + 0.10) s.
///          `harmonic_change` compares mean 12-bin chroma (tuning 0, each frame max-normalised)
///          over consecutive beat spans; the last span lasts one median beat interval, and the
///          first beat has no predecessor (0). Both are empty when there is no beat or frame.
DownbeatObservations downbeat_phase_evidence(const std::vector<float>& power, int n_bins,
                                             int n_frames, int sr, int n_fft, int hop,
                                             const std::vector<Beat>& beats);

/// @brief Selects the bar phase whose beats carry the most bar-start evidence.
/// @details Each feature is z-scored over the track and the two are summed per beat; the phase
///          is the bar position with the highest median sum, ties to the earliest. Phase 0 when
///          a position has no beat; beat 0 alone when the numerator is 1.
///          Confidence is 0.5 * meter confidence + 0.5 * the clamped z-unit margin to the
///          runner-up position.
DownbeatResult select_downbeat_phase(const std::vector<Beat>& beats,
                                     const TimeSignature& time_signature,
                                     const DownbeatObservations& observations);

/// @brief Converts chord segment starts to beat-level chord-change observations.
std::vector<float> chord_change_observations(const std::vector<Beat>& beats,
                                             const std::vector<Chord>& chords,
                                             float tolerance_seconds = 0.08f);

/// @brief Extracts beat-level low-frequency energy observations from audio.
std::vector<float> low_frequency_energy_observations(const std::vector<Beat>& beats,
                                                     const Audio& audio, float cutoff_hz = 200.0f,
                                                     float window_seconds = 0.08f);

/// @brief Aggregates frame-level onset strength around each beat.
std::vector<float> onset_strength_observations(const std::vector<Beat>& beats,
                                               const std::vector<float>& onset_strength, int sr,
                                               int hop_length, float window_seconds = 0.08f);

}  // namespace sonare
