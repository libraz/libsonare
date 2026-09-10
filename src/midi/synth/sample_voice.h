#pragma once

/// @file sample_voice.h
/// @brief Sample-playback source for the NativeSynth voice: resolves a keymap
///        zone at note-on and steps it, leaving the filter, envelopes, LFOs and
///        the mod matrix to the voice around it.
///
/// This is the oscillator's place in the subtractive chain rather than an
/// engine of its own, which is what puts host PCM behind the resonant multi-mode
/// filter and the mod matrix instead of behind a fixed lowpass.
///
/// The bank is attached by the host before start() (the pointer wiring the
/// waveguide cores use for their slabs) and must outlive every voice reading it.
///
/// RT contract: attach() / start() / render() are allocation-free.

#include <cstdint>

#include "midi/synth/sample_bank.h"
#include "midi/synth/sample_reader.h"

namespace sonare::midi::synth {

/// Sample section of a NativeSynthPatch (used when mode == kSample).
struct SamplePatchParams {
  /// Keymap set in the attached bank; a set the bank does not have is silent.
  int32_t set_index = 0;
  /// Linear gain on the sample, before the voice's own amp stage.
  float level = 1.0f;
  /// Overrides the sample's loop mode (0 = none, 1 = continuous, 3 = while key
  /// down); negative keeps what the bank recorded.
  int32_t loop_override = -1;
  /// Attack skip, as a fraction of the region.
  float start_offset01 = 0.0f;
  /// False plays every key at the sample's recorded pitch (one-shot drums).
  bool key_track = true;
};

/// One voice's sample source.
class SampleVoiceCore {
 public:
  void attach(const SampleBank* bank) noexcept { bank_ = bank; }

  /// Resolves (@p note, @p velocity) against the patch's keymap set. False when
  /// nothing covers the note, which leaves the voice silent.
  bool start(const SamplePatchParams& p, double sample_rate, uint8_t note,
             uint8_t velocity) noexcept;

  /// True once the region ran out; a looping zone never reports it while held.
  bool finished() const noexcept { return finished_; }
  /// Zone gain folded with the patch level.
  float gain() const noexcept { return gain_; }
  /// Zone pan in SF2 units, for the voice's pan sum.
  float pan_units() const noexcept { return pan_units_; }

  /// Advances one sample. @p pitch_ratio is the voice's accumulated pitch
  /// modulation (1.0 = the note as resolved at start).
  float render(float pitch_ratio, bool key_down) noexcept;

 private:
  const SampleBank* bank_ = nullptr;
  SampleReader reader_;
  double increment_ = 1.0;
  float gain_ = 1.0f;
  float pan_units_ = 0.0f;
  bool finished_ = true;
};

}  // namespace sonare::midi::synth
