#pragma once

/// @file pitch_corrector.h
/// @brief Monophonic pitch correction built on F0 tracks and pitch shifting.

#include <vector>

#include "core/audio.h"
#include "editing/pitch_editor/f0_provider.h"
#include "editing/pitch_editor/scale_quantizer.h"
#include "effects/time_stretch.h"
#include "util/constants.h"

namespace sonare::editing::pitch_editor {

struct PitchCorrectionConfig {
  ScaleQuantizerConfig scale{};
  StretchBackend backend = StretchBackend::NativeSpectral;
  float retune_amount = 1.0f;
  /// How far a RETUNE may drag a measured pitch toward its target. Bounds
  /// correction_to_midi / correction_to_scale and the per-frame pipeline only;
  /// a caller-stated interval (shift / correct_to_midi from an explicit
  /// current_midi) is applied in full.
  float max_correction_semitones = constants::kSemitonesPerOctave;
  float retune_speed_ms = 50.0f;          ///< Retune IIR time constant (ms)
  float vibrato_threshold_cents = 20.0f;  ///< Below this, preserve natural pitch (vibrato bypass)
};

class PitchCorrector {
 public:
  explicit PitchCorrector(PitchCorrectionConfig config = {});

  /// @brief Transposes the whole buffer by @p semitones.
  /// @details The caller names the interval, so it is applied in full:
  ///          @c max_correction_semitones does not bound this call. A
  ///          non-finite @p semitones transposes by 0.
  Audio shift(const Audio& audio, float semitones) const;

  /// @brief Applies one constant transpose from current MIDI to target MIDI.
  /// @details The whole (target_midi - current_midi) interval is applied. Both
  ///          endpoints are caller-stated, so this is a transposition rather
  ///          than a correction and @c max_correction_semitones does not bound
  ///          it: C3 -> C5 transposes by the full 24 semitones.
  /// @throws SonareException(InvalidParameter) when either MIDI number is
  ///         non-finite or outside [0, 127].
  Audio correct_to_midi(const Audio& audio, float current_midi, float target_midi) const;
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
