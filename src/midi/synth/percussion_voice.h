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

#include "midi/synth/filter_models.h"
#include "midi/synth/svf.h"
#include "midi/synth/voice_random.h"

namespace sonare::midi::synth {

inline constexpr int kMaxPercussionModes = 6;

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

  // --- noise layer ---
  float noise_gain = 0.0f;
  float noise_decay_ms = 150.0f;
  float noise_cutoff_hz = 2500.0f;
  float noise_q = 1.0f;
  SynthFilterOutput noise_output = SynthFilterOutput::kBandpass;
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
};

}  // namespace sonare::midi::synth
