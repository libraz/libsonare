#pragma once

/// @file additive_voice.h
/// @brief Additive drawbar-organ core for the NativeSynth voice (synthesis
///        method (5) of the instrument build plan).
///
/// A tonewheel organ is a sum of near-pure sinusoidal partials at the nine
/// Hammond drawbar pitches (16', 5-1/3', 8', 4', 2-2/3', 2', 1-3/5',
/// 1-1/3', 1' = ratios 0.5, 1.5, 1, 2, 3, 4, 5, 6, 8) with stepped levels
/// (~3 dB per drawbar stop). What carries the realism beyond the sine sum:
///   - KEY CLICK: the percussive contact transient at note-on (a short
///     seeded noise burst), the signature that separates "organ" from
///     "sine pad". Level and decay are patch parameters.
///   - Seeded per-partial start phases, so stacked voices do not
///     phase-align into a static buzz.
///   - PERCUSSION: a decaying sine at the second or third harmonic, fired
///     once per phrase rather than once per note. The single-shot is what
///     distinguishes percussion from a fast envelope, and the arming lives on
///     the channel (see the hosts' ChannelState), not here: this core is told
///     at start() whether this note is the one that takes it.
/// Partials above Nyquist are dropped, mirroring tonewheel top-octave
/// behaviour closely enough for a sketch. Chorus/Leslie movement is a
/// track-level modulation insert, not voice state.
///
/// RT contract: start()/render() are allocation-free. Determinism: phases
/// and the click noise derive from the (voice_index, note, age) stream.

#include <array>
#include <cstdint>

#include "midi/synth/voice_random.h"

namespace sonare::midi::synth {

inline constexpr int kAdditivePartials = 9;

/// Additive section of a NativeSynthPatch (used when mode == kAdditive).
struct AdditivePatchParams {
  /// Drawbar stop levels in [0, 8] (Hammond registration digits). The
  /// default 88 8000 000 is the classic gospel/jazz base registration.
  std::array<float, kAdditivePartials> drawbars = {8.0f, 8.0f, 8.0f, 0.0f, 0.0f,
                                                   0.0f, 0.0f, 0.0f, 0.0f};
  /// Key-click transient level in [0, 1].
  float key_click = 0.4f;
  /// Key-click decay time constant (ms).
  float click_decay_ms = 6.0f;
  /// Percussion harmonic: 0 = off, 2 = second (4'), 3 = third (2-2/3').
  /// A Hammond offers those two and nothing else, and off is the default so a
  /// registration that does not ask for percussion renders as it always did.
  int percussion_harmonic = 0;
  /// Percussion decay time constant (ms) — not a time to inaudibility, which is
  /// what the figures quoted for the instrument's two positions are; consuming
  /// one as this makes the ping some seven times too long. A reference drawbar
  /// organ measures 45 ms, which is 310 ms to -60 dB. The shipped percussive
  /// registration sets its own.
  float percussion_decay_ms = 340.0f;
  /// Percussion level, as a fraction of the normalized drawbar sum.
  float percussion_level = 0.6f;
};

/// Per-voice additive state, embedded in NativeSynthVoice.
class AdditiveVoiceCore {
 public:
  /// @p percussion fires the single-shot on this note; the channel decides.
  void start(const AdditivePatchParams& params, double sample_rate, uint8_t note, uint8_t velocity,
             uint64_t seed, bool percussion = false) noexcept;
  /// Renders one sample; @p pitch_ratio is the common per-sample pitch factor.
  float render(float pitch_ratio) noexcept;
  /// Immediate silence (note-off is the wrapper amp envelope's job — the
  /// tonewheels themselves do not decay).
  void kill() noexcept;

 private:
  struct Partial {
    double phase = 0.0;
    float base_inc = 0.0f;  // cycles/sample at pitch_ratio == 1
    float gain = 0.0f;
  };

  std::array<Partial, kAdditivePartials> partials_{};
  // Key click: seeded noise burst with a one-pole exponential level decay.
  VoiceRandomSequence noise_;
  float click_level_ = 0.0f;
  float click_coeff_ = 0.0f;
  uint64_t click_index_ = 0;
  // Percussion: one decaying sine, outside the drawbar sum and its normalizer.
  double perc_phase_ = 0.0;
  float perc_base_inc_ = 0.0f;
  float perc_level_ = 0.0f;
  float perc_coeff_ = 0.0f;
};

}  // namespace sonare::midi::synth
