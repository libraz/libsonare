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
/// Each mode is a radius-preserving quadrature oscillator (equivalent to a
/// two-pole resonator at fixed pitch) excited by a single strike impulse, with:
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

#include "midi/control_value.h"
#include "midi/synth/contact_force.h"
#include "util/constants.h"

namespace sonare::midi::synth {

inline constexpr int kMaxModalModes = 16;

/// One resonator mode of a modal patch.
struct ModalMode {
  /// Frequency ratio to the played note (mode 0 is usually 1).
  float ratio = 1.0f;
  /// Excitation weight before the mallet-hardness curve.
  float gain = 1.0f;
  /// t60 multiplier for this mode (< 1 = dies faster than the fundamental).
  float decay_scale = 1.0f;
  /// Octaves the ratio moves per octave of key, ratio * 2^(key_slope * octaves
  /// above A4): the untuned upper modes of a short bar fall against its
  /// fundamental as the bar shortens. 0 = the ratio at every key, bit-identical.
  float key_slope = 0.0f;
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
  /// Contact time of the mallet at full velocity (ms). Above 0 each mode's share
  /// of the strike is the contact force's spectrum at its frequency,
  /// W(f_k * tau) (Chaigne & Doutaut, JASA 101, 1997), in place of the hardness
  /// curve in the mode's index — which also retires @c strike_brightness and
  /// @c vel_to_brightness, the curve's two inputs. 0 = that curve, bit-identical.
  float mallet_ms = 0.0f;
  /// How far the contact shortens as the strike hardens: tau = mallet_ms *
  /// vel01 ^ -mallet_vel_exp. 0.2 is the Hertzian mallet-on-bar result.
  float mallet_vel_exp = 0.0f;
  /// The force pulse read at each mode under @c mallet_ms: the raised cosine
  /// nulls a mode sitting at twice 1 / tau, the Gaussian does not.
  ContactShape mallet_shape = ContactShape::kRaisedCosine;
  /// Phase LFO2 starts its cycle at when the bar is struck, in degrees. A
  /// motor's vane is wherever it happens to be; a module's sampled tremolo
  /// starts from one point every time. 0 = the LFO's own start, bit-identical.
  float tremolo_phase_deg = 0.0f;
  /// Level of the mallet's direct contact radiation, a pulse outside the
  /// envelope that the slow resonators cannot supply in the first milliseconds.
  /// Scaled by velocity. 0 = off, bit-identical.
  float contact = 0.0f;
  /// Contact time of that pulse (ms): its energy peaks at 1 / contact_ms.
  float contact_ms = 0.5f;
  /// Exponent of the voice-amplifier velocity curve, (velocity/127)^e; 2 is the
  /// SoundFont curve, bit-identical to it.
  float velocity_exponent = 2.0f;
};

/// Per-voice modal state, embedded in NativeSynthVoice.
class ModalVoiceCore {
 public:
  void start(const ModalPatchParams& params, double sample_rate, uint8_t note, Velocity16 velocity,
             uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (coefficients are re-derived only when it changes).
  float render(float pitch_ratio) noexcept;
  /// Note-off: damp every mode towards release_damp_s.
  void release() noexcept;
  /// Immediate silence.
  void kill() noexcept;
  /// The direct contact radiation for this sample, outside render() so the
  /// voice's drive, filter and envelope do not swallow it. Once per sample.
  float next_contact() noexcept { return contact_.next(); }

 private:
  void refresh_coefficients(float pitch_ratio) noexcept;

  struct Mode {
    double omega = 0.0;       // radians/sample at pitch_ratio == 1
    double r = 0.0;           // per-sample decay radius
    double excitation = 0.0;  // impulse weight before sin normalization
    double sin_w = 0.0;       // current oscillator angle, sine component
    double cos_w = 1.0;       // current oscillator angle, cosine component
    double y_s = 0.0;         // radius-scaled quadrature sine state
    double y_c = 0.0;         // radius-scaled quadrature cosine state
    bool started = false;     // strike consumed, including while out of band
    bool audible = false;
  };

  std::array<Mode, kMaxModalModes> modes_{};
  int num_modes_ = 0;
  double sample_rate_ = ::sonare::constants::kDefaultDawSampleRate;
  float cached_ratio_ = 0.0f;
  /// Per-sample damp radius cap installed by release().
  double release_r_ = 1.0;
  bool excite_ = false;
  ContactPulse contact_;
};

}  // namespace sonare::midi::synth
