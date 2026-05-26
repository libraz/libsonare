#pragma once

/// @file pitch_corrector.h
/// @brief Monophonic pitch correction built on F0 tracks and pitch shifting.

#include <vector>

#include "core/audio.h"
#include "editing/pitch_editor/f0_provider.h"
#include "editing/pitch_editor/scale_quantizer.h"
#include "effects/time_stretch.h"

namespace sonare::editing::pitch_editor {

struct PitchCorrectionConfig {
  ScaleQuantizerConfig scale{};
  StretchBackend backend = StretchBackend::NativeSpectral;
  float retune_amount = 1.0f;
  float max_correction_semitones = 12.0f;
  float retune_speed_ms = 50.0f;          ///< Retune IIR time constant (ms)
  float vibrato_threshold_cents = 20.0f;  ///< Below this, preserve natural pitch (vibrato bypass)
};

class PitchCorrector {
 public:
  explicit PitchCorrector(PitchCorrectionConfig config = {});

  Audio shift(const Audio& audio, float semitones) const;

  /// @brief Corrects every voiced frame toward a fixed MIDI target (time-varying).
  Audio correct_to_midi(const Audio& audio, const F0Track& track, float target_midi) const;
  /// @brief Corrects every voiced frame toward the nearest scale degree (time-varying).
  Audio correct_to_scale(const Audio& audio, const F0Track& track) const;

  /// @brief Per-frame correction toward a fixed MIDI target.
  Audio correct_to_midi_timevarying(const Audio& audio, const F0Track& track,
                                    float target_midi) const;
  /// @brief Per-frame correction toward the configured scale.
  Audio correct_to_scale_timevarying(const Audio& audio, const F0Track& track) const;

  float estimate_median_midi(const F0Track& track) const;
  float correction_to_midi(const F0Track& track, float target_midi) const;
  float correction_to_scale(const F0Track& track) const;

  static float hz_to_midi(float hz);
  static float midi_to_hz(float midi);

 private:
  enum class TargetMode { kFixedMidi, kScale };

  /// @brief Shared per-frame correction pipeline (target -> retune IIR -> resynthesis).
  Audio correct_timevarying(const Audio& audio, const F0Track& track, TargetMode mode,
                            float fixed_target_midi) const;

  /// @brief Phase 1+2: per-frame smoothed correction in semitones (size == track frames).
  std::vector<float> compute_smooth_deltas(const F0Track& track, TargetMode mode,
                                           float fixed_target_midi) const;

  /// @brief Phase 3: TD-PSOLA resynthesis driven by a per-frame delta curve.
  Audio resynthesize(const Audio& audio, const F0Track& track,
                     const std::vector<float>& smooth_deltas) const;

  float apply_limits(float semitones) const noexcept;

  PitchCorrectionConfig config_{};
};

}  // namespace sonare::editing::pitch_editor
