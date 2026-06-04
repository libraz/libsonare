#pragma once

/// @file modal_voice.h
/// @brief Modal-resonator-bank core for the NativeSynth voice — the mallet /
///        bell / glass family (synthesis method (4) of the instrument build
///        plan; Adrien 1991, Essl & Cook banded waveguides).
///
/// A struck bar/bell is a sum of exponentially decaying sinusoidal modes; the
/// realism is ALL in the mode-ratio data:
///   - uniform bar / glockenspiel: 1 : 2.756 : 5.404 : 8.933 (inharmonic)
///   - tuned marimba / vibraphone bars: 1 : 4 : 10 (deep-arch tuning)
/// Each mode is a two-pole resonator (y = a1*y1 + a2*y2) excited by a single
/// strike impulse, with:
///   - mallet hardness: velocity weights the per-mode excitation (soft mallet
///     = fundamental only, hard strike = upper modes ring), the modal twin of
///     the KS dynamic-level lowpass.
///   - per-mode decay scaling (upper partials die faster) and decay
///     stretching down the keyboard.
///   - note-off damping towards a short t60 (hand/pedal damp); one-shot
///     patches simply never call it.
///
/// RT contract: start()/render() are allocation-free (the bank is a fixed
/// array). Determinism: the only "variation" is a seeded per-mode gain
/// scatter from the (voice_index, note, age) stream.

#include <array>
#include <cstdint>

namespace sonare::midi::synth {

inline constexpr int kMaxModalModes = 8;

/// One resonator mode of a modal patch.
struct ModalMode {
  /// Frequency ratio to the played note (mode 0 is usually 1).
  float ratio = 1.0f;
  /// Excitation weight before the mallet-hardness curve.
  float gain = 1.0f;
  /// t60 multiplier for this mode (< 1 = dies faster than the fundamental).
  float decay_scale = 1.0f;
};

/// Modal section of a NativeSynthPatch (used when mode == kModal).
struct ModalPatchParams {
  int num_modes = 0;
  std::array<ModalMode, kMaxModalModes> modes{};
  /// Fundamental t60 at A4 in seconds.
  float decay_s = 2.0f;
  /// t60 scales by 2^(stretch * octaves below A4) (big bars ring longer).
  float decay_stretch = 0.3f;
  /// Mallet hardness at full velocity in [0,1] (how much the upper modes are
  /// excited).
  float strike_brightness = 0.7f;
  /// Velocity -> hardness amount in [0,1].
  float vel_to_brightness = 0.6f;
  /// Damped t60 in seconds applied at note-off (hand/pedal damp).
  float release_damp_s = 0.15f;
};

/// Per-voice modal state, embedded in NativeSynthVoice.
class ModalVoiceCore {
 public:
  void start(const ModalPatchParams& params, double sample_rate, uint8_t note, uint8_t velocity,
             uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (coefficients are re-derived only when it changes).
  float render(float pitch_ratio) noexcept;
  /// Note-off: damp every mode towards release_damp_s.
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;

 private:
  void refresh_coefficients(float pitch_ratio) noexcept;

  struct Mode {
    float omega = 0.0f;  // radians/sample at pitch_ratio == 1
    float r = 0.0f;      // per-sample decay radius
    float gain = 0.0f;   // impulse weight (mallet curve + sin normalization)
    float a1 = 0.0f;
    float a2 = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
  };

  std::array<Mode, kMaxModalModes> modes_{};
  int num_modes_ = 0;
  double sample_rate_ = 48000.0;
  float cached_ratio_ = 0.0f;
  /// Per-sample damp radius cap installed by release().
  float release_r_ = 1.0f;
  bool excite_ = false;
};

}  // namespace sonare::midi::synth
