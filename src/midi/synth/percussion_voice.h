#pragma once

/// @file percussion_voice.h
/// @brief Membrane-modal + filtered-noise percussion core for the NativeSynth
///        voice — the data-free GM drum kit (synthesis method (6) of the
///        instrument build plan; Rossing, Cook).
///
/// Two summed layers per kit piece:
///   - TONE: a small modal bank at the circular-membrane (Rayleigh) ratios
///     1 : 1.59 : 2.14 : 2.30 : 2.65 with a DESCENDING pitch envelope (the
///     struck-membrane tension release that makes a kick/tom read as a drum
///     and not a sine blip). The base frequency tracks the struck key or is
///     pinned per piece (snare shell, cymbal bell).
///   - NOISE: a seeded noise burst with an exponential level decay through a
///     dedicated TPT SVF band (snare wires = band-pass crack, hats/cymbals =
///     high-pass shimmer).
/// Pieces are config PODs in the GM fallback drum map; voices play one-shot
/// (the patch's one_shot flag) so note-off never chokes a strike.
///
/// RT contract: start()/render() are allocation-free. Determinism: noise is
/// the counter-based (voice_index, note, age) stream — every bounce is
/// bit-identical while distinct strikes still decorrelate.

#include <array>
#include <cstdint>

#include "midi/synth/body_resonator.h"
#include "midi/synth/filter_models.h"
#include "midi/synth/svf.h"
#include "midi/synth/voice_random.h"

namespace sonare::midi::synth {

inline constexpr int kMaxPercussionModes = 6;
inline constexpr int kMaxShellModes = 4;

/// Percussion section of a NativeSynthPatch (used when mode == kPercussion).
struct PercussionPatchParams {
  /// GM kit mode: instead of playing this single kit piece on every key,
  /// note-on resolves the struck note through the GM drum map
  /// (gm_fallback_drum_patch), so one patch is the whole kit — the
  /// `drum-kit` preset. The remaining fields are ignored when set.
  bool gm_kit = false;

  // --- membrane/tone layer ---
  int num_modes = 0;
  /// Mode ratios to the base frequency (circular membrane: 1, 1.59, 2.14,
  /// 2.30, 2.65).
  std::array<float, kMaxPercussionModes> mode_ratios = {1.0f, 1.59f, 2.14f, 2.3f, 2.65f, 0.0f};
  /// Fundamental t60 (seconds) of the tone layer.
  float mode_decay_s = 0.3f;
  /// Tone layer mix gain.
  float tone_gain = 1.0f;
  /// Base frequency override in Hz (0 = the struck key's frequency).
  float base_freq_hz = 0.0f;
  /// Strike pitch overshoot: the tone starts (1 + pitch_drop) x the base
  /// frequency and falls back through a one-pole (0 = static pitch).
  float pitch_drop = 0.0f;
  float pitch_drop_ms = 40.0f;

  // --- strike point (membrane excitation weighting) ---
  /// Normalized strike radius, 0 = membrane centre .. 1 = rim. At 0 every
  /// mode keeps its base strike gain (legacy uniform excitation); above 0
  /// each mode is weighted by its shape J_m(alpha_mn * strike_r) *
  /// cos(m * strike_theta) evaluated at the strike, so a centre hit drops the
  /// m>=1 modes (J_{m>0}(0) = 0) to a pitchless thump and a rim hit excites
  /// them.
  float strike_r = 0.0f;
  /// Strike angle (radians); orients the m>=1 degenerate sin/cos pair.
  float strike_theta = 0.0f;
  /// Per-mode angular order m (nodal diameters), parallel to mode_ratios.
  /// Defaults to the ideal circular-membrane set (0,1)(1,1)(2,1)(0,2)(3,1).
  std::array<uint8_t, kMaxPercussionModes> mode_m = {0, 1, 2, 0, 3, 0};
  /// Per-mode Bessel zero alpha_mn (the spatial argument scale), parallel to
  /// mode_ratios. mode_ratios[k] == mode_alpha[k] / mode_alpha[0] for the
  /// ideal membrane, but the two serve different roles: ratio scales
  /// frequency, alpha scales the strike-shape argument.
  std::array<float, kMaxPercussionModes> mode_alpha = {2.4048f, 3.8317f, 5.1356f,
                                                       5.5201f, 6.3802f, 0.0f};

  // --- noise layer ---
  float noise_gain = 0.0f;
  float noise_decay_ms = 150.0f;
  float noise_cutoff_hz = 2500.0f;
  float noise_q = 1.0f;
  SynthFilterOutput noise_output = SynthFilterOutput::kBandpass;

  // --- shell resonance ---
  /// Mix of the drum-shell resonance over the summed tone+noise hit (0 =
  /// bypass, the legacy dry voice). The shell is a small fixed bandpass bank
  /// (normalized to unit peak, so it never blows up), letting the strike ring
  /// through the body of the drum.
  float shell_mix = 0.0f;
  int shell_num_modes = 0;
  std::array<float, kMaxShellModes> shell_freq_hz = {0.0f, 0.0f, 0.0f, 0.0f};
  std::array<float, kMaxShellModes> shell_t60_s = {0.08f, 0.06f, 0.05f, 0.04f};
  std::array<float, kMaxShellModes> shell_weight = {1.0f, 0.7f, 0.5f, 0.35f};

  // --- snare wire rattle ---
  /// Wire-against-head buzz amount (0 = off, no rattle). While the membrane
  /// displacement exceeds wire_threshold the snare wires contact the bottom
  /// head and rattle: a high-passed noise burst gated by how far the head is
  /// over the threshold and scaled by strike velocity, so hard hits buzz
  /// louder and longer. Couples to the tone layer (no membrane => no rattle).
  float wire_buzz = 0.0f;
  /// Membrane level at which the wires start contacting the head.
  float wire_threshold = 0.1f;
  /// Cutoff of the high-pass through which the rattle is voiced.
  float wire_cutoff_hz = 4000.0f;
};

/// Per-voice percussion state, embedded in NativeSynthVoice.
class PercussionVoiceCore {
 public:
  void start(const PercussionPatchParams& params, double sample_rate, uint8_t note,
             uint8_t velocity, uint64_t seed) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor
  /// (multiplied with the internal descending pitch envelope).
  float render(float pitch_ratio) noexcept;
  /// Immediate silence.
  void kill() noexcept;

 private:
  struct Mode {
    float omega = 0.0f;
    float r = 0.0f;
    float gain = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
  };

  std::array<Mode, kMaxPercussionModes> modes_{};
  int num_modes_ = 0;
  float tone_gain_ = 1.0f;
  // Descending pitch envelope: ratio = 1 + drop_state_ (one-pole decay).
  float drop_state_ = 0.0f;
  float drop_coeff_ = 0.0f;
  float cached_ratio_ = 0.0f;
  bool excite_ = false;

  VoiceRandomSequence noise_;
  uint64_t noise_index_ = 0;
  float noise_level_ = 0.0f;
  float noise_coeff_ = 0.0f;
  TptSvf noise_filter_;
  SynthFilterOutput noise_output_ = SynthFilterOutput::kBandpass;

  BodyResonator shell_;

  // Snare wire rattle: gated, velocity-scaled high-passed noise driven by the
  // membrane displacement crossing wire_threshold_.
  float wire_buzz_ = 0.0f;
  float wire_threshold_ = 0.1f;
  float wire_vel01_ = 0.0f;
  uint64_t wire_index_ = 0;
  TptSvf wire_filter_;
};

}  // namespace sonare::midi::synth
